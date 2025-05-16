#include "camera_service.h"
#include <esp_log.h>
#include <esp_camera.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include "boards/esp32s3-korvo2-v3/pin_config.h"
#include <cstring>
#include <lvgl.h>
static const char* TAG = "CameraService";

class CameraServiceImpl {
public:
    camera_config_t camera_config = {};
    TaskHandle_t preview_task = nullptr;
    std::function<void(int, int, const uint8_t*, size_t)> preview_cb;
    bool inited = false;

    CameraServiceImpl() {
        camera_config.ledc_channel = LEDC_CHANNEL_1;
        camera_config.ledc_timer = LEDC_TIMER_1;
        camera_config.pin_d0 = Y2_GPIO_NUM;
        camera_config.pin_d1 = Y3_GPIO_NUM;
        camera_config.pin_d2 = Y4_GPIO_NUM;
        camera_config.pin_d3 = Y5_GPIO_NUM;
        camera_config.pin_d4 = Y6_GPIO_NUM;
        camera_config.pin_d5 = Y7_GPIO_NUM;
        camera_config.pin_d6 = Y8_GPIO_NUM;
        camera_config.pin_d7 = Y9_GPIO_NUM;
        camera_config.pin_xclk = XCLK_GPIO_NUM;
        camera_config.pin_pclk = PCLK_GPIO_NUM;
        camera_config.pin_vsync = VSYNC_GPIO_NUM;
        camera_config.pin_href = HREF_GPIO_NUM;
        camera_config.pin_sscb_sda = -1;//SIOD_GPIO_NUM;
        camera_config.pin_sscb_scl = SIOC_GPIO_NUM;
        camera_config.sccb_i2c_port = 1;
        camera_config.pin_pwdn = PWDN_GPIO_NUM;
        camera_config.pin_reset = RESET_GPIO_NUM;
        camera_config.xclk_freq_hz = 20000000;
        camera_config.pixel_format = PIXFORMAT_RGB565;
        camera_config.frame_size = FRAMESIZE_240X240; //FRAMESIZE_96X96;// 320x240  FRAMESIZE_QVGA
        camera_config.jpeg_quality = 12;
        camera_config.fb_count = 2;//2
        camera_config.fb_location = CAMERA_FB_IN_PSRAM;
        camera_config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;//CAMERA_GRAB_LATEST;
        
        // config.pin_sccb_sda = -1;   // 这里写-1 表示使用已经初始化的I2C接口
        // config.pin_sccb_scl = CAMERA_PIN_SIOC;
        // config.sccb_i2c_port = 1;
        // config.pin_pwdn = CAMERA_PIN_PWDN;
        // config.pin_reset = CAMERA_PIN_RESET;
        // config.xclk_freq_hz = XCLK_FREQ_HZ;
        // config.pixel_format = PIXFORMAT_RGB565;
        // config.frame_size = FRAMESIZE_QVGA;
        // config.jpeg_quality = 12;
        // config.fb_count = 1;
        // config.fb_location = CAMERA_FB_IN_PSRAM;
        // config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    }

    bool Init() {
        if (inited) return true;
        esp_err_t err = esp_camera_init(&camera_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Camera Init Failed: %d", err);
            return false;
        }else{
            ESP_LOGE(TAG, "Camera Init Success");
        }
         sensor_t *s = esp_camera_sensor_get(); // 获取摄像头型号
        printf("摄像头型号PID: 0x%02X\n", s->id.PID);
        if (s->id.PID == OV2640_PID) {
            // s->set_hmirror(s, 1);  // 这里控制摄像头镜像 写1镜像 写0不镜像
            
        }
        
        inited = true;
        return true;
    }

    void StartPreview(std::function<void(int, int, const uint8_t*, size_t)> cb) {
        preview_cb = cb;
        if (preview_task == nullptr) {
            xTaskCreate([](void* arg) {
                auto self = (CameraServiceImpl*)arg;
                while (self->preview_cb) {
                    self->Init();
                    vTaskDelay(pdMS_TO_TICKS(100));
                    camera_fb_t* fb = esp_camera_fb_get();
                    // fb = esp_camera_fb_get(); // 第一帧的数据不使用
                    esp_camera_fb_return(fb); // 处理结束以后把这部分的buf返回
                    fb = esp_camera_fb_get();
                    if (fb && fb->format == PIXFORMAT_RGB565) {
                        self->preview_cb(fb->width, fb->height, fb->buf, fb->len);
                    }
                    if (fb) esp_camera_fb_return(fb);
                    // vTaskDelay(pdMS_TO_TICKS(30));
                    esp_camera_deinit();
                    self->inited=false;
                    vTaskDelete(nullptr);
                }
                vTaskDelete(nullptr);
            }, "cam_preview", 4096, this, 5, &preview_task);
        }
        ESP_LOGE(TAG, "StartPreview");
    }

    void StopPreview() {
        preview_cb = nullptr;
        preview_task = nullptr;
    }

    bool TakePhoto(std::vector<uint8_t>& jpg_data) {
        camera_fb_t* fb = nullptr;
        camera_config.pixel_format = PIXFORMAT_JPEG;
        Init();
        ESP_LOGE(TAG, "TakePhoto");
        vTaskDelay(pdMS_TO_TICKS(100));        
        fb = esp_camera_fb_get(); // 第一帧的数据不使用
        esp_camera_fb_return(fb); // 处理结束以后把这部分的buf返回
        fb = esp_camera_fb_get();
        // sensor_t* s = esp_camera_sensor_get();
        // if (s->pixformat != PIXFORMAT_JPEG) {
        //     s->set_pixformat(s, PIXFORMAT_JPEG);
        //     s->set_framesize(s, FRAMESIZE_QVGA);
        //     vTaskDelay(pdMS_TO_TICKS(100));
        // }
        // fb = esp_camera_fb_get();
        if (!fb || fb->format != PIXFORMAT_JPEG) {
            if (fb) esp_camera_fb_return(fb);
            ESP_LOGE(TAG, "TakePhoto failed");
            return false;
        }
        jpg_data.assign(fb->buf, fb->buf + fb->len);
        esp_camera_fb_return(fb);
        // 恢复为预览模式
        // s->set_pixformat(s, PIXFORMAT_RGB565);
        // vTaskDelay(pdMS_TO_TICKS(100));
        esp_camera_deinit();
        inited=false;
        ESP_LOGE(TAG, "TakePhoto success");
        return true;
    }

    bool TakePhotoToBuffer(uint8_t* buf, size_t buf_size, size_t& out_len) {
        camera_fb_t* fb = nullptr;
        camera_config.pixel_format = PIXFORMAT_JPEG;
        Init();
        ESP_LOGE(TAG, "TakePhotoToBuffer");
        vTaskDelay(pdMS_TO_TICKS(100));
        fb = esp_camera_fb_get(); // 丢弃第一帧
        esp_camera_fb_return(fb);
        fb = esp_camera_fb_get();
        if (!fb || fb->format != PIXFORMAT_JPEG) {
            if (fb) esp_camera_fb_return(fb);
            ESP_LOGE(TAG, "TakePhotoToBuffer failed");
            return false;
        }
        if (fb->len > buf_size) {
            esp_camera_fb_return(fb);
            ESP_LOGE(TAG, "SPIRAM缓冲区不足");
            return false;
        }
        memcpy(buf, fb->buf, fb->len);
        out_len = fb->len;
        esp_camera_fb_return(fb);
        esp_camera_deinit();
        inited = false;
        ESP_LOGE(TAG, "TakePhotoToBuffer success");
        return true;
    }

    bool TakePhotoRgb565ToBuffer(uint8_t* buf, size_t buf_size, int& out_w, int& out_h) {
        camera_fb_t* fb = nullptr;
        camera_config.pixel_format = PIXFORMAT_RGB565;
        Init();
        ESP_LOGE(TAG, "TakePhotoRgb565ToBuffer");
        vTaskDelay(pdMS_TO_TICKS(100));
        fb = esp_camera_fb_get(); // 丢弃第一帧
        esp_camera_fb_return(fb);
        fb = esp_camera_fb_get();
        if (!fb || fb->format != PIXFORMAT_RGB565) {
            if (fb) esp_camera_fb_return(fb);
            ESP_LOGE(TAG, "TakePhotoRgb565ToBuffer failed");
            return false;
        }
        if (fb->len > buf_size) {
            esp_camera_fb_return(fb);
            ESP_LOGE(TAG, "SPIRAM缓冲区不足");
            return false;
        }
        memcpy(buf, fb->buf, fb->len);
        out_w = fb->width;
        out_h = fb->height;
        esp_camera_fb_return(fb);
        esp_camera_deinit();
        inited = false;
        ESP_LOGE(TAG, "TakePhotoRgb565ToBuffer success");
        return true;
    }

    bool ShowPhotoToLvgl(lv_obj_t* img_obj) {
        const size_t max_jpg_size = 60 * 1024;
        uint8_t* jpg_buf = (uint8_t*)heap_caps_malloc(max_jpg_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t jpg_len = 0;
        if (!jpg_buf) return false;
        camera_config.pixel_format = PIXFORMAT_JPEG;
        if (!Init()) {
            heap_caps_free(jpg_buf);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        camera_fb_t* fb = esp_camera_fb_get();
        esp_camera_fb_return(fb);
        fb = esp_camera_fb_get();
        if (!fb || fb->format != PIXFORMAT_JPEG) {
            if (fb) esp_camera_fb_return(fb);
            heap_caps_free(jpg_buf);
            return false;
        }
        if (fb->len > max_jpg_size) {
            esp_camera_fb_return(fb);
            heap_caps_free(jpg_buf);
            return false;
        }
        memcpy(jpg_buf, fb->buf, fb->len);
        jpg_len = fb->len;
        int w = fb->width, h = fb->height;
        esp_camera_fb_return(fb);
        esp_camera_deinit();
        inited = false;
        // JPEG转RGB888
        static uint8_t* rgb888_buf = (uint8_t*)heap_caps_malloc(w * h * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!rgb888_buf) {
            heap_caps_free(jpg_buf);
            return false;
        }
        if (!fmt2rgb888(jpg_buf, jpg_len, PIXFORMAT_JPEG, rgb888_buf)) {
            heap_caps_free(jpg_buf);
            heap_caps_free(rgb888_buf);
            return false;
        }
        heap_caps_free(jpg_buf);
        // 构造LVGL图片描述符
        static lv_img_dsc_t img_dsc;
        img_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
        img_dsc.header.w = w;
        img_dsc.header.h = h;
        img_dsc.data_size = w * h * 3;
        img_dsc.data = rgb888_buf;
        lv_img_set_src(img_obj, &img_dsc);
        // heap_caps_free(rgb888_buf);
        return true;
    }
};

static CameraServiceImpl g_camera_service_impl;

CameraService& CameraService::GetInstance() {
    static CameraService instance;
    return instance;
}

bool CameraService::Init() {
    return g_camera_service_impl.Init();
}

void CameraService::StartPreview(std::function<void(int, int, const uint8_t*, size_t)> on_frame) {
    g_camera_service_impl.StartPreview(on_frame);
}

void CameraService::StopPreview() {
    g_camera_service_impl.StopPreview();
}

bool CameraService::TakePhoto(std::vector<uint8_t>& jpg_data) {
    return g_camera_service_impl.TakePhoto(jpg_data);
}

bool CameraService::TakePhotoToBuffer(uint8_t* buf, size_t buf_size, size_t& out_len) {
    return g_camera_service_impl.TakePhotoToBuffer(buf, buf_size, out_len);
}

bool CameraService::TakePhotoRgb565ToBuffer(uint8_t* buf, size_t buf_size, int& w, int& h) {
    return g_camera_service_impl.TakePhotoRgb565ToBuffer(buf, buf_size, w, h);
}

bool CameraService::ShowPhotoToLvgl(lv_obj_t* img_obj) {
    return g_camera_service_impl.ShowPhotoToLvgl(img_obj);
} 