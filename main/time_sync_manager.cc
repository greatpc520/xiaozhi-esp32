#include "time_sync_manager.h"
#include "pcf8563_rtc.h"
#include "ntp_sync.h"
#include "clock_ui.h"
#include "alarm_manager.h"
#include <esp_log.h>
#include <memory>
#include <functional>
#include <ctime>

#define TAG "TimeSyncManager"

TimeSyncManager::TimeSyncManager() 
    : initialized_(false) {
}

TimeSyncManager::~TimeSyncManager() {
}

bool TimeSyncManager::Initialize(i2c_master_bus_handle_t i2c_bus) {
    ESP_LOGI(TAG, "Initializing TimeSyncManager");
    
    if (initialized_) {
        ESP_LOGW(TAG, "TimeSyncManager already initialized");
        return true;
    }
    
    // 初始化PCF8563 RTC
    rtc_ = std::make_unique<Pcf8563Rtc>(i2c_bus);
    if (!rtc_->Initialize()) {
        ESP_LOGE(TAG, "Failed to initialize PCF8563 RTC");
        return false;
    }
    
    // RTC基本功能测试（不修改当前时间）
    ESP_LOGI(TAG, "Testing RTC basic functionality...");
    struct tm read_time;
    if (rtc_->GetTime(&read_time)) {
        ESP_LOGI(TAG, "RTC read test successful. Current RTC time: %04d-%02d-%02d %02d:%02d:%02d", 
                 read_time.tm_year + 1900, read_time.tm_mon + 1, read_time.tm_mday,
                 read_time.tm_hour, read_time.tm_min, read_time.tm_sec);
        
        // 只在RTC时间明显不合理时才警告（比如年份小于2020）
        if (read_time.tm_year + 1900 < 2020) {
            ESP_LOGW(TAG, "RTC time appears to be invalid (year < 2020). Will need NTP sync.");
        }
    } else {
        ESP_LOGE(TAG, "RTC read test failed: GetTime failed.");
    }
    
    // 初始化NTP同步
    ntp_sync_ = std::make_unique<NtpSync>();
    if (!ntp_sync_->Initialize()) {
        ESP_LOGE(TAG, "Failed to initialize NTP sync");
        return false;
    }
    
    // 设置NTP同步的RTC实例
    ntp_sync_->SetRtc(rtc_.get());
    
    // 暂时禁用时钟UI的创建，避免启动时卡死
    // clock_ui_ = std::make_unique<ClockUI>();
    // clock_ui_->SetRtc(rtc_.get());
    
    // 设置闹钟管理器的RTC实例
    auto& alarm_manager = AlarmManager::GetInstance();
    alarm_manager.SetRtc(rtc_.get());
    
    initialized_ = true;
    ESP_LOGI(TAG, "TimeSyncManager initialized successfully");
    return true;
}

void TimeSyncManager::SyncTimeOnBoot() {
    if (!initialized_) {
        ESP_LOGE(TAG, "TimeSyncManager not initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Starting boot time synchronization");
    
    // 检查RTC时间是否有效
    if (IsRtcWorking()) {
        struct tm rtc_time;
        if (rtc_->GetTime(&rtc_time)) {
            int rtc_year = rtc_time.tm_year + 1900;
            // 只有当RTC时间是合理的（2024年以后）才同步到系统
            if (rtc_year >= 2024) {
                ESP_LOGI(TAG, "RTC has valid time (%d), syncing to system", rtc_year);
                rtc_->SyncRtcToSystemTime();
            } else {
                ESP_LOGW(TAG, "RTC time year (%d) is too old, not syncing to system", rtc_year);
            }
        }
    } else {
        ESP_LOGW(TAG, "RTC not working or invalid time, keeping system time unchanged");
    }
    
    // NTP同步将在网络连接后由WiFi回调触发
    ESP_LOGI(TAG, "Boot time sync completed. NTP sync will be triggered after WiFi connection.");
}

void TimeSyncManager::SetSyncCallback(std::function<void(bool success, const std::string& message)> callback) {
    sync_callback_ = callback;
}

void TimeSyncManager::TriggerNtpSync() {
    if (!initialized_ || !ntp_sync_) {
        ESP_LOGE(TAG, "TimeSyncManager not properly initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Manually triggering NTP sync");
    ntp_sync_->SyncTimeAsync([this](bool success, const std::string& message) {
        OnNtpSyncComplete(success, message);
    });
}

bool TimeSyncManager::IsRtcWorking() const {
    if (!rtc_) {
        return false;
    }
    
    // 检查RTC是否运行
    if (!rtc_->IsRunning()) {
        return false;
    }
    
    // 检查RTC时间是否有效（不是默认值）
    struct tm rtc_time;
    if (!rtc_->GetTime(&rtc_time)) {
        return false;
    }
    
    // 检查年份是否合理（2020年以后）
    if (rtc_time.tm_year + 1900 < 2020) {
        return false;
    }
    
    return true;
}

std::string TimeSyncManager::GetCurrentTimeString() const {
    if (!initialized_) {
        return "TimeSyncManager未初始化";
    }
    
    if (rtc_) {
        return rtc_->GetTimeString();
    }
    
    // 备用：系统时间
    time_t now;
    time(&now);
    struct tm* timeinfo = localtime(&now);
    
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", timeinfo);
    
    return std::string(time_str);
}

void TimeSyncManager::OnNtpSyncComplete(bool success, const std::string& message) {
    ESP_LOGI(TAG, "NTP sync completed: %s - %s", success ? "SUCCESS" : "FAILED", message.c_str());
    
    if (success) {
        ESP_LOGI(TAG, "Time synchronized successfully");
        
        // 检查系统时间是否已更新到RTC
        if (rtc_) {
            ESP_LOGI(TAG, "Current RTC time: %s", rtc_->GetTimeString().c_str());
        }
    } else {
        ESP_LOGW(TAG, "NTP sync failed: %s", message.c_str());
    }
    
    // 调用用户回调
    if (sync_callback_) {
        sync_callback_(success, message);
    }
} 