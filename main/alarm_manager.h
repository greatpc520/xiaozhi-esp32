#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

#include <string>
#include <vector>
#include <functional>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
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
    
    // 设置闹钟（通过语音命令）
    int SetAlarmFromVoice(const std::string& voice_command);
    
    // 设置闹钟（绝对时间）
    int SetAlarm(int hour, int minute, const std::string& description, bool repeat_daily = false, bool is_tomorrow = false);
    
    // 设置闹钟（相对时间，分钟后）
    int SetAlarmRelative(int minutes_later, const std::string& description);
    
    // 获取所有闹钟
    std::vector<AlarmInfo> GetAlarms() const;
    
    // 取消闹钟
    bool CancelAlarm(int alarm_id);
    
    // 取消所有闹钟
    void CancelAllAlarms();
    
    // 获取下一个闹钟信息
    AlarmInfo GetNextAlarm() const;
    
    // 获取当前时间字符串
    std::string GetCurrentTimeString() const;
    
    // 设置闹钟触发回调
    void SetAlarmCallback(std::function<void(const AlarmInfo&)> callback);
    
    // 检查闹钟触发（内部调用）
    void CheckAlarms();
    
    // 处理触发的闹钟（异步调用）
    void ProcessTriggeredAlarms(const std::vector<int>& triggered_ids, time_t trigger_time);
    
    // 设置RTC实例
    void SetRtc(Pcf8563Rtc* rtc);
    
    // 获取RTC当前时间
    bool GetRtcTime(time_t* timestamp) const;

private:
    AlarmManager();
    ~AlarmManager();
    
    std::vector<AlarmInfo> alarms_;
    std::function<void(const AlarmInfo&)> alarm_callback_;
    TimerHandle_t check_timer_;
    static int next_alarm_id_;
    Pcf8563Rtc* rtc_;  // RTC实例
    
    // 从NVS加载闹钟数据
    bool LoadAlarmsFromNVS();
    
    // 保存闹钟数据到NVS
    bool SaveAlarmsToNVS();
    
    // 解析语音命令中的时间
    bool ParseVoiceCommand(const std::string& command, int& hour, int& minute, 
                          int& minutes_later, std::string& description, bool& is_relative, bool& repeat_daily);
    
    // 解析相对时间（如"2小时后"、"30分钟后"）
    bool ParseRelativeTime(const std::string& time_str, int& minutes);
    
    // 解析绝对时间（如"明天8点"、"今天下午3点"）
    bool ParseAbsoluteTime(const std::string& time_str, int& hour, int& minute, bool& is_tomorrow);
    
    // 定时器回调函数
    static void CheckTimerCallback(TimerHandle_t timer);
    
    // 生成新的闹钟ID
    int GenerateAlarmId();
    
    // 计算下一次触发时间
    time_t CalculateNextTriggerTime(int hour, int minute, bool is_tomorrow = false);
    
    // 判断是否为有效的时间格式
    bool IsValidTime(int hour, int minute);
};

#endif // ALARM_MANAGER_H 