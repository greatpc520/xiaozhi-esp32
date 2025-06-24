#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

#include <string>
#include <vector>
#include <functional>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <freertos/semphr.h>
#include "pcf8563_rtc.h"
#include "alarm_info.h"

// 闹钟管理器类
class AlarmManager {
public:
    static AlarmManager& GetInstance() {
        static AlarmManager instance;
        return instance;
    }
    
    // 删除拷贝构造函数和赋值运算符
    AlarmManager(const AlarmManager&) = delete;
    AlarmManager& operator=(const AlarmManager&) = delete;
    
    // 初始化
    bool Initialize();
    
    // 设置闹钟
    int SetAlarm(int hour, int minute, const std::string& description, const std::vector<int>& weekdays = {}, bool is_tomorrow = false);
    
    // 设置相对时间闹钟（分钟后触发）
    int SetRelativeAlarm(int minutes_later, const std::string& description);
    
    // 从语音命令创建闹钟
    int CreateAlarmFromVoice(const std::string& voice_command);
    
    // 获取所有闹钟
    std::vector<AlarmInfo> GetAllAlarms();
    
    // 取消闹钟
    bool CancelAlarm(int alarm_id);
    
    // 取消所有闹钟
    void CancelAllAlarms();
    
    // 获取下一个闹钟
    AlarmInfo GetNextAlarm();
    
    // 获取当前时间字符串
    std::string GetCurrentTimeString() const;
    
    // 闹钟回调设置
    void SetAlarmCallback(std::function<void(const AlarmInfo&)> callback);
    
    // 检查闹钟触发（内部调用）
    void CheckAlarms();
    
    // 处理触发的闹钟（异步调用）
    void ProcessTriggeredAlarms(const std::vector<int>& triggered_ids, time_t trigger_time);
    
    // 设置RTC实例
    void SetRtc(Pcf8563Rtc* rtc);
    
    // 获取RTC时间
    bool GetRtcTime(time_t* timestamp) const;
    
    // 保存闹钟到NVS
    bool SaveAlarmsToNVS();
    
    // 解析语音命令中的时间（供MCP工具使用）
    bool ParseVoiceCommand(const std::string& command, int& hour, int& minute,
                          int& minutes_later, std::string& description, 
                          bool& is_relative, std::vector<int>& weekdays, bool& is_tomorrow);
    
    // 设置默认声音类型
    void SetDefaultSoundType(AlarmSoundType sound_type);
    
    // 获取默认声音类型
    AlarmSoundType GetDefaultSoundType() const;
    
    // 清理过期的闹钟
    int RemoveExpiredAlarms();
    
    // 检查时间是否正常（年份大于2020）
    bool IsTimeValid() const;

private:
    AlarmManager();
    ~AlarmManager();
    
    bool initialized_;
    std::vector<AlarmInfo> alarms_;
    static int next_alarm_id_;
    TimerHandle_t check_timer_;
    std::function<void(const AlarmInfo&)> alarm_callback_;
    Pcf8563Rtc* rtc_;
    AlarmSoundType default_sound_type_;
    SemaphoreHandle_t alarms_mutex_;  // 保护alarms_容器的互斥锁
    
    // 内部辅助方法
    void LoadAlarmsFromNVS();
    static void CheckTimerCallback(TimerHandle_t timer);
    int GenerateAlarmId();
    time_t CalculateNextTriggerTime(int hour, int minute, bool is_tomorrow = false);
    bool IsValidTime(int hour, int minute);
    bool ParseRelativeTime(const std::string& time_str, int& minutes);
    bool ParseAbsoluteTime(const std::string& time_str, int& hour, int& minute, bool& is_tomorrow);
};

#endif // ALARM_MANAGER_H 