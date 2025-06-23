#include "alarm_mcp_tools.h"
#include <esp_log.h>
#include <cJSON.h>
#include <sstream>

#define TAG "AlarmMcpTools"

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
    
    ESP_LOGI(TAG, "Alarm MCP tools registered successfully");
}

ReturnValue AlarmMcpTools::SetAlarmTool(const PropertyList& properties) {
    try {
        std::string command = properties["command"].value<std::string>();
        
        auto& alarm_manager = AlarmManager::GetInstance();
        int alarm_id = alarm_manager.SetAlarmFromVoice(command);
        
        if (alarm_id > 0) {
            // 获取设置的闹钟信息
            auto alarms = alarm_manager.GetAlarms();
            for (const auto& alarm : alarms) {
                if (alarm.id == alarm_id) {
                    cJSON* json = cJSON_CreateObject();
                    cJSON_AddBoolToObject(json, "success", true);
                    cJSON_AddNumberToObject(json, "alarm_id", alarm_id);
                    cJSON_AddStringToObject(json, "message", "闹钟设置成功");
                    cJSON_AddStringToObject(json, "description", alarm.description.c_str());
                    
                    char time_str[32];
                    snprintf(time_str, sizeof(time_str), "%02d:%02d", alarm.hour, alarm.minute);
                    cJSON_AddStringToObject(json, "time", time_str);
                    cJSON_AddBoolToObject(json, "repeat_daily", alarm.repeat_daily);
                    
                    char* json_str = cJSON_PrintUnformatted(json);
                    std::string result(json_str);
                    cJSON_free(json_str);
                    cJSON_Delete(json);
                    
                    ESP_LOGI(TAG, "Alarm set successfully: ID=%d, %s", alarm_id, command.c_str());
                    return result;
                }
            }
        }
        
        // 设置失败
        cJSON* json = cJSON_CreateObject();
        cJSON_AddBoolToObject(json, "success", false);
        cJSON_AddStringToObject(json, "message", "无法解析闹钟命令，请检查时间格式");
        cJSON_AddStringToObject(json, "command", command.c_str());
        
        char* json_str = cJSON_PrintUnformatted(json);
        std::string result(json_str);
        cJSON_free(json_str);
        cJSON_Delete(json);
        
        ESP_LOGW(TAG, "Failed to set alarm: %s", command.c_str());
        return result;
        
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception in SetAlarmTool: %s", e.what());
        return "{\"success\": false, \"message\": \"设置闹钟时发生错误\"}";
    }
}

ReturnValue AlarmMcpTools::ListAlarmsTool(const PropertyList& properties) {
    try {
        auto& alarm_manager = AlarmManager::GetInstance();
        auto alarms = alarm_manager.GetAlarms();
        
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
            cJSON_AddBoolToObject(alarm_obj, "repeat_daily", alarm.repeat_daily);
            
            // 添加触发时间信息
            if (!alarm.repeat_daily) {
                char trigger_time_str[64];
                struct tm* tm_info = localtime(&alarm.trigger_time);
                strftime(trigger_time_str, sizeof(trigger_time_str), "%Y-%m-%d %H:%M:%S", tm_info);
                cJSON_AddStringToObject(alarm_obj, "trigger_time", trigger_time_str);
            }
            
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
            auto alarms = alarm_manager.GetAlarms();
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
        auto& alarm_manager = AlarmManager::GetInstance();
        std::string current_time = alarm_manager.GetCurrentTimeString();
        
        // 获取详细时间信息
        time_t now;
        time(&now);
        struct tm* timeinfo = localtime(&now);
        
        cJSON* json = cJSON_CreateObject();
        cJSON_AddBoolToObject(json, "success", true);
        cJSON_AddStringToObject(json, "current_time", current_time.c_str());
        cJSON_AddNumberToObject(json, "year", timeinfo->tm_year + 1900);
        cJSON_AddNumberToObject(json, "month", timeinfo->tm_mon + 1);
        cJSON_AddNumberToObject(json, "day", timeinfo->tm_mday);
        cJSON_AddNumberToObject(json, "hour", timeinfo->tm_hour);
        cJSON_AddNumberToObject(json, "minute", timeinfo->tm_min);
        cJSON_AddNumberToObject(json, "second", timeinfo->tm_sec);
        cJSON_AddNumberToObject(json, "weekday", timeinfo->tm_wday);
        
        // 添加星期几的中文名称
        const char* weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
        cJSON_AddStringToObject(json, "weekday_name", weekdays[timeinfo->tm_wday]);
        
        // 添加时间戳
        cJSON_AddNumberToObject(json, "timestamp", now);
        
        char* json_str = cJSON_PrintUnformatted(json);
        std::string result(json_str);
        cJSON_free(json_str);
        cJSON_Delete(json);
        
        return result;
        
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception in GetCurrentTimeTool: %s", e.what());
        return "{\"success\": false, \"message\": \"获取当前时间时发生错误\"}";
    }
}

std::string AlarmMcpTools::FormatAlarmInfoJson(const AlarmInfo& alarm) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "id", alarm.id);
    cJSON_AddStringToObject(json, "description", alarm.description.c_str());
    cJSON_AddNumberToObject(json, "hour", alarm.hour);
    cJSON_AddNumberToObject(json, "minute", alarm.minute);
    cJSON_AddBoolToObject(json, "enabled", alarm.enabled);
    cJSON_AddBoolToObject(json, "repeat_daily", alarm.repeat_daily);
    cJSON_AddNumberToObject(json, "trigger_time", alarm.trigger_time);
    
    char* json_str = cJSON_PrintUnformatted(json);
    std::string result(json_str);
    cJSON_free(json_str);
    cJSON_Delete(json);
    
    return result;
}

std::string AlarmMcpTools::FormatAlarmListJson(const std::vector<AlarmInfo>& alarms) {
    cJSON* json = cJSON_CreateArray();
    
    for (const auto& alarm : alarms) {
        cJSON* alarm_json = cJSON_Parse(FormatAlarmInfoJson(alarm).c_str());
        cJSON_AddItemToArray(json, alarm_json);
    }
    
    char* json_str = cJSON_PrintUnformatted(json);
    std::string result(json_str);
    cJSON_free(json_str);
    cJSON_Delete(json);
    
    return result;
} 