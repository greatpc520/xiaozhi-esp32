/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <algorithm>
#include <cstring>
#include <esp_pthread.h>
#include <dirent.h>
#include <fstream>
#include <vector>
#include <variant>

#include "application.h"
#include "display.h"
#include "board.h"
#include "display/lcd_display.h"
#include "display/spi_lcd_anim_display.h"
#include "alarm_mcp_tools.h"
#include "time_sync_manager.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "MCP"
#define MOUNT_POINT "/sdcard"
#define AUDIO_FILE_EXTENSION ".P3"
#define DEFAULT_TOOLCALL_STACK_SIZE 6144
#define MOUNT_POINT "/sdcard"
#define AUDIO_FILE_EXTENSION ".P3"

// 播放SD卡音乐的MCP工具函数
ReturnValue PlayMusicsd(int file_number)
    {
      // 挂载 SD 卡文件系统
    //   bsp_sdcard_mount();
      DIR *dir = opendir(MOUNT_POINT);
      if (dir == NULL)
      {
        ESP_LOGE(TAG, "Failed to open directory: %s", MOUNT_POINT);
        return "{\"success\": false, \"message\": \"Failed to open SD card directory\"}";
      }
      struct dirent *entry;
      std::vector<std::string> audio_files; // 用来存储符合条件的音频文件
      // 遍历目录中的文件
      while ((entry = readdir(dir)) != NULL)
      {
        // 只处理扩展名为 .p3 的文件
        if (strstr(entry->d_name, AUDIO_FILE_EXTENSION) || strstr(entry->d_name, ".p3"))
        {
          audio_files.push_back(entry->d_name); // 将符合条件的文件存入容器
        }
        ESP_LOGI(TAG, "Found file: %s", entry->d_name);
      }
      ESP_LOGI(TAG, "Total audio files found: %d", audio_files.size());
      closedir(dir);
      
      if (audio_files.empty())
      {
        ESP_LOGE(TAG, "No valid audio file found.");
        return "{\"success\": false, \"message\": \"No valid audio files found on SD card\"}";
      }
      
      // 判断文件序号是否有效
      if (file_number < 0 || file_number >= audio_files.size())
      {
        ESP_LOGW(TAG, "Invalid file number: %d, using first file", file_number);
        file_number = 0; // 自动使用第一个文件
      }
      
      // 根据 file_number 获取文件路径
      char file_path[512];
      snprintf(file_path, sizeof(file_path), "%s/%s", MOUNT_POINT, audio_files[file_number].c_str());
      auto &app = Application::GetInstance();
      ESP_LOGI(TAG, "Playing file: %s", file_path);
      
      // 尝试打开文件
      std::ifstream file(file_path, std::ios::binary);
      if (!file.is_open())
      {
        ESP_LOGE(TAG, "Failed to open file: %s", file_path);
        return "{\"success\": false, \"message\": \"Failed to open audio file: " + std::string(audio_files[file_number]) + "\"}";
      }
      
      // 获取文件大小并读取文件内容
      file.seekg(0, std::ios::end);
      size_t size = file.tellg();
      file.seekg(0, std::ios::beg);
      
      if (size == 0)
      {
        ESP_LOGE(TAG, "Audio file is empty: %s", file_path);
        return "{\"success\": false, \"message\": \"Audio file is empty: " + std::string(audio_files[file_number]) + "\"}";
      }
      
      std::vector<char> file_data(size);
      file.read(file_data.data(), size);
      if (!file)
      {
        ESP_LOGE(TAG, "Failed to read the entire file: %s", file_path);
        return "{\"success\": false, \"message\": \"Failed to read audio file: " + std::string(audio_files[file_number]) + "\"}";
      }
      
      // 读取并播放声音
      std::string_view sound_view(file_data.data(), file_data.size());
        auto codec = Board::GetInstance().GetAudioCodec();       
        // 确保音频输出打开
        codec->EnableOutput(true);
      app.PlaySound(sound_view);
      ESP_LOGI(TAG, "File %s played successfully", file_path);
      
      return "{\"success\": true, \"message\": \"Audio file played successfully\", \"file\": \"" + std::string(audio_files[file_number]) + "\", \"size\": " + std::to_string(size) + ", \"total_files\": " + std::to_string(audio_files.size()) + "}";
    }
McpServer::McpServer() {
}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools() {
    // To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();
    
    // 注册闹钟相关的MCP工具
    AlarmMcpTools::RegisterTools(*this);
    
    // 注册时间同步管理工具
    AddTool("rtc.get_status",
        "获取RTC时钟状态和当前时间信息",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& time_sync_manager = TimeSyncManager::GetInstance();
            std::string status = "{";
            status += "\"rtc_working\": " + std::string(time_sync_manager.IsRtcWorking() ? "true" : "false") + ",";
            status += "\"current_time\": \"" + time_sync_manager.GetCurrentTimeString() + "\"";
            status += "}";
            return status;
        });
        
    AddTool("rtc.sync_time",
        "手动触发NTP时间同步到RTC",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            auto& time_sync_manager = TimeSyncManager::GetInstance();
            
            // 设置临时回调，确保手动同步后立即更新时钟UI
            time_sync_manager.SetSyncCallback([&board](bool success, const std::string& message) {
                if (success) {
                    ESP_LOGI("McpServer", "Manual NTP sync completed successfully: %s", message.c_str());
                    
                    // 如果时钟界面可见，立即更新显示
                    if (board.IsClockVisible()) {
                        auto clock_ui = board.GetClockUI();
                        if (clock_ui) {
                            // 在主线程中更新时钟UI
                            Application::GetInstance().Schedule([clock_ui]() {
                                clock_ui->ForceUpdateDisplay();
                                clock_ui->UpdateClockDisplay();
                                ESP_LOGI("McpServer", "Clock UI updated after manual NTP sync");
                            });
                        }
                    }
                } else {
                    ESP_LOGW("McpServer", "Manual NTP sync failed: %s", message.c_str());
                }
            });
            
            time_sync_manager.TriggerNtpSync();
            return "{\"success\": true, \"message\": \"NTP同步已启动，时钟界面将在同步完成后自动更新\"}";
        });
        
    AddTool("rtc.force_sync_time",
        "强制立即NTP时间同步（跳过系统空闲检查）",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            auto& time_sync_manager = TimeSyncManager::GetInstance();
            
            // 设置临时回调，确保强制同步后立即更新时钟UI
            time_sync_manager.SetSyncCallback([&board](bool success, const std::string& message) {
                if (success) {
                    ESP_LOGI("McpServer", "Force NTP sync completed successfully: %s", message.c_str());
                    
                    // 如果时钟界面可见，立即更新显示
                    if (board.IsClockVisible()) {
                        auto clock_ui = board.GetClockUI();
                        if (clock_ui) {
                            // 在主线程中更新时钟UI
                            Application::GetInstance().Schedule([clock_ui]() {
                                clock_ui->ForceUpdateDisplay();
                                clock_ui->UpdateClockDisplay();
                                ESP_LOGI("McpServer", "Clock UI updated after force NTP sync");
                            });
                        }
                    }
                } else {
                    ESP_LOGW("McpServer", "Force NTP sync failed: %s", message.c_str());
                }
            });
            
            time_sync_manager.ForceNtpSync();
            return "{\"success\": true, \"message\": \"强制NTP同步已启动，时钟界面将在同步完成后自动更新\"}";
        });

    AddTool("test.clock_update_after_sync",
        "测试工具：显示时钟界面并触发NTP同步，验证同步后时钟UI是否立即更新",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI("McpServer", "Starting clock update test after NTP sync");
            
            // 1. 先显示时钟界面
            board.ShowClock();
            
            // 2. 等待1秒确保时钟界面显示完成
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            // 3. 设置同步回调并触发NTP同步
            auto& time_sync_manager = TimeSyncManager::GetInstance();
            time_sync_manager.SetSyncCallback([&board](bool success, const std::string& message) {
                ESP_LOGI("McpServer", "Test NTP sync result: %s - %s", success ? "SUCCESS" : "FAILED", message.c_str());
                
                if (success && board.IsClockVisible()) {
                    auto clock_ui = board.GetClockUI();
                    if (clock_ui) {
                        Application::GetInstance().Schedule([clock_ui]() {
                            clock_ui->ForceUpdateDisplay();
                            clock_ui->UpdateClockDisplay();
                            ESP_LOGI("McpServer", "TEST: Clock UI updated successfully after NTP sync");
                        });
                    }
                }
            });
            
            // 4. 触发强制NTP同步
            time_sync_manager.ForceNtpSync();
            
            return "{\"success\": true, \"message\": \"测试已启动：时钟界面已显示，NTP同步已触发，将在同步完成后自动更新时间显示\"}";
        });

    AddTool("test.show_alarm_emotion", "测试闹钟表情显示功能", 
        PropertyList({
            Property("emotion_type", kPropertyTypeString)
        }),
        [&board](const PropertyList& properties) -> ReturnValue {
            auto clock_ui = board.GetClockUI();
            
            if (!clock_ui) {
                return "{\"success\": false, \"message\": \"Clock UI not available\"}";
            }
            
            if (!board.IsClockVisible()) {
                return "{\"success\": false, \"message\": \"Clock UI not visible. Please show clock first.\"}";
            }
            
            try {
                std::string emotion_type = properties["emotion_type"].value<std::string>();
                
                if (emotion_type.empty()) {
                    return "{\"success\": false, \"message\": \"emotion_type parameter is required\"}";
                }
                
                clock_ui->SetAlarmEmotion(emotion_type);
                
                return "{\"success\": true, \"message\": \"Alarm emotion displayed successfully\", \"emotion_type\": \"" + emotion_type + "\"}";
            } catch (const std::exception& e) {
                return "{\"success\": false, \"message\": \"Exception: " + std::string(e.what()) + "\"}";
            }
        });

    AddTool("test.cleanup_expired_alarms", "测试清理过期闹钟功能", 
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            auto& alarm_manager = AlarmManager::GetInstance();
            
            try {
                // 获取清理前的闹钟数量
                auto all_alarms_before = alarm_manager.GetAllAlarms();
                int total_before = all_alarms_before.size();
                
                // 执行过期闹钟清理
                int removed_count = alarm_manager.RemoveExpiredAlarms();
                
                // 获取清理后的闹钟数量
                auto all_alarms_after = alarm_manager.GetAllAlarms();
                int total_after = all_alarms_after.size();
                
                std::string message = "Expired alarms cleanup completed. ";
                message += "Before: " + std::to_string(total_before) + " alarms, ";
                message += "After: " + std::to_string(total_after) + " alarms, ";
                message += "Removed: " + std::to_string(removed_count) + " expired alarms";
                
                // 如果时钟界面可见，刷新闹钟显示
                if (board.IsClockVisible()) {
                    auto clock_ui = board.GetClockUI();
                    if (clock_ui) {
                        Application::GetInstance().Schedule([clock_ui]() {
                            clock_ui->RefreshNextAlarmDisplay();
                        });
                        message += ". Clock display refreshed.";
                    }
                }
                
                return "{\"success\": true, \"message\": \"" + message + "\", \"removed_count\": " + std::to_string(removed_count) + ", \"total_before\": " + std::to_string(total_before) + ", \"total_after\": " + std::to_string(total_after) + "}";
                
            } catch (const std::exception& e) {
                return "{\"success\": false, \"message\": \"Exception during cleanup: " + std::string(e.what()) + "\"}";
            }
        });

    AddTool("test.network_wallpaper", "测试网络壁纸下载和显示", 
        PropertyList({
            Property("test_url", kPropertyTypeString, "http://www.replime.cn/ejpg/happy.jpg")
        }),
        [&board](const PropertyList& properties) -> ReturnValue {
            if (!board.IsClockVisible()) {
                return "{\"success\": false, \"message\": \"Clock UI not visible. Please show clock first.\"}";
            }
            
            auto clock_ui = board.GetClockUI();
            if (!clock_ui) {
                return "{\"success\": false, \"message\": \"Clock UI not available\"}";
            }
            
            std::string test_url = properties["test_url"].value<std::string>();
            
            try {
                ESP_LOGI("MCP", "Testing network wallpaper with URL: %s", test_url.c_str());
                clock_ui->SetNetworkWallpaper(test_url.c_str());
                return "{\"success\": true, \"message\": \"Network wallpaper test started\", \"url\": \"" + test_url + "\"}";
            } catch (const std::exception& e) {
                return "{\"success\": false, \"message\": \"Failed to test network wallpaper: " + std::string(e.what()) + "\"}";
            }
        });

    AddTool("test.debug_next_alarm", "调试下一次闹钟显示功能，显示所有闹钟信息和当前时间", 
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            try {
                auto& alarm_manager = AlarmManager::GetInstance();
                
                // 获取所有闹钟信息
                std::string debug_info = "{\"alarms\": [";
                auto alarms = alarm_manager.GetAllAlarms();
                
                for (size_t i = 0; i < alarms.size(); i++) {
                    const auto& alarm = alarms[i];
                    if (i > 0) debug_info += ", ";
                    
                    debug_info += "{\"id\": " + std::to_string(alarm.id) + 
                                 ", \"time\": \"" + std::to_string(alarm.hour) + ":" + 
                                 (alarm.minute < 10 ? "0" : "") + std::to_string(alarm.minute) + 
                                 "\", \"enabled\": " + (alarm.enabled ? "true" : "false") +
                                 ", \"description\": \"" + alarm.description + 
                                 "\", \"weekdays\": [";
                    
                    for (size_t j = 0; j < alarm.weekdays.size(); j++) {
                        if (j > 0) debug_info += ", ";
                        debug_info += std::to_string(alarm.weekdays[j]);
                    }
                    debug_info += "]}";
                }
                debug_info += "], ";
                
                // 获取下一个闹钟
                AlarmInfo next_alarm = alarm_manager.GetNextAlarm();
                debug_info += "\"next_alarm\": {\"id\": " + std::to_string(next_alarm.id) + 
                             ", \"time\": \"" + std::to_string(next_alarm.hour) + ":" + 
                             (next_alarm.minute < 10 ? "0" : "") + std::to_string(next_alarm.minute) + 
                             "\", \"description\": \"" + next_alarm.description + "\"}";
                
                // 获取当前时间
                struct tm current_tm;
                auto& time_sync_manager = TimeSyncManager::GetInstance();
                if (time_sync_manager.GetUnifiedTime(&current_tm)) {
                    debug_info += ", \"current_time\": \"" + std::to_string(current_tm.tm_hour) + ":" + 
                                 (current_tm.tm_min < 10 ? "0" : "") + std::to_string(current_tm.tm_min) + 
                                 "\", \"current_weekday\": " + std::to_string(current_tm.tm_wday);
                } else {
                    debug_info += ", \"current_time\": \"ERROR\", \"current_weekday\": -1";
                }
                
                debug_info += ", \"success\": true}";
                return debug_info;
                
            } catch (const std::exception& e) {
                return "{\"success\": false, \"message\": \"Exception: " + std::string(e.what()) + "\"}";
            }
        });

    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    AddTool("self.audio_speaker.set_volume", 
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }), 
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            return true;
        });
    
    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness",
            "Set the brightness of the screen.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);
                return true;
            });
    }

    auto display = board.GetDisplay();
    if (display && !display->GetTheme().empty()) {
        AddTool("self.screen.set_theme",
            "Set the theme of the screen. The theme can be `light` or `dark`.",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                display->SetTheme(properties["theme"].value<std::string>().c_str());
                return true;
            });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
            "Take a photo and explain it. Use this tool after the user asks you to see something.\n"
            "Args:\n"
            "  `question`: The question that you want to ask about the photo.\n"
            "Return:\n"
            "  A JSON object that provides the photo information.",
            PropertyList({
                Property("question", kPropertyTypeString)
            }),
            [camera](const PropertyList& properties) -> ReturnValue {
                if (!camera->Capture()) {
                    return "{\"success\": false, \"message\": \"Failed to capture photo\"}";
                }
                auto question = properties["question"].value<std::string>();
                return camera->Explain(question);
            });
    }

   

    // Add Image Display related tools
    AddTool("showurlimg", "显示网络图片", PropertyList(), [&board](const PropertyList& parameters) -> ReturnValue {
        auto display = board.GetDisplay();
        if (!display) {
            return "{\"success\": false, \"message\": \"Display not available\"}";
        }
        
        // 使用static_cast替代dynamic_cast以兼容-fno-rtti
        auto anim_display = static_cast<SpiLcdAnimDisplay*>(display);
        if (!anim_display) {
            return "{\"success\": false, \"message\": \"Invalid display type\"}";
        }

        const char* url = "http://www.replime.cn/ejpg/laughing.jpg";
        anim_display->showurl(url);
        
        return "{\"success\": true, \"message\": \"Image display request sent\", \"url\": \"" + std::string(url) + "\"}";
    });

    AddTool("show_emotion", "显示表情图片,支持的表情名称：\n"
            "- 表情名称: `neutral`, `happy`, `laughing`, `funny`, `sad`, `angry`, `crying`, `loving`, `embarrassed`, `surprised`, `shocked`, `thinking`, `winking`, `cool`, `relaxed`, `delicious`, `kissy`, `confident`, `sleepy`, `silly`, `confused`\n", 
        PropertyList({
            Property("emotion", kPropertyTypeString)
        }),
        [&board](const PropertyList& properties) -> ReturnValue {
            auto display = board.GetDisplay();
            if (!display) {
                return "{\"success\": false, \"message\": \"Display not available\"}";
            }
            
            auto anim_display = static_cast<SpiLcdAnimDisplay*>(display);
            if (!anim_display) {
                return "{\"success\": false, \"message\": \"Invalid display type\"}";
            }

            static const std::vector<std::pair<const char*, const char*>> emotions = {
                {"neutral", "neutral"},
                {"happy", "happy"},
                {"laughing", "laughing"},
                {"funny", "funny"},
                {"sad", "sad"},
                {"angry", "angry"},
                {"crying", "crying"},
                {"loving", "loving"},
                {"embarrassed", "embarrassed"},
                {"surprised", "surprised"},
                {"shocked", "shocked"},
                {"thinking", "thinking"},
                {"winking", "winking"},
                {"cool", "cool"},
                {"relaxed", "relaxed"},
                {"delicious", "delicious"},
                {"kissy", "kissy"},
                {"confident", "confident"},
                {"sleepy", "sleepy"},
                {"silly", "silly"},
                {"confused", "confused"}
            };

            std::string emotion = properties["emotion"].value<std::string>();
            auto it = std::find_if(emotions.begin(), emotions.end(),
                [&emotion](const auto& e) { return e.first == emotion; });
            
            if (it == emotions.end()) {
                return "{\"success\": false, \"message\": \"Invalid emotion name\"}";
            }

            std::string url = "http://www.replime.cn/ejpg/" + std::string(it->second) + ".jpg";
            anim_display->showurl(url.c_str());
            
            return "{\"success\": true, \"message\": \"Emotion image display request sent\", \"emotion\": \"" + emotion + "\", \"url\": \"" + url + "\"}";
        });

    AddTool("show_sd_image", "显示SD卡中的图片", 
        PropertyList({
            Property("index", kPropertyTypeInteger, 0, 999)
        }),
        [this, &board](const PropertyList& properties) -> ReturnValue {
            auto display = board.GetDisplay();
            if (!display) {
                return "{\"success\": false, \"message\": \"Display not available\"}";
            }
            int index = properties["index"].value<int>();
            if (index == 0) {
                return "{\"success\": true, \"message\": \"Index 0, no action taken\"}";
            }
            
            // 构建文件路径
            std::string image_path = "F" + std::to_string(index) + ".RAW";
            
            // 显示图片
            if (this->ShowRawImageFromSD(display, image_path, 240, 240)) {
                return "{\"success\": true, \"message\": \"Image displayed successfully\", \"file\": \"" + image_path + "\"}";
            } else {
                return "{\"success\": false, \"message\": \"Failed to display image\", \"file\": \"" + image_path + "\"}";
            }
        });

    AddTool("close_image", "关闭当前显示的图片", 
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            auto display = board.GetDisplay();
            if (!display) {
                return "{\"success\": false, \"message\": \"Display not available\"}";
            }
            if (display->HasCanvas()) {
                display->DestroyCanvas();
                return "{\"success\": true, \"message\": \"Image closed successfully\"}";
            } else {
                return "{\"success\": false, \"message\": \"No canvas to close\"}";
            }
        });

    AddTool("delay_close_canvas", "延时关闭画布", 
        PropertyList({
            Property("delay_ms", kPropertyTypeInteger, 100, 10000)
        }),
        [this, &board](const PropertyList& properties) -> ReturnValue {
            auto display = board.GetDisplay();
            if (!display) {
                return "{\"success\": false, \"message\": \"Display not available\"}";
            }
            int delay_ms = properties["delay_ms"].value<int>();
            this->CreateDestroyCanvasTask(display, delay_ms);
            return "{\"success\": true, \"message\": \"Canvas will be closed after " + std::to_string(delay_ms) + "ms\"}";
        });

    // Add Chassis related tools
    AddTool("motorReset", "复位终端", PropertyList(), [this](const PropertyList& parameters) -> ReturnValue {
        printf("control_motor reset.\r\n");
        control_motor_impl(0,343,0);control_motor_impl(0,171,1);
        control_motor_impl(1,200,0);control_motor_impl(1,100,1);
        printf("control_motor reset ok.\r\n");
        return true;
    });
    AddTool("GoForward", "向上转", PropertyList(), [this](const PropertyList& parameters) -> ReturnValue {
        control_motor_impl(1, 30, true);
        return true;
    });

    AddTool("GoBack", "向下转", PropertyList(), [this](const PropertyList& parameters) -> ReturnValue {
        control_motor_impl(1, 30, false);
        return true;
    });

    AddTool("TurnLeft", "向左转", PropertyList(), [this](const PropertyList& parameters) -> ReturnValue {
        control_motor_impl(0, 30, false);
        return true;
    });

    AddTool("TurnRight", "向右转", PropertyList(), [this](const PropertyList& parameters) -> ReturnValue {
        control_motor_impl(0, 30, true);
        return true;
    });

    AddTool("BlOff", "打开屏幕", PropertyList(), [this](const PropertyList& parameters) -> ReturnValue {
        set_backlight_impl(0);
        return true;
    });

    AddTool("BlOn", "关闭屏幕", PropertyList(), [this](const PropertyList& parameters) -> ReturnValue {
        set_backlight_impl(1);
        return true;
    });
    
    AddTool("TurnOn", "打开灯", PropertyList(), [this](const PropertyList& parameters) -> ReturnValue {
        set_led_impl(0);
        return true;
    });

    AddTool("TurnOff", "关闭灯", PropertyList(), [this](const PropertyList& parameters) -> ReturnValue {
        set_led_impl(1);
        return true;
    });
    AddTool("sysReset", "系统重启", PropertyList(), [this](const PropertyList& parameters) -> ReturnValue {
        esp_restart();
        return true;
    });
    AddTool("PlayMusic", "播放本地音乐", PropertyList(), [this](const PropertyList& parameters) -> ReturnValue {
        play_music_impl(0);
        return true;
    });
    
    AddTool("PlayMusicSD", "播放SD卡中的音频文件", 
        PropertyList({
            Property("file_number", kPropertyTypeInteger, 0, 0, 999)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            int file_number = properties["file_number"].value<int>();
            return PlayMusicsd(file_number);
        });

    // 添加时钟壁纸相关工具
    AddTool("clock.set_solid_wallpaper", "设置时钟纯色壁纸,支持的颜色格式：\n"
            "- 颜色名称: `red`, `green`, `blue`, `yellow`, `purple`, `cyan`, `white`, `black`, `orange`, `pink`\n"
            "- 十六进制格式: `#FF0000` (红色)\n"
            "- 0x格式: `0xFF0000` (红色)", 
        PropertyList({
            Property("color", kPropertyTypeString)
        }),
        [&board](const PropertyList& properties) -> ReturnValue {
            // 检查是否有时钟界面
            if (!board.IsClockVisible()) {
                ESP_LOGI(TAG, "Clock not visible, showing clock first for wallpaper setting");
                board.ShowClock();
                // 等待时钟界面创建完成
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            
            // 获取时钟UI实例
            auto clock_ui = board.GetClockUI();
            if (!clock_ui) {
                return "{\"success\": false, \"message\": \"Clock UI not available\"}";
            }
            
            std::string color_str = properties["color"].value<std::string>();
            uint32_t color = 0x0000FF; // 默认蓝色
            
            // 解析颜色字符串（支持#RRGGBB和0xRRGGBB格式）
            if (color_str.length() > 0) {
                if (color_str[0] == '#' && color_str.length() == 7) {
                    color = strtol(color_str.c_str() + 1, nullptr, 16);
                } else if (color_str.substr(0, 2) == "0x" && color_str.length() == 8) {
                    color = strtol(color_str.c_str(), nullptr, 16);
                } else {
                    // 支持颜色名称
                    if (color_str == "red") color = 0xFF0000;
                    else if (color_str == "green") color = 0x00FF00;
                    else if (color_str == "blue") color = 0x0000FF;
                    else if (color_str == "yellow") color = 0xFFFF00;
                    else if (color_str == "purple") color = 0xFF00FF;
                    else if (color_str == "cyan") color = 0x00FFFF;
                    else if (color_str == "white") color = 0xFFFFFF;
                    else if (color_str == "black") color = 0x000000;
                    else if (color_str == "orange") color = 0xFF7F00;
                    else if (color_str == "pink") color = 0xFF69B4;
                    else {
                        return "{\"success\": false, \"message\": \"Invalid color format. Use #RRGGBB, 0xRRGGBB, or color names\"}";
                    }
                }
            }
            
            // 直接应用壁纸（因为时钟界面已经可见）
            clock_ui->SetSolidColorWallpaper(color);
            
            // 延迟显示壁纸效果
            vTaskDelay(pdMS_TO_TICKS(3000));
            
            // 关闭时钟显示
            ESP_LOGI(TAG, "Wallpaper setting completed, hiding clock");
            board.HideClock();
            
            return "{\"success\": true, \"message\": \"Solid color wallpaper set successfully\", \"color\": \"" + color_str + "\"}";
        });

    AddTool("clock.set_image_wallpaper", "设置时钟图片壁纸（从SD卡）,支持的图片名称：- 图片名称: `BJ`, `BJ2`, `BJ3`, `BJ4`, `BJ5`, `BJ6`, `BJ7`, `BJ8`, `BJ9`, `BJ10`\n", 
        PropertyList({
            Property("image_name", kPropertyTypeString)
        }),
        [&board](const PropertyList& properties) -> ReturnValue {
            // 检查是否有时钟界面
            if (!board.IsClockVisible()) {
                ESP_LOGI(TAG, "Clock not visible, showing clock first for image wallpaper setting");
                board.ShowClock();
                // 等待时钟界面创建完成
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            
            auto clock_ui = board.GetClockUI();
            if (!clock_ui) {
                return "{\"success\": false, \"message\": \"Clock UI not available\"}";
            }
            
            std::string image_name = properties["image_name"].value<std::string>();
            
            // 验证文件名（不超过8位，大写）
            if (image_name.length() > 8) {
                return "{\"success\": false, \"message\": \"Image name too long (max 8 characters)\"}";
            }
            
            // 转换为大写
            std::transform(image_name.begin(), image_name.end(), image_name.begin(), ::toupper);
            
            // 直接应用壁纸（因为时钟界面已经可见）
            clock_ui->SetImageWallpaper(image_name.c_str());
            
            // 延迟显示壁纸效果
            vTaskDelay(pdMS_TO_TICKS(3000));
            
            // 关闭时钟显示
            ESP_LOGI(TAG, "Image wallpaper setting completed, hiding clock");
            board.HideClock();
            
            return "{\"success\": true, \"message\": \"Image wallpaper set successfully\", \"image_name\": \"" + image_name + "\"}";
        });

    AddTool("clock.set_network_wallpaper", "设置时钟网络壁纸\n"
            "- 默认URL（未指定时）: `http://www.replime.cn/ejpg/happy.jpg` (保存为本地文件名 HAPPY.JPG)\n"
            "- URL格式: `http://www.replime.cn/ejpg/图片名称.jpg`\n"
            "- 支持的图片名称: `happy`, `laughing`, `funny`, `sad`, `angry`, `crying`, `loving`, `embarrassed`, `surprised`, `shocked`, `thinking`, `winking`, `cool`, `relaxed`, `delicious`, `kissy`, `confident`, `sleepy`, `silly`, `confused`\n"
            "- 本地文件名规则: 从URL提取文件名（去掉.jpg扩展名），自动转换为大写，最多8个字符\n", 
        PropertyList({
            Property("url", kPropertyTypeString, "http://www.replime.cn/ejpg/happy.jpg")  // 设置默认值
        }),
        [&board](const PropertyList& properties) -> ReturnValue {
            // 检查是否有时钟界面
            if (!board.IsClockVisible()) {
                ESP_LOGI(TAG, "Clock not visible, showing clock first for network wallpaper setting");
                board.ShowClock();
                // 等待时钟界面创建完成
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            
            auto clock_ui = board.GetClockUI();
            if (!clock_ui) {
                return "{\"success\": false, \"message\": \"Clock UI not available\"}";
            }
            
            std::string url = properties["url"].value<std::string>();
            
            // 直接应用壁纸（因为时钟界面已经可见）
            clock_ui->SetNetworkWallpaper(url.c_str());
            
            // 延迟显示壁纸效果（网络下载可能需要更长时间）
            vTaskDelay(pdMS_TO_TICKS(5000));
            
            // 关闭时钟显示
            ESP_LOGI(TAG, "Network wallpaper setting completed, hiding clock");
            board.HideClock();
            
            return "{\"success\": true, \"message\": \"Network wallpaper download and set successfully\", \"url\": \"" + url + "\"}";
        });

    AddTool("clock.clear_wallpaper", "清除时钟壁纸", 
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            // 检查是否有时钟界面
            if (!board.IsClockVisible()) {
                ESP_LOGI(TAG, "Clock not visible, showing clock first for clearing wallpaper");
                board.ShowClock();
                // 等待时钟界面创建完成
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            
            auto clock_ui = board.GetClockUI();
            if (!clock_ui) {
                return "{\"success\": false, \"message\": \"Clock UI not available\"}";
            }
            
            // 直接清除壁纸（因为时钟界面已经可见）
            clock_ui->ClearWallpaper();
            
            // 延迟显示清除效果
            vTaskDelay(pdMS_TO_TICKS(2000));
            
            // 关闭时钟显示
            ESP_LOGI(TAG, "Wallpaper clearing completed, hiding clock");
            board.HideClock();
            
            return "{\"success\": true, \"message\": \"Wallpaper cleared successfully\"}";
        });

    AddTool("clock.set_animation", "设置时钟动画（从SD卡）", 
        PropertyList({
            Property("animation_name", kPropertyTypeString)
        }),
        [&board](const PropertyList& properties) -> ReturnValue {
            auto clock_ui = board.GetClockUI();
            if (!clock_ui) {
                return "{\"success\": false, \"message\": \"Clock UI not available\"}";
            }
            
            std::string anim_name = properties["animation_name"].value<std::string>();
            
            // 检查时钟UI是否可见
            if (!board.IsClockVisible()) {
                clock_ui->SaveAnimationFromSDConfig(anim_name.c_str());
                return "{\"success\": true, \"message\": \"Animation config saved. Will be applied when clock is shown.\", \"animation_name\": \"" + anim_name + "\"}";
            } else {
                clock_ui->SetAnimationFromSD(anim_name.c_str());
                return "{\"success\": true, \"message\": \"Animation set and applied\", \"animation_name\": \"" + anim_name + "\"}";
            }
        });

    AddTool("clock.set_network_animation", "设置时钟网络动画", 
        PropertyList({
            Property("url", kPropertyTypeString)
        }),
        [&board](const PropertyList& properties) -> ReturnValue {
            auto clock_ui = board.GetClockUI();
            if (!clock_ui) {
                return "{\"success\": false, \"message\": \"Clock UI not available\"}";
            }
            
            std::string url = properties["url"].value<std::string>();
            
            // 检查时钟UI是否可见
            if (!board.IsClockVisible()) {
                clock_ui->SaveAnimationFromNetworkConfig(url.c_str());
                return "{\"success\": true, \"message\": \"Network animation config saved. Download will start when clock is shown.\", \"url\": \"" + url + "\"}";
            } else {
                clock_ui->SetAnimationFromNetwork(url.c_str());
                return "{\"success\": true, \"message\": \"Network animation download started\", \"url\": \"" + url + "\"}";
            }
        });

    AddTool("clock.show_animation", "显示/隐藏时钟动画", 
        PropertyList({
            Property("show", kPropertyTypeBoolean, true)
        }),
        [&board](const PropertyList& properties) -> ReturnValue {
            if (!board.IsClockVisible()) {
                return "{\"success\": false, \"message\": \"Clock UI not visible\"}";
            }
            
            auto clock_ui = board.GetClockUI();
            
            if (!clock_ui) {
                return "{\"success\": false, \"message\": \"Clock UI not available\"}";
            }
            
            bool show = properties["show"].value<bool>();
            clock_ui->ShowAnimation(show);
            
            return "{\"success\": true, \"message\": \"Animation " + std::string(show ? "shown" : "hidden") + "\"}";
        });

    AddTool("clock.clear_animation", "清除时钟动画", 
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            if (!board.IsClockVisible()) {
                return "{\"success\": false, \"message\": \"Clock UI not visible\"}";
            }
            
            auto clock_ui = board.GetClockUI();
            
            if (!clock_ui) {
                return "{\"success\": false, \"message\": \"Clock UI not available\"}";
            }
            
            clock_ui->ClearAnimation();
            
            return "{\"success\": true, \"message\": \"Animation cleared\"}";
        });

    // 添加AudioChannelClosed模式设置工具
    AddTool("config.set_audio_channel_closed_mode", 
        "设置待机时的操作模式\n"
        "模式1 (value=1): 时钟界面（默认）\n"
        "模式2 (value=2): 关闭屏幕并低头\n"
        "此设置会持续化存储，断电后仍然有效。",
        PropertyList({
            Property("mode", kPropertyTypeInteger, 1, 1, 2)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            int mode = properties["mode"].value<int>();
            
            auto& app = Application::GetInstance();
            if (mode == 1) {
                app.SetAudioChannelClosedMode(kAudioChannelClosedModeNormal);
                return "{\"success\": true, \"message\": \"AudioChannelClosed模式已设置为模式1：时钟界面\", \"mode\": 1}";
            } else if (mode == 2) {
                app.SetAudioChannelClosedMode(kAudioChannelClosedModeWithActions);
                return "{\"success\": true, \"message\": \"AudioChannelClosed模式已设置为模式2：关闭屏幕并低头\", \"mode\": 2}";
            } else {
                return "{\"success\": false, \"message\": \"无效的模式值，必须是1或2\", \"mode\": " + std::to_string(mode) + "}";
            }
        });

    AddTool("config.get_audio_channel_closed_mode", 
        "获取当前待机时的操作模式\n"
        "返回值：\n"
        "模式1: 时钟界面\n"
        "模式2: 关闭屏幕并低头",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            int current_mode = static_cast<int>(app.GetAudioChannelClosedMode());
            
            std::string mode_description;
            if (current_mode == 1) {
                mode_description = "时钟界面（默认）";
            } else if (current_mode == 2) {
                mode_description = "关闭屏幕并低头";
            } else {
                mode_description = "未知模式";
            }
            
            return "{\"success\": true, \"current_mode\": " + std::to_string(current_mode) + 
                   ", \"description\": \"" + mode_description + "\"}";
        });

     // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddTool(McpTool* tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s", tool->name().c_str());
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }
    
    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }
    
    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }
    
    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;
    
    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
        }
        GetToolsList(id_int, cursor_str);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        auto stack_size = cJSON_GetObjectItem(params, "stackSize");
        if (stack_size != nullptr && !cJSON_IsNumber(stack_size)) {
            ESP_LOGE(TAG, "tools/call: Invalid stackSize");
            ReplyError(id_int, "Invalid stackSize");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments, stack_size ? stack_size->valueint : DEFAULT_TOOLCALL_STACK_SIZE);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string& cursor) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";
    
    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";
    
    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }
        
        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }
        
        json += tool_json;
        ++it;
    }
    
    if (json.back() == ',') {
        json.pop_back();
    }
    
    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }
    
    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments, int stack_size) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                 [&tool_name](const McpTool* tool) { 
                                     return tool->name() == tool_name; 
                                 });
    
    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::runtime_error& e) {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Start a task to receive data with stack size
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name = "tool_call";
    cfg.stack_size = stack_size;
    cfg.prio = 1;
    esp_pthread_set_cfg(&cfg);

    // Use a thread to call the tool to avoid blocking the main thread
    tool_call_thread_ = std::thread([this, id, tool_iter, arguments = std::move(arguments)]() {
        try {
            ReplyResult(id, (*tool_iter)->Call(arguments));
        } catch (const std::runtime_error& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    });
    tool_call_thread_.detach();
}

void McpServer::set_backlight_impl(uint8_t brightness)
{
    auto& board = Board::GetInstance();
    auto motor = board.SetMotor();
    uint8_t level=0;
    if(brightness>0)level=1;
    motor->setbl(level);
}

void McpServer::set_led_impl(uint8_t brightness)
{
    auto& board = Board::GetInstance();
    auto motor = board.SetMotor();
    uint8_t level=0;
    if(brightness>0)level=1;
    motor->setled(level);
}

void McpServer::control_motor_impl(uint8_t motorid, int steps, bool direction)
{
    auto& board = Board::GetInstance();
    auto motor = board.SetMotor();
    motor->control_motor(motorid, steps, direction);
}

void McpServer::play_music_impl(int id) {
    ESP_LOGI(TAG, "Executing play_music with ID: %d", id);
    
    // Get current audio state
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    
    ESP_LOGI(TAG, "Current device state: %d", state);
    
    // Stop any current audio playback first
    if (state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Stopping current speech");
        app.AbortSpeaking(kAbortReasonNone);
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait for abort to complete
    }
    ReturnValue result = PlayMusicsd(id);
    // Implementation for different music sources would go here
    if (std::holds_alternative<std::string>(result)) {
        ESP_LOGI(TAG, "Playing music with ID: %d, result: %s", id, std::get<std::string>(result).c_str());
    } else {
        ESP_LOGI(TAG, "Playing music with ID: %d, result type: %zu", id, result.index());
    }
}

// 延迟销毁画布的参数结构
struct CanvasDelayParam {
    void* display;
    int delay_ms;
};

// Image display helper functions
bool McpServer::ShowRawImageFromSD(void* display_ptr, const std::string& image_path, int width, int height) {
    auto display = static_cast<LcdDisplay*>(display_ptr);
    if (!display) {
        ESP_LOGE(TAG, "Display is null");
        return false;
    }
    
    if (!display->HasCanvas()) {
        display->CreateCanvas();
    }

    std::string full_path = "/sdcard/" + image_path;
    FILE* fp = fopen(full_path.c_str(), "rb");
    if (!fp) {
        ESP_LOGE(TAG, "无法打开图片文件: %s", full_path.c_str());
        display->ShowNotification("图片不存在", 2000);
        return false;
    }

    size_t expected_size = width * height * 2;
    uint8_t* rgb565_data = (uint8_t*)heap_caps_malloc(expected_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb565_data) {
        ESP_LOGE(TAG, "PSRAM分配RGB565缓冲区失败");
        fclose(fp);
        display->ShowNotification("内存不足", 2000);
        return false;
    }
    
    size_t read_size = fread(rgb565_data, 1, expected_size, fp);
    fclose(fp);

    if (read_size != expected_size) {
        ESP_LOGE(TAG, "图片数据大小不符: 期望%zu字节, 实际%zu字节", expected_size, read_size);
        display->ShowNotification("图片尺寸错误", 2000);
        heap_caps_free(rgb565_data);
        return false;
    }

    display->DrawImageOnCanvas(0, 0, width, height, rgb565_data);
    ESP_LOGI(TAG, "raw RGB565图片显示成功: %s", full_path.c_str());
    
    heap_caps_free(rgb565_data);
    return true;
}

// 延迟销毁画布的任务函数
void McpServer::DestroyCanvasDelay(void* param) {
    auto* delay_param = static_cast<CanvasDelayParam*>(param);
    auto display = static_cast<LcdDisplay*>(delay_param->display);
    int delay_ms = delay_param->delay_ms;
    
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    
    if (display && display->HasCanvas()) {  
        display->DestroyCanvas();
        ESP_LOGI(TAG, "已关闭画布显示");
    }
    
    free(delay_param);
    vTaskDelete(NULL);
}

// 创建延迟销毁画布的任务
void McpServer::CreateDestroyCanvasTask(void* display_param, int delay_ms) {
    auto* param = (CanvasDelayParam*)malloc(sizeof(CanvasDelayParam));
    if (!param) {
        ESP_LOGE(TAG, "Failed to allocate memory for CanvasDelayParam");
        return;
    }
    
    param->display = display_param;
    param->delay_ms = delay_ms;
    
    xTaskCreate(
        DestroyCanvasDelay,      // 任务函数
        "DestroyCanvasTask",     // 任务名称
        4096,                    // 堆栈大小
        param,                   // 传递结构体指针      
        5,                       // 优先级
        NULL                     // 任务句柄
    );
}