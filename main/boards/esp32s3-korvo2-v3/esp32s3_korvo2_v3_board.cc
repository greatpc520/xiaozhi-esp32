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
#include <esp_task_wdt.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_io_expander_tca9554.h>
#include <esp_lcd_ili9341.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>
#include <ssid_manager.h>

#include "pcf8574.h"
#include "file_manager.h"

// #include "camera_service.h"
// #include "display/spi_lcd_anim_display.h"
// #include "esp_camera.h"
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include "esp32_camera.h"
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <fstream>
#include <vector>
#include <dirent.h>

#define TAG "esp32s3_korvo2_v3"
#define MOUNT_POINT "/sdcard"
#define AUDIO_FILE_EXTENSION ".P3"


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
    esp_timer_handle_t clock_update_timer_;

    // 轻量级栈监控函数（仅在需要时使用）
    static void CheckTaskStack(const char* task_name) {
        if (!task_name) return;
        
        try {
            TaskHandle_t task_handle = xTaskGetHandle(task_name);
            if (task_handle) {
                UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(task_handle);
                // 只在栈不足时警告
                if (stack_remaining < 200) { // 小于200字节时警告
                    ESP_LOGW(TAG, "Task '%s' low stack: %u bytes remaining", task_name, stack_remaining * sizeof(StackType_t));
                }
            }
        } catch (...) {
            // 忽略异常，不影响主线程
        }
    }
    
    // 轻量级内存检查（只在必要时使用）
    static void CheckMemoryIfNeeded() {
        try {
            size_t free_heap = esp_get_free_heap_size();
            // 只在内存真正不足时记录警告
            if (free_heap < 20000) { // 小于20KB时警告
                ESP_LOGW(TAG, "Low memory: %zu bytes", free_heap);
            }
        } catch (...) {
            // 忽略异常，不影响主线程
        }
    }

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
        // 等待系统启动
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        auto &board = (Esp32S3Korvo2V3Board &)Board::GetInstance();
        auto touchpad = board.GetTouchpad();
        
        // 检查触摸屏是否可用
        if (touchpad == nullptr) {
            ESP_LOGW(TAG, "Touchpad not available, exiting touchpad daemon");
            vTaskDelete(NULL);
            return;
        }
        
        ESP_LOGI(TAG, "Touchpad daemon started (simplified mode)");
        
        bool was_touched = false;
        TickType_t last_touch_time = 0;
        
        while (1)
        {
            try {
                auto current_state = Application::GetInstance().GetDeviceState();
                
                // 只在空闲和监听状态处理触摸
                if ((current_state == kDeviceStateIdle || current_state == kDeviceStateListening) && touchpad) {
                    touchpad->UpdateTouchPoint();
                    auto touch_point = touchpad->GetTouchPoint();
                    
                    TickType_t current_time = xTaskGetTickCount();
                    
                    if (touch_point.num > 0 && !was_touched) {
                        if ((current_time - last_touch_time) > pdMS_TO_TICKS(1000)) { // 1秒防抖
                            was_touched = true;
                            last_touch_time = current_time;
                            
                            ESP_LOGI(TAG, "Touch detected");
                            
                            // 直接调用，避免Schedule的复杂性
                            Application::GetInstance().ToggleChatState();
                        }
                    } else if (touch_point.num == 0 && was_touched) {
                        was_touched = false;
                    }
                }
            } catch (...) {
                ESP_LOGW(TAG, "Touch processing error, continuing...");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            
            // 降低刷新率，减少CPU占用
            vTaskDelay(pdMS_TO_TICKS(300)); // 300ms间隔
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
            xTaskCreate(touchpad_daemon, "tp", 4096, NULL, 2, NULL);  // 增加栈大小到4096，降低优先级到2
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
        ESP_LOGI(TAG, "Starting camera initialization");
        
        try {
            // 检查PSRAM可用空间，不足1MB时跳过摄像头初始化
            size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            if (free_spiram < 1024 * 1024) {
                ESP_LOGW(TAG, "Insufficient SPIRAM (%zu bytes), skipping camera initialization", free_spiram);
                camera_ = nullptr;
                return;
            }
            
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
            config.frame_size = FRAMESIZE_VGA;  // 降低分辨率，减少内存使用
            config.jpeg_quality = 12;
            config.fb_count = 1;  // 减少帧缓冲数量
            config.fb_location = CAMERA_FB_IN_PSRAM;
            config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

            camera_ = new Esp32Camera(config);
            
            if (camera_) {
                ESP_LOGI(TAG, "Camera initialized successfully");
            } else {
                ESP_LOGW(TAG, "Camera initialization returned null pointer");
            }
            
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "Exception during camera initialization: %s", e.what());
            camera_ = nullptr;
        } catch (...) {
            ESP_LOGE(TAG, "Unknown exception during camera initialization");
            camera_ = nullptr;
        }
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
            
            // 设置字体信息
            clock_ui_->SetFonts(&font_puhui_20_4, &font_awesome_20_4, font_emoji_64_init());
            
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
        
        // 创建异步任务来处理所有闹钟相关操作，增加栈大小
        AlarmTaskParams* params = new AlarmTaskParams{this, alarm};
        xTaskCreate(HandleAlarmTask, "handle_alarm", 6144, params, 3, nullptr); // 增加栈大小到6144，降低优先级到3
    }
    
    struct AlarmTaskParams {
        Esp32S3Korvo2V3Board* board;
        AlarmInfo alarm;
    };
    
    static void HandleAlarmTask(void* param) {
        // 注册任务到看门狗
        esp_task_wdt_add(NULL);
        
        auto* params = static_cast<AlarmTaskParams*>(param);
        auto* board = params->board;
        auto alarm = params->alarm;
        
        // 在异步任务中记录日志
        ESP_LOGI(TAG, "HandleAlarmTask: Processing alarm: %s", alarm.description.c_str());
        
        esp_task_wdt_reset(); // 重置看门狗
        
        // 异步显示时钟界面
        Application::GetInstance().Schedule([board, alarm]() {
            // 确保时钟界面可见
            if (!board->clock_enabled_) {
                board->ShowClock();
            }
            
            ESP_LOGI(TAG, "Clock UI shown, scheduling alarm notification: %s", alarm.description.c_str());
        });
        
        // 等待UI创建完成
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_task_wdt_reset(); // 重置看门狗
        
        // 直接在当前任务中处理通知，避免创建额外的嵌套任务
        Application::GetInstance().Schedule([board, description = alarm.description]() {
            if (board->clock_ui_) {
                board->clock_ui_->ShowAlarmNotification(description);
                // 同时设置相应的表情图标
                if (!description.empty()) {
                    board->clock_ui_->SetAlarmEmotion(description);
                }
                ESP_LOGI(TAG, "Alarm notification displayed: %s", description.c_str());
            } else {
                ESP_LOGE(TAG, "Failed to show alarm notification: clock_ui_ is null");
            }
        });
        
        // 通知应用程序
        Application::GetInstance().Schedule([alarm]() {
            Application::GetInstance().Alert("闹钟提醒", alarm.description.c_str(), "happy");
        });
        Application::GetInstance().PlaySound(Lang::Sounds::P3_SUCCESS);
        
        esp_task_wdt_reset(); // 重置看门狗
        
        // 自动播放音乐功能：检查服务器连接并发送播放音乐指令
        board->TriggerMusicPlayback(alarm);
        
        // 延时隐藏通知
        vTaskDelay(pdMS_TO_TICKS(8000)); // 延长显示时间到8秒
        esp_task_wdt_reset(); // 重置看门狗
        
        if (board->clock_ui_) {
            Application::GetInstance().Schedule([board]() {
                if (board->clock_ui_) {
                    board->clock_ui_->HideAlarmNotification();
                    // 注意：RefreshNextAlarmDisplay() 已经在 HideAlarmNotification() 中被调用
                }
            });
        }
        
        ESP_LOGI(TAG, "HandleAlarmTask: Alarm processing completed");
        
        // 清理看门狗注册
        esp_task_wdt_delete(NULL);
        delete params;
        vTaskDelete(nullptr);
    }
    
public:
    Esp32S3Korvo2V3Board() : boot_button_(BOOT_BUTTON_GPIO), clock_ui_(nullptr), clock_enabled_(false), clock_update_timer_(nullptr) {
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
        
        // 打印初始内存状态（轻量级检查）
        CheckMemoryIfNeeded();
        
        // 监控关键任务的栈使用情况（延迟调用，等待任务创建完成）
        // 完全移除初始化监控任务，避免栈溢出
        ESP_LOGI(TAG, "Board initialization completed, skipping resource monitoring to ensure stability");
        
    }
    
    ~Esp32S3Korvo2V3Board() {
        if (clock_update_timer_) {
            esp_timer_stop(clock_update_timer_);
            esp_timer_delete(clock_update_timer_);
            clock_update_timer_ = nullptr;
        }
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
        // 添加空指针检查，确保安全访问
        if (!camera_) {
            ESP_LOGD(TAG, "Camera not available (null pointer)");
        }
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
        if (!clock_ui_) {
            ESP_LOGW(TAG, "Clock UI not initialized");
            return;
        }
        
        if (clock_enabled_) {
            ESP_LOGD(TAG, "Clock already enabled, just updating display");
            // 直接更新显示，不使用异步调度
            if (clock_ui_ && clock_enabled_) {
                clock_ui_->UpdateClockDisplay();
            }
            return;
        }
        
        // 第一步：检查时间是否正常，如果不正常先同步时间
        auto& alarm_manager = AlarmManager::GetInstance();
        if (!alarm_manager.IsTimeValid()) {
            ESP_LOGW(TAG, "Time not valid, triggering sync before showing clock");
            auto& time_sync_manager = TimeSyncManager::GetInstance();
            time_sync_manager.TriggerNtpSync();
            
            // 异步延迟显示时钟，等待时间同步完成
            xTaskCreate([](void* param) {
                auto* board = static_cast<Esp32S3Korvo2V3Board*>(param);
                vTaskDelay(pdMS_TO_TICKS(5000)); // 等待5秒让时间同步
                
                // 重新尝试显示时钟
                if (board->clock_ui_ && !board->clock_enabled_) {
                    board->ShowClock();
                }
                vTaskDelete(nullptr);
            }, "delayed_clock", 2048, this, 2, nullptr);
            return;
        }
        
        // 第二步：清理过期的一次性闹钟，确保显示正确的下一个闹钟
        int removed_count = alarm_manager.RemoveExpiredAlarms();
        if (removed_count > 0) {
            ESP_LOGI(TAG, "ShowClock: Removed %d expired alarms", removed_count);
        }
        
        // 第三步：获取下一个闹钟信息（现在已经清理了过期闹钟）
        AlarmInfo next_alarm = alarm_manager.GetNextAlarm();
        
        ESP_LOGI(TAG, "ShowClock: GetNextAlarm returned ID=%d, Time=%02d:%02d, Description='%s'", 
                 next_alarm.id, next_alarm.hour, next_alarm.minute, next_alarm.description.c_str());
        

        

        
        // 第四步：显示时钟UI
        clock_ui_->Show();
        clock_enabled_ = true;
        
        // 第五步：等待UI创建完成后再设置闹钟信息
        // 使用异步调用确保在UI创建完成后设置闹钟显示
        if (next_alarm.id > 0) {
            // 构建闹钟显示文本，避免使用动态分配
            static char alarm_text[64]; // 使用静态变量减少栈使用
            
            // 闹钟时间需要使用相同的时区转换逻辑，确保与当前时间显示一致
            // 创建闹钟时间的时间戳，然后通过localtime转换为显示时间
            time_t current_time;
            struct tm alarm_tm;
            
            // 获取当前时间用于构建闹钟时间戳
            auto& time_sync_manager = TimeSyncManager::GetInstance();
            auto* rtc = time_sync_manager.GetRtc();
            
            if (rtc && rtc->GetTime(&current_time)) {
                localtime_r(&current_time, &alarm_tm);
            } else {
                time(&current_time);
                localtime_r(&current_time, &alarm_tm);
            }
            
            // 设置闹钟时间
            alarm_tm.tm_hour = next_alarm.hour;
            alarm_tm.tm_min = next_alarm.minute;
            alarm_tm.tm_sec = 0;
            
            // 转换为12小时制显示（使用与时间标签相同的逻辑）
            int display_hour = alarm_tm.tm_hour;
            // const char* am_pm = "AM";
            const char* am_pm = "上午";
            if (display_hour >= 12) {
                am_pm = "下午";
                if (display_hour > 12) {
                    display_hour -= 12;
                }
            } else if (display_hour == 0) {
                display_hour = 12;
            }
            
            snprintf(alarm_text, sizeof(alarm_text), "%02d:%02d %s", 
                    display_hour, alarm_tm.tm_min, am_pm);
            
            ESP_LOGI(TAG, "ShowClock: Scheduling alarm text setting: '%s'", alarm_text);
            
            // 异步设置闹钟信息，确保在UI创建完成后执行
            std::string alarm_text_str(alarm_text);
            std::string description = next_alarm.description;
            Application::GetInstance().Schedule([this, alarm_text_str, description]() {
                if (clock_ui_) {
                    clock_ui_->SetNextAlarm(alarm_text_str.c_str());
                    if (!description.empty()) {
                        clock_ui_->SetAlarmEmotion(description);
                    }
                    ESP_LOGI(TAG, "ShowClock: Alarm info set asynchronously");
                }
            });
        } else {
            ESP_LOGI(TAG, "ShowClock: Scheduling alarm clear");
            Application::GetInstance().Schedule([this]() {
                if (clock_ui_) {
                    clock_ui_->SetNextAlarm("");
                    clock_ui_->SetAlarmEmotion("");
                    ESP_LOGI(TAG, "ShowClock: Alarm info cleared asynchronously");
                }
            });
        }
        
        // 第五步：启动时钟更新定时器（低频率更新）
        if (!clock_update_timer_) {
            esp_timer_create_args_t timer_args = {
                .callback = ClockUpdateTimerCallback,
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "clock_update_timer"
            };
            esp_timer_create(&timer_args, &clock_update_timer_);
        }
        esp_timer_start_periodic(clock_update_timer_, 30 * 1000000); // 5分钟间隔，大幅减少资源占用
        
        ESP_LOGI(TAG, "Clock UI enabled with time validation and alarm cleanup");
    }
    
    virtual void HideClock() override {
        if (clock_ui_ && clock_enabled_) {
            clock_ui_->Hide();
            clock_enabled_ = false;
            
            // 停止时钟更新定时器
            if (clock_update_timer_) {
                esp_timer_stop(clock_update_timer_);
            }
            
            ESP_LOGI(TAG, "Clock UI hidden and update timer stopped");
        }
    }
    
    // 简化的时钟更新定时器回调
    static void ClockUpdateTimerCallback(void* arg) {
        auto* board = static_cast<Esp32S3Korvo2V3Board*>(arg);
        
        // 基本安全检查
        if (!board || !board->clock_ui_ || !board->clock_enabled_) {
            return;
        }
        
        // 简单的时间检查，大幅减少更新频率
        static time_t last_update = 0;
        time_t current_time = time(nullptr);
        
        // 只有当时间变化超过2分钟时才更新
        if (current_time - last_update < 30) {
            return;
        }
        last_update = current_time;
        
        // 直接更新，避免异步调度的复杂性
        try {
            board->clock_ui_->UpdateClockDisplay();
        } catch (...) {
            // 忽略异常，不影响系统稳定性
        }
    }
    
    virtual bool IsClockVisible() const override {
        return clock_enabled_ && clock_ui_ && clock_ui_->IsVisible();
    }
    
    // 时钟UI实例访问接口
    virtual ClockUI* GetClockUI() override {
        return clock_ui_;
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
    
    // 重写WiFi网络启动，添加智能时间同步
    virtual void StartNetwork() override {
        // 先设置WiFi回调，再调用基类的网络启动
        auto& wifi_station = WifiStation::GetInstance();
        
        // 设置扫描回调
        wifi_station.OnScanBegin([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
        });
        
        // 设置连接中回调  
        wifi_station.OnConnect([this](const std::string& ssid) {
            auto display = Board::GetInstance().GetDisplay();
            std::string notification = Lang::Strings::CONNECT_TO;
            notification += ssid;
            notification += "...";
            display->ShowNotification(notification.c_str(), 30000);
        });
        
        // 设置连接成功回调（包含NTP同步逻辑）
        wifi_station.OnConnected([this](const std::string& ssid) {
            auto display = Board::GetInstance().GetDisplay();
            std::string notification = Lang::Strings::CONNECTED_TO;
            notification += ssid;
            display->ShowNotification(notification.c_str(), 30000);
            
            ESP_LOGI(TAG, "WiFi connected to %s, scheduling smart NTP sync", ssid.c_str());
            
            // 启动智能时间同步（异步、避免资源冲突、自动重试）
            ScheduleSmartTimeSync();
        });
        
        // 调用基类的网络启动（但跳过基类的回调设置）
        // 直接调用核心逻辑，避免回调被覆盖
        if (wifi_config_mode_) {
            EnterWifiConfigMode();
            return;
        }

        // 检查WiFi配置
        auto& ssid_manager = SsidManager::GetInstance();
        auto ssid_list = ssid_manager.GetSsidList();
        if (ssid_list.empty()) {
            wifi_config_mode_ = true;
            EnterWifiConfigMode();
            return;
        }

        // 启动WiFi连接（回调已经设置）
        wifi_station.Start();

        // 等待连接
        if (!wifi_station.WaitForConnected(60 * 1000)) {
            wifi_station.Stop();
            wifi_config_mode_ = true;
            EnterWifiConfigMode();
            return;
        }
    }

private:
    void ScheduleSmartTimeSync() {
        ESP_LOGI(TAG, "Scheduling smart time sync after network connection");
        
        // 创建时间同步任务，延迟启动以确保网络稳定，增加栈大小
        xTaskCreate([](void* param) {
            // 注册任务到看门狗
            esp_task_wdt_add(NULL);
            
            // 获取board指针
            auto* board_ptr = static_cast<Esp32S3Korvo2V3Board*>(param);
            
            // 等待网络稳定
            vTaskDelay(pdMS_TO_TICKS(8000));
            esp_task_wdt_reset(); // 重置看门狗
            
            ESP_LOGI(TAG, "Starting smart time synchronization");
            auto& time_sync_manager = TimeSyncManager::GetInstance();
        
        // 设置NTP同步回调，包含时钟UI立即更新逻辑
        time_sync_manager.SetSyncCallback([board_ptr](bool success, const std::string& message) {
                if (success) {
                    ESP_LOGI(TAG, "Time sync completed successfully: %s", message.c_str());
                    
                    // 同步成功后显示通知
                    auto display = Board::GetInstance().GetDisplay();
                    if (display) {
                        display->ShowNotification("时间同步成功", 3000);
                    }
                    
                                    // 如果时钟界面正在显示，立即更新时间显示
                if (board_ptr->clock_ui_ && board_ptr->clock_enabled_ && board_ptr->IsClockVisible()) {
                    ESP_LOGI(TAG, "Clock UI is visible, updating time display after NTP sync");
                    
                    // 使用异步调用确保在主线程中更新UI
                    Application::GetInstance().Schedule([board_ptr]() {
                        if (board_ptr->clock_ui_ && board_ptr->clock_enabled_) {
                            // 强制立即更新时钟显示
                            board_ptr->clock_ui_->ForceUpdateDisplay();
                            // 同时更新整个时钟界面
                            board_ptr->clock_ui_->UpdateClockDisplay();
                            ESP_LOGI(TAG, "Clock UI time display updated after NTP sync");
                        }
                    });
                    } else {
                        ESP_LOGI(TAG, "Clock UI not visible, time update will occur when clock is shown");
                    }
                } else {
                    ESP_LOGW(TAG, "Time sync failed: %s", message.c_str());
                    
                    // 同步失败显示通知
                    auto display = Board::GetInstance().GetDisplay();
                    if (display) {
                        display->ShowNotification("时间同步失败，将稍后重试", 3000);
                    }
                }
            });
            
            esp_task_wdt_reset(); // 重置看门狗
            
            // 触发智能时间同步
            time_sync_manager.TriggerNtpSync();
            
            // 清理看门狗注册
            esp_task_wdt_delete(NULL);
            vTaskDelete(nullptr);
        }, "smart_time_sync", 4096, this, 2, nullptr);  // 减少栈大小到4096，降低优先级
    }

    // 播放本地音乐（改进版本，支持遍历SD卡目录）
    static void playLocalMusic(int file_number = 0) {
        
        ESP_LOGI(TAG, "Starting local music playback, file_number: %d", file_number);
        
        DIR *dir = opendir(MOUNT_POINT);
        if (dir == nullptr) {
            ESP_LOGE(TAG, "Failed to open directory: %s", MOUNT_POINT);
            return;
        }
        
        struct dirent *entry;
        std::vector<std::string> audio_files; // 存储符合条件的音频文件
        
        // 遍历目录中的文件
        while ((entry = readdir(dir)) != nullptr) {
            // 只处理扩展名为 .P3 的文件（支持大小写）
            if (strstr(entry->d_name, AUDIO_FILE_EXTENSION) || strstr(entry->d_name, ".p3")) {
                audio_files.push_back(entry->d_name);
                ESP_LOGI(TAG, "Found audio file: %s", entry->d_name);
            }
        }
        closedir(dir);
        
        ESP_LOGI(TAG, "Total audio files found: %d", audio_files.size());
        
        if (audio_files.empty()) {
            ESP_LOGE(TAG, "No valid audio file found in %s", MOUNT_POINT);
            ESP_LOGI(TAG, "Playing default success sound as fallback");
            
            // 播放默认提示音作为最后备用
            auto& app = Application::GetInstance();
            app.PlaySound(Lang::Sounds::P3_SUCCESS);
            return;
        }
        
        // 判断文件序号是否有效，如果超出范围则使用第一个文件
        if (file_number < 0 || file_number >= audio_files.size()) {
            ESP_LOGW(TAG, "Invalid file number: %d, using first file instead", file_number);
            file_number = 0;
        }
        
        // 构建完整文件路径
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s/%s", MOUNT_POINT, audio_files[file_number].c_str());
        
        ESP_LOGI(TAG, "Playing local music file: %s", file_path);
        
        // 尝试打开文件
        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) {
            ESP_LOGE(TAG, "Failed to open local music file: %s", file_path);
            return;
        }
        
        // 获取文件大小并读取文件内容
        file.seekg(0, std::ios::end);
        size_t size = file.tellg();
        file.seekg(0, std::ios::beg);
        
        if (size == 0) {
            ESP_LOGE(TAG, "Local music file is empty: %s", file_path);
            return;
        }
        
        std::vector<char> file_data(size);
        file.read(file_data.data(), size);
        
        if (!file) {
            ESP_LOGE(TAG, "Failed to read the entire local music file: %s", file_path);
            return;
        }
        
        // 在主线程中播放声音
        auto& app = Application::GetInstance();
        std::string_view sound_view(file_data.data(), file_data.size());
        auto codec = Board::GetInstance().GetAudioCodec();
        
        // 确保音频输出打开
        codec->EnableOutput(true);
        
        // 播放音乐（异步操作）
        app.PlaySound(sound_view); 
        
        ESP_LOGI(TAG, "Local music file %s playback started", file_path);
        
        // 注意：不立即关闭codec输出，让播放完成后自然处理
    }

    // 网络音乐下载相关的静态变量
    static uint8_t* network_music_buffer;
    static size_t network_music_size;
    
    // HTTP事件处理函数
    static esp_err_t http_music_event_handler(esp_http_client_event_t *evt) {
        static size_t buffer_len = 0;
        static size_t allocated_buffer_size = 0;
        static bool download_completed = false;
        static bool download_started = false;

        switch (evt->event_id) {
            case HTTP_EVENT_ON_CONNECTED:
                ESP_LOGI(TAG, "HTTP client connected for music download");
                download_completed = false;
                download_started = false;
                buffer_len = 0;
                allocated_buffer_size = 0;
                
                if (network_music_buffer) {
                    heap_caps_free(network_music_buffer);
                    network_music_buffer = nullptr;
                }
                network_music_size = 0;
                break;

            case HTTP_EVENT_ON_HEADER:
                if (!download_started && strcasecmp(evt->header_key, "Content-Length") == 0) {
                    size_t content_length = atoi(evt->header_value);
                    ESP_LOGI(TAG, "Music Content-Length: %zu bytes", content_length);
                    
                    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                    ESP_LOGI(TAG, "Free SPIRAM: %zu bytes", free_spiram);
                    
                    if (content_length > free_spiram / 2) {
                        ESP_LOGE(TAG, "Music file too large: %zu bytes", content_length);
                        return ESP_FAIL;
                    }
                    
                    if (content_length > 0) {
                        allocated_buffer_size = content_length;
                        network_music_buffer = (uint8_t*)heap_caps_malloc(allocated_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (!network_music_buffer) {
                            ESP_LOGE(TAG, "Failed to allocate music buffer: %zu bytes", allocated_buffer_size);
                            return ESP_FAIL;
                        }
                        buffer_len = 0;
                        network_music_size = 0;
                        download_completed = false;
                        download_started = true;
                        ESP_LOGI(TAG, "Pre-allocated music buffer: %zu bytes", allocated_buffer_size);
                    }
                }
                break;

            case HTTP_EVENT_ON_DATA:
                if (download_completed) {
                    ESP_LOGW(TAG, "Ignoring additional music data after download completed");
                    return ESP_OK;
                }
                
                if (!network_music_buffer) {
                    allocated_buffer_size = 512 * 1024; // 默认512KB
                    network_music_buffer = (uint8_t*)heap_caps_malloc(allocated_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (!network_music_buffer) {
                        ESP_LOGE(TAG, "Failed to allocate music buffer: %zu bytes", allocated_buffer_size);
                        return ESP_FAIL;
                    }
                    buffer_len = 0;
                    network_music_size = 0;
                    download_completed = false;
                    download_started = true;
                    ESP_LOGI(TAG, "Default allocated music buffer: %zu bytes", allocated_buffer_size);
                }
                
                if (buffer_len + evt->data_len > allocated_buffer_size) {
                    ESP_LOGE(TAG, "Music buffer overflow! Current: %zu, adding: %d, allocated: %zu",
                             buffer_len, evt->data_len, allocated_buffer_size);
                    return ESP_FAIL;
                }
                
                memcpy(network_music_buffer + buffer_len, evt->data, evt->data_len);
                buffer_len += evt->data_len;
                
                if (buffer_len % (10 * 1024) == 0 || buffer_len < 10 * 1024) {
                    ESP_LOGI(TAG, "Music download progress: %zu/%zu bytes", buffer_len, allocated_buffer_size);
                }
                break;

            case HTTP_EVENT_DISCONNECTED:
                if (!download_completed && buffer_len > 0) {
                    network_music_size = buffer_len;
                    download_completed = true;
                    ESP_LOGI(TAG, "Music download completed, total size: %zu bytes", network_music_size);
                } else if (download_completed) {
                    ESP_LOGI(TAG, "Additional music DISCONNECTED event ignored");
                } else {
                    ESP_LOGW(TAG, "Music DISCONNECTED event with no data (buffer_len: %zu)", buffer_len);
                }
                
                buffer_len = 0;
                allocated_buffer_size = 0;
                download_started = false;
                break;

            case HTTP_EVENT_ERROR:
                ESP_LOGE(TAG, "HTTP music download error");
                if (network_music_buffer) {
                    heap_caps_free(network_music_buffer);
                    network_music_buffer = nullptr;
                }
                network_music_size = 0;
                buffer_len = 0;
                allocated_buffer_size = 0;
                download_completed = false;
                download_started = false;
                break;

            default:
                break;
        }
        return ESP_OK;
    }
    
    // 下载网络音乐
    static bool downloadNetworkMusic(const char* url) {
        ESP_LOGI(TAG, "Starting music download from: %s", url);
        
        network_music_size = 0;
        if (network_music_buffer) {
            heap_caps_free(network_music_buffer);
            network_music_buffer = nullptr;
        }
        
        size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "Free SPIRAM before music download: %zu bytes", free_spiram);
        
        esp_http_client_config_t config = {};
        config.url = url;
        config.event_handler = http_music_event_handler;
        config.timeout_ms = 30000;
        config.buffer_size = 4096;
        config.buffer_size_tx = 1024;
        config.user_agent = "ESP32-MusicDownloader/1.0";
        config.method = HTTP_METHOD_GET;
        config.skip_cert_common_name_check = true;
        config.disable_auto_redirect = false;
        config.max_redirection_count = 3;
        
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr) {
            ESP_LOGE(TAG, "Failed to initialize HTTP client for music");
            return false;
        }
        
        esp_http_client_set_header(client, "Accept", "audio/*, */*");
        esp_http_client_set_header(client, "Connection", "close");
        esp_http_client_set_header(client, "Cache-Control", "no-cache");
        
        esp_err_t err = esp_http_client_perform(client);
        
        int status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);
        
        ESP_LOGI(TAG, "Music HTTP Status: %d, Content-Length: %d, downloaded: %zu",
                 status_code, content_length, network_music_size);
        
        esp_http_client_cleanup(client);
        
        bool success = (err == ESP_OK && status_code == 200 && network_music_size > 0);
        
        if (!success && network_music_buffer) {
            heap_caps_free(network_music_buffer);
            network_music_buffer = nullptr;
            network_music_size = 0;
        }
        
        ESP_LOGI(TAG, "Music download %s, size: %zu", success ? "succeeded" : "failed", network_music_size);
        return success;
    }
    
    // 播放下载的音乐
    static void playDownloadedMusic() {
        if (!network_music_buffer || network_music_size == 0) {
            ESP_LOGE(TAG, "No downloaded music to play");
            return;
        }
        
        ESP_LOGI(TAG, "Playing downloaded network music, size: %zu bytes", network_music_size);
        
        auto& app = Application::GetInstance();
        std::string_view sound_view(reinterpret_cast<char*>(network_music_buffer), network_music_size);
        auto codec = Board::GetInstance().GetAudioCodec();
        
        // 确保音频输出打开
        codec->EnableOutput(true);
        
        // 播放音乐（异步操作）
        app.PlaySound(sound_view);
        
        ESP_LOGI(TAG, "Network music playback started");
        
        // 创建延迟任务来清理内存和关闭输出
        // 音乐文件大小约13KB，24kHz采样率，预估播放时间约6-8秒，延迟10秒确保播放完成
        xTaskCreate([](void* param) {
            // 等待音乐播放完成
            vTaskDelay(pdMS_TO_TICKS(10000)); // 等待10秒
            
            // 清理内存
            if (network_music_buffer) {
                heap_caps_free(network_music_buffer);
                network_music_buffer = nullptr;
                network_music_size = 0;
                ESP_LOGI(TAG, "Network music buffer cleaned up");
            }
            
            // 注意：不在这里关闭codec输出，让其他音频播放决定
            
            vTaskDelete(nullptr);
        }, "music_cleanup", 2048, nullptr, 1, nullptr);
    }

    void TriggerMusicPlayback(const AlarmInfo& alarm) {
        ESP_LOGI(TAG, "TriggerMusicPlayback: Checking alarm sound type: %d", (int)alarm.sound_type);
        
        // 创建异步任务处理音乐播放请求，避免阻塞当前任务，增加栈大小
        struct PlaybackParams {
            AlarmSoundType sound_type;
            std::string description;
        };
        
        auto* params = new PlaybackParams{alarm.sound_type, alarm.description};
        
        BaseType_t task_result = xTaskCreate([](void* param) {
            auto* play_params = static_cast<PlaybackParams*>(param);
            auto& app = Application::GetInstance();
            auto protocol = app.GetProtocolPtr();
            
            ESP_LOGI(TAG, "Music playback task started with sound_type: %d", (int)play_params->sound_type);
            
            switch (play_params->sound_type) {
                case AlarmSoundType::LOCAL_MUSIC:
                    ESP_LOGI(TAG, "Playing local music from SD card");
                    // 播放本地音乐文件（使用改进的方法）
                    app.Schedule([]() {
                        playLocalMusic(0); // 播放第一个找到的音频文件
                    });
                    break;
                    
                case AlarmSoundType::NETWORK_MUSIC:
                    ESP_LOGI(TAG, "Playing network music: http://www.replime.cn/a1.p3");
                    // 下载并播放网络音乐
                    {
                        const char* url = "http://www.replime.cn/a1.p3";
                        ESP_LOGI(TAG, "Downloading network music from: %s", url);
                        
                        if (downloadNetworkMusic(url)) {
                            ESP_LOGI(TAG, "Network music downloaded successfully, playing...");
                            
                            // 在主线程中播放下载的音乐
                            app.Schedule([]() {
                                playDownloadedMusic();
                            });
                        } else {
                            ESP_LOGE(TAG, "Failed to download network music from: %s", url);
                            ESP_LOGI(TAG, "Fallback: trying to play local music from SD card");
                            
                            // 回退到播放本地音乐
                            app.Schedule([]() {
                                playLocalMusic(0);
                            });
                        }
                    }
                    break;
                    
                case AlarmSoundType::AI_MUSIC:
                default:
                    ESP_LOGI(TAG, "Playing AI music");
                    // AI音乐 - 使用原有的逻辑
                    if (!protocol) {
                        ESP_LOGW(TAG, "Protocol not available, cannot trigger AI music playback");
                        delete play_params;
                        vTaskDelete(nullptr);
                        return;
                    }
                    
                    // 检查当前设备状态
                    auto current_state = app.GetDeviceState();
                    ESP_LOGI(TAG, "Current device state: %d", current_state);
                    
                    // 如果正在说话，先打断
                    if (current_state == kDeviceStateSpeaking) {
                        ESP_LOGI(TAG, "Device is speaking, aborting current speech");
                        app.AbortSpeaking(kAbortReasonNone);
                        vTaskDelay(pdMS_TO_TICKS(1000)); // 等待打断完成
                    }
                    
                    // 检查音频通道是否已连接
                    if (!protocol->IsAudioChannelOpened()) {
                        ESP_LOGI(TAG, "Audio channel not opened, attempting to connect");
                        
                        // 如果设备处于空闲状态，触发连接
                        if (current_state == kDeviceStateIdle) {
                            ESP_LOGI(TAG, "Starting chat state to connect to server");
                            app.ToggleChatState();
                            
                            // 等待连接建立
                            int retry_count = 0;
                            const int max_retries = 30; // 最多等待30秒
                            while (!protocol->IsAudioChannelOpened() && retry_count < max_retries) {
                                vTaskDelay(pdMS_TO_TICKS(1000));
                                retry_count++;
                                ESP_LOGI(TAG, "Waiting for audio channel to open... (%d/%d)", retry_count, max_retries);
                            }
                            
                            if (!protocol->IsAudioChannelOpened()) {
                                ESP_LOGW(TAG, "Failed to establish audio channel after %d seconds", max_retries);
                                delete play_params;
                                vTaskDelete(nullptr);
                                return;
                            }
                        }
                    }
                    
                    // 现在通道已连接，发送播放音乐的指令
                    ESP_LOGI(TAG, "Audio channel is open, sending AI music playback request");
                    
                    // 使用Schedule确保在主线程中执行
                    app.Schedule([protocol, description = play_params->description]() {
                        // 发送模拟语音指令：随便播放一首歌曲 你对我说：你定的时间到了
                        std::string command = "你直接对我说：" + description + "时间到了";
                        protocol->SendWakeWordDetected(command.c_str());
                        ESP_LOGI(TAG, "AI music playback request sent to server: %s", command.c_str());
                    });
                    break;
            }
            
            delete play_params;
            vTaskDelete(nullptr);
        }, "music_playback", 6144, params, 1, nullptr); // 增加栈大小到6144，降低优先级到1
        
        if (task_result != pdPASS) {
            ESP_LOGE(TAG, "Failed to create music playback task, error: %d", task_result);
            ESP_LOGI(TAG, "Free heap: %lu bytes, minimum heap: %lu bytes", 
                     (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size());
            delete params; // 清理内存
        } else {
            ESP_LOGI(TAG, "Music playback task created successfully for sound_type: %d", (int)alarm.sound_type);
        }
    }

        // 移除系统监控任务以确保稳定性
    // 所有监控功能已禁用，避免额外的资源消耗和潜在的栈溢出问题
};

// 静态变量定义
uint8_t* Esp32S3Korvo2V3Board::network_music_buffer = nullptr;
size_t Esp32S3Korvo2V3Board::network_music_size = 0;

DECLARE_BOARD(Esp32S3Korvo2V3Board);
