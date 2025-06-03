#include "iot/thing.h"
#include "board.h"
#include "display/lcd_display.h"
#include "settings.h"
#include "application.h"

#include <esp_log.h>
#include <string>
#include <string.h>
#include <vector>
#include <stdio.h>
#include <sys/stat.h>
#include "lodepng.h"
#include <esp_heap_caps.h>

#define TAG "ImageDisplayer"

namespace iot {

class ImageDisplayer : public Thing {
public:
    ImageDisplayer() : Thing("ImageDisplayer", "本地图片显示器") {
        // 你现在可以通过如下方式调用：
        // PNG图片：ShowImage("xxx.png", 0, 0)（宽高参数会被忽略，自动获取）
        // RAW图片：ShowImage("xxx.raw", 240, 240)（宽高需手动指定）
        // 添加显示图片的方法
    
        // methods_.AddMethod("ShowImage", "显示图片", ParameterList({
        //     Parameter("image_path", "图片路径 F图片序号.RAW", kValueTypeString, true),
        //     Parameter("width", "图片宽度 240", kValueTypeNumber, true),
        //     Parameter("height", "图片高度 240", kValueTypeNumber, true)
        // }), [this](const ParameterList& parameters) {
        //     std::string image_path = parameters["image_path"].string();
        //     int width = static_cast<int>(parameters["width"].number());
        //     int height = static_cast<int>(parameters["height"].number());
        //     ShowImageAuto(image_path, width, height);
        //     send_cmd_to_server(-1);
        // });
        methods_.AddMethod("ShowSdImage", "显示图片", ParameterList({
                Parameter("index", "序号", kValueTypeNumber, true)
        }), [this](const ParameterList& parameters) {
            int index = static_cast<int>(parameters["index"].number());
            send_cmd_to_server(index);
            if(index == 0)return;
            ShowRawImageFromSD("F" + std::to_string(index) + ".RAW", 240, 240);
            
        });
        methods_.AddMethod("CloseImage", "关闭图片", ParameterList(), [this](const ParameterList& parameters) {
            auto display = Board::GetInstance().GetDisplay();
            if (display && display->HasCanvas()) {
                display->DestroyCanvas();
                ESP_LOGI(TAG, "已关闭画布显示");
            }
        });
        methods_.AddMethod("DelayCloseCanvas", "延时关闭画布", ParameterList({
            Parameter("delay_ms", "延迟毫秒数", kValueTypeNumber, true)
        }), [this](const ParameterList& parameters) {
            int delay_ms = static_cast<int>(parameters["delay_ms"].number());
            CreateDestroyCanvasTask(Board::GetInstance().GetDisplay(), delay_ms);
        });
    
    }
// 发送照片到服务器
    bool send_cmd_to_server(int cmd) {

        ESP_LOGI(TAG, "准备发送命令到服务器: %d", cmd);
        
        // 获取Protocol实例
        Protocol* protocol = &Application::GetInstance().GetProtocol();
        if (!protocol) {
            ESP_LOGE(TAG, "无法获取协议实例，无法发送命令");
            return false;
        }

        // 发送命令---使用发送照片的通道
        // 检查：SendIotCameraPhoto的参数含义是否正确？第一个参数"cmd"是否为命令类型？0,0是否为图片相关参数？cmd为要发送的命令字符串
        // std::vector<uint8_t> cmd_data(cmd.begin(), cmd.end());
        std::vector<uint8_t> cmd_data(1);
        cmd_data[0] = 1;
        protocol->SendIotCameraPhoto(cmd_data, cmd, cmd, "cmd");
        
        // auto display = Board::GetInstance().GetDisplay();
        // display->ShowNotification("已发送命令到服务器", 3000);
        
        return true;
    }
    void ShowImageAuto(const std::string& image_path, int width, int height) {
        // 判断扩展名
        std::string ext = GetFileExt(image_path);
        if (ext == "png" || ext == "PNG") {
            ShowPngImageFromSD(image_path);
        } else if (ext == "raw" || ext == "RAW") {
            ShowRawImageFromSD(image_path, width, height);
        } else {
            auto display = Board::GetInstance().GetDisplay();
            if (display) display->ShowNotification("不支持的图片格式", 2000);
        }
    }

    // 获取文件扩展名
    std::string GetFileExt(const std::string& filename) {
        size_t pos = filename.rfind('.');
        if (pos == std::string::npos) return "";
        return filename.substr(pos + 1);
    }

    // PNG图片显示逻辑
    void ShowPngImageFromSD(const std::string& image_path) {
        auto display = Board::GetInstance().GetDisplay();
        if (!display) {
            ESP_LOGE(TAG, "无法获取显示对象");
            return;
        }
        if (!display->HasCanvas()) {
            display->CreateCanvas();
        }

        std::string full_path = "/sdcard/" + image_path;
        FILE* fp = fopen(full_path.c_str(), "rb");
        if (!fp) {
            ESP_LOGE(TAG, "无法打开图片文件: %s", full_path.c_str());
            display->ShowNotification("图片不存在", 2000);
            return;
        }

        // 获取文件大小
        struct stat st;
        if (stat(full_path.c_str(), &st) != 0) {
            ESP_LOGE(TAG, "无法获取图片文件大小: %s", full_path.c_str());
            fclose(fp);
            display->ShowNotification("图片读取失败", 2000);
            return;
        }
        size_t file_size = st.st_size;

        // 在PSRAM中分配PNG数据缓冲区
        uint8_t* png_data = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!png_data) {
            ESP_LOGE(TAG, "PSRAM分配PNG缓冲区失败");
            fclose(fp);
            display->ShowNotification("内存不足", 2000);
            return;
        }
        fread(png_data, 1, file_size, fp);
        fclose(fp);

        // 解码PNG为RGBA8888
        std::vector<unsigned char> image_rgba;
        unsigned width = 0, height = 0;
        unsigned error = lodepng::decode(image_rgba, width, height, png_data, file_size);
        heap_caps_free(png_data);
        if (error) {
            ESP_LOGE(TAG, "PNG解码失败: %s", lodepng_error_text(error));
            display->ShowNotification("PNG解码失败", 2000);
            return;
        }

        // 在PSRAM中分配RGB565缓冲区
        uint8_t* rgb565_data = (uint8_t*)heap_caps_malloc(width * height * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!rgb565_data) {
            ESP_LOGE(TAG, "PSRAM分配RGB565缓冲区失败");
            display->ShowNotification("内存不足", 2000);
            return;
        }
        for (size_t i = 0; i < width * height; ++i) {
            uint8_t r = image_rgba[i * 4 + 0];
            uint8_t g = image_rgba[i * 4 + 1];
            uint8_t b = image_rgba[i * 4 + 2];
            uint16_t pixel = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
            // 若需要交换字节序（如LVGL要求），可加上：
            pixel = (pixel >> 8) | (pixel << 8);
            rgb565_data[i * 2 + 0] = pixel & 0xFF;
            rgb565_data[i * 2 + 1] = (pixel >> 8) & 0xFF;
        }

        display->DrawImageOnCanvas(0, 0, width, height, rgb565_data);
        ESP_LOGI(TAG, "PNG图片显示成功: %s", full_path.c_str());

        vTaskDelay(pdMS_TO_TICKS(3000));
        if (display->HasCanvas()) {
            display->DestroyCanvas();
            ESP_LOGI(TAG, "已关闭画布显示");
        }
        heap_caps_free(rgb565_data);
    }

    // raw RGB565图片显示逻辑
    void ShowRawImageFromSD(const std::string& image_path, int width, int height) {
        auto display = Board::GetInstance().GetDisplay();
        if (!display) {
            ESP_LOGE(TAG, "无法获取显示对象");
            return;
        }
        if (!display->HasCanvas()) {
            display->CreateCanvas();
        }

        std::string full_path = "/sdcard/" + image_path;
        FILE* fp = fopen(full_path.c_str(), "rb");
        if (!fp) {
            ESP_LOGE(TAG, "无法打开图片文件: %s", full_path.c_str());
            display->ShowNotification("图片不存在", 2000);
            return;
        }

        size_t expected_size = width * height * 2;
        uint8_t* rgb565_data = (uint8_t*)heap_caps_malloc(expected_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!rgb565_data) {
            ESP_LOGE(TAG, "PSRAM分配RGB565缓冲区失败");
            fclose(fp);
            display->ShowNotification("内存不足", 2000);
            return;
        }
        size_t read_size = fread(rgb565_data, 1, expected_size, fp);
        fclose(fp);

        if (read_size != expected_size) {
            ESP_LOGE(TAG, "图片数据大小不符: 期望%zu字节, 实际%zu字节", expected_size, read_size);
            display->ShowNotification("图片尺寸错误", 2000);
            heap_caps_free(rgb565_data);
            return;
        }

        display->DrawImageOnCanvas(0, 0, width, height, rgb565_data);
        ESP_LOGI(TAG, "raw RGB565图片显示成功: %s", full_path.c_str());

        if(image_path == "F3.RAW"  ){            

        vTaskDelay(pdMS_TO_TICKS(2000));
        ShowRawImageFromSD("F4.RAW", 240, 240);
        // vTaskDelay(pdMS_TO_TICKS(5000));
        // if (display->HasCanvas()) {
        //     display->DestroyCanvas();
        //     ESP_LOGI(TAG, "已关闭画布显示");
        // }
        
         // 使用多线程方式延迟销毁画布
        auto destroy_canvas_task = [](void* param) {
            auto display = static_cast<LcdDisplay*>(param);
            vTaskDelay(pdMS_TO_TICKS(5000));
            if (display && display->HasCanvas()) {
                display->DestroyCanvas();
                ESP_LOGI(TAG, "已关闭画布显示");
            }
            vTaskDelete(NULL);
        };
        // 创建新任务执行延迟销毁
        xTaskCreate(
            destroy_canvas_task,      // 任务函数
            "DestroyCanvasTask",      // 任务名称
            4096,                     // 堆栈大小
            display,                  // 传递display指针
            5,                        // 优先级
            NULL                      // 任务句柄
        );
        }
        heap_caps_free(rgb565_data);
    }
    // 延迟销毁画布
    struct CanvasDelayParam {
        LcdDisplay* display;
        int delay_ms;
    };
    static void DestroyCanvasDelay(void* param) {
        auto* delay_param = static_cast<CanvasDelayParam*>(param);
        auto display = delay_param->display;
        int delay_ms = delay_param->delay_ms;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        if (display && display->HasCanvas()) {  
            display->DestroyCanvas();
            ESP_LOGI(TAG, "已关闭画布显示");
        }
        free(delay_param);
        vTaskDelete(NULL);
    }
    // 创建新任务执行延迟销毁
    static void CreateDestroyCanvasTask(void* display_param, int delay_ms) {
        auto* param = (CanvasDelayParam*)malloc(sizeof(CanvasDelayParam));
        param->display = static_cast<LcdDisplay*>(display_param);
        param->delay_ms = delay_ms;
        xTaskCreate(
            DestroyCanvasDelay,      // 任务函数
            "DestroyCanvasTask",      // 任务名称
            4096,                     // 堆栈大小
            param,                  // 传递结构体指针      
            5,                        // 优先级
            NULL                      // 任务句柄
        );
    }

};

} // namespace iot

DECLARE_THING(ImageDisplayer);