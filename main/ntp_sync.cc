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
    
    // 设置时区为中国北京时间 (UTC+8)
    // 使用明确的时区格式：CST表示中国标准时间，相对于UTC+8
    // setenv("TZ", "CST-8", 1);
    // tzset();
    
    ESP_LOGI(TAG, "Timezone set to Beijing (UTC+8)");
    
    // 不立即配置SNTP，等待网络连接
    ESP_LOGI(TAG, "NTP sync initialized, timezone set to Beijing (UTC+8). SNTP will be configured after network connection.");
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
}

bool NtpSync::SyncTime(NtpSyncCallback callback) {
    ESP_LOGI(TAG, "Starting NTP time sync");
    
    // 配置SNTP（首次或重新配置）
    ConfigureSntp();
    
    // 启动SNTP
    esp_sntp_init();
    
    // 等待时间同步
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 10;
    
    while (timeinfo.tm_year < (2020 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for NTP sync... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    
    if (timeinfo.tm_year < (2020 - 1900)) {
        ESP_LOGE(TAG, "Failed to sync time from NTP servers");
        if (callback) {
            callback(false, "NTP同步失败：无法连接到NTP服务器");
        }
        return false;
    }
    
    // NTP同步成功
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "NTP time synced: %s", time_str);
    
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