#include "alarm_manager.h"
#include "time_sync_manager.h"
#include <cstring>
#include <sstream>
#include <iomanip>
#include <regex>
#include <algorithm>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <freertos/semphr.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <ctime>
#include <cJSON.h>

static const char* TAG = "AlarmManager";

// 静态成员变量定义
int AlarmManager::next_alarm_id_ = 1;

#define NVS_NAMESPACE "alarms"
#define MAX_ALARMS 20
#define CHECK_INTERVAL_MS 10000   // 10秒检查一次，配合秒数容错确保闹钟触发

AlarmManager::AlarmManager() : check_timer_(nullptr), rtc_(nullptr), default_sound_type_(AlarmSoundType::NETWORK_MUSIC) {
    // 创建互斥锁
    alarms_mutex_ = xSemaphoreCreateMutex();
    if (alarms_mutex_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create alarms mutex");
    }
}

AlarmManager::~AlarmManager() {
    if (check_timer_ != nullptr) {
        xTimerDelete(check_timer_, portMAX_DELAY);
    }
    SaveAlarmsToNVS();
    
    // 销毁互斥锁
    if (alarms_mutex_ != nullptr) {
        vSemaphoreDelete(alarms_mutex_);
    }
}

bool AlarmManager::Initialize() {
    ESP_LOGI(TAG, "Initializing AlarmManager");
    
    // 创建定时检查定时器
    check_timer_ = xTimerCreate(
        "alarm_check",
        pdMS_TO_TICKS(CHECK_INTERVAL_MS),
        pdTRUE,  // 自动重载
        this,
        CheckTimerCallback
    );
    
    if (check_timer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create check timer");
        return false;
    }
    
    // 从NVS加载闹钟数据
    LoadAlarmsFromNVS();
    
    // 启动定时器
    if (xTimerStart(check_timer_, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start check timer");
        return false;
    }
    
    ESP_LOGI(TAG, "AlarmManager initialized successfully");
    return true;
}

int AlarmManager::CreateAlarmFromVoice(const std::string& voice_command) {
    int hour, minute, minutes_later;
    std::string description;
    bool is_relative;
    std::vector<int> weekdays;
    bool is_tomorrow = false;
    
    if (ParseVoiceCommand(voice_command, hour, minute, minutes_later, description, is_relative, weekdays, is_tomorrow)) {
        if (is_relative) {
            return SetRelativeAlarm(minutes_later, description);
        } else {
            return SetAlarm(hour, minute, description, weekdays, is_tomorrow);
        }
    }
    
    return -1; // 解析失败
}

int AlarmManager::SetAlarm(int hour, int minute, const std::string& description, const std::vector<int>& weekdays, bool is_tomorrow) {
    if (!IsValidTime(hour, minute)) {
        ESP_LOGE(TAG, "Invalid time: %02d:%02d", hour, minute);
        return -1;
    }
    
    AlarmInfo alarm;
    alarm.id = GenerateAlarmId();
    alarm.hour = hour;
    alarm.minute = minute;
    alarm.description = description.empty() ? "闹钟提醒" : description;
    alarm.weekdays = weekdays;
    alarm.enabled = true;
    alarm.sound_type = default_sound_type_;
    
    // 如果weekdays不为空，说明是重复闹钟
    if (!weekdays.empty()) {
        ESP_LOGI(TAG, "Setting recurring alarm: %02d:%02d, weekdays size: %d", hour, minute, weekdays.size());
    } else {
        ESP_LOGI(TAG, "Setting one-time alarm: %02d:%02d %s", hour, minute, is_tomorrow ? "(tomorrow)" : "(today)");
    }
    
    // 获取互斥锁保护闹钟列表
    if (xSemaphoreTake(alarms_mutex_, pdMS_TO_TICKS(1000)) == pdTRUE) {
        alarms_.push_back(alarm);
        SaveAlarmsToNVS();
        xSemaphoreGive(alarms_mutex_);
        
        ESP_LOGI(TAG, "Alarm set successfully. ID: %d, Time: %02d:%02d, Description: %s", 
                 alarm.id, hour, minute, alarm.description.c_str());
        
        return alarm.id;
    } else {
        ESP_LOGE(TAG, "Failed to acquire mutex in SetAlarm");
        return -1;
    }
}

int AlarmManager::SetRelativeAlarm(int minutes_later, const std::string& description) {
    if (minutes_later <= 0) {
        ESP_LOGE(TAG, "Invalid minutes_later: %d", minutes_later);
        return -1;
    }
    
    // 获取当前时间
    time_t now;
    if (rtc_ && rtc_->GetTime(&now)) {
        // 使用RTC时间
    } else {
        time(&now);
    }
    
    // 计算触发时间
    time_t trigger_time = now + (minutes_later * 60);
    struct tm* trigger_tm = localtime(&trigger_time);
    
    AlarmInfo alarm;
    alarm.id = GenerateAlarmId();
    alarm.hour = trigger_tm->tm_hour;
    alarm.minute = trigger_tm->tm_min;
    alarm.description = description.empty() ? "闹钟提醒" : description;
    alarm.weekdays = {}; // 相对时间闹钟不重复
    alarm.enabled = true;
    alarm.sound_type = default_sound_type_;
    
    // 获取互斥锁保护闹钟列表
    if (xSemaphoreTake(alarms_mutex_, pdMS_TO_TICKS(1000)) == pdTRUE) {
        alarms_.push_back(alarm);
        SaveAlarmsToNVS();
        xSemaphoreGive(alarms_mutex_);
        
        ESP_LOGI(TAG, "Relative alarm set: %d minutes later at %02d:%02d - %s (ID: %d)", 
                 minutes_later, alarm.hour, alarm.minute, alarm.description.c_str(), alarm.id);
        
        return alarm.id;
    } else {
        ESP_LOGE(TAG, "Failed to acquire mutex in SetRelativeAlarm");
        return -1;
    }
}

std::vector<AlarmInfo> AlarmManager::GetAllAlarms() {
    return alarms_;
}

bool AlarmManager::CancelAlarm(int alarm_id) {
    auto it = std::find_if(alarms_.begin(), alarms_.end(),
        [alarm_id](const AlarmInfo& alarm) {
            return alarm.id == alarm_id;
        });
    
    if (it != alarms_.end()) {
        ESP_LOGI(TAG, "Canceling alarm ID: %d", alarm_id);
        alarms_.erase(it);
        SaveAlarmsToNVS();
        return true;
    }
    
    ESP_LOGW(TAG, "Alarm ID %d not found", alarm_id);
    return false;
}

void AlarmManager::CancelAllAlarms() {
    ESP_LOGI(TAG, "Canceling all alarms");
    alarms_.clear();
    SaveAlarmsToNVS();
}

AlarmInfo AlarmManager::GetNextAlarm() {
    // 获取互斥锁
    if (xSemaphoreTake(alarms_mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex in GetNextAlarm");
        return AlarmInfo{}; // 返回空的AlarmInfo
    }
    
    AlarmInfo next_alarm;
    time_t current_time;
    
    if (rtc_ && rtc_->GetTime(&current_time)) {
        // 使用RTC时间
    } else {
        time(&current_time);
    }
    
    struct tm current_tm;
    localtime_r(&current_time, &current_tm);
    
    ESP_LOGI(TAG, "GetNextAlarm: Current time: %02d:%02d, checking %zu alarms", 
             current_tm.tm_hour, current_tm.tm_min, alarms_.size());
    
    time_t closest_time = 0;
    
    for (const auto& alarm : alarms_) {
        if (!alarm.enabled) continue;
        
        // 计算下一次触发时间
        time_t next_trigger_time = 0;
        
        if (alarm.weekdays.empty()) {
            // 一次性闹钟：检查今天是否还未到时间
            struct tm alarm_tm = current_tm;
            alarm_tm.tm_hour = alarm.hour;
            alarm_tm.tm_min = alarm.minute;
            alarm_tm.tm_sec = 0;
            
            time_t today_alarm_time = mktime(&alarm_tm);
            if (today_alarm_time > current_time) {
                next_trigger_time = today_alarm_time;
            }
        } else {
            // 重复闹钟：检查每个星期几
            for (int weekday : alarm.weekdays) {
                struct tm alarm_tm = current_tm;
                alarm_tm.tm_hour = alarm.hour;
                alarm_tm.tm_min = alarm.minute;
                alarm_tm.tm_sec = 0;
                
                // 计算到目标星期几的天数差
                int days_diff = (weekday - current_tm.tm_wday + 7) % 7;
                
                // 如果是今天但时间已过，设置为下周同一天
                if (days_diff == 0) {
                    time_t today_alarm_time = mktime(&alarm_tm);
                    if (today_alarm_time <= current_time) {
                        days_diff = 7; // 下周同一天
                    }
                }
                
                alarm_tm.tm_mday += days_diff;
                time_t trigger_time = mktime(&alarm_tm);
                
                // 找到最近的触发时间
                if (next_trigger_time == 0 || trigger_time < next_trigger_time) {
                    next_trigger_time = trigger_time;
                }
            }
        }
        
        // 比较所有闹钟，找到最早的
        if (next_trigger_time > 0 && (closest_time == 0 || next_trigger_time < closest_time)) {
            next_alarm = alarm;
            closest_time = next_trigger_time;
            ESP_LOGI(TAG, "GetNextAlarm: Found candidate alarm ID=%d, Time=%02d:%02d, Description='%s'", 
                     alarm.id, alarm.hour, alarm.minute, alarm.description.c_str());
        }
    }
    
    if (next_alarm.id > 0) {
        ESP_LOGI(TAG, "GetNextAlarm: Returning alarm ID=%d, Time=%02d:%02d", 
                 next_alarm.id, next_alarm.hour, next_alarm.minute);
    } else {
        ESP_LOGI(TAG, "GetNextAlarm: No valid next alarm found");
    }
    
    // 释放互斥锁
    xSemaphoreGive(alarms_mutex_);
    return next_alarm;
}

std::string AlarmManager::GetCurrentTimeString() const {
    if (rtc_) {
        return rtc_->GetTimeString();
    }
    
    time_t now;
    time(&now);
    struct tm* timeinfo = localtime(&now);
    
    std::ostringstream oss;
    oss << std::setfill('0') 
        << std::setw(4) << (timeinfo->tm_year + 1900) << "-"
        << std::setw(2) << (timeinfo->tm_mon + 1) << "-"
        << std::setw(2) << timeinfo->tm_mday << " "
        << std::setw(2) << timeinfo->tm_hour << ":"
        << std::setw(2) << timeinfo->tm_min << ":"
        << std::setw(2) << timeinfo->tm_sec;
    
    return oss.str();
}

void AlarmManager::SetAlarmCallback(std::function<void(const AlarmInfo&)> callback) {
    alarm_callback_ = callback;
}

void AlarmManager::CheckAlarms() {
    struct tm current_tm;
    bool time_valid = false;
    
    // 防重复触发：记录上一次触发的时间和闹钟ID
    static int last_triggered_hour = -1;
    static int last_triggered_minute = -1;
    static std::vector<int> last_triggered_ids;
    
    // 使用TimeSyncManager的统一时间获取函数
    auto& time_sync_manager = TimeSyncManager::GetInstance();
    time_valid = time_sync_manager.GetUnifiedTime(&current_tm);
    
    if (!time_valid) {
        ESP_LOGE(TAG, "Failed to get unified time for alarm check");
        return;
    }
    
    // 如果时间改变了，清空上次触发的闹钟列表
    if (last_triggered_hour != current_tm.tm_hour || last_triggered_minute != current_tm.tm_min) {
        last_triggered_hour = current_tm.tm_hour;
        last_triggered_minute = current_tm.tm_min;
        last_triggered_ids.clear();
        ESP_LOGD(TAG, "Time changed to %02d:%02d, cleared trigger history", 
                 current_tm.tm_hour, current_tm.tm_min);
    }
    
    // 每次检查都打印当前时间和闹钟状态，便于调试
    ESP_LOGI(TAG, "CheckAlarms: Current time: %02d:%02d:%02d, weekday: %d, total alarms: %zu", 
             current_tm.tm_hour, current_tm.tm_min, current_tm.tm_sec, current_tm.tm_wday, alarms_.size());
    
    // 打印每个闹钟的详细信息
    for (size_t i = 0; i < alarms_.size(); i++) {
        const auto& alarm = alarms_[i];
        ESP_LOGI(TAG, "Alarm[%zu]: ID=%d, Time=%02d:%02d, Enabled=%s, Weekdays=%s, Desc='%s'", 
                 i, alarm.id, alarm.hour, alarm.minute, 
                 alarm.enabled ? "YES" : "NO",
                 alarm.weekdays.empty() ? "Once" : "Repeat",
                 alarm.description.c_str());
    }
    
    // 在定时器任务中只检查触发条件，立即标记已触发避免重复
    std::vector<int> triggered_alarm_ids;
    
    for (auto& alarm : alarms_) {
        if (!alarm.enabled) {
            ESP_LOGD(TAG, "Alarm ID=%d is disabled, skipping", alarm.id);
            continue;
        }
        
        // 详细的时间匹配检查
        ESP_LOGD(TAG, "Checking alarm ID=%d: current=%02d:%02d:%02d vs alarm=%02d:%02d", 
                 alarm.id, current_tm.tm_hour, current_tm.tm_min, current_tm.tm_sec, alarm.hour, alarm.minute);
        
        // 检查当前时间是否匹配闹钟时间（增加容错机制）
        // 只要在闹钟时间的同一分钟内就认为匹配，避免因为检查间隔错过闹钟
        if (current_tm.tm_hour != alarm.hour || current_tm.tm_min != alarm.minute) {
            ESP_LOGV(TAG, "Alarm ID=%d time mismatch: %02d:%02d != %02d:%02d", 
                     alarm.id, current_tm.tm_hour, current_tm.tm_min, alarm.hour, alarm.minute);
            continue;
        }
        
        ESP_LOGI(TAG, "Alarm ID=%d time matched! Now checking weekday...", alarm.id);
        
        // 防重复触发检查：如果这个闹钟在当前分钟已经触发过，跳过
        if (std::find(last_triggered_ids.begin(), last_triggered_ids.end(), alarm.id) != last_triggered_ids.end()) {
            ESP_LOGW(TAG, "Alarm ID=%d already triggered this minute, skipping", alarm.id);
            continue;
        }
        
        // 检查星期几匹配：如果weekdays为空（一次性闹钟），则总是匹配
        bool weekday_matches = false;
        if (alarm.weekdays.empty()) {
            // 一次性闹钟，总是匹配
            weekday_matches = true;
            ESP_LOGI(TAG, "Alarm ID=%d is one-time alarm, weekday matches", alarm.id);
        } else {
            // 重复闹钟，检查当前星期几是否在列表中
            ESP_LOGI(TAG, "Alarm ID=%d is recurring, checking weekdays. Current weekday: %d", 
                     alarm.id, current_tm.tm_wday);
            for (int weekday : alarm.weekdays) {
                ESP_LOGD(TAG, "Checking weekday %d", weekday);
                if (current_tm.tm_wday == weekday) {
                    weekday_matches = true;
                    ESP_LOGI(TAG, "Alarm ID=%d weekday matches! Current: %d, Target: %d", 
                             alarm.id, current_tm.tm_wday, weekday);
                    break;
                }
            }
            if (!weekday_matches) {
                ESP_LOGI(TAG, "Alarm ID=%d weekday does not match. Current: %d", 
                         alarm.id, current_tm.tm_wday);
            }
        }
        
        if (!weekday_matches) continue;
        
        // 闹钟触发
        ESP_LOGI(TAG, "🔔 ALARM TRIGGERED! ID=%d, Time=%02d:%02d, Description='%s', Type=%s", 
                 alarm.id, alarm.hour, alarm.minute, alarm.description.c_str(),
                 alarm.weekdays.empty() ? "One-time" : "Recurring");
        
        // 添加到防重复触发列表
        last_triggered_ids.push_back(alarm.id);
        
        if (alarm_callback_) {
            ESP_LOGI(TAG, "Calling alarm callback for ID=%d", alarm.id);
            alarm_callback_(alarm);
        } else {
            ESP_LOGW(TAG, "No alarm callback set! Alarm triggered but no handler available.");
        }
        
        // 检查是否已经在触发列表中，避免重复添加
        if (std::find(triggered_alarm_ids.begin(), triggered_alarm_ids.end(), alarm.id) == triggered_alarm_ids.end()) {
            triggered_alarm_ids.push_back(alarm.id);
        }
        
        // 立即禁用一次性闹钟（weekdays为空的为一次性闹钟）
        if (alarm.weekdays.empty()) {
            alarm.enabled = false;
            ESP_LOGI(TAG, "Disabled one-time alarm ID=%d", alarm.id);
        }
    }
    
    if (triggered_alarm_ids.empty()) {
        ESP_LOGD(TAG, "No alarms triggered at %02d:%02d", current_tm.tm_hour, current_tm.tm_min);
    }
    
    // 如果有闹钟触发，异步处理保存操作
    if (!triggered_alarm_ids.empty()) {
        auto* ids = new std::vector<int>(triggered_alarm_ids);
        xTaskCreate([](void* param) {
            auto* ids_ptr = static_cast<std::vector<int>*>(param);
            auto& manager = AlarmManager::GetInstance();
            manager.ProcessTriggeredAlarms(*ids_ptr, time(nullptr));
            delete ids_ptr;
            vTaskDelete(nullptr);
        }, "process_alarms", 4096, ids, 5, nullptr);
    }
}

void AlarmManager::ProcessTriggeredAlarms(const std::vector<int>& triggered_ids, time_t trigger_time) {
    ESP_LOGI(TAG, "ProcessTriggeredAlarms: Processing %d triggered alarms", triggered_ids.size());
    
    // 获取互斥锁
    if (xSemaphoreTake(alarms_mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex in ProcessTriggeredAlarms");
        return;
    }
    
    // 移除已禁用的一次性闹钟
    size_t before_count = alarms_.size();
    alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(),
        [](const AlarmInfo& alarm) {
            // 移除已禁用的一次性闹钟
            return !alarm.enabled && alarm.weekdays.empty();
        }), alarms_.end());
    
    size_t after_count = alarms_.size();
    int removed_count = before_count - after_count;
    
    ESP_LOGI(TAG, "ProcessTriggeredAlarms: Removed %d one-time alarms", removed_count);
    
    // 保存到NVS
    SaveAlarmsToNVS();
    
    // 释放互斥锁
    xSemaphoreGive(alarms_mutex_);
}

void AlarmManager::SetRtc(Pcf8563Rtc* rtc) {
    rtc_ = rtc;
}

bool AlarmManager::GetRtcTime(time_t* timestamp) const {
    if (rtc_) {
        return rtc_->GetTime(timestamp);
    }
    return false;
}

void AlarmManager::LoadAlarmsFromNVS() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }
    
    // 加载下一个ID
    int32_t saved_next_id = 1;
    if (nvs_get_i32(nvs_handle, "next_id", &saved_next_id) == ESP_OK) {
        next_alarm_id_ = saved_next_id;
        ESP_LOGI(TAG, "Loaded next alarm ID: %d", next_alarm_id_);
    }
    
    // 从JSON格式加载闹钟数据
    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, "alarms_json", nullptr, &required_size);
    
    if (err == ESP_OK && required_size > 0) {
        char* json_str = (char*)malloc(required_size);
        if (json_str) {
            err = nvs_get_str(nvs_handle, "alarms_json", json_str, &required_size);
            if (err == ESP_OK) {
                cJSON* json_array = cJSON_Parse(json_str);
                if (json_array && cJSON_IsArray(json_array)) {
                    alarms_.clear();
                    
                    int array_size = cJSON_GetArraySize(json_array);
                    for (int i = 0; i < array_size; i++) {
                        cJSON* alarm_obj = cJSON_GetArrayItem(json_array, i);
                        if (alarm_obj) {
                            AlarmInfo alarm;
                            
                            cJSON* id = cJSON_GetObjectItem(alarm_obj, "id");
                            if (id && cJSON_IsNumber(id)) alarm.id = id->valueint;
                            
                            cJSON* desc = cJSON_GetObjectItem(alarm_obj, "description");
                            if (desc && cJSON_IsString(desc)) alarm.description = desc->valuestring;
                            
                            cJSON* enabled = cJSON_GetObjectItem(alarm_obj, "enabled");
                            if (enabled && cJSON_IsBool(enabled)) alarm.enabled = cJSON_IsTrue(enabled);
                            
                            cJSON* hour = cJSON_GetObjectItem(alarm_obj, "hour");
                            if (hour && cJSON_IsNumber(hour)) alarm.hour = hour->valueint;
                            
                            cJSON* minute = cJSON_GetObjectItem(alarm_obj, "minute");
                            if (minute && cJSON_IsNumber(minute)) alarm.minute = minute->valueint;
                            
                            cJSON* sound_type = cJSON_GetObjectItem(alarm_obj, "sound_type");
                            if (sound_type && cJSON_IsNumber(sound_type)) {
                                alarm.sound_type = static_cast<AlarmSoundType>(sound_type->valueint);
                            } else {
                                // 默认为网络音乐
                                alarm.sound_type = AlarmSoundType::NETWORK_MUSIC;
                            }
                            
                            cJSON* weekdays_array = cJSON_GetObjectItem(alarm_obj, "weekdays");
                            if (weekdays_array && cJSON_IsArray(weekdays_array)) {
                                alarm.weekdays.clear();
                                int weekdays_size = cJSON_GetArraySize(weekdays_array);
                                for (int j = 0; j < weekdays_size; j++) {
                                    cJSON* weekday_obj = cJSON_GetArrayItem(weekdays_array, j);
                                    if (weekday_obj && cJSON_IsNumber(weekday_obj)) {
                                        alarm.weekdays.push_back(weekday_obj->valueint);
                                    }
                                }
                            }
                            
                            alarms_.push_back(alarm);
                        }
                    }
                    
                    ESP_LOGI(TAG, "Loaded %zu alarms from NVS JSON", alarms_.size());
                } else {
                    ESP_LOGE(TAG, "Invalid JSON format in NVS");
                }
                
                if (json_array) cJSON_Delete(json_array);
            } else {
                ESP_LOGE(TAG, "Failed to read alarms JSON from NVS: %s", esp_err_to_name(err));
            }
            free(json_str);
        } else {
            ESP_LOGE(TAG, "Failed to allocate memory for JSON string");
            err = ESP_ERR_NO_MEM;
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No alarms found in NVS, starting fresh");
        err = ESP_OK;  // 没有数据是正常的
    }
    
    nvs_close(nvs_handle);
}

bool AlarmManager::SaveAlarmsToNVS() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }
    
    // 序列化闹钟数据为JSON格式存储（避免std::string的内存布局问题）
    cJSON* json_array = cJSON_CreateArray();
    
    for (const auto& alarm : alarms_) {
        cJSON* alarm_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(alarm_obj, "id", alarm.id);
        cJSON_AddStringToObject(alarm_obj, "description", alarm.description.c_str());
        cJSON_AddNumberToObject(alarm_obj, "hour", alarm.hour);
        cJSON_AddNumberToObject(alarm_obj, "minute", alarm.minute);
        cJSON_AddBoolToObject(alarm_obj, "enabled", alarm.enabled);
        cJSON_AddNumberToObject(alarm_obj, "sound_type", static_cast<int>(alarm.sound_type));
        
        // 添加weekdays数组
        cJSON* weekdays_array = cJSON_CreateArray();
        for (int weekday : alarm.weekdays) {
            cJSON_AddItemToArray(weekdays_array, cJSON_CreateNumber(weekday));
        }
        cJSON_AddItemToObject(alarm_obj, "weekdays", weekdays_array);
        
        cJSON_AddItemToArray(json_array, alarm_obj);
    }
    
    char* json_str = cJSON_PrintUnformatted(json_array);
    if (json_str) {
        err = nvs_set_str(nvs_handle, "alarms_json", json_str);
        if (err == ESP_OK) {
            err = nvs_commit(nvs_handle);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Saved %zu alarms to NVS as JSON", alarms_.size());
            }
        } else {
            ESP_LOGE(TAG, "Failed to save alarms JSON to NVS: %s", esp_err_to_name(err));
        }
        cJSON_free(json_str);
    } else {
        ESP_LOGE(TAG, "Failed to serialize alarms to JSON");
        err = ESP_FAIL;
    }
    
    cJSON_Delete(json_array);
    
    // 同时保存下一个ID
    if (err == ESP_OK) {
        nvs_set_i32(nvs_handle, "next_id", next_alarm_id_);
        nvs_commit(nvs_handle);
    }
    
    nvs_close(nvs_handle);
    return err == ESP_OK;
}

bool AlarmManager::ParseVoiceCommand(const std::string& command, int& hour, int& minute, 
                                   int& minutes_later, std::string& description, 
                                   bool& is_relative, std::vector<int>& weekdays, bool& is_tomorrow) {
    // 初始化输出参数
    hour = 0;
    minute = 0;
    minutes_later = 0;
    description = "";
    is_relative = false;
    is_tomorrow = false;
    
    std::string lower_cmd = command;
    std::transform(lower_cmd.begin(), lower_cmd.end(), lower_cmd.begin(), ::tolower);
    
    // 相对时间正则表达式
    std::regex relative_hour_pattern(R"((?:设置|请)?(?:在)?(\d+)(?:个)?小时(?:(\d+)(?:分钟?)?)?(?:后|之后)(?:提醒我?)?(.*))", 
                                   std::regex_constants::icase);
    std::regex relative_minute_pattern(R"((?:设置|请)?(?:在)?(\d+)分钟?(?:后|之后)(?:提醒我?)?(.*))", 
                                     std::regex_constants::icase);
    
    // 绝对时间正则表达式 - 修改以捕获时间段信息
    std::regex absolute_pattern(R"((?:设置|请)?(?:明天|今天)?(早上|上午|中午|下午|晚上)?(\d{1,2})(?:点|:)(\d{0,2})(?:分?)?(?:提醒我?)?(.*))", 
                              std::regex_constants::icase);
    
    // 检查特殊关键词
    weekdays.clear(); // 先清空
    if (command.find("每天") != std::string::npos || command.find("每日") != std::string::npos) {
        weekdays = {1, 2, 3, 4, 5, 6, 0}; // 周一到周日
    } else if (command.find("每周") != std::string::npos) {
        weekdays = {1, 2, 3, 4, 5, 6, 0}; // 每周等同于每天
    } else if (command.find("工作日") != std::string::npos || command.find("周一到周五") != std::string::npos) {
        weekdays = {1, 2, 3, 4, 5}; // 周一到周五
    } else if (command.find("周末") != std::string::npos) {
        weekdays = {6, 0}; // 周六、周日
    } else if (command.find("周一") != std::string::npos) {
        weekdays = {1}; // 仅周一
    } else if (command.find("周二") != std::string::npos) {
        weekdays = {2}; // 仅周二
    } else if (command.find("周三") != std::string::npos) {
        weekdays = {3}; // 仅周三
    } else if (command.find("周四") != std::string::npos) {
        weekdays = {4}; // 仅周四
    } else if (command.find("周五") != std::string::npos) {
        weekdays = {5}; // 仅周五
    } else if (command.find("周六") != std::string::npos) {
        weekdays = {6}; // 仅周六
    } else if (command.find("周日") != std::string::npos || command.find("星期日") != std::string::npos) {
        weekdays = {0}; // 仅周日
    }
    
    // 检查是否是明天
    is_tomorrow = (command.find("明天") != std::string::npos);
    
    std::smatch matches;
    
    // 检查相对时间（小时+分钟）
    if (std::regex_search(command, matches, relative_hour_pattern)) {
        is_relative = true;
        int hours = std::stoi(matches[1].str());
        int mins = 0;
        if (matches[2].matched && !matches[2].str().empty()) {
            mins = std::stoi(matches[2].str());
        }
        minutes_later = hours * 60 + mins;
        description = matches[3].str();
        return true;
    }
    // 检查相对时间（仅分钟）
    else if (std::regex_search(command, matches, relative_minute_pattern)) {
        is_relative = true;
        minutes_later = std::stoi(matches[1].str());
        description = matches[2].str();
        return true;
    }
    // 检查绝对时间
    else if (std::regex_search(command, matches, absolute_pattern)) {
        is_relative = false;
        
        // 提取时间段信息（可能为空）
        std::string time_period = matches[1].str();
        hour = std::stoi(matches[2].str());
        if (matches[3].matched && !matches[3].str().empty()) {
            minute = std::stoi(matches[3].str());
        } else {
            minute = 0;
        }
        description = matches[4].str();
        
        // 根据时间段调整小时数
        if (!time_period.empty()) {
            int original_hour = hour;
            if (time_period == "下午" || time_period == "晚上") {
                // 下午和晚上：如果小时数小于12，则加12
                if (hour < 12) {
                    hour += 12;
                }
            } else if ((time_period == "早上" || time_period == "上午") && hour == 12) {
                // 早上和上午：如果是12点，转换为0点（午夜）
                hour = 0;
            } else if (time_period == "中午" && hour == 12) {
                // 中午12点保持为12点
                // hour = 12; // 保持不变
            }
            // 简化的日志记录
            if (hour != original_hour) {
                // Time period adjustment applied
            }
        }
        
        // 验证调整后的时间是否有效
        if (!IsValidTime(hour, minute)) {
            return false;
        }
        
        // Parsed absolute time successfully
        
        return true;
    }
    
    return false;
}

bool AlarmManager::ParseRelativeTime(const std::string& time_str, int& minutes) {
    std::regex hour_pattern(R"((\d+)(?:个)?小时)", std::regex_constants::icase);
    std::regex minute_pattern(R"((\d+)分钟?)", std::regex_constants::icase);
    
    std::smatch matches;
    minutes = 0;
    
    if (std::regex_search(time_str, matches, hour_pattern)) {
        minutes += std::stoi(matches[1].str()) * 60;
    }
    
    if (std::regex_search(time_str, matches, minute_pattern)) {
        minutes += std::stoi(matches[1].str());
    }
    
    return minutes > 0;
}

bool AlarmManager::ParseAbsoluteTime(const std::string& time_str, int& hour, int& minute, bool& is_tomorrow) {
    std::regex time_pattern(R"((\d{1,2})(?:点|:)(\d{0,2}))", std::regex_constants::icase);
    std::smatch matches;
    
    if (std::regex_search(time_str, matches, time_pattern)) {
        hour = std::stoi(matches[1].str());
        if (matches[2].matched && !matches[2].str().empty()) {
            minute = std::stoi(matches[2].str());
        } else {
            minute = 0;
        }
        
        is_tomorrow = (time_str.find("明天") != std::string::npos);
        return true;
    }
    
    return false;
}

void AlarmManager::CheckTimerCallback(TimerHandle_t timer) {
    AlarmManager* manager = static_cast<AlarmManager*>(pvTimerGetTimerID(timer));
    if (manager) {
        manager->CheckAlarms();
    }
}

int AlarmManager::GenerateAlarmId() {
    return next_alarm_id_++;
}

time_t AlarmManager::CalculateNextTriggerTime(int hour, int minute, bool is_tomorrow) {
    time_t now;
    if (rtc_ && rtc_->GetTime(&now)) {
        // 使用RTC时间
    } else {
        time(&now);
    }
    
    struct tm* tm_now = localtime(&now);
    tm_now->tm_hour = hour;
    tm_now->tm_min = minute;
    tm_now->tm_sec = 0;
    
    if (is_tomorrow) {
        tm_now->tm_mday += 1;
    }
    
    time_t trigger_time = mktime(tm_now);
    
    // 如果时间已经过了（今天），设置为明天
    if (!is_tomorrow && trigger_time <= now) {
        tm_now->tm_mday += 1;
        trigger_time = mktime(tm_now);
    }
    
    return trigger_time;
}

bool AlarmManager::IsValidTime(int hour, int minute) {
    return (hour >= 0 && hour <= 23) && (minute >= 0 && minute <= 59);
}

void AlarmManager::SetDefaultSoundType(AlarmSoundType sound_type) {
    default_sound_type_ = sound_type;
    ESP_LOGI(TAG, "Default sound type set to: %d", static_cast<int>(sound_type));
}

AlarmSoundType AlarmManager::GetDefaultSoundType() const {
    return default_sound_type_;
}

int AlarmManager::RemoveExpiredAlarms() {
    // 获取互斥锁
    if (xSemaphoreTake(alarms_mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex in RemoveExpiredAlarms");
        return 0;
    }
    
    if (alarms_.empty()) {
        xSemaphoreGive(alarms_mutex_);
        return 0;
    }
    
    time_t now;
    if (rtc_ && rtc_->GetTime(&now)) {
        // 使用RTC时间
    } else {
        time(&now);
    }
    
    struct tm* timeinfo = localtime(&now);
    if (!timeinfo) {
        xSemaphoreGive(alarms_mutex_);
        return 0;
    }
    
    int current_hour = timeinfo->tm_hour;
    int current_minute = timeinfo->tm_min;
    
    int removed_count = 0;
    auto it = alarms_.begin();
    while (it != alarms_.end()) {
        // 如果闹钟没有设置重复的星期几（weekdays为空），且已经过了当天的闹钟时间，则删除
        if (it->weekdays.empty() && 
            (it->hour < current_hour || (it->hour == current_hour && it->minute <= current_minute))) {
            ESP_LOGI(TAG, "Removing expired one-time alarm: ID=%d, time=%02d:%02d", 
                     it->id, it->hour, it->minute);
            it = alarms_.erase(it);
            removed_count++;
        } else {
            ++it;
        }
    }
    
    if (removed_count > 0) {
        SaveAlarmsToNVS();
        ESP_LOGI(TAG, "Removed %d expired alarms", removed_count);
    }
    
    // 释放互斥锁
    xSemaphoreGive(alarms_mutex_);
    return removed_count;
}

bool AlarmManager::IsTimeValid() const {
    time_t now;
    if (rtc_ && rtc_->GetTime(&now)) {
        // 使用RTC时间
    } else {
        time(&now);
    }
    
    struct tm* timeinfo = localtime(&now);
    // 检查年份是否大于2020（表示时间已同步）
    return (timeinfo && timeinfo->tm_year >= (2020 - 1900));
}