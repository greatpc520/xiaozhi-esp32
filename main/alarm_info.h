#ifndef ALARM_INFO_H
#define ALARM_INFO_H

#include <string>
#include <time.h>

// 闹钟信息结构体
struct AlarmInfo {
    int id;                          // 闹钟ID
    std::string description;         // 闹钟描述
    time_t trigger_time;             // 触发时间
    bool enabled;                    // 是否启用
    bool repeat_daily;               // 是否每日重复
    int hour;                        // 小时 (0-23)
    int minute;                      // 分钟 (0-59)
    std::string sound_file;          // 闹钟音效文件路径
    
    AlarmInfo() : id(0), trigger_time(0), enabled(true), repeat_daily(false), hour(0), minute(0) {}
};

#endif // ALARM_INFO_H 