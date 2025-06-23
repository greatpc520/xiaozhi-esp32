#include "alarm_manager.h"
#include <esp_log.h>
#include <ctime>
#include <sstream>
#include <algorithm>
#include <nvs_flash.h>
#include <nvs.h>
#include <sys/time.h>
#include <regex>
#include <iomanip>
#include <cJSON.h>

#define TAG "AlarmManager"
#define NVS_NAMESPACE "alarms"
#define MAX_ALARMS 20
#define CHECK_INTERVAL_MS 30000  // 30秒检查一次

int AlarmManager::next_alarm_id_ = 1;

AlarmManager::AlarmManager() : check_timer_(nullptr), rtc_(nullptr) {
}

AlarmManager::~AlarmManager() {
    if (check_timer_ != nullptr) {
        xTimerDelete(check_timer_, portMAX_DELAY);
    }
    SaveAlarmsToNVS();
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
    if (!LoadAlarmsFromNVS()) {
        ESP_LOGW(TAG, "Failed to load alarms from NVS, starting with empty list");
    }
    
    // 启动定时器
    if (xTimerStart(check_timer_, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start check timer");
        return false;
    }
    
    ESP_LOGI(TAG, "AlarmManager initialized successfully");
    return true;
}

int AlarmManager::SetAlarmFromVoice(const std::string& voice_command) {
    ESP_LOGI(TAG, "Processing voice command: %s", voice_command.c_str());
    
    int hour, minute, minutes_later;
    std::string description;
    bool is_relative, repeat_daily;
    
    if (ParseVoiceCommand(voice_command, hour, minute, minutes_later, description, is_relative, repeat_daily)) {
        if (is_relative) {
            return SetAlarmRelative(minutes_later, description);
        } else {
            bool is_tomorrow = (voice_command.find("明天") != std::string::npos);
            return SetAlarm(hour, minute, description, repeat_daily, is_tomorrow);
        }
    }
    
    ESP_LOGE(TAG, "Failed to parse voice command");
    return -1;
}

int AlarmManager::SetAlarm(int hour, int minute, const std::string& description, bool repeat_daily, bool is_tomorrow) {
    if (!IsValidTime(hour, minute)) {
        ESP_LOGE(TAG, "Invalid time: %02d:%02d", hour, minute);
        return -1;
    }
    
    AlarmInfo alarm;
    alarm.id = GenerateAlarmId();
    alarm.hour = hour;
    alarm.minute = minute;
    alarm.description = description.empty() ? "闹钟提醒" : description;
    alarm.enabled = true;
    alarm.repeat_daily = repeat_daily;
    alarm.trigger_time = CalculateNextTriggerTime(hour, minute, is_tomorrow);
    
    alarms_.push_back(alarm);
    SaveAlarmsToNVS();
    
    ESP_LOGI(TAG, "Alarm set: %02d:%02d - %s (ID: %d)%s", 
             hour, minute, alarm.description.c_str(), alarm.id,
             is_tomorrow ? " [Tomorrow]" : "");
    
    return alarm.id;
}

int AlarmManager::SetAlarmRelative(int minutes_later, const std::string& description) {
    time_t now;
    if (rtc_ && rtc_->GetTime(&now)) {
        // 使用RTC时间
    } else {
        // 回退到系统时间
        time(&now);
    }
    
    time_t trigger_time = now + (minutes_later * 60);
    struct tm* trigger_tm = localtime(&trigger_time);
    
    AlarmInfo alarm;
    alarm.id = GenerateAlarmId();
    alarm.hour = trigger_tm->tm_hour;
    alarm.minute = trigger_tm->tm_min;
    alarm.description = description.empty() ? 
                       (std::to_string(minutes_later) + "分钟后的提醒") : description;
    alarm.enabled = true;
    alarm.repeat_daily = false;
    alarm.trigger_time = trigger_time;
    
    alarms_.push_back(alarm);
    SaveAlarmsToNVS();
    
    ESP_LOGI(TAG, "Relative alarm set: %02d:%02d - %s (ID: %d)", 
             alarm.hour, alarm.minute, alarm.description.c_str(), alarm.id);
    
    return alarm.id;
}

std::vector<AlarmInfo> AlarmManager::GetAlarms() const {
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

AlarmInfo AlarmManager::GetNextAlarm() const {
    AlarmInfo next_alarm;
    time_t current_time;
    
    if (rtc_ && rtc_->GetTime(&current_time)) {
        // 使用RTC时间
    } else {
        time(&current_time);
    }
    
    time_t closest_time = 0;
    
    for (const auto& alarm : alarms_) {
        if (alarm.enabled && (closest_time == 0 || alarm.trigger_time < closest_time)) {
            if (alarm.trigger_time > current_time) {
                next_alarm = alarm;
                closest_time = alarm.trigger_time;
            }
        }
    }
    
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
    time_t current_time;
    
    if (rtc_ && rtc_->GetTime(&current_time)) {
        // 使用RTC时间
    } else {
        time(&current_time);
    }
    
    // 在定时器任务中只检查触发条件，立即标记已触发避免重复
    std::vector<int> triggered_alarm_ids;
    
    for (auto& alarm : alarms_) {
        if (alarm.enabled && alarm.trigger_time <= current_time) {
            // 移除在定时器任务中的日志记录，避免栈溢出
            // ESP_LOGI(TAG, "Alarm triggered: %s", alarm.description.c_str());
            
            if (alarm_callback_) {
                alarm_callback_(alarm);
            }
            
            // 检查是否已经在触发列表中，避免重复添加
            if (std::find(triggered_alarm_ids.begin(), triggered_alarm_ids.end(), alarm.id) == triggered_alarm_ids.end()) {
                triggered_alarm_ids.push_back(alarm.id);
            }
            
            // 立即禁用一次性闹钟或标记重复闹钟已触发
            if (alarm.repeat_daily) {
                // 临时设置一个很大的触发时间，ProcessTriggeredAlarms会正确设置
                alarm.trigger_time = current_time + 86400; // 24小时后
            } else {
                // 一次性闹钟立即禁用
                alarm.enabled = false;
            }
        }
    }
    
    // 如果有闹钟触发，异步处理vector修改和保存操作
    if (!triggered_alarm_ids.empty()) {
        // 创建参数结构体传递触发的闹钟ID
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
    
    int processed_count = 0;
    // 处理重复闹钟的下一次触发时间
    for (auto& alarm : alarms_) {
        // 只处理触发列表中的重复闹钟
        if (alarm.repeat_daily && 
            std::find(triggered_ids.begin(), triggered_ids.end(), alarm.id) != triggered_ids.end()) {
            
            ESP_LOGI(TAG, "Rescheduling daily alarm ID %d: %s", alarm.id, alarm.description.c_str());
            
            // 基于原始触发时间计算下一天
            struct tm tm_trigger = {};
            localtime_r(&trigger_time, &tm_trigger);
            tm_trigger.tm_mday += 1; // 明天同一时间
            alarm.trigger_time = mktime(&tm_trigger);
            
            ESP_LOGI(TAG, "Daily repeat alarm %d scheduled for next day", alarm.id);
            processed_count++;
        }
    }
    
    // 移除已禁用的一次性闹钟
    size_t before_count = alarms_.size();
    alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(),
        [](const AlarmInfo& alarm) {
            // 移除已禁用的一次性闹钟
            return !alarm.enabled && !alarm.repeat_daily;
        }), alarms_.end());
    
    size_t after_count = alarms_.size();
    int removed_count = before_count - after_count;
    
    ESP_LOGI(TAG, "ProcessTriggeredAlarms: Rescheduled %d daily alarms, removed %d one-time alarms", 
             processed_count, removed_count);
    
    // 保存到NVS
    SaveAlarmsToNVS();
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

bool AlarmManager::LoadAlarmsFromNVS() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }
    
    // 加载下一个ID
    int32_t saved_next_id = 1;
    size_t size = sizeof(saved_next_id);
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
                            
                            cJSON* trigger_time = cJSON_GetObjectItem(alarm_obj, "trigger_time");
                            if (trigger_time && cJSON_IsNumber(trigger_time)) alarm.trigger_time = (time_t)trigger_time->valuedouble;
                            
                            cJSON* enabled = cJSON_GetObjectItem(alarm_obj, "enabled");
                            if (enabled && cJSON_IsBool(enabled)) alarm.enabled = cJSON_IsTrue(enabled);
                            
                            cJSON* repeat_daily = cJSON_GetObjectItem(alarm_obj, "repeat_daily");
                            if (repeat_daily && cJSON_IsBool(repeat_daily)) alarm.repeat_daily = cJSON_IsTrue(repeat_daily);
                            
                            cJSON* hour = cJSON_GetObjectItem(alarm_obj, "hour");
                            if (hour && cJSON_IsNumber(hour)) alarm.hour = hour->valueint;
                            
                            cJSON* minute = cJSON_GetObjectItem(alarm_obj, "minute");
                            if (minute && cJSON_IsNumber(minute)) alarm.minute = minute->valueint;
                            
                            cJSON* sound_file = cJSON_GetObjectItem(alarm_obj, "sound_file");
                            if (sound_file && cJSON_IsString(sound_file)) alarm.sound_file = sound_file->valuestring;
                            
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
    return err == ESP_OK;
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
        cJSON_AddNumberToObject(alarm_obj, "trigger_time", (double)alarm.trigger_time);
        cJSON_AddBoolToObject(alarm_obj, "enabled", alarm.enabled);
        cJSON_AddBoolToObject(alarm_obj, "repeat_daily", alarm.repeat_daily);
        cJSON_AddNumberToObject(alarm_obj, "hour", alarm.hour);
        cJSON_AddNumberToObject(alarm_obj, "minute", alarm.minute);
        cJSON_AddStringToObject(alarm_obj, "sound_file", alarm.sound_file.c_str());
        
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
                                   bool& is_relative, bool& repeat_daily) {
    // 初始化输出参数
    hour = 0;
    minute = 0;
    minutes_later = 0;
    description = "";
    is_relative = false;
    repeat_daily = false;
    
    std::string lower_cmd = command;
    std::transform(lower_cmd.begin(), lower_cmd.end(), lower_cmd.begin(), ::tolower);
    
    // 相对时间正则表达式
    std::regex relative_hour_pattern(R"((?:设置|请)?(?:在)?(\d+)(?:个)?小时(?:(\d+)(?:分钟?)?)?(?:后|之后)(?:提醒我?)?(.*))", 
                                   std::regex_constants::icase);
    std::regex relative_minute_pattern(R"((?:设置|请)?(?:在)?(\d+)分钟?(?:后|之后)(?:提醒我?)?(.*))", 
                                     std::regex_constants::icase);
    
    // 绝对时间正则表达式
    std::regex absolute_pattern(R"((?:设置|请)?(?:明天|今天)?(?:早上|上午|中午|下午|晚上)?(\d{1,2})(?:点|:)(\d{0,2})(?:分?)?(?:提醒我?)?(.*))", 
                              std::regex_constants::icase);
    
    // 检查特殊关键词
    repeat_daily = (command.find("每天") != std::string::npos || command.find("每日") != std::string::npos);
    bool is_tomorrow = (command.find("明天") != std::string::npos);
    
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
        hour = std::stoi(matches[1].str());
        if (matches[2].matched && !matches[2].str().empty()) {
            minute = std::stoi(matches[2].str());
        }
        description = matches[3].str();
        
        // 注意：如果是明天，hour和minute已经是正确的，
        // CalculateNextTriggerTime会处理明天的逻辑
        
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