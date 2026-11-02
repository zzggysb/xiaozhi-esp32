#include "wifi_board.h"
#include "alarm/alarm_manager.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "CompactWifiBoard"

class CompactWifiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .scl_speed_hz = 400 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        touch_button_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    // 物联网初始化，逐步迁移到 MCP 协议
    void InitializeTools() {
        static LampController lamp(LAMP_GPIO);
        InitializeAlarmTools();
    }

    // 闹钟 / 提醒 / 倒计时：设备本地调度，不依赖联网，掉电不丢
    void InitializeAlarmTools() {
        auto& alarm = AlarmManager::GetInstance();
        alarm.Initialize();
        auto& mcp = McpServer::GetInstance();

        mcp.AddTool("self.alarm.set",
                    "设置闹钟。用户说\"几点叫我\"\"定个闹钟\"\"每天早上 6 点叫我\"时调用。"
                    "hour/minute 是本地时间；repeat=daily 每天重复，repeat=once 只响一次；"
                    "day_offset 表示几天后（0=今天，1=明天），今日已过的时间自动顺延。",
                    PropertyList({
                        Property("hour", kPropertyTypeInteger, 0, 23),
                        Property("minute", kPropertyTypeInteger, 0, 59),
                        Property("repeat", kPropertyTypeString, std::string("daily")),
                        Property("day_offset", kPropertyTypeInteger, 0),
                        Property("label", kPropertyTypeString, std::string("")),
                    }),
                    [&alarm](const PropertyList& properties) -> ToolResult {
                        const int hour = properties["hour"].value<int>();
                        const int minute = properties["minute"].value<int>();
                        const std::string repeat = properties["repeat"].value<std::string>();
                        const int day_offset = properties["day_offset"].value<int>();
                        const std::string label = properties["label"].value<std::string>();
                        const bool daily = (repeat != "once");
                        std::string error;
                        if (!alarm.AddAlarm(hour, minute, daily, day_offset, label, error)) {
                            return std::unexpected(error);
                        }
                        return alarm.DescribeAll();
                    });

        mcp.AddTool("self.alarm.list",
                    "列出所有闹钟、提醒、倒计时，并返回设备当前时间。"
                    "回答\"设了什么\"\"还有多久\"\"现在几点\"前必须调用。",
                    PropertyList(),
                    [&alarm](const PropertyList& properties) -> ToolResult {
                        (void)properties;
                        return alarm.DescribeAll();
                    });

        mcp.AddTool("self.alarm.cancel",
                    "取消闹钟/提醒/倒计时。id 为编号，0 表示全部取消。",
                    PropertyList({Property("id", kPropertyTypeInteger, 0)}),
                    [&alarm](const PropertyList& properties) -> ToolResult {
                        const int id = properties["id"].value<int>();
                        std::string error;
                        if (!alarm.Cancel(id, error)) {
                            return std::unexpected(error);
                        }
                        return alarm.DescribeAll();
                    });

        mcp.AddTool("self.alarm.stop",
                    "立即停止正在响的铃声。用户说\"别响了\"\"关掉闹钟\"时调用。",
                    PropertyList(),
                    [&alarm](const PropertyList& properties) -> ToolResult {
                        (void)properties;
                        if (!alarm.IsRinging()) {
                            return std::string("{\"ringing\":false}");
                        }
                        alarm.StopRinging("tool");
                        return std::string("{\"ringing\":false,\"stopped\":true}");
                    });

        mcp.AddTool("self.reminder.set",
                    "一次性提醒，minutes 分钟后响。用户说\"N 分钟后提醒我\"时调用。",
                    PropertyList({
                        Property("minutes", kPropertyTypeInteger, 1, 1440),
                        Property("label", kPropertyTypeString, std::string("")),
                    }),
                    [&alarm](const PropertyList& properties) -> ToolResult {
                        const int minutes = properties["minutes"].value<int>();
                        const std::string label = properties["label"].value<std::string>();
                        std::string error;
                        if (!alarm.AddReminder(minutes, label, error)) {
                            return std::unexpected(error);
                        }
                        return alarm.DescribeAll();
                    });

        mcp.AddTool("self.timer.set",
                    "设置倒计时，seconds 秒后响。用户说\"倒计时 N 分钟\"时用，分钟换算成秒。",
                    PropertyList({
                        Property("seconds", kPropertyTypeInteger, 1, 86400),
                        Property("label", kPropertyTypeString, std::string("")),
                    }),
                    [&alarm](const PropertyList& properties) -> ToolResult {
                        const int seconds = properties["seconds"].value<int>();
                        const std::string label = properties["label"].value<std::string>();
                        std::string error;
                        if (!alarm.AddTimer(seconds, label, error)) {
                            return std::unexpected(error);
                        }
                        return alarm.DescribeAll();
                    });
    }

public:
    CompactWifiBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializeTools();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(CompactWifiBoard);
