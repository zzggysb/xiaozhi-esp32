#include "alarm/alarm_manager.h"

#include <esp_err.h>
#include <esp_log.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "cjson_utils.h"
#include "display.h"
#include "settings.h"

#define TAG "Alarm"

namespace {

constexpr const char* kNamespace = "alarm";
constexpr const char* kItemsKey = "items";

// 响铃节奏：每 1.5 秒一声，最多 20 声（约 30 秒）后自动停
constexpr int64_t kRingIntervalMs = 1500;
constexpr int kRingMaxBeeps = 20;

// 2025-01-01 00:00:00，早于它的系统时间视为"还没同步过"
constexpr time_t kTimeValidThreshold = 1735689600;

const char* KindName(AlarmManager::Kind kind) {
    switch (kind) {
        case AlarmManager::Kind::kAlarm:
            return "alarm";
        case AlarmManager::Kind::kReminder:
            return "reminder";
        case AlarmManager::Kind::kTimer:
            return "timer";
    }
    return "unknown";
}

std::string Trimmed(const std::string& text) {
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

}  // namespace

AlarmManager& AlarmManager::GetInstance() {
    static AlarmManager instance;
    return instance;
}

AlarmManager::~AlarmManager() {
    if (tick_timer_ != nullptr) {
        esp_timer_stop(tick_timer_);
        esp_timer_delete(tick_timer_);
        tick_timer_ = nullptr;
    }
    if (ring_timer_ != nullptr) {
        esp_timer_stop(ring_timer_);
        esp_timer_delete(ring_timer_);
        ring_timer_ = nullptr;
    }
}

int64_t AlarmManager::NowUs() { return esp_timer_get_time(); }

bool AlarmManager::IsWallClockValid() { return time(nullptr) >= kTimeValidThreshold; }

void AlarmManager::Initialize() {
    if (initialized_) {
        return;
    }
    initialized_ = true;

    Load();

    esp_timer_create_args_t tick_args = {
        .callback =
            [](void* arg) {
                static_cast<AlarmManager*>(arg)->Tick();
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alarm_tick",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer_, 1000000));  // 1 秒

    // 响铃定时器：创建后不启动，响铃时再 start
    esp_timer_create_args_t ring_args = {
        .callback =
            [](void* arg) {
                auto* self = static_cast<AlarmManager*>(arg);
                bool keep_going = false;
                {
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    if (!self->ringing_) {
                        return;
                    }
                    self->ring_beeps_++;
                    keep_going = self->ring_beeps_ < kRingMaxBeeps;
                }
                if (!keep_going) {
                    // 不再从定时器自己的回调里 stop 自己，丢回主任务处理更稳妥
                    Application::GetInstance().Schedule([self]() {
                        self->StopRinging("timeout");
                    });
                    return;
                }
                Application::GetInstance().Schedule([self]() {
                    self->ShowRingFrame();
                    int beeps = 0;
                    {
                        std::lock_guard<std::mutex> lock(self->mutex_);
                        beeps = self->ring_beeps_;
                    }
                    // 两种音效交替，比单音循环更"像闹钟"，也更容易被听见
                    Application::GetInstance().PlaySound(
                        (beeps % 2 == 0) ? Lang::Sounds::OGG_EXCLAMATION : Lang::Sounds::OGG_POPUP);
                });
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alarm_ring",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&ring_args, &ring_timer_));

    size_t daily = 0;
    size_t once = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : items_) {
            if (item.kind == Kind::kAlarm && item.daily) {
                daily++;
            } else {
                once++;
            }
        }
    }
    ESP_LOGI(TAG, "AlarmManager ready: %u daily alarm(s), %u one-shot task(s)",
             static_cast<unsigned>(daily), static_cast<unsigned>(once));
}

uint8_t AlarmManager::AllocId() {
    for (uint8_t id = 1; id < 255; ++id) {
        bool used = false;
        for (const auto& item : items_) {
            if (item.id == id) {
                used = true;
                break;
            }
        }
        if (!used) {
            return id;
        }
    }
    return 255;
}

AlarmManager::Item* AlarmManager::FindById(uint8_t id) {
    for (auto& item : items_) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

void AlarmManager::ReArm(Item& item) {
    if (item.kind == Kind::kAlarm && item.daily) {
        item.due_us = 0;
        return;
    }
    if (!IsWallClockValid() || item.due_epoch == 0) {
        item.due_us = 0;  // 等时间同步后再装填
        return;
    }
    const int64_t delta_s = static_cast<int64_t>(item.due_epoch) - static_cast<int64_t>(time(nullptr));
    item.due_us = NowUs() + delta_s * 1000000LL;
}

bool AlarmManager::AddAlarm(int hour, int minute, bool daily, int day_offset,
                            const std::string& label, std::string& error) {
    if (hour < 0 || hour > 23) {
        error = "hour 必须在 0-23 之间";
        return false;
    }
    if (minute < 0 || minute > 59) {
        error = "minute 必须在 0-59 之间";
        return false;
    }
    if (day_offset < 0 || day_offset > 7) {
        error = "day_offset 必须在 0-7 之间";
        return false;
    }
    if (!daily && !IsWallClockValid()) {
        error = "设备还没跟服务器对过时间（需要先联网开机一次），现在只能设每天重复的闹钟或相对提醒";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (items_.size() >= kMaxItems) {
        error = "闹钟/提醒最多存 8 个，请先取消一个";
        return false;
    }

    Item item;
    item.id = AllocId();
    item.kind = Kind::kAlarm;
    item.daily = daily;
    item.hour = hour;
    item.minute = minute;
    item.label = Trimmed(label);

    if (!daily) {
        const time_t now = time(nullptr);
        struct tm tm_now = {};
        localtime_r(&now, &tm_now);
        const time_t day_start = now - (tm_now.tm_hour * 3600 + tm_now.tm_min * 60 + tm_now.tm_sec);
        item.due_epoch = day_start + static_cast<time_t>(day_offset) * 86400 + hour * 3600 + minute * 60;
        // "今天 7 点"但此刻已过 7 点 → 顺延到明天，避免设完立刻响
        if (day_offset == 0 && item.due_epoch <= now) {
            item.due_epoch += 86400;
        }
        ReArm(item);
    }

    items_.push_back(item);
    Save();
    error.clear();
    ESP_LOGI(TAG, "Add alarm id=%d %02d:%02d daily=%d label=%s", item.id, hour, minute,
             daily ? 1 : 0, item.label.c_str());
    return true;
}

bool AlarmManager::AddReminder(int minutes, const std::string& label, std::string& error) {
    if (minutes <= 0 || minutes > 1440) {
        error = "minutes 必须在 1-1440 之间";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (items_.size() >= kMaxItems) {
        error = "闹钟/提醒最多存 8 个，请先取消一个";
        return false;
    }

    Item item;
    item.id = AllocId();
    item.kind = Kind::kReminder;
    item.duration_s = minutes * 60;
    item.label = Trimmed(label);
    item.due_us = NowUs() + static_cast<int64_t>(minutes) * 60 * 1000000LL;
    item.due_epoch = time(nullptr) + static_cast<time_t>(minutes) * 60;

    items_.push_back(item);
    Save();
    error.clear();
    ESP_LOGI(TAG, "Add reminder id=%d %d min", item.id, minutes);
    return true;
}

bool AlarmManager::AddTimer(int seconds, const std::string& label, std::string& error) {
    if (seconds <= 0 || seconds > 86400) {
        error = "seconds 必须在 1-86400 之间";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (items_.size() >= kMaxItems) {
        error = "闹钟/提醒最多存 8 个，请先取消一个";
        return false;
    }

    Item item;
    item.id = AllocId();
    item.kind = Kind::kTimer;
    item.duration_s = seconds;
    item.label = Trimmed(label);
    item.due_us = NowUs() + static_cast<int64_t>(seconds) * 1000000LL;
    item.due_epoch = time(nullptr) + seconds;

    items_.push_back(item);
    Save();
    error.clear();
    ESP_LOGI(TAG, "Add timer id=%d %d s", item.id, seconds);
    return true;
}

bool AlarmManager::Cancel(int id, std::string& error) {
    bool stopped_ringing = false;
    size_t removed = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id == 0) {
            removed = items_.size();
            stopped_ringing = ringing_;
            items_.clear();
        } else {
            auto it = std::remove_if(items_.begin(), items_.end(),
                                     [id](const Item& item) { return item.id == id; });
            removed = static_cast<size_t>(std::distance(it, items_.end()));
            items_.erase(it, items_.end());
            if (removed > 0 && ringing_ && ringing_id_ == id) {
                stopped_ringing = true;
            }
        }
        if (removed > 0) {
            Save();
        }
    }

    if (removed == 0) {
        error = id == 0 ? "当前没有任何闹钟或提醒" : "没找到编号为 " + std::to_string(id) + " 的任务";
        return false;
    }
    if (stopped_ringing) {
        StopRinging("cancelled");
    }
    error.clear();
    ESP_LOGI(TAG, "Cancel id=%d removed=%u", id, static_cast<unsigned>(removed));
    return true;
}

void AlarmManager::Tick() {
    // 开机 20 秒内不判到期：音频和屏幕还在初始化，这时候响铃只会被吃掉或者出错。
    // 闹钟最小粒度是分钟，缓冲 20 秒对她没有可感知的影响。
    constexpr int64_t kBootGraceUs = 20LL * 1000 * 1000;
    if (NowUs() < kBootGraceUs) {
        return;
    }

    std::vector<Item> due;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const time_t now = time(nullptr);
        const bool time_ok = IsWallClockValid();
        struct tm tm_now = {};
        localtime_r(&now, &tm_now);
        const int64_t now_us = NowUs();

        bool need_save = false;
        std::vector<uint8_t> fired_once;

        for (auto& item : items_) {
            if (item.kind == Kind::kAlarm && item.daily) {
                if (!time_ok) {
                    continue;
                }
                if (tm_now.tm_hour == item.hour && tm_now.tm_min == item.minute &&
                    item.last_fired_yday != tm_now.tm_yday) {
                    item.last_fired_yday = tm_now.tm_yday;
                    due.push_back(item);
                    need_save = true;
                }
                continue;
            }

            if (item.due_us == 0) {
                if (time_ok) {
                    ReArm(item);  // 开机时还没对时，对上了再装填
                    need_save = true;
                }
                continue;
            }

            if (now_us >= item.due_us) {
                due.push_back(item);
                fired_once.push_back(item.id);
            }
        }

        if (!fired_once.empty()) {
            items_.erase(std::remove_if(items_.begin(), items_.end(),
                                        [&fired_once](const Item& item) {
                                            return std::find(fired_once.begin(), fired_once.end(),
                                                             item.id) != fired_once.end();
                                        }),
                         items_.end());
            need_save = true;
        }

        if (need_save) {
            Save();
        }
    }

    for (const auto& item : due) {
        // 响铃涉及字符串拼接、屏幕刷新、音频入队，统一丢回主任务做，
        // 不在 esp_timer 的小栈里冒险
        Application::GetInstance().Schedule([this, item]() { Fire(item); });
    }
}

void AlarmManager::Fire(const Item& item) {
    std::string text = Trimmed(DescribeItem(item));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ringing_ = true;
        ringing_id_ = item.id;
        ring_beeps_ = 0;
        ringing_text_ = text;
    }

    ESP_LOGW(TAG, "Ringing: %s", text.c_str());
    if (ring_timer_ != nullptr) {
        esp_timer_stop(ring_timer_);
        esp_timer_start_periodic(ring_timer_, kRingIntervalMs * 1000);
    }

    Application::GetInstance().Schedule([this]() {
        ShowRingFrame();
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_EXCLAMATION);
    });
}

void AlarmManager::ShowRingFrame() {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        text = ringing_text_;
    }
    if (text.empty()) {
        return;
    }
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->SetChatMessage("system", text.c_str());
    }
}

void AlarmManager::StopRinging(const char* reason) {
    bool was_ringing = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        was_ringing = ringing_;
        ringing_ = false;
        ring_beeps_ = 0;
        ringing_id_ = 0;
        ringing_text_.clear();
    }
    if (ring_timer_ != nullptr) {
        esp_timer_stop(ring_timer_);
    }
    if (!was_ringing) {
        return;
    }
    ESP_LOGI(TAG, "Ringing stopped (%s)", reason != nullptr ? reason : "user");
    Application::GetInstance().Schedule([]() {
        if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->SetChatMessage("system", "");
            }
        }
    });
}

bool AlarmManager::IsRinging() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ringing_;
}

std::string AlarmManager::FormatClock(const Item& item) {
    char buffer[8];
    snprintf(buffer, sizeof(buffer), "%02d:%02d", item.hour, item.minute);
    return std::string(buffer);
}

std::string AlarmManager::DescribeItem(const Item& item) {
    const std::string label = item.label.empty() ? "" : " " + item.label;

    switch (item.kind) {
        case Kind::kAlarm:
            return "闹钟 " + FormatClock(item) + label;
        case Kind::kReminder:
            return "提醒时间到了" + label;
        case Kind::kTimer:
            return "倒计时结束" + label;
    }
    return "任务完成" + label;
}

std::string AlarmManager::DescribeAll() const {
    std::lock_guard<std::mutex> lock(mutex_);

    CJsonUniquePtr root(cJSON_CreateObject());
    if (root == nullptr) {
        return "{}";
    }

    CJsonUniquePtr items(cJSON_CreateArray());
    const time_t now = time(nullptr);
    const int64_t now_us = NowUs();
    const bool time_ok = IsWallClockValid();
    struct tm tm_now = {};
    localtime_r(&now, &tm_now);

    for (const auto& item : items_) {
        CJsonUniquePtr node(cJSON_CreateObject());
        if (node == nullptr) {
            continue;
        }
        cJSON_AddNumberToObject(node.get(), "id", item.id);
        cJSON_AddStringToObject(node.get(), "type", KindName(item.kind));
        if (!item.label.empty()) {
            cJSON_AddStringToObject(node.get(), "label", item.label.c_str());
        }

        if (item.kind == Kind::kAlarm) {
            cJSON_AddStringToObject(node.get(), "time", FormatClock(item).c_str());
            cJSON_AddBoolToObject(node.get(), "daily", item.daily);
            if (!item.daily && item.due_epoch != 0) {
                const int64_t delta = static_cast<int64_t>(item.due_epoch) - static_cast<int64_t>(now);
                std::string when = (delta < 86400 ? "今天 " : "明天 ") + FormatClock(item);
                cJSON_AddStringToObject(node.get(), "when", when.c_str());
                cJSON_AddNumberToObject(node.get(), "in_seconds",
                                        delta > 0 ? static_cast<double>(delta) : 0);
            }
        } else if (item.due_us != 0) {
            const int64_t remain_s = (item.due_us - now_us) / 1000000LL;
            cJSON_AddNumberToObject(node.get(), "in_seconds",
                                    remain_s > 0 ? static_cast<double>(remain_s) : 0);
        } else {
            cJSON_AddNumberToObject(node.get(), "in_seconds", item.duration_s);
        }

        cJSON_AddItemToArray(items.get(), node.release());
    }

    cJSON_AddNumberToObject(root.get(), "count", static_cast<double>(items_.size()));
    cJSON_AddBoolToObject(root.get(), "time_synced", time_ok);
    if (time_ok) {
        char clock[16];
        snprintf(clock, sizeof(clock), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        cJSON_AddStringToObject(root.get(), "now", clock);
    }
    cJSON_AddItemToObject(root.get(), "items", items.release());

    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return text != nullptr ? std::string(text.get()) : "{}";
}

void AlarmManager::Save() {
    // 调用方必须已持锁
    CJsonUniquePtr array(cJSON_CreateArray());
    if (array == nullptr) {
        return;
    }
    for (const auto& item : items_) {
        CJsonUniquePtr node(cJSON_CreateObject());
        if (node == nullptr) {
            continue;
        }
        cJSON_AddNumberToObject(node.get(), "id", item.id);
        cJSON_AddNumberToObject(node.get(), "kind", static_cast<int>(item.kind));
        cJSON_AddBoolToObject(node.get(), "daily", item.daily);
        cJSON_AddNumberToObject(node.get(), "hour", item.hour);
        cJSON_AddNumberToObject(node.get(), "minute", item.minute);
        cJSON_AddNumberToObject(node.get(), "due", static_cast<double>(item.due_epoch));
        cJSON_AddNumberToObject(node.get(), "duration", item.duration_s);
        cJSON_AddStringToObject(node.get(), "label", item.label.c_str());
        cJSON_AddNumberToObject(node.get(), "yday", item.last_fired_yday);
        cJSON_AddItemToArray(array.get(), node.release());
    }

    CJsonStringUniquePtr text(cJSON_PrintUnformatted(array.get()));
    if (text == nullptr) {
        return;
    }

    Settings settings(kNamespace, true);
    settings.SetString(kItemsKey, text.get());
}

void AlarmManager::Load() {
    Settings settings(kNamespace, false);
    std::string raw = settings.GetString(kItemsKey);
    if (raw.empty()) {
        return;
    }

    CJsonUniquePtr root(cJSON_Parse(raw.c_str()));
    if (root == nullptr || !cJSON_IsArray(root.get())) {
        ESP_LOGW(TAG, "Stored alarm list is corrupted, ignoring");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();

    const int count = cJSON_GetArraySize(root.get());
    for (int i = 0; i < count && items_.size() < kMaxItems; ++i) {
        cJSON* node = cJSON_GetArrayItem(root.get(), i);
        if (node == nullptr) {
            continue;
        }
        Item item;
        cJSON* id = cJSON_GetObjectItem(node, "id");
        cJSON* kind = cJSON_GetObjectItem(node, "kind");
        cJSON* daily = cJSON_GetObjectItem(node, "daily");
        cJSON* hour = cJSON_GetObjectItem(node, "hour");
        cJSON* minute = cJSON_GetObjectItem(node, "minute");
        cJSON* due = cJSON_GetObjectItem(node, "due");
        cJSON* duration = cJSON_GetObjectItem(node, "duration");
        cJSON* label = cJSON_GetObjectItem(node, "label");
        cJSON* yday = cJSON_GetObjectItem(node, "yday");

        if (!cJSON_IsNumber(id) || !cJSON_IsNumber(kind)) {
            continue;
        }
        item.id = static_cast<uint8_t>(id->valueint);
        item.kind = static_cast<Kind>(kind->valueint);
        item.daily = cJSON_IsTrue(daily);
        item.hour = cJSON_IsNumber(hour) ? hour->valueint : 0;
        item.minute = cJSON_IsNumber(minute) ? minute->valueint : 0;
        item.due_epoch = cJSON_IsNumber(due) ? static_cast<time_t>(due->valuedouble) : 0;
        item.duration_s = cJSON_IsNumber(duration) ? duration->valueint : 0;
        item.label = cJSON_IsString(label) ? label->valuestring : "";
        item.last_fired_yday = cJSON_IsNumber(yday) ? yday->valueint : -1;

        ReArm(item);
        items_.push_back(item);
    }

    ESP_LOGI(TAG, "Loaded %u item(s) from NVS", static_cast<unsigned>(items_.size()));
}
