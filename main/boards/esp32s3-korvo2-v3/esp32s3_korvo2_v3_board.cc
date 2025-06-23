#include "wifi_board.h"
#include "audio_codecs/box_audio_codec.h"
#include "display/spi_lcd_anim_display.h" //"display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "iot/thing_manager.h"
#include "alarm_info.h"
#include "alarm_manager.h"
#include "clock_ui.h"
#include "time_sync_manager.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_io_expander_tca9554.h>
#include <esp_lcd_ili9341.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>

#include "pcf8574.h"
#include "file_manager.h"

// #include "camera_service.h"
// #include "display/spi_lcd_anim_display.h"
// #include "esp_camera.h"
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include "esp32_camera.h"

#define TAG "esp32s3_korvo2_v3"


LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

// Init ili9341 by custom cmd
static const ili9341_lcd_init_cmd_t vendor_specific_init[] = {
    {0xC8, (uint8_t []){0xFF, 0x93, 0x42}, 3, 0},
    {0xC0, (uint8_t []){0x0E, 0x0E}, 2, 0},
    {0xC5, (uint8_t []){0xD0}, 1, 0},
    {0xC1, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39, 0x48, 0x02, 0x0a, 0x08, 0x17, 0x17, 0x0F}, 15, 0},
    {0xE1, (uint8_t []){0x00, 0x28, 0x29, 0x01, 0x0d, 0x03, 0x3f, 0x33, 0x52, 0x04, 0x0f, 0x0e, 0x37, 0x38, 0x0F}, 15, 0},

    {0xB1, (uint8_t []){00, 0x1B}, 2, 0},
    {0x36, (uint8_t []){0x08}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0},
    {0xB7, (uint8_t []){0x06}, 1, 0},

    {0x11, (uint8_t []){0}, 0x80, 0},
    {0x29, (uint8_t []){0}, 0x80, 0},

    {0, (uint8_t []){0}, 0xff, 0},
};


class Esp32S3Korvo2V3Board : public WifiBoard {
private:
    Button boot_button_;
    i2c_master_bus_handle_t i2c_bus_;
    Cst816x *cst816d_;
    Pcf8574 *pcf8574_;
    LcdDisplay* display_;
    esp_io_expander_handle_t io_expander_ = NULL;
    esp_io_expander_handle_t io_expander2_ = NULL;
    Esp32Camera* camera_;
    ClockUI* clock_ui_;
    bool clock_enabled_;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            // .freq_hz = 100000, // 设置I2C频率为400kHz
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void I2cDetect() {
        uint8_t address;
        printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
        for (int i = 0; i < 128; i += 16) {
            printf("%02x: ", i);
            for (int j = 0; j < 16; j++) {
                fflush(stdout);
                address = i + j;
                esp_err_t ret = i2c_master_probe(i2c_bus_, address, pdMS_TO_TICKS(200));
                if (ret == ESP_OK) {
                    printf("%02x ", address);
                } else if (ret == ESP_ERR_TIMEOUT) {
                    printf("UU ");
                } else {
                    printf("-- ");
                }
            }
            printf("\r\n");
        }
    }

    static void touchpad_daemon(void *param)
    {
        vTaskDelay(pdMS_TO_TICKS(2000));
        auto &board = (Esp32S3Korvo2V3Board &)Board::GetInstance();
        auto touchpad = board.GetTouchpad();
        
        // 检查触摸屏是否初始化成功
        if (touchpad == nullptr) {
            ESP_LOGW(TAG, "Touchpad not available, exiting touchpad daemon");
            vTaskDelete(NULL);
            return;
        }
        
        bool was_touched = false;
        while (1)
        {
            touchpad->UpdateTouchPoint();
            if (touchpad->GetTouchPoint().num > 0)
            {
                // On press
                if (!was_touched)
                {
                    was_touched = true;
                    Application::GetInstance().ToggleChatState();
                }
            }
            // On release
            else if (was_touched)
            {
                was_touched = false;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelete(NULL);
    }
    
    void InitCst816d()
    {
        ESP_LOGI(TAG, "Init CST816x");
        
        // 检查CST816x是否存在
        esp_err_t ret = i2c_master_probe(i2c_bus_, 0x15, pdMS_TO_TICKS(100));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "CST816x not found at address 0x15, skipping touch initialization");
            cst816d_ = nullptr;
            return;
        }
        
        try
        {
            cst816d_ = new Cst816x(i2c_bus_, 0x15);
            xTaskCreate(touchpad_daemon, "tp", 2048, NULL, 9, NULL);
            ESP_LOGI(TAG, "CST816x initialized successfully");
        }
        catch(const std::exception& e)
        {
            ESP_LOGE(TAG, "Failed to initialize CST816x: %s", e.what());
            cst816d_ = nullptr;
        }
    }
    static void motor_daemon(void *param)
    {
        vTaskDelay(pdMS_TO_TICKS(3800));
        auto &board = (Esp32S3Korvo2V3Board &)Board::GetInstance();
        auto motor = board.SetMotor();
        
        motor->motor_reset();//step_test();
        // motor->control_motor(1, 100, 0);
        Application::GetInstance().ToggleChatState();
        auto protocol = Application::GetInstance().GetProtocolPtr();
        if (protocol) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            protocol->SendWakeWordDetected("你好");
        } else {
            ESP_LOGE(TAG, "Protocol not initialized");
        }
         vTaskDelay(pdMS_TO_TICKS(10));
        vTaskDelete(NULL);
    }

    void InitPcf8574()
    {
        ESP_LOGI(TAG, "Init Pcf8574");
        pcf8574_ = new Pcf8574(i2c_bus_, 0x27,io_expander_);
        
        // xTaskCreate(motor_daemon, "motor", 2048*2, NULL, 1, NULL);
        // 1开机：亮屏，复位+抬头，倾听动画
        // 2倾听：亮屏，抬头，倾听动画
        // 3说话：亮屏，抬头，说话动画+表情动画
        // 4待机： 黑屏，低头
    }
    // void InitializeTca9554_2() {
    //     return;
    //     esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_001, &io_expander2_);
    //     if(ret != ESP_OK) {
    //         ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_001, &io_expander2_);
    //         if(ret != ESP_OK) {
    //             ESP_LOGE(TAG, "TCA9554 create returned error");  
    //             return;
    //         }
    //     }
    //     // 配置IO0-IO3为输出模式 
    //     // IO0=nc,IO1=usb-det, IO2=chrg, IO3=rtc-int, IO4=hall1, IO5=hall2, IO6=ir, IO7=motor-en
    //     ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander2_, IO_EXPANDER_PIN_NUM_7 ,  IO_EXPANDER_OUTPUT));
    //     // ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander2_, 
    //     //     IO_EXPANDER_PIN_NUM_4 | IO_EXPANDER_PIN_NUM_1 | 
    //     //     IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_5 | IO_EXPANDER_PIN_NUM_3, 
    //     //     IO_EXPANDER_INPUT));

    //     // 复位LCD和TouchPad
    //     ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander2_, IO_EXPANDER_PIN_NUM_7, 1));
    // }
    void InitializeTca9554() {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander_);
        if(ret != ESP_OK) {
            ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_000, &io_expander_);
            if(ret != ESP_OK) {
                ESP_LOGE(TAG, "TCA9554 create returned error");  
                return;
            }
        }
        // 配置IO0-IO3为输出模式
        ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander_, 
            IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | 
            IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_3  | IO_EXPANDER_PIN_NUM_7, 
            IO_EXPANDER_OUTPUT));

        // 复位LCD和TouchPad
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_,
            IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2, 1));
        vTaskDelay(pdMS_TO_TICKS(300));
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_,
            IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2, 0));
        vTaskDelay(pdMS_TO_TICKS(300));
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_,
            IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2, 1));
    }

    void EnableLcdCs() {
        if(io_expander_ != NULL) {
            esp_io_expander_set_level(io_expander_, IO_EXPANDER_PIN_NUM_3, 0);// 置低 LCD CS
        }
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_0;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_1;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeIli9341Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_NC;
        io_config.dc_gpio_num = GPIO_NUM_2;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const ili9341_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(ili9341_lcd_init_cmd_t),
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        // panel_config.flags.reset_active_high = 0,
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void *)&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        EnableLcdCs();
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                    {
                                        .text_font = &font_puhui_20_4,
                                        .icon_font = &font_awesome_20_4,
                                        .emoji_font = font_emoji_64_init(),
                                    });
    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_46;
        io_config.dc_gpio_num = GPIO_NUM_2;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 60 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        EnableLcdCs();
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));

        display_ = new SpiLcdAnimDisplay(panel_io, panel,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                     {
                                         .text_font = &font_puhui_20_4,
                                         .icon_font = &font_awesome_20_4,
                                         .emoji_font = font_emoji_64_init(),
                                     });
    }

    void InitializeCamera() {
        // Open camera power

        camera_config_t config = {};
        config.ledc_channel = LEDC_CHANNEL_2;  // LEDC通道选择  用于生成XCLK时钟 但是S3不用
        config.ledc_timer = LEDC_TIMER_2; // LEDC timer选择  用于生成XCLK时钟 但是S3不用
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = -1;   // 这里写-1 表示使用已经初始化的I2C接口
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 1;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_SVGA;//FRAMESIZE_VGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        camera_ = new Esp32Camera(config);
    }
    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Chassis"));
        // thing_manager.AddThing(iot::CreateThing("Camera"));
        thing_manager.AddThing(iot::CreateThing("ImageDisplayer"));
    }

        void init_sd()
    {
        esp_err_t ret = fm_sdcard_init();
        if(ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize SD card, error=%d", ret);
            // return;
        }
        // ESP_LOGI(TAG, "Initializing esp32s3_korvo2_v3 Board");
        // #elif CONFIG_IDF_TARGET_ESP32P4
    }
    void init_camera1()
    {
   
//    int w = 0, h = 0;
//     size_t buf_size = 240 * 240 * 2;
//     static uint8_t* buf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
//     if (CameraService::GetInstance().TakePhotoRgb565ToBuffer(buf, buf_size, w, h)) {
//         static_cast<SpiLcdAnimDisplay*>(display_)->ShowRgb565(buf, w, h);
//     }
  
    }

      // 摄像头硬件初始化
    void InitializeCamera_mc(void)
    {

        camera_config_t config;
        config.ledc_channel = LEDC_CHANNEL_1;  // LEDC通道选择  用于生成XCLK时钟 但是S3不用
        config.ledc_timer = LEDC_TIMER_1; // LEDC timer选择  用于生成XCLK时钟 但是S3不用
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = -1;   // 这里写-1 表示使用已经初始化的I2C接口
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 1;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_240X240;
        config.jpeg_quality = 12;
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        // camera init
        esp_err_t err = esp_camera_init(&config); // 配置上面定义的参数
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
            return;
        }

        sensor_t *s = esp_camera_sensor_get(); // 获取摄像头型号

        if (s->id.PID == GC0308_PID) {
            s->set_hmirror(s, 1);  // 这里控制摄像头镜像 写1镜像 写0不镜像
        }
    }

    void InitializeClockAndAlarm() {
        ESP_LOGI(TAG, "Initializing Time Sync Manager, Clock UI and Alarm Manager");
        
        // 初始化时间同步管理器
        auto& time_sync_manager = TimeSyncManager::GetInstance();
        if (!time_sync_manager.Initialize(i2c_bus_)) {
            ESP_LOGE(TAG, "Failed to initialize TimeSyncManager");
            return;
        }
        
        // 创建时钟UI（延迟初始化）
        clock_ui_ = new ClockUI();
        
        // 初始化时钟UI（在Display准备好后）
        if (display_ && clock_ui_->Initialize(display_)) {
            if (time_sync_manager.GetRtc()) {
                clock_ui_->SetRtc(time_sync_manager.GetRtc());
            }
            ESP_LOGI(TAG, "Clock UI initialized successfully");
        } else {
            ESP_LOGE(TAG, "Failed to initialize Clock UI");
        }
        
        clock_enabled_ = false;
        
        // 初始化闹钟管理器
        auto& alarm_manager = AlarmManager::GetInstance();
        if (!alarm_manager.Initialize()) {
            ESP_LOGE(TAG, "Failed to initialize AlarmManager");
            return;
        }
        
        // 设置闹钟触发回调
        alarm_manager.SetAlarmCallback([this](const AlarmInfo& alarm) {
            OnAlarmTriggered(alarm);
        });
        
        // 注意：时间同步将在WiFi连接成功后自动触发
        
        ESP_LOGI(TAG, "Time Sync Manager, Clock UI and Alarm Manager initialized");
    }
    

    
    void OnAlarmTriggered(const AlarmInfo& alarm) {
        // 在定时器回调中只做最少的工作，避免任何可能的阻塞操作
        // 移除所有日志记录和其他操作，只创建异步任务
        
        // 创建异步任务来处理所有闹钟相关操作
        AlarmTaskParams* params = new AlarmTaskParams{this, alarm};
        xTaskCreate(HandleAlarmTask, "handle_alarm", 4096, params, 5, nullptr);
    }
    
    struct AlarmTaskParams {
        Esp32S3Korvo2V3Board* board;
        AlarmInfo alarm;
    };
    
    static void HandleAlarmTask(void* param) {
        auto* params = static_cast<AlarmTaskParams*>(param);
        auto* board = params->board;
        auto alarm = params->alarm;
        
        // 在异步任务中记录日志
        ESP_LOGI(TAG, "HandleAlarmTask: Processing alarm: %s", alarm.description.c_str());
        
        // 显示闹钟通知
        if (board->clock_ui_ && board->clock_enabled_) {
            board->clock_ui_->ShowAlarmNotification(alarm.description);
        }
        
        // 通知应用程序
        Application::GetInstance().Schedule([alarm]() {
            Application::GetInstance().Alert("闹钟提醒", alarm.description.c_str(), "happy");
        });
        
        // 延时隐藏通知
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (board->clock_ui_) {
            board->clock_ui_->HideAlarmNotification();
        }
        
        ESP_LOGI(TAG, "HandleAlarmTask: Alarm processing completed");
        delete params;
        vTaskDelete(nullptr);
    }

public:
    Esp32S3Korvo2V3Board() : boot_button_(BOOT_BUTTON_GPIO), clock_ui_(nullptr), clock_enabled_(false) {
        ESP_LOGI(TAG, "Initializing esp32s3_korvo2_v3 Board");
        InitializeI2c();
        I2cDetect();
        init_sd();
        InitCst816d();
        InitializeTca9554();
        InitPcf8574();
        InitializeCamera();
        InitializeSpi();
        InitializeButtons();
        #ifdef LCD_TYPE_ILI9341_SERIAL
        InitializeIli9341Display(); 
        #else
        InitializeSt7789Display(); 
        #endif
        // InitializeCamera_mc();
        InitializeIot();
        InitializeClockAndAlarm();
        
    }
    
    ~Esp32S3Korvo2V3Board() {
        if (clock_ui_) {
            delete clock_ui_;
            clock_ui_ = nullptr;
        }
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display *GetDisplay() override {
        return display_;
    }
        Cst816x *GetTouchpad()
    {
        return cst816d_;  // 可能为nullptr，调用者需要检查
    }
    
    virtual Pcf8574 *SetMotor() override
    {
        return pcf8574_;
    }
    virtual Camera* GetCamera() override {
        return camera_;
    }
    
    // 用于异步任务隐藏闹钟通知
    void HideAlarmNotificationInternal() {
        if (clock_ui_) {
            clock_ui_->HideAlarmNotification();
        }
    }
    
    // 时钟相关接口
    virtual void ShowClock() override {
        if (clock_ui_ && !clock_enabled_) {
            clock_ui_->Show();
            clock_enabled_ = true;
            
            // 更新下一个闹钟显示
            auto& alarm_manager = AlarmManager::GetInstance();
            AlarmInfo next_alarm = alarm_manager.GetNextAlarm();
            if (next_alarm.id > 0) {
                char alarm_text[64];
                snprintf(alarm_text, sizeof(alarm_text), "Next: %02d:%02d %s", 
                        next_alarm.hour, next_alarm.minute, next_alarm.description.c_str());
                clock_ui_->SetNextAlarm(alarm_text);
            } else {
                clock_ui_->SetNextAlarm("");
            }
            
            ESP_LOGI(TAG, "Clock UI shown");
        }
    }
    
    virtual void HideClock() override {
        if (clock_ui_ && clock_enabled_) {
            clock_ui_->Hide();
            clock_enabled_ = false;
            ESP_LOGI(TAG, "Clock UI hidden");
        }
    }
    
    virtual bool IsClockVisible() const override {
        return clock_enabled_ && clock_ui_ && clock_ui_->IsVisible();
    }
    
    // RTC时钟相关接口实现
    virtual bool InitializeRtcClock() override {
        auto& time_sync_manager = TimeSyncManager::GetInstance();
        return time_sync_manager.Initialize(i2c_bus_);
    }
    
    virtual void SyncTimeOnBoot() override {
        auto& time_sync_manager = TimeSyncManager::GetInstance();
        time_sync_manager.SyncTimeOnBoot();
    }
    
    // 重写WiFi网络启动，添加时间同步
    virtual void StartNetwork() override {
        // 先设置时间同步回调
        auto& wifi_station = WifiStation::GetInstance();
        
        // 保存原有的OnConnected回调
        wifi_station.OnConnected([this](const std::string& ssid) {
            auto display = Board::GetInstance().GetDisplay();
            std::string notification = Lang::Strings::CONNECTED_TO;
            notification += ssid;
            display->ShowNotification(notification.c_str(), 30000);
            
            ESP_LOGI(TAG, "WiFi connected to %s, triggering NTP sync", ssid.c_str());
            
            // 在任务中执行时间同步，避免阻塞WiFi回调
            xTaskCreate([](void* param) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                auto& time_sync_manager = TimeSyncManager::GetInstance();
                time_sync_manager.TriggerNtpSync();
                vTaskDelete(nullptr);
            }, "ntp_sync_task", 4096, nullptr, 5, nullptr);
        });
        
        // 调用基类的网络启动
        WifiBoard::StartNetwork();
    }
};

DECLARE_BOARD(Esp32S3Korvo2V3Board);
