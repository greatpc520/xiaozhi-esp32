#include "alarm_mcp_tools.h"
#include "alarm_manager.h"
#include "time_sync_manager.h"
#include "esp_log.h"
#include <cJSON.h>
#include <regex>
#include <cstring>

static const char* TAG = "AlarmMcpTools";

void AlarmMcpTools::RegisterTools(McpServer& server) {
    ESP_LOGI(TAG, "Registering alarm MCP tools");
    
    // 设置闹钟工具
    server.AddTool("alarm.set_alarm",
        "设置闹钟提醒。支持语音命令如：'设置2小时后提醒我吃药'、'明天8点提醒我开会'、'每天7点提醒我起床'等。",
        PropertyList({
            Property("command", kPropertyTypeString)
        }),
        SetAlarmTool
    );
    
    // 列出闹钟工具
    server.AddTool("alarm.list_alarms",
        "列出所有已设置的闹钟，包括启用和禁用的闹钟。",
        PropertyList(),
        ListAlarmsTool
    );
    
    // 取消闹钟工具
    server.AddTool("alarm.cancel_alarm",
        "取消指定ID的闹钟。如果ID为0或负数，则取消所有闹钟。",
        PropertyList({
            Property("alarm_id", kPropertyTypeInteger, 0)
        }),
        CancelAlarmTool
    );
    
    // 获取当前时间工具
    server.AddTool("alarm.get_current_time",
        "获取设备当前的日期和时间信息，用于设置闹钟时的时间参考。",
        PropertyList(),
        GetCurrentTimeTool
    );
    
    // 设置闹钟声音类型工具
    server.AddTool("alarm.set_alarm_sound",
        "设置指定闹钟的声音类型。sound_type: 0=本地音乐, 1=网络音乐(默认), 2=AI音乐。使用alarm_id=0可设置默认声音类型。",
        PropertyList({
            Property("alarm_id", kPropertyTypeInteger),
            Property("sound_type", kPropertyTypeInteger)
        }),
        SetAlarmSoundTool
    );
    
    // 获取声音类型列表工具
    server.AddTool("alarm.get_sound_types",
        "获取所有可用的闹钟声音类型列表。",
        PropertyList(),
        GetAlarmSoundTypesTool
    );
    
    // 获取默认声音类型工具
    server.AddTool("alarm.get_default_sound_type",
        "获取当前的默认闹钟声音类型。新创建的闹钟将使用此默认类型。",
        PropertyList(),
        GetDefaultSoundTypeTool
    );
    
    ESP_LOGI(TAG, "Registered 7 alarm MCP tools");
}

ReturnValue AlarmMcpTools::SetAlarmTool(const PropertyList& properties) {
    try {
        std::string command = properties["command"].value<std::string>();
        ESP_LOGI(TAG, "SetAlarmTool: Processing command: %s", command.c_str());
        
        auto& alarm_manager = AlarmManager::GetInstance();
        
        int hour, minute, minutes_later;
        std::string description;
        std::vector<int> weekdays;
        bool is_relative = false;
        bool is_tomorrow = false;
        
        // 解析语音命令
        if (alarm_manager.ParseVoiceCommand(command, hour, minute, minutes_later, description, is_relative, weekdays, is_tomorrow)) {
            int alarm_id;
            
            if (is_relative) {
                // 相对时间设置
                alarm_id = alarm_manager.SetRelativeAlarm(minutes_later, description);
            } else {
                // 绝对时间设置
                alarm_id = alarm_manager.SetAlarm(hour, minute, description, weekdays, is_tomorrow);
            }
            
            if (alarm_id > 0) {
                // 获取设置的闹钟信息
                auto alarms = alarm_manager.GetAllAlarms();
                for (const auto& alarm : alarms) {
                    if (alarm.id == alarm_id) {
                        cJSON* json = cJSON_CreateObject();
                        cJSON_AddBoolToObject(json, "success", true);
                        cJSON_AddStringToObject(json, "message", "闹钟设置成功");
                        cJSON_AddNumberToObject(json, "alarm_id", alarm.id);
                        cJSON_AddStringToObject(json, "description", alarm.description.c_str());
                        
                        char time_str[32];
                        snprintf(time_str, sizeof(time_str), "%02d:%02d", alarm.hour, alarm.minute);
                        cJSON_AddStringToObject(json, "time", time_str);
                        
                        // 添加weekdays信息
                        cJSON* weekdays_array = cJSON_CreateArray();
                        for (int weekday : alarm.weekdays) {
                            cJSON_AddItemToArray(weekdays_array, cJSON_CreateNumber(weekday));
                        }
                        cJSON_AddItemToObject(json, "weekdays", weekdays_array);
                        cJSON_AddBoolToObject(json, "is_daily", alarm.weekdays.size() > 1);
                        
                        // 添加声音类型信息
                        cJSON_AddNumberToObject(json, "sound_type", static_cast<int>(alarm.sound_type));
                        
                        char* json_str = cJSON_PrintUnformatted(json);
                        std::string result(json_str);
                        cJSON_free(json_str);
                        cJSON_Delete(json);
                        
                        ESP_LOGI(TAG, "Alarm set successfully: ID=%d, %s", alarm_id, time_str);
                        return result;
                    }
                }
            }
        }
        
        // 解析失败或设置失败
        ESP_LOGW(TAG, "Failed to parse or set alarm: %s", command.c_str());
        return "{\"success\": false, \"message\": \"无法解析闹钟命令或设置失败\"}";
        
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception in SetAlarmTool: %s", e.what());
        return "{\"success\": false, \"message\": \"设置闹钟时发生错误\"}";
    }
}

ReturnValue AlarmMcpTools::ListAlarmsTool(const PropertyList& properties) {
    try {
        auto& alarm_manager = AlarmManager::GetInstance();
        auto alarms = alarm_manager.GetAllAlarms();
        
        cJSON* json = cJSON_CreateObject();
        cJSON_AddBoolToObject(json, "success", true);
        cJSON_AddNumberToObject(json, "count", alarms.size());
        
        cJSON* alarms_array = cJSON_CreateArray();
        
        for (const auto& alarm : alarms) {
            cJSON* alarm_obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(alarm_obj, "id", alarm.id);
            cJSON_AddStringToObject(alarm_obj, "description", alarm.description.c_str());
            
            char time_str[32];
            snprintf(time_str, sizeof(time_str), "%02d:%02d", alarm.hour, alarm.minute);
            cJSON_AddStringToObject(alarm_obj, "time", time_str);
            
            cJSON_AddBoolToObject(alarm_obj, "enabled", alarm.enabled);
            
            // 添加weekdays信息
            cJSON* weekdays_array = cJSON_CreateArray();
            for (int weekday : alarm.weekdays) {
                cJSON_AddItemToArray(weekdays_array, cJSON_CreateNumber(weekday));
            }
            cJSON_AddItemToObject(alarm_obj, "weekdays", weekdays_array);
            cJSON_AddBoolToObject(alarm_obj, "is_daily", alarm.weekdays.size() > 1);
            
            // 添加声音类型信息
            cJSON_AddNumberToObject(alarm_obj, "sound_type", static_cast<int>(alarm.sound_type));
            
            cJSON_AddItemToArray(alarms_array, alarm_obj);
        }
        
        cJSON_AddItemToObject(json, "alarms", alarms_array);
        
        // 添加下一个闹钟信息
        AlarmInfo next_alarm = alarm_manager.GetNextAlarm();
        if (next_alarm.id > 0) {
            cJSON* next_alarm_obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(next_alarm_obj, "id", next_alarm.id);
            cJSON_AddStringToObject(next_alarm_obj, "description", next_alarm.description.c_str());
            
            char next_time_str[32];
            snprintf(next_time_str, sizeof(next_time_str), "%02d:%02d", next_alarm.hour, next_alarm.minute);
            cJSON_AddStringToObject(next_alarm_obj, "time", next_time_str);
            
            cJSON_AddItemToObject(json, "next_alarm", next_alarm_obj);
        }
        
        char* json_str = cJSON_PrintUnformatted(json);
        std::string result(json_str);
        cJSON_free(json_str);
        cJSON_Delete(json);
        
        ESP_LOGI(TAG, "Listed %zu alarms", alarms.size());
        return result;
        
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception in ListAlarmsTool: %s", e.what());
        return "{\"success\": false, \"message\": \"获取闹钟列表时发生错误\"}";
    }
}

ReturnValue AlarmMcpTools::CancelAlarmTool(const PropertyList& properties) {
    try {
        int alarm_id = properties["alarm_id"].value<int>();
        auto& alarm_manager = AlarmManager::GetInstance();
        
        if (alarm_id <= 0) {
            // 取消所有闹钟
            auto alarms = alarm_manager.GetAllAlarms();
            int count = alarms.size();
            alarm_manager.CancelAllAlarms();
            
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "success", true);
            cJSON_AddStringToObject(json, "message", "已取消所有闹钟");
            cJSON_AddNumberToObject(json, "cancelled_count", count);
            
            char* json_str = cJSON_PrintUnformatted(json);
            std::string result(json_str);
            cJSON_free(json_str);
            cJSON_Delete(json);
            
            ESP_LOGI(TAG, "Cancelled all alarms (%d total)", count);
            return result;
        } else {
            // 取消指定ID的闹钟
            bool success = alarm_manager.CancelAlarm(alarm_id);
            
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "success", success);
            cJSON_AddNumberToObject(json, "alarm_id", alarm_id);
            
            if (success) {
                cJSON_AddStringToObject(json, "message", "闹钟已取消");
                ESP_LOGI(TAG, "Cancelled alarm ID=%d", alarm_id);
            } else {
                cJSON_AddStringToObject(json, "message", "未找到指定的闹钟");
                ESP_LOGW(TAG, "Alarm not found: ID=%d", alarm_id);
            }
            
            char* json_str = cJSON_PrintUnformatted(json);
            std::string result(json_str);
            cJSON_free(json_str);
            cJSON_Delete(json);
            
            return result;
        }
        
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception in CancelAlarmTool: %s", e.what());
        return "{\"success\": false, \"message\": \"取消闹钟时发生错误\"}";
    }
}

ReturnValue AlarmMcpTools::GetCurrentTimeTool(const PropertyList& properties) {
    try {
        // 优先使用RTC时间，确保时间一致性
        auto& time_sync_manager = TimeSyncManager::GetInstance();
        auto* rtc = time_sync_manager.GetRtc();
        
        struct tm timeinfo_direct; // 直接获取tm结构体
        time_t current_time_for_timestamp;
        bool is_rtc_source = false;
        
        // 统一的时间获取方式，确保与其他部分一致
        if (rtc && rtc->GetTime(&timeinfo_direct)) {
            // 使用RTC时间 - 但不使用有问题的GetTime(time_t*)方法
            // 直接使用tm结构体，时间戳通过系统时间获取以保证一致性
            is_rtc_source = true;
            ESP_LOGD(TAG, "Using RTC time for MCP response (tm struct)");
            
            // 获取系统时间作为时间戳（保持一致性）
            time(&current_time_for_timestamp);
        } else {
            // 回退到系统时间
            time(&current_time_for_timestamp);
            struct tm* temp_timeinfo = localtime(&current_time_for_timestamp);
            if (temp_timeinfo) {
                timeinfo_direct = *temp_timeinfo;  // 使用赋值操作符替代memcpy
            }
            is_rtc_source = false;
            ESP_LOGD(TAG, "Using system time for MCP response (RTC unavailable)");
        }
        
        // 格式化时间字符串 - 使用获取到的tm结构体
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo_direct);
        
        cJSON* json = cJSON_CreateObject();
        cJSON_AddBoolToObject(json, "success", true);
        cJSON_AddStringToObject(json, "current_time", time_str);
        cJSON_AddNumberToObject(json, "year", timeinfo_direct.tm_year + 1900);
        cJSON_AddNumberToObject(json, "month", timeinfo_direct.tm_mon + 1);
        cJSON_AddNumberToObject(json, "day", timeinfo_direct.tm_mday);
        cJSON_AddNumberToObject(json, "hour", timeinfo_direct.tm_hour);
        cJSON_AddNumberToObject(json, "minute", timeinfo_direct.tm_min);
        cJSON_AddNumberToObject(json, "second", timeinfo_direct.tm_sec);
        cJSON_AddNumberToObject(json, "weekday", timeinfo_direct.tm_wday);
        
        // 添加星期几的中文名称
        const char* weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
        cJSON_AddStringToObject(json, "weekday_name", weekdays[timeinfo_direct.tm_wday]);
        
        // 添加时间戳 - 使用系统时间戳保持一致性
        cJSON_AddNumberToObject(json, "timestamp", current_time_for_timestamp);
        
        // 添加时间源信息用于调试
        cJSON_AddStringToObject(json, "time_source", is_rtc_source ? "RTC" : "System");
        
        // 添加时区信息，便于调试
        cJSON_AddStringToObject(json, "timezone", "CST-8");
        
        // 为了调试，同时提供本地时间的时间戳
        time_t local_timestamp = mktime(&timeinfo_direct);
        cJSON_AddNumberToObject(json, "local_timestamp", local_timestamp);
        
        char* json_str = cJSON_PrintUnformatted(json);
        std::string result(json_str);
        cJSON_free(json_str);
        cJSON_Delete(json);
        
        ESP_LOGI(TAG, "GetCurrentTime: %s (source: %s, system timestamp: %lld, local timestamp: %lld)", 
                 time_str, is_rtc_source ? "RTC" : "System", 
                 (long long)current_time_for_timestamp, (long long)local_timestamp);
        return result;
        
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception in GetCurrentTimeTool: %s", e.what());
        return "{\"success\": false, \"message\": \"获取当前时间时发生错误\"}";
    }
}

ReturnValue AlarmMcpTools::SetAlarmSoundTool(const PropertyList& properties) {
    try {
        int alarm_id = properties["alarm_id"].value<int>();
        int sound_type_int = properties["sound_type"].value<int>();
        
        auto& alarm_manager = AlarmManager::GetInstance();
        
        // 验证声音类型
        if (sound_type_int < 0 || sound_type_int > 2) {
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "success", false);
            cJSON_AddStringToObject(json, "message", "Invalid sound_type. Must be 0 (local), 1 (network), or 2 (AI)");
            
            char* json_str = cJSON_PrintUnformatted(json);
            std::string result(json_str);
            cJSON_free(json_str);
            cJSON_Delete(json);
            return result;
        }
        
        AlarmSoundType sound_type = static_cast<AlarmSoundType>(sound_type_int);
        
        // 获取所有闹钟
        auto alarms = alarm_manager.GetAllAlarms();
        bool found = false;
        
        // 特殊处理：如果alarm_id为0或负数，设置默认声音类型
        if (alarm_id <= 0) {
            alarm_manager.SetDefaultSoundType(sound_type);
            
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "success", true);
            cJSON_AddStringToObject(json, "message", "默认闹钟声音类型设置成功");
            cJSON_AddNumberToObject(json, "sound_type", sound_type_int);
            
            const char* sound_names[] = {"本地音乐", "网络音乐", "AI音乐"};
            cJSON_AddStringToObject(json, "sound_name", sound_names[sound_type_int]);
            cJSON_AddStringToObject(json, "note", "此设置将应用于新创建的闹钟");
            
            char* json_str = cJSON_PrintUnformatted(json);
            std::string result(json_str);
            cJSON_free(json_str);
            cJSON_Delete(json);
            
            ESP_LOGI(TAG, "Set default sound type to %d", sound_type_int);
            return result;
        }
        
        // 查找指定ID的闹钟
        for (auto& alarm : alarms) {
            if (alarm.id == alarm_id) {
                alarm.sound_type = sound_type;
                found = true;
                break;
            }
        }
        
        if (found) {
            // 保存修改到NVS
            alarm_manager.SaveAlarmsToNVS();
            
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "success", true);
            cJSON_AddStringToObject(json, "message", "闹钟声音类型设置成功");
            cJSON_AddNumberToObject(json, "alarm_id", alarm_id);
            cJSON_AddNumberToObject(json, "sound_type", sound_type_int);
            
            const char* sound_names[] = {"本地音乐", "网络音乐", "AI音乐"};
            cJSON_AddStringToObject(json, "sound_name", sound_names[sound_type_int]);
            
            char* json_str = cJSON_PrintUnformatted(json);
            std::string result(json_str);
            cJSON_free(json_str);
            cJSON_Delete(json);
            
            ESP_LOGI(TAG, "Set alarm %d sound type to %d", alarm_id, sound_type_int);
            return result;
        } else {
            // 闹钟不存在，检查是否有任何闹钟
            if (alarms.empty()) {
                // 没有闹钟时，自动设置默认声音类型
                alarm_manager.SetDefaultSoundType(sound_type);
                
                cJSON* json = cJSON_CreateObject();
                cJSON_AddBoolToObject(json, "success", true);
                cJSON_AddStringToObject(json, "message", "未找到指定闹钟，已设置为默认声音类型");
                cJSON_AddNumberToObject(json, "alarm_id", alarm_id);
                cJSON_AddNumberToObject(json, "sound_type", sound_type_int);
                
                const char* sound_names[] = {"本地音乐", "网络音乐", "AI音乐"};
                cJSON_AddStringToObject(json, "sound_name", sound_names[sound_type_int]);
                cJSON_AddStringToObject(json, "note", "此设置将应用于新创建的闹钟");
                
                char* json_str = cJSON_PrintUnformatted(json);
                std::string result(json_str);
                cJSON_free(json_str);
                cJSON_Delete(json);
                
                ESP_LOGI(TAG, "No alarms found, set default sound type to %d", sound_type_int);
                return result;
            } else {
                cJSON* json = cJSON_CreateObject();
                cJSON_AddBoolToObject(json, "success", false);
                cJSON_AddStringToObject(json, "message", "未找到指定ID的闹钟");
                cJSON_AddNumberToObject(json, "alarm_id", alarm_id);
                cJSON_AddStringToObject(json, "suggestion", "使用alarm_id=0设置默认声音类型，或使用alarm.list_alarms查看现有闹钟");
                
                char* json_str = cJSON_PrintUnformatted(json);
                std::string result(json_str);
                cJSON_free(json_str);
                cJSON_Delete(json);
                
                ESP_LOGW(TAG, "Alarm not found: ID=%d", alarm_id);
                return result;
            }
        }
        
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception in SetAlarmSoundTool: %s", e.what());
        return "{\"success\": false, \"message\": \"设置闹钟声音类型时发生错误\"}";
    }
    
    // 不应该到达这里，但为了安全起见添加默认返回值
    return "{\"success\": false, \"message\": \"未知错误\"}";
}

ReturnValue AlarmMcpTools::GetAlarmSoundTypesTool(const PropertyList& properties) {
    try {
        cJSON* json = cJSON_CreateObject();
        cJSON_AddBoolToObject(json, "success", true);
        
        cJSON* types_array = cJSON_CreateArray();
        
        // 本地音乐
        cJSON* local_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(local_obj, "type", 0);
        cJSON_AddStringToObject(local_obj, "name", "本地音乐");
        cJSON_AddStringToObject(local_obj, "description", "播放存储在/sdcard/test3.p3的本地音乐文件");
        cJSON_AddStringToObject(local_obj, "format", "opus");
        cJSON_AddItemToArray(types_array, local_obj);
        
        // 网络音乐
        cJSON* network_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(network_obj, "type", 1);
        cJSON_AddStringToObject(network_obj, "name", "网络音乐");
        cJSON_AddStringToObject(network_obj, "description", "播放网络音乐http://www.replime.cn/a1.p3");
        cJSON_AddStringToObject(network_obj, "format", "opus");
        cJSON_AddBoolToObject(network_obj, "is_default", true);
        cJSON_AddItemToArray(types_array, network_obj);
        
        // AI音乐
        cJSON* ai_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(ai_obj, "type", 2);
        cJSON_AddStringToObject(ai_obj, "name", "AI音乐");
        cJSON_AddStringToObject(ai_obj, "description", "使用AI播放随机音乐");
        cJSON_AddStringToObject(ai_obj, "command", "随便播放一首歌曲");
        cJSON_AddItemToArray(types_array, ai_obj);
        
        cJSON_AddItemToObject(json, "sound_types", types_array);
        
        char* json_str = cJSON_PrintUnformatted(json);
        std::string result(json_str);
        cJSON_free(json_str);
        cJSON_Delete(json);
        
        ESP_LOGI(TAG, "Returned available sound types");
        return result;
        
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception in GetAlarmSoundTypesTool: %s", e.what());
        return "{\"success\": false, \"message\": \"获取声音类型列表时发生错误\"}";
    }
}

ReturnValue AlarmMcpTools::GetDefaultSoundTypeTool(const PropertyList& properties) {
    try {
        auto& alarm_manager = AlarmManager::GetInstance();
        AlarmSoundType default_type = alarm_manager.GetDefaultSoundType();
        int sound_type_int = static_cast<int>(default_type);
        
        cJSON* json = cJSON_CreateObject();
        cJSON_AddBoolToObject(json, "success", true);
        cJSON_AddStringToObject(json, "message", "获取默认声音类型成功");
        cJSON_AddNumberToObject(json, "sound_type", sound_type_int);
        
        const char* sound_names[] = {"本地音乐", "网络音乐", "AI音乐"};
        const char* sound_descriptions[] = {
            "本地音乐：播放SD卡中的.P3音频文件", 
            "网络音乐：下载并播放指定的网络音乐", 
            "AI音乐：发送语音指令给AI播放音乐"
        };
        
        cJSON_AddStringToObject(json, "sound_name", sound_names[sound_type_int]);
        cJSON_AddStringToObject(json, "description", sound_descriptions[sound_type_int]);
        cJSON_AddStringToObject(json, "note", "新创建的闹钟将使用此默认声音类型");
        
        char* json_str = cJSON_PrintUnformatted(json);
        std::string result(json_str);
        cJSON_free(json_str);
        cJSON_Delete(json);
        
        ESP_LOGI(TAG, "Current default sound type: %d", sound_type_int);
        return result;
        
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception in GetDefaultSoundTypeTool: %s", e.what());
        return "{\"success\": false, \"message\": \"获取默认声音类型时发生错误\"}";
    }
}

std::string AlarmMcpTools::FormatAlarmInfoJson(const AlarmInfo& alarm) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "id", alarm.id);
    cJSON_AddStringToObject(json, "description", alarm.description.c_str());
    cJSON_AddNumberToObject(json, "hour", alarm.hour);
    cJSON_AddNumberToObject(json, "minute", alarm.minute);
    cJSON_AddBoolToObject(json, "enabled", alarm.enabled);
    
    // 添加weekdays信息
    cJSON* weekdays_array = cJSON_CreateArray();
    for (int weekday : alarm.weekdays) {
        cJSON_AddItemToArray(weekdays_array, cJSON_CreateNumber(weekday));
    }
    cJSON_AddItemToObject(json, "weekdays", weekdays_array);
    cJSON_AddBoolToObject(json, "is_daily", alarm.weekdays.size() > 1);
    
    // 添加声音类型信息
    cJSON_AddNumberToObject(json, "sound_type", static_cast<int>(alarm.sound_type));
    
    char* json_str = cJSON_PrintUnformatted(json);
    std::string result(json_str);
    cJSON_free(json_str);
    cJSON_Delete(json);
    
    return result;
} 