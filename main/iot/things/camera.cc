#include "iot/thing.h"
#include "board.h"
#include "display/lcd_display.h"
#include "settings.h"
#include "application.h"

#include <esp_log.h>
#include <string>
#include <string.h>
#include <esp_camera.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <sys/param.h>
#include <esp_vfs_fat.h>
#include <esp_wifi.h>
#include <esp_eth.h>
#include <esp_netif.h>
#include "boards/esp32s3-korvo2-v3/config.h"

#define TAG "Camera"


namespace iot {

// 声明全局变量
static QueueHandle_t xQueueLCDFrame = NULL;
static TaskHandle_t xCameraTaskHandle = NULL;

static bool camera_running = false;
static bool camera_is_init = false;

// 照片相关变量
static camera_fb_t *last_captured_photo = NULL;
static SemaphoreHandle_t photo_mutex = NULL;


// 这里仅定义 Camera 的属性和方法，不包含具体的实现
class Camera : public Thing {
private:
    bool power_ = false; // 添加power_成员变量
    bool http_route_registered_ = false; // 是否已注册HTTP路由

          // 摄像头硬件初始化
    void InitCamera(void)
    {
// #define CAMERA_PIN_PWDN -1
// #define CAMERA_PIN_RESET -1
// #define CAMERA_PIN_XCLK 40
// #define CAMERA_PIN_SIOD 17
// #define CAMERA_PIN_SIOC 18

// #define CAMERA_PIN_D7 39
// #define CAMERA_PIN_D6 41
// #define CAMERA_PIN_D5 42
// #define CAMERA_PIN_D4 12
// #define CAMERA_PIN_D3 3
// #define CAMERA_PIN_D2 14
// #define CAMERA_PIN_D1 47
// #define CAMERA_PIN_D0 13
// #define CAMERA_PIN_VSYNC 21
// #define CAMERA_PIN_HREF 38
// #define CAMERA_PIN_PCLK 11

// #define XCLK_FREQ_HZ 24000000

        if (camera_is_init) {
            return;
        }
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
        camera_is_init = true;
        ESP_LOGI(TAG, "Camera init success");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    //释放摄像头资源，反初始化涉嫌头
    void deinit_camera(void) {
        // if (last_captured_photo) {
        //     esp_camera_fb_return(last_captured_photo);
        //     last_captured_photo = NULL;
        // }
        if (camera_is_init) {
            esp_camera_deinit();
            
        }
       camera_is_init = false; 
        
    }

    // 单一任务处理摄像头采集和显示
    static void task_camera_and_lcd(void *arg)
    {
        ESP_LOGI(TAG, "开始摄像头和显示单一任务");
        camera_running = true;
        
        auto display = Board::GetInstance().GetDisplay();
        int frame_count = 0;
        int error_count = 0;
        
        // 创建画布（如果不存在）
        if (!display->HasCanvas()) {
            ESP_LOGI(TAG, "创建画布");
            display->CreateCanvas();
        } else {
            ESP_LOGI(TAG, "画布已存在");
        }

        // 分配图像转换缓冲区
        uint16_t* convertedData = NULL;
        int lastWidth = 0;
        int lastHeight = 0;

        while (camera_running)
        {
            // 直接获取摄像头帧
            camera_fb_t *frame = esp_camera_fb_get();
            
            if (frame && frame->buf && frame->len > 0) {
                frame_count++;
                
                if (frame_count % 30 == 0) {
                    ESP_LOGI(TAG, "已捕获和显示 %d 帧图像, 大小: %dx%d, 格式: %d", 
                             frame_count, frame->width, frame->height, frame->format);
                }
                
                // 检查分辨率是否变化，如需重新分配内存
                bool need_alloc = (lastWidth != frame->width || lastHeight != frame->height || convertedData == NULL);
                if (need_alloc) {
                    // 释放旧内存
                    if (convertedData != NULL) {
                        free(convertedData);
                        convertedData = NULL;
                    }
                    
                    // 分配新内存 - 使用malloc而不是new以减少异常风险
                    size_t buf_size = frame->width * frame->height * sizeof(uint16_t);
                    ESP_LOGI(TAG, "分配图像缓冲区: %dx%d (%d字节)", frame->width, frame->height, buf_size);
                    convertedData = (uint16_t*)malloc(buf_size);
                    
                    if (convertedData) {
                        lastWidth = frame->width;
                        lastHeight = frame->height;
                    } else {
                        ESP_LOGE(TAG, "无法分配内存: %d字节", buf_size);
                        esp_camera_fb_return(frame);
                        vTaskDelay(pdMS_TO_TICKS(100));
                        continue;
                    }
                }
                
                // 确保画布存在
                if (!display->HasCanvas()) {
                    ESP_LOGW(TAG, "画布丢失，重新创建");
                    display->CreateCanvas();
                }
                
                // 简单的数据转换 - 只处理RGB565格式
                if (frame->format == PIXFORMAT_RGB565 && convertedData) {
                    for (int i = 0; i < frame->width * frame->height; i++) {
                        uint16_t pixel = ((uint16_t*)frame->buf)[i];
                        // 交换字节顺序（大小端转换）
                        convertedData[i] = ((pixel & 0xFF) << 8) | ((pixel & 0xFF00) >> 8);
                    }
                    
                    // 将图像绘制到画布上
                    display->DrawImageOnCanvas(0, 0, frame->width, frame->height, (const uint8_t*)convertedData);
                    
                    // 存储最新捕获的帧用于拍照功能
                    if (xSemaphoreTake(photo_mutex, 0) == pdTRUE) {
                        if (last_captured_photo != NULL) {
                            esp_camera_fb_return(last_captured_photo);
                        }
                        last_captured_photo = frame;
                        xSemaphoreGive(photo_mutex);
                        // 不释放帧，因为它现在是最新捕获的照片
                    } else {
                        // 如果无法获取互斥锁，正常释放帧
                        esp_camera_fb_return(frame);
                    }
                } else {
                    ESP_LOGW(TAG, "不支持的图像格式: %d", frame->format);
                    esp_camera_fb_return(frame);
                }
            } else {
                error_count++;
                if (error_count % 10 == 0) {
                    ESP_LOGE(TAG, "获取摄像头帧失败 (错误计数: %d)", error_count);
                }
                
                if (frame) esp_camera_fb_return(frame);
                
                // 等待一段时间再重试
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            
            // 添加短暂延迟，控制帧率
            vTaskDelay(pdMS_TO_TICKS(20)); // 约50FPS
        }
        
        // 释放资源
        if (convertedData != NULL) {
            free(convertedData);
            convertedData = NULL;
        }
        
        ESP_LOGI(TAG, "摄像头与显示任务退出，共处理 %d 帧", frame_count);
        xCameraTaskHandle = NULL;
        vTaskDelete(NULL);
    }

    // 拍照函数 - 直接使用当前帧
    bool take_photo() {
        bool success = false;
        
        // 获取互斥锁
        if (xSemaphoreTake(photo_mutex, portMAX_DELAY) == pdTRUE) {
            // 如果没有捕获的照片，尝试直接获取一帧
            if (last_captured_photo == NULL) {
                ESP_LOGI(TAG, "尝试直接获取摄像头帧");
                last_captured_photo = esp_camera_fb_get();
            }
            
            success = (last_captured_photo != NULL);
            
            if (!success) {
                ESP_LOGE(TAG, "拍照失败，无法获取相机帧");
            }
            
            xSemaphoreGive(photo_mutex);
        } else {
            ESP_LOGE(TAG, "获取照片互斥锁失败");
        }
        
        return success;
    }

    // 启动摄像头与LCD任务
    void start_camera_lcd(void)
    {
        ESP_LOGI(TAG, "启动摄像头");
        
        // 检查摄像头状态
        sensor_t *sensor = esp_camera_sensor_get();
        if (sensor == NULL) {
            ESP_LOGE(TAG, "无法获取摄像头传感器");
            auto display = Board::GetInstance().GetDisplay();
            display->ShowNotification("摄像头初始化失败", 3000);
            return;
        }
        
        ESP_LOGI(TAG, "摄像头已初始化，传感器ID: 0x%x, 当前分辨率: %dx%d", 
                 sensor->id.PID, sensor->status.framesize ? sensor->status.framesize : 0, 
                 sensor->status.framesize ? sensor->status.framesize : 0);
        
        // 设置摄像头参数
        sensor->set_framesize(sensor, FRAMESIZE_240X240);  // 320x240
        sensor->set_pixformat(sensor, PIXFORMAT_RGB565);
        
        camera_running = true;
        
        // 创建互斥锁用于照片访问
        if (photo_mutex == NULL) {
            photo_mutex = xSemaphoreCreateMutex();
            if (photo_mutex == NULL) {
                ESP_LOGE(TAG, "创建互斥锁失败");
                auto display = Board::GetInstance().GetDisplay();
                display->ShowNotification("创建互斥锁失败", 3000);
                return;
            }
        }
        
        // 先创建画布
        auto display = Board::GetInstance().GetDisplay();
        if (!display->HasCanvas()) {
            ESP_LOGI(TAG, "创建画布");
            display->CreateCanvas();
        } else {
            ESP_LOGI(TAG, "画布已存在");
        }
        
        // 创建单一任务处理摄像头获取和显示
        if (xCameraTaskHandle == NULL) {
            ESP_LOGI(TAG, "创建摄像头与显示任务");
            BaseType_t task_created = xTaskCreatePinnedToCore(
                task_camera_and_lcd, 
                "task_camera_lcd", 
                4 * 1024,  // 单一任务需要足够的堆栈
                NULL, 
                2,  // 低优先级
                &xCameraTaskHandle, 
                1);  // 在核心1运行
                
            if (task_created != pdPASS) {
                ESP_LOGE(TAG, "创建摄像头与显示任务失败: %d (1=内存不足)", task_created);
                display->ShowNotification("创建任务失败: 内存不足", 3000);
                camera_running = false;
                return;
            } else {
                ESP_LOGI(TAG, "摄像头与显示任务创建成功");
            }
        }

        // 等待一段时间，确保摄像头开始工作
        display->ShowNotification("摄像头启动中...", 2000);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    // 停止摄像头与LCD任务
    void stop_camera_lcd(void)
    {
        // 设置标志位通知任务停止
        camera_running = false;
        
        // 等待任务自然结束
        // 注意：在实际应用中，可能需要确保任务已经结束，或者添加超时机制
        vTaskDelay(pdMS_TO_TICKS(500));
        
        // 删除队列
        if (xQueueLCDFrame != NULL) {
            vQueueDelete(xQueueLCDFrame);
            xQueueLCDFrame = NULL;
        }
        
        // 释放上次拍摄的照片
        if (xSemaphoreTake(photo_mutex, portMAX_DELAY) == pdTRUE) {
            if (last_captured_photo != NULL) {
                esp_camera_fb_return(last_captured_photo);
                last_captured_photo = NULL;
            }
            xSemaphoreGive(photo_mutex);
        }
        
        // 删除互斥锁
        if (photo_mutex != NULL) {
            vSemaphoreDelete(photo_mutex);
            photo_mutex = NULL;
        }
        
        // 注意：不关闭HTTP服务器，因为它可能还被其他设备使用
        // 只是重置注册状态
        http_route_registered_ = false;
    }

    // 发送照片到服务器
    bool send_photo_to_server(camera_fb_t* photo) {
        if (!photo) {
            ESP_LOGE(TAG, "照片为空，无法发送到服务器");
            return false;
        }

        ESP_LOGI(TAG, "准备发送照片到服务器，尺寸: %d x %d, 大小: %d 字节", 
                 photo->width, photo->height, photo->len);
        
        // 获取Protocol实例
        Protocol* protocol = &Application::GetInstance().GetProtocol();
        if (!protocol) {
            ESP_LOGE(TAG, "无法获取协议实例，无法发送照片");
            return false;
        }

        // 创建数据向量
        std::vector<uint8_t> photo_data;
        
        // 确定照片格式并处理数据
        std::string format = "jpg"; // 默认格式
        
        // 检查图像大小，如果过大需要压缩
        const size_t MAX_WEBSOCKET_SIZE = 60000; // 设置为稍小于65535的值以留出一些空间

        if (photo->format == PIXFORMAT_RGB565) {
            format = "jpg";
                // 将RGB565转换为JPEG以减小大小
                size_t out_len = 0;
                uint8_t* out_buf = NULL;
                const int quality = 85; // 使用较高的JPEG质量
                bool jpeg_converted = frame2jpg(photo, quality, &out_buf, &out_len);
                
                if (jpeg_converted && out_len <= MAX_WEBSOCKET_SIZE) {
                    ESP_LOGI(TAG, "成功将RGB565转换为JPEG，大小: %d 字节", out_len);
                    photo_data.assign(out_buf, out_buf + out_len);
                    free(out_buf); // 释放esp-camera分配的内存
                } else {
                    if (out_buf) free(out_buf);
                    
                    // 尝试更低的质量
                    out_len = 0;
                    out_buf = NULL;
                    const int lower_quality = 60; // 降低质量但仍保持良好水平
                    jpeg_converted = frame2jpg(photo, lower_quality, &out_buf, &out_len);
                    
                    if (jpeg_converted && out_len <= MAX_WEBSOCKET_SIZE) {
                        ESP_LOGI(TAG, "使用更低质量(%d)成功将RGB565转换为JPEG，大小: %d 字节", 
                                 lower_quality, out_len);
                        photo_data.assign(out_buf, out_buf + out_len);
                        free(out_buf);
                    } else {
                        if (out_buf) free(out_buf);
                        ESP_LOGE(TAG, "无法将图像压缩到合适的大小，放弃发送");
                        
                        auto display = Board::GetInstance().GetDisplay();
                        display->ShowNotification("图像过大，无法发送到服务器", 3000);
                        return false;
                    }
                }
        } else {
            format = "unknown";
        }

        // 发送照片数据
        ESP_LOGI(TAG, "开始发送照片数据，大小: %zu 字节", photo_data.size());
        protocol->SendIotCameraPhoto(photo_data, photo->width, photo->height, format);
        
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification("已发送照片到服务器", 3000);
        
        return true;
    }

public:
    Camera() : Thing("Camera", "这是一个摄像机，可看到外面的世界也可以拍照，拍照后回复“这是​​阿司匹林肠溶片​​，用于预防血栓，您每天早餐后吃一片即可，注意不要空腹服用。") {
        // 定义设备的属性
        properties_.AddBooleanProperty("power", "摄像机是否打开", [this]() -> bool {
            return power_;
        });

        // 定义设备可以被远程执行的指令
        methods_.AddMethod("TurnOn", "打开摄像机", ParameterList(), [this](const ParameterList& parameters) {
            power_ = true;
            InitCamera();
            // 摄像头已经在板级初始化中配置好，直接启动任务即可
            ESP_LOGI(TAG, "开始打开摄像机");
            auto display = Board::GetInstance().GetDisplay();
            display->SetAnimState("cam");
            // 启动摄像头与LCD - 实时显示摄像头图像
            start_camera_lcd();
        });

        methods_.AddMethod("TurnOff", "关闭摄像机", ParameterList(), [this](const ParameterList& parameters) {
            power_ = false;
            
            ESP_LOGI(TAG, "开始关闭摄像机");
            // 清理资源并复位队列
            stop_camera_lcd();
            
            // 销毁画布，释放资源
            auto display = Board::GetInstance().GetDisplay();
            if (display->HasCanvas()) {
                display->DestroyCanvas();
            }
            deinit_camera();
        });

        
        methods_.AddMethod("TakePhoto", "拍照", ParameterList(), [this](const ParameterList& parameters) {
            ESP_LOGI(TAG, "开始拍照");
            InitCamera();
            auto display = Board::GetInstance().GetDisplay();
            display->SetAnimState("cam");
            camera_fb_t *frame = NULL;
            
            if (camera_running) {
                // 摄像头任务正在运行，等待互斥锁以获取当前帧的副本
                ESP_LOGI(TAG, "摄像头正在运行，尝试获取最新帧");
                
                if (photo_mutex == NULL) {
                    ESP_LOGW(TAG, "照片互斥锁不存在，尝试创建");
                    photo_mutex = xSemaphoreCreateMutex();
                    if (photo_mutex == NULL) {
                        ESP_LOGE(TAG, "创建互斥锁失败");
                        display->ShowNotification("拍照失败：无法创建互斥锁", 3000);
                        return;
                    }
                }
                
                // 尝试获取互斥锁
                if (xSemaphoreTake(photo_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    if (last_captured_photo != NULL) {
                        // 从已有的帧创建一个新的副本
                        size_t len = last_captured_photo->len;
                        uint8_t* buf = (uint8_t*)malloc(len);
                        if (buf) {
                            memcpy(buf, last_captured_photo->buf, len);
                            int width = last_captured_photo->width;
                            int height = last_captured_photo->height;
                            pixformat_t format = last_captured_photo->format;
                            
                            // 创建新的帧
                            frame = (camera_fb_t*)malloc(sizeof(camera_fb_t));
                            if (frame) {
                                frame->buf = buf;
                                frame->len = len;
                                frame->width = width;
                                frame->height = height;
                                frame->format = format;
                                frame->timestamp.tv_sec = 0;
                                frame->timestamp.tv_usec = 0;
                                
                                ESP_LOGI(TAG, "成功从现有帧创建副本");
                            } else {
                                ESP_LOGE(TAG, "无法分配内存给帧结构体");
                                free(buf);  // 释放已分配的缓冲区
                            }
                        } else {
                            ESP_LOGE(TAG, "无法分配内存给图像缓冲区");
                        }
                    } else {
                        ESP_LOGW(TAG, "没有可用的捕获帧");
                    }
                    xSemaphoreGive(photo_mutex);
                } else {
                    ESP_LOGE(TAG, "获取互斥锁超时");
                    display->ShowNotification("拍照失败：无法获取互斥锁", 3000);
                    return;
                }
            } else {
                // 摄像头没有运行，先启动它，获取最新帧，然后再停止
                ESP_LOGI(TAG, "摄像头未运行，临时启动摄像头获取最新帧");
                
                // 检查摄像头状态
                sensor_t *sensor = esp_camera_sensor_get();
                if (sensor == NULL) {
                    ESP_LOGE(TAG, "无法获取摄像头传感器");
                    display->ShowNotification("摄像头初始化失败", 3000);
                    return;
                }
                
                // 设置摄像头参数
                sensor->set_framesize(sensor, FRAMESIZE_240X240);  // 320x240
                sensor->set_pixformat(sensor, PIXFORMAT_RGB565);
                
                // 启动摄像头并等待稳定
                ESP_LOGI(TAG, "临时启动摄像头...");
                vTaskDelay(pdMS_TO_TICKS(500));  // 等待摄像头稳定
                
                // 多次获取帧以确保获得最新的图像(前几帧可能不稳定)
                camera_fb_t *temp_frame = NULL;
                for (int i = 0; i < 3; i++) {
                    if (temp_frame) {
                        esp_camera_fb_return(temp_frame);
                    }
                    temp_frame = esp_camera_fb_get();
                    vTaskDelay(pdMS_TO_TICKS(100));  // 短暂延迟
                }
                
                // 使用最后一帧
                frame = temp_frame;
                
                if (frame == NULL) {
                    ESP_LOGE(TAG, "临时启动摄像头后仍无法获取帧");
                    display->ShowNotification("无法获取图像", 3000);
                    return;
                }
                
                ESP_LOGI(TAG, "成功获取新鲜帧，大小: %dx%d, 格式: %d", 
                         frame->width, frame->height, frame->format);
            }
            
            if (frame == NULL) {
                ESP_LOGE(TAG, "拍照失败，无法获取帧");
                display->ShowNotification("拍照失败", 3000);
                return;
            }
            
            // 发送照片到服务器
            ESP_LOGI(TAG, "获取帧成功，发送到服务器");
            send_photo_to_server(frame);
            
            // 显示照片
            // 如果摄像头运行中，跳过显示（因为已经有实时画面）
            if (camera_running) {
                ESP_LOGI(TAG, "摄像头运行中，跳过显示照片");
                
                // 释放资源 - 如果是从现有帧创建的副本
                if (frame && frame != last_captured_photo) {
                    if (frame->buf) free(frame->buf);
                    free(frame);
                }
                
                return;
            }
            
            // 确保有画布可用，显示照片
            if (!display->HasCanvas()) {
                display->CreateCanvas();
            }
            
            // 图像格式转换 - 修复花屏问题
            int width = frame->width;
            int height = frame->height;
            uint16_t* convertedData = new uint16_t[width * height];
            
            if (convertedData != nullptr) {
                // 转换图像数据 - 交换RGB565字节序
                for (int i = 0; i < width * height; i++) {
                    uint16_t pixel = ((uint16_t*)frame->buf)[i];
                    // 交换字节顺序（大小端转换）
                    convertedData[i] = ((pixel & 0xFF) << 8) | ((pixel & 0xFF00) >> 8);
                }
                
                // 在画布上显示图像
                int x_start = 0;
                int y_start = 0;
                
                // 直接在画布上绘制图像
                display->DrawImageOnCanvas(x_start, y_start, width, height, (const uint8_t*)convertedData);
                
                // 释放临时内存
                delete[] convertedData;
                
                ESP_LOGI(TAG, "照片已显示在画布上");
                
                // 创建一个计时器，3秒后关闭画布
                ESP_LOGI(TAG, "照片将显示3秒");
                vTaskDelay(pdMS_TO_TICKS(3000));  // 延时3秒
                
                // 5秒后销毁画布
                if (display->HasCanvas()) {
                    display->DestroyCanvas();
                    ESP_LOGI(TAG, "已关闭画布显示");
                }
            } else {
                ESP_LOGE(TAG, "为图像转换分配内存失败");
            }
            
            // 释放帧资源 - 如果不是从last_captured_photo创建的副本
            if (frame && frame != last_captured_photo) {
                esp_camera_fb_return(frame);
            }
            deinit_camera();
        });
    }
};

} // namespace iot

DECLARE_THING(Camera);