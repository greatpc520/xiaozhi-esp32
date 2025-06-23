#ifndef TIME_SYNC_MANAGER_H
#define TIME_SYNC_MANAGER_H

#include "pcf8563_rtc.h"
#include "ntp_sync.h"
#include "clock_ui.h"
#include "alarm_manager.h"
#include <functional>
#include <memory>
#include <string>

// 时间同步管理器 - 负责RTC、NTP、闹钟和时钟界面的协调
class TimeSyncManager {
public:
    static TimeSyncManager& GetInstance() {
        static TimeSyncManager instance;
        return instance;
    }
    
    // 删除拷贝构造函数和赋值运算符
    TimeSyncManager(const TimeSyncManager&) = delete;
    TimeSyncManager& operator=(const TimeSyncManager&) = delete;
    
    // 初始化时间同步管理器
    bool Initialize(i2c_master_bus_handle_t i2c_bus);
    
    // 开机时同步时间
    void SyncTimeOnBoot();
    
    // 获取RTC实例
    Pcf8563Rtc* GetRtc() { return rtc_.get(); }
    
    // 获取NTP同步实例
    NtpSync* GetNtpSync() { return ntp_sync_.get(); }
    
    // 获取时钟UI实例
    ClockUI* GetClockUI() { return clock_ui_.get(); }
    
    // 设置同步完成回调
    void SetSyncCallback(std::function<void(bool success, const std::string& message)> callback);
    
    // 手动触发NTP同步
    void TriggerNtpSync();
    
    // 检查RTC是否正常工作
    bool IsRtcWorking() const;
    
    // 获取当前时间（优先从RTC）
    std::string GetCurrentTimeString() const;

private:
    TimeSyncManager();
    ~TimeSyncManager();
    
    std::unique_ptr<Pcf8563Rtc> rtc_;
    std::unique_ptr<NtpSync> ntp_sync_;
    std::unique_ptr<ClockUI> clock_ui_;
    std::function<void(bool, const std::string&)> sync_callback_;
    
    bool initialized_;
    
    // NTP同步完成回调
    void OnNtpSyncComplete(bool success, const std::string& message);
};

#endif // TIME_SYNC_MANAGER_H 