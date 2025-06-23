#ifndef NTP_SYNC_H
#define NTP_SYNC_H

#include "pcf8563_rtc.h"
#include <string>
#include <functional>

// NTP同步回调函数类型
typedef std::function<void(bool success, const std::string& message)> NtpSyncCallback;

// NTP时间同步管理器
class NtpSync {
public:
    NtpSync();
    ~NtpSync();

    // 初始化NTP同步（设置时区为北京时间）
    bool Initialize();
    
    // 设置RTC实例
    void SetRtc(Pcf8563Rtc* rtc);
    
    // 同步网络时间
    bool SyncTime(NtpSyncCallback callback = nullptr);
    
    // 异步同步网络时间
    void SyncTimeAsync(NtpSyncCallback callback = nullptr);
    
    // 检查是否已同步
    bool IsSynced() const;
    
    // 获取上次同步时间
    time_t GetLastSyncTime() const;
    
    // 获取当前时区偏移（秒）
    int GetTimezoneOffset() const;

private:
    Pcf8563Rtc* rtc_;
    bool is_synced_;
    time_t last_sync_time_;
    
    // SNTP配置
    void ConfigureSntp();
    
    // 同步任务
    static void SyncTask(void* parameters);
    
    // 同步参数结构
    struct SyncTaskParams {
        NtpSync* ntp_sync;
        NtpSyncCallback callback;
    };
};

#endif // NTP_SYNC_H 