#include "ntp_sync.h"
#include <esp_log.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include <sys/time.h>

#define TAG "NTP_SYNC"

// 中国NTP服务器列表
static const char* ntp_servers[] = {
    "ntp.aliyun.com",        // 阿里云NTP服务器
    "time.pool.aliyun.com",  // 阿里云时间池
    "cn.pool.ntp.org",       // 中国NTP池
    "pool.ntp.org"           // 全球NTP池（备用）
};

NtpSync::NtpSync() 
    : rtc_(nullptr), is_synced_(false), last_sync_time_(0) {
}

NtpSync::~NtpSync() {
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
}

bool NtpSync::Initialize() {
    ESP_LOGI(TAG, "Initializing NTP sync (network-independent)");
    
    // 注意：时区由TimeSyncManager统一设置，这里不重复设置
    // 确保时区环境变量已正确设置
    char* tz = getenv("TZ");
    if (tz) {
        ESP_LOGI(TAG, "Current timezone setting: %s", tz);
    } else {
        ESP_LOGW(TAG, "No timezone setting found in environment");
    }
    
    ESP_LOGI(TAG, "NTP sync initialized. SNTP will be configured after network connection.");
    return true;
}

void NtpSync::SetRtc(Pcf8563Rtc* rtc) {
    rtc_ = rtc;
}

void NtpSync::ConfigureSntp() {
    // 停止之前的SNTP服务（仅在网络初始化后）
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    
    // 设置操作模式为轮询模式
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    
    // 设置同步模式为立即更新
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    
    // 添加NTP服务器
    for (int i = 0; i < sizeof(ntp_servers) / sizeof(ntp_servers[0]); i++) {
        esp_sntp_setservername(i, ntp_servers[i]);
        ESP_LOGI(TAG, "Added NTP server %d: %s", i, ntp_servers[i]);
    }
    
    // 设置同步间隔（1小时）
    esp_sntp_set_sync_interval(3600000);  // 毫秒
    
    // 关键修复：SNTP配置后重新确认时区设置
    // 因为ESP32的SNTP库可能会重置时区设置
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "Timezone re-confirmed after SNTP configuration: CST-8 (UTC+8)");
}

bool NtpSync::SyncTime(NtpSyncCallback callback) {
    ESP_LOGI(TAG, "Starting NTP time sync");
    
    // 配置SNTP（首次或重新配置）
    ConfigureSntp();
    
    // 关键修复：启动SNTP前再次确认时区设置
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "Timezone confirmed before SNTP start: CST-8 (UTC+8)");
    
    // 启动SNTP
    esp_sntp_init();
    
    // 等待时间同步 - 使用更可靠的同步状态检查
    int retry = 0;
    const int retry_count = 10;
    sntp_sync_status_t sync_status = SNTP_SYNC_STATUS_RESET;
    
    while (sync_status != SNTP_SYNC_STATUS_COMPLETED && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for NTP sync... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        
        sync_status = esp_sntp_get_sync_status();
        ESP_LOGI(TAG, "SNTP sync status: %d", (int)sync_status);
        
        // 检查当前时间是否合理
        time_t now;
        time(&now);
        struct tm timeinfo;
        
        // 关键修复：每次获取时间前确认时区设置
        setenv("TZ", "CST-8", 1);
        tzset();
        
        localtime_r(&now, &timeinfo);
        
        // 调试：输出详细的时间信息
        ESP_LOGD(TAG, "Sync attempt %d: Raw timestamp=%lld, converted time=%04d-%02d-%02d %02d:%02d:%02d", 
                 retry, (long long)now,
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        
        // 如果同步状态显示已完成，验证时间的合理性
        if (sync_status == SNTP_SYNC_STATUS_COMPLETED) {
            // 验证年份是否合理（2025年）
            if (timeinfo.tm_year + 1900 >= 2025 && timeinfo.tm_year + 1900 <= 2026) {
                // 验证月份是否合理（1月前后）
                if (timeinfo.tm_mon >= 0 && timeinfo.tm_mon <= 11) {
                    ESP_LOGI(TAG, "NTP sync completed with reasonable time");
                    break;
                } else {
                    ESP_LOGW(TAG, "NTP sync completed but month (%d) seems unreasonable", timeinfo.tm_mon + 1);
                }
            } else {
                ESP_LOGW(TAG, "NTP sync completed but year (%d) seems unreasonable", timeinfo.tm_year + 1900);
                // 强制重新同步
                esp_sntp_stop();
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                esp_sntp_init();
                sync_status = SNTP_SYNC_STATUS_RESET;
            }
        }
    }
    
    if (sync_status != SNTP_SYNC_STATUS_COMPLETED) {
        ESP_LOGE(TAG, "Failed to sync time from NTP servers after %d attempts", retry_count);
        if (callback) {
            callback(false, "NTP同步失败：无法连接到NTP服务器");
        }
        return false;
    }
    
    // 关键修复：同步成功后最后一次确认时区设置
    setenv("TZ", "CST-8", 1);
    tzset();
    
    // 详细调试：检查最终同步后的时间
    time_t now;
    time(&now);
    ESP_LOGI(TAG, "Final NTP sync - Raw UTC timestamp: %lld", (long long)now);
    
    // 分别用UTC和本地时间转换来验证
    struct tm* utc_tm = gmtime(&now);
    struct tm* local_tm = localtime(&now);
    
    if (utc_tm) {
        ESP_LOGI(TAG, "UTC time: %04d-%02d-%02d %02d:%02d:%02d", 
                 utc_tm->tm_year + 1900, utc_tm->tm_mon + 1, utc_tm->tm_mday,
                 utc_tm->tm_hour, utc_tm->tm_min, utc_tm->tm_sec);
    }
    
    if (local_tm) {
        ESP_LOGI(TAG, "Local time (CST-8): %04d-%02d-%02d %02d:%02d:%02d", 
                 local_tm->tm_year + 1900, local_tm->tm_mon + 1, local_tm->tm_mday,
                 local_tm->tm_hour, local_tm->tm_min, local_tm->tm_sec);
    }
    
    // 最终时间验证和显示
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    
    // 最后一次验证时间合理性
    if (timeinfo.tm_year + 1900 < 2025 || timeinfo.tm_year + 1900 > 2026) {
        ESP_LOGE(TAG, "Final synced time year (%d) is unreasonable - possible timezone issue", timeinfo.tm_year + 1900);
        if (callback) {
            callback(false, "NTP同步失败：获取到的时间不合理");
        }
        return false;
    }
    
    if (timeinfo.tm_mon < 0 || timeinfo.tm_mon > 11) {
        ESP_LOGE(TAG, "Final synced time month (%d) is invalid", timeinfo.tm_mon + 1);
        if (callback) {
            callback(false, "NTP同步失败：获取到的月份无效");
        }
        return false;
    }
    
    ESP_LOGI(TAG, "NTP time synced (with timezone CST-8): %s", time_str);
    
    // 同步到RTC
    if (rtc_) {
        if (rtc_->SyncSystemTimeToRtc()) {
            ESP_LOGI(TAG, "System time synced to RTC successfully");
        } else {
            ESP_LOGW(TAG, "Failed to sync system time to RTC");
        }
    }
    
    // 更新状态
    is_synced_ = true;
    last_sync_time_ = now;
    
    if (callback) {
        std::string message = "NTP时间同步成功：" + std::string(time_str);
        callback(true, message);
    }
    
    return true;
}

void NtpSync::SyncTimeAsync(NtpSyncCallback callback) {
    // 创建同步任务参数
    SyncTaskParams* params = new SyncTaskParams{this, callback};
    
    // 创建异步任务
    xTaskCreate(SyncTask, "ntp_sync_task", 4096, params, 5, nullptr);
}

void NtpSync::SyncTask(void* parameters) {
    SyncTaskParams* params = static_cast<SyncTaskParams*>(parameters);
    
    if (params && params->ntp_sync) {
        params->ntp_sync->SyncTime(params->callback);
    }
    
    // 清理参数
    delete params;
    
    // 删除任务
    vTaskDelete(nullptr);
}

bool NtpSync::IsSynced() const {
    return is_synced_;
}

time_t NtpSync::GetLastSyncTime() const {
    return last_sync_time_;
}

int NtpSync::GetTimezoneOffset() const {
    // 北京时间 UTC+8
    return 8 * 3600;  // 8小时的秒数
} 