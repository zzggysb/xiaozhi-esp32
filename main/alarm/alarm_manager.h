#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include <esp_timer.h>

// 本地闹钟 / 提醒 / 倒计时
//
// 三类任务共用一套调度：
//   kAlarm     闹钟，本地时间 HH:MM 触发，可每日重复或一次性（可指定今天/明天）
//   kReminder  提醒，"N 分钟后提醒我"，一次性
//   kTimer     倒计时，"倒计时 N 分钟"，一次性
//
// 时间策略（这块是本模块最容易出错的地方，写清楚）：
//   1. 设备时间来自 OTA 下发的 server_time，且已经加过时区偏移，所以统一用
//      localtime_r 取"墙上时间"，和 LVGL 状态栏时钟保持一致；
//   2. 每日闹钟按 HH:MM 比较，并用"年内第几天"去重，避免同一分钟内重复响；
//   3. 一次性任务同时保存两个到期点：
//        - due_epoch：墙上时间，掉电重启后能恢复
//        - due_us：单调时钟，校时跳变也不会误触发
//      两者不一致时以单调时钟为准，due_us 为 0 表示"还没装填"，等时间同步后再算；
//   4. 设备时间还没同步（未联网/首次开机）时，拒绝设置绝对时间闹钟，
//      但相对时间的提醒/倒计时照常可用。
class AlarmManager {
public:
    enum class Kind : uint8_t {
        kAlarm = 0,     // 闹钟
        kReminder = 1,  // 提醒
        kTimer = 2,     // 倒计时
    };

    static constexpr size_t kMaxItems = 8;

    struct Item {
        uint8_t id = 0;
        Kind kind = Kind::kAlarm;
        bool daily = false;      // 仅闹钟有效
        int hour = 0;            // 仅闹钟有效（本地时间）
        int minute = 0;          // 仅闹钟有效
        time_t due_epoch = 0;    // 一次性任务的墙上到期时间
        int64_t due_us = 0;      // 一次性任务的单调到期点（0 = 待装填）
        int duration_s = 0;      // 提醒/倒计时的原始时长，用于展示
        std::string label;       // 用户说的事情，可为空
        int last_fired_yday = -1;
    };

    static AlarmManager& GetInstance();

    // 幂等：重复调用只生效一次
    void Initialize();

    // 返回空字符串表示成功，否则是错误的用户可读说明
    bool AddAlarm(int hour, int minute, bool daily, int day_offset, const std::string& label,
                  std::string& error);
    bool AddReminder(int minutes, const std::string& label, std::string& error);
    bool AddTimer(int seconds, const std::string& label, std::string& error);

    // id == 0 表示取消全部
    bool Cancel(int id, std::string& error);

    // 给 MCP 工具返回的 JSON 文本
    std::string DescribeAll() const;

    // 停止正在响的铃声（唤醒词/按键时会调用）
    void StopRinging(const char* reason);
    bool IsRinging() const;

    // 设备时间是否可信（OTA 同步过）
    static bool IsWallClockValid();

private:
    AlarmManager() = default;
    ~AlarmManager();

    AlarmManager(const AlarmManager&) = delete;
    AlarmManager& operator=(const AlarmManager&) = delete;

    mutable std::mutex mutex_;
    std::vector<Item> items_;
    esp_timer_handle_t tick_timer_ = nullptr;
    esp_timer_handle_t ring_timer_ = nullptr;
    bool initialized_ = false;
    bool ringing_ = false;
    uint8_t ringing_id_ = 0;
    int ring_beeps_ = 0;
    std::string ringing_text_;

    void Tick();
    void Fire(const Item& item);
    void ShowRingFrame();
    void ReArm(Item& item);
    void Load();
    void Save();
    Item* FindById(uint8_t id);
    uint8_t AllocId();
    static int64_t NowUs();
    static std::string FormatClock(const Item& item);
    static std::string DescribeItem(const Item& item);
};

#endif  // ALARM_MANAGER_H
