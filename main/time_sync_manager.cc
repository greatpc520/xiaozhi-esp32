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
    
    // 设置时区为北京时间 UTC+8 (仅在此处设置一次)
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to Beijing (UTC+8) in TimeSyncManager");
    
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
    
    // 打印当前时间状态以供调试
    PrintCurrentTimeStatus();
    
    return true;
}

void TimeSyncManager::SyncTimeOnBoot() {
    if (!initialized_) {
        ESP_LOGE(TAG, "TimeSyncManager not initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Starting boot time synchronization with enhanced timezone safety");
    
    // 打印启动前的时间状态
    ESP_LOGI(TAG, "=== Boot Time Sync Debug - BEFORE ===");
    PrintCurrentTimeStatus();
    
    // 检查RTC时间是否有效
    if (IsRtcWorking()) {
        struct tm rtc_time;
        time_t rtc_timestamp;
        
        if (rtc_->GetTime(&rtc_time) && rtc_->GetTime(&rtc_timestamp)) {
            int rtc_year = rtc_time.tm_year + 1900;
            
            // 获取当前系统时间用于比较
            time_t system_timestamp;
            time(&system_timestamp);
            struct tm* system_time = localtime(&system_timestamp);
            
            ESP_LOGI(TAG, "RTC time: %04d-%02d-%02d %02d:%02d:%02d (timestamp: %lld)", 
                     rtc_time.tm_year + 1900, rtc_time.tm_mon + 1, rtc_time.tm_mday,
                     rtc_time.tm_hour, rtc_time.tm_min, rtc_time.tm_sec, (long long)rtc_timestamp);
            
            ESP_LOGI(TAG, "System time: %04d-%02d-%02d %02d:%02d:%02d (timestamp: %lld)", 
                     system_time->tm_year + 1900, system_time->tm_mon + 1, system_time->tm_mday,
                     system_time->tm_hour, system_time->tm_min, system_time->tm_sec, (long long)system_timestamp);
            
            // 计算时间差（秒）
            long long time_diff = (long long)rtc_timestamp - (long long)system_timestamp;
            ESP_LOGI(TAG, "Time difference (RTC - System): %lld seconds", time_diff);
            
            // 只有当RTC时间是合理的（2024年以后）才考虑同步
            if (rtc_year >= 2025) {
                // 检查时间差异：只有当差异超过1小时时才同步，避免因时区问题导致的小差异
                if (abs((int)time_diff) > 60) { // 1小时 = 3600秒
                    ESP_LOGI(TAG, "Large time difference detected (%lld sec), syncing RTC to system", time_diff);
                    
                    // 使用RTC时间更新系统时间
                    if (rtc_->SyncRtcToSystemTime()) {
                        ESP_LOGI(TAG, "System time synced from RTC successfully");
                    } else {
                        ESP_LOGW(TAG, "Failed to sync system time from RTC");
                    }
                } else {
                    ESP_LOGI(TAG, "Time difference is small (%lld sec), no sync needed", time_diff);
                }
            } else {
                ESP_LOGW(TAG, "RTC time year (%d) is too old, not syncing to system", rtc_year);
            }
        } else {
            ESP_LOGW(TAG, "Failed to read RTC time for boot sync");
        }
    } else {
        ESP_LOGW(TAG, "RTC not working or invalid time, keeping system time unchanged");
    }
    
    // 打印同步后的时间状态
    ESP_LOGI(TAG, "=== Boot Time Sync Debug - AFTER ===");
    PrintCurrentTimeStatus();
    
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

void TimeSyncManager::ForceNtpSync() {
    if (!initialized_ || !ntp_sync_) {
        ESP_LOGE(TAG, "TimeSyncManager not properly initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Forcing immediate NTP sync (skipping idle checks)");
    
    // 使用异步任务进行强制同步
    xTaskCreate([](void* param) {
        auto* manager = static_cast<TimeSyncManager*>(param);
        
        // 直接执行NTP同步，跳过空闲检查
        if (manager->ntp_sync_) {
            bool sync_success = manager->ntp_sync_->SyncTime([manager](bool success, const std::string& message) {
                manager->OnNtpSyncComplete(success, message);
            });
            
            if (sync_success) {
                ESP_LOGI(TAG, "Force NTP sync completed successfully");
            } else {
                ESP_LOGW(TAG, "Force NTP sync failed");
            }
        }
        
        vTaskDelete(nullptr);
    }, "force_ntp_sync", 6144, this, 5, nullptr);
}

bool TimeSyncManager::IsRtcWorking() const {
    if (!rtc_) {
        ESP_LOGE(TAG, "RTC not initialized");
        return false;
    }
    
    // 检查RTC是否运行
    if (!rtc_->IsRunning()) {
        ESP_LOGE(TAG, "RTC not running");
        return false;
    }
    
    // 检查RTC时间是否有效（不是默认值）
    struct tm rtc_time;
    if (!rtc_->GetTime(&rtc_time)) {
        ESP_LOGE(TAG, "Failed to read RTC time");
        return false;
    }
    
    // 检查年份是否合理（2020年以后）
    if (rtc_time.tm_year + 1900 < 2020) {
        ESP_LOGE(TAG, "RTC time is too old");
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
    
    // 优化同步策略：更快速的重试和更长的总尝试时间
    int retry_count = 0;
    const int max_retries = 5; // 增加重试次数
    const int base_interval_ms = 10000; // 基础间隔10秒
    
    while (retry_count < max_retries) {
        // 检查系统是否空闲
        if (IsSystemIdleForSync()) {
            ESP_LOGI(TAG, "System ready for NTP sync (attempt %d/%d)", retry_count + 1, max_retries);
            
            // 执行NTP同步
            bool sync_success = false;
            if (ntp_sync_) {
                sync_success = ntp_sync_->SyncTime([this](bool success, const std::string& message) {
                    OnNtpSyncComplete(success, message);
                });
            }
            
            if (sync_success) {
                ESP_LOGI(TAG, "Smart NTP sync completed successfully on attempt %d", retry_count + 1);
                return;
            } else {
                ESP_LOGW(TAG, "NTP sync failed on attempt %d, will retry", retry_count + 1);
            }
        } else {
            ESP_LOGI(TAG, "System busy, will retry NTP sync (attempt %d/%d)", retry_count + 1, max_retries);
        }
        
        retry_count++;
        if (retry_count < max_retries) {
            // 使用递增的重试间隔：10s, 15s, 20s, 25s
            int retry_interval = base_interval_ms + (retry_count * 5000);
            ESP_LOGI(TAG, "Waiting %d seconds before next sync attempt", retry_interval / 1000);
            vTaskDelay(pdMS_TO_TICKS(retry_interval));
        }
    }
    
    ESP_LOGW(TAG, "Smart NTP sync failed after %d attempts, will rely on next trigger", max_retries);
}

bool TimeSyncManager::IsSystemIdleForSync() {
    // 简化的系统空闲检查，放宽条件以提高同步成功率
    
    // 检查1：内存使用情况（降低阈值）
    size_t free_heap = esp_get_free_heap_size();
    
    if (free_heap < 30000) { // 降低到30KB，提高同步机会
        ESP_LOGD(TAG, "Low memory detected: %zu bytes free", free_heap);
        return false;
    }
    
    // 检查2：RTC基本可用性（简化检查）
    if (rtc_) {
        // 尝试快速读取RTC状态
        struct tm test_time;
        if (!rtc_->GetTime(&test_time)) {
            ESP_LOGD(TAG, "RTC appears busy or unavailable");
            return false;
        }
    }
    
    // 检查3：简化时间窗口检查（放宽限制）
    // 只避免在整分钟的前后2秒进行同步，减少冲突
    time_t now;
    time(&now);
    struct tm* tm_now = localtime(&now);
    
    if (tm_now->tm_sec <= 2 || tm_now->tm_sec >= 58) {
        ESP_LOGD(TAG, "Avoiding sync near minute boundary (current second: %d)", tm_now->tm_sec);
        return false;
    }
    
    ESP_LOGI(TAG, "System idle for sync: heap=%zu bytes, sec=%d", free_heap, tm_now->tm_sec);
    return true;
}

void TimeSyncManager::OnNtpSyncComplete(bool success, const std::string& message) {
    ESP_LOGI(TAG, "NTP sync completed: %s - %s", success ? "SUCCESS" : "FAILED", message.c_str());
    
    if (success) {
        ESP_LOGI(TAG, "Time synchronized successfully");
        
        // 关键修复：NTP同步完成后确保时区设置正确
        setenv("TZ", "CST-8", 1);
        tzset();
        ESP_LOGI(TAG, "Timezone confirmed in TimeSyncManager after NTP sync: CST-8 (UTC+8)");
        
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

// 统一的时间获取函数 - 所有其他模块都应该使用这个函数
bool TimeSyncManager::GetUnifiedTime(struct tm* timeinfo) {
    if (!timeinfo) {
        ESP_LOGE(TAG, "GetUnifiedTime: timeinfo is null");
        return false;
    }
    
    bool success = false;
    
    // 优先使用RTC时间（如果可用且有效）
    if (rtc_ && IsRtcWorking()) {
        success = rtc_->GetTime(timeinfo);
        if (success) {
            ESP_LOGV(TAG, "GetUnifiedTime: Using RTC time: %04d-%02d-%02d %02d:%02d:%02d", 
                     timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
                     timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
            return true;
        }
    }
    
    // 回退到系统时间
    time_t current_time;
    time(&current_time);
    struct tm* system_timeinfo = localtime(&current_time);
    if (system_timeinfo) {
        *timeinfo = *system_timeinfo;
        ESP_LOGV(TAG, "GetUnifiedTime: Using system time: %04d-%02d-%02d %02d:%02d:%02d", 
                 timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
                 timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
        return true;
    }
    
    ESP_LOGE(TAG, "GetUnifiedTime: Failed to get time from both RTC and system");
    return false;
}

// 统一的时间戳获取函数
bool TimeSyncManager::GetUnifiedTimestamp(time_t* timestamp) {
    if (!timestamp) {
        ESP_LOGE(TAG, "GetUnifiedTimestamp: timestamp is null");
        return false;
    }
    
    // 优先使用RTC时间戳（如果可用且有效）
    if (rtc_ && IsRtcWorking()) {
        if (rtc_->GetTime(timestamp)) {
            ESP_LOGV(TAG, "GetUnifiedTimestamp: Using RTC timestamp: %lld", (long long)*timestamp);
            return true;
        }
    }
    
    // 回退到系统时间戳
    time(timestamp);
    ESP_LOGV(TAG, "GetUnifiedTimestamp: Using system timestamp: %lld", (long long)*timestamp);
    return true;
}

// 打印当前时间状态（用于调试）
void TimeSyncManager::PrintCurrentTimeStatus() {
    ESP_LOGI(TAG, "=== Current Time Status Debug ===");
    
    // 确保时区设置正确
    setenv("TZ", "CST-8", 1);
    tzset();
    
    // 系统时间
    time_t system_time;
    time(&system_time);
    struct tm* system_tm = localtime(&system_time);
    if (system_tm) {
        ESP_LOGI(TAG, "System time: %04d-%02d-%02d %02d:%02d:%02d (timestamp: %lld)", 
                 system_tm->tm_year + 1900, system_tm->tm_mon + 1, system_tm->tm_mday,
                 system_tm->tm_hour, system_tm->tm_min, system_tm->tm_sec,
                 (long long)system_time);
    }
    
    // RTC时间
    if (rtc_) {
        struct tm rtc_tm;
        if (rtc_->GetTime(&rtc_tm)) {
            ESP_LOGI(TAG, "RTC time: %04d-%02d-%02d %02d:%02d:%02d", 
                     rtc_tm.tm_year + 1900, rtc_tm.tm_mon + 1, rtc_tm.tm_mday,
                     rtc_tm.tm_hour, rtc_tm.tm_min, rtc_tm.tm_sec);
            
            time_t rtc_timestamp;
            if (rtc_->GetTime(&rtc_timestamp)) {
                ESP_LOGI(TAG, "RTC timestamp: %lld", (long long)rtc_timestamp);
                
                // 计算时差
                int time_diff = abs((int)(system_time - rtc_timestamp));
                ESP_LOGI(TAG, "Time difference (system - RTC): %d seconds", time_diff);
            }
        } else {
            ESP_LOGW(TAG, "Failed to read RTC time");
        }
    } else {
        ESP_LOGW(TAG, "RTC not available");
    }
    
    // 统一时间
    struct tm unified_tm;
    if (GetUnifiedTime(&unified_tm)) {
        ESP_LOGI(TAG, "Unified time: %04d-%02d-%02d %02d:%02d:%02d", 
                 unified_tm.tm_year + 1900, unified_tm.tm_mon + 1, unified_tm.tm_mday,
                 unified_tm.tm_hour, unified_tm.tm_min, unified_tm.tm_sec);
    }
    
    ESP_LOGI(TAG, "=== End Time Status Debug ===");
} 