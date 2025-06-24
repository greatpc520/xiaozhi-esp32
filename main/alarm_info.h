#ifndef ALARM_INFO_H
#define ALARM_INFO_H

#include <string>
#include <vector>

// 闹钟声音类型
enum class AlarmSoundType {
    LOCAL_MUSIC = 0,    // 本地音乐 /sdcard/test3.p3
    NETWORK_MUSIC = 1,  // 网络音乐 http://www.replime.cn/a1.p3
    AI_MUSIC = 2        // AI音乐（随便播放一首歌曲）
};

// 闹钟信息结构体
struct AlarmInfo {
    int id;
    std::string description;         // 闹钟描述
    int hour;                        // 小时 (0-23)
    int minute;                      // 分钟 (0-59)
    std::vector<int> weekdays;        // 0=Sunday, 1=Monday, ..., 6=Saturday
    bool enabled;                    // 是否启用
    AlarmSoundType sound_type;         // 闹钟声音类型
    
    AlarmInfo() : id(0), description(""), hour(0), minute(0), weekdays(), enabled(false), sound_type(AlarmSoundType::NETWORK_MUSIC) {}
    
    AlarmInfo(int h, int m, const std::vector<int>& days, const std::string& desc = "", AlarmSoundType sound = AlarmSoundType::NETWORK_MUSIC) 
        : id(0), description(desc), hour(h), minute(m), weekdays(days), enabled(true), sound_type(sound) {}
};

#endif // ALARM_INFO_H 