#include "time_sync_manager.h"
#include "pcf8563_rtc.h"
#include "ntp_sync.h"
#include "clock_ui.h"
#include "alarm_manager.h"
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>
#include <functional>
#include <ctime>
#include <cstdlib>

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
    
    ESP_LOGI(TAG, "Triggering smart NTP sync with resource conflict avoidance");
    
    // 使用异步任务进行智能时间同步
    xTaskCreate([](void* param) {
        auto* manager = static_cast<TimeSyncManager*>(param);
        manager->SmartNtpSync();
        vTaskDelete(nullptr);
    }, "smart_ntp_sync", 6144, this, 4, nullptr);  // 降低优先级避免干扰关键任务
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

void TimeSyncManager::SmartNtpSync() {
    ESP_LOGI(TAG, "Starting smart NTP sync process");
    
    // 步骤1：等待合适的时机（避免与其他操作冲突）
    int retry_count = 0;
    const int max_retries = 3;
    const int retry_interval_ms = 30000; // 30秒重试间隔
    
    while (retry_count < max_retries) {
        // 检查系统是否空闲（简单的启发式检查）
        if (IsSystemIdleForSync()) {
            ESP_LOGI(TAG, "System appears idle, proceeding with NTP sync (attempt %d)", retry_count + 1);
            
            // 执行NTP同步
            bool sync_success = false;
            if (ntp_sync_) {
                // 设置超时时间，避免长时间阻塞
                sync_success = ntp_sync_->SyncTime([this](bool success, const std::string& message) {
                    OnNtpSyncComplete(success, message);
                });
            }
            
            if (sync_success) {
                ESP_LOGI(TAG, "Smart NTP sync completed successfully");
                return;
            } else {
                ESP_LOGW(TAG, "NTP sync failed, will retry after delay");
            }
        } else {
            ESP_LOGI(TAG, "System busy, delaying NTP sync (attempt %d)", retry_count + 1);
        }
        
        retry_count++;
        if (retry_count < max_retries) {
            ESP_LOGI(TAG, "Waiting %d seconds before next sync attempt", retry_interval_ms / 1000);
            vTaskDelay(pdMS_TO_TICKS(retry_interval_ms));
        }
    }
    
    ESP_LOGW(TAG, "Smart NTP sync failed after %d attempts", max_retries);
}

bool TimeSyncManager::IsSystemIdleForSync() {
    // 简单的系统空闲检查
    // 可以根据需要添加更多检查条件
    
    // 检查1：内存使用情况
    size_t free_heap = esp_get_free_heap_size();
    size_t min_heap = esp_get_minimum_free_heap_size();
    
    if (free_heap < 50000) { // 少于50KB空闲内存
        ESP_LOGD(TAG, "Low memory detected: %zu bytes free", free_heap);
        return false;
    }
    
    // 检查2：RTC是否被其他操作占用（简单检查）
    if (rtc_) {
        // 尝试快速读取RTC状态
        struct tm test_time;
        if (!rtc_->GetTime(&test_time)) {
            ESP_LOGD(TAG, "RTC appears busy or unavailable");
            return false;
        }
    }
    
    // 检查3：确保不在闹钟检查周期内（避免与闹钟管理器冲突）
    // 获取当前秒数，避免在整分钟时进行同步
    time_t now;
    time(&now);
    struct tm* tm_now = localtime(&now);
    
    if (tm_now->tm_sec < 5 || tm_now->tm_sec > 55) {
        ESP_LOGD(TAG, "Avoiding sync near minute boundary (current second: %d)", tm_now->tm_sec);
        return false;
    }
    
    ESP_LOGD(TAG, "System appears idle for sync: heap=%zu, min_heap=%zu, sec=%d", 
             free_heap, min_heap, tm_now->tm_sec);
    return true;
}

void TimeSyncManager::OnNtpSyncComplete(bool success, const std::string& message) {
    ESP_LOGI(TAG, "NTP sync completed: %s - %s", success ? "SUCCESS" : "FAILED", message.c_str());
    
    if (success) {
        ESP_LOGI(TAG, "Time synchronized successfully");
        
        // 确保系统时间正确设置
        time_t current_time;
        time(&current_time);
        struct tm* timeinfo = localtime(&current_time);
        
        char sys_time_str[64];
        strftime(sys_time_str, sizeof(sys_time_str), "%Y-%m-%d %H:%M:%S", timeinfo);
        ESP_LOGI(TAG, "Current system time: %s", sys_time_str);
        
        // 检查RTC时间是否与系统时间一致
        if (rtc_) {
            time_t rtc_time;
            if (rtc_->GetTime(&rtc_time)) {
                struct tm* rtc_timeinfo = localtime(&rtc_time);
                char rtc_time_str[64];
                strftime(rtc_time_str, sizeof(rtc_time_str), "%Y-%m-%d %H:%M:%S", rtc_timeinfo);
                ESP_LOGI(TAG, "Current RTC time: %s", rtc_time_str);
                
                // 检查时间差（允许几秒钟的差异）
                int time_diff = abs((int)(current_time - rtc_time));
                if (time_diff > 5) {
                    ESP_LOGW(TAG, "System and RTC time difference: %d seconds, re-syncing RTC", time_diff);
                    
                    // 重新同步RTC到系统时间
                    if (rtc_->SyncSystemTimeToRtc()) {
                        ESP_LOGI(TAG, "RTC re-synced to system time");
                    } else {
                        ESP_LOGW(TAG, "Failed to re-sync RTC");
                    }
                } else {
                    ESP_LOGI(TAG, "System and RTC time are synchronized (difference: %d seconds)", time_diff);
                }
            } else {
                ESP_LOGW(TAG, "Failed to read RTC time for comparison");
            }
        }
    } else {
        ESP_LOGW(TAG, "NTP sync failed: %s", message.c_str());
    }
    
    // 调用用户回调
    if (sync_callback_) {
        sync_callback_(success, message);
    }
} 