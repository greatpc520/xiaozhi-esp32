#ifndef CLOCK_UI_H
#define CLOCK_UI_H

#include <string>
#include <lvgl.h>

// 前向声明
class Display;
class Pcf8563Rtc;

// 简化的时钟UI类
class ClockUI {
public:
    ClockUI();
    ~ClockUI();
    
    // 初始化时钟UI
    bool Initialize(Display* display);
    
    // 显示/隐藏时钟
    void Show();
    void Hide();
    bool IsVisible() const { return is_visible_; }
    
    // 设置字体信息
    void SetFonts(const void* text_font, const void* icon_font, const void* emoji_font);
    
    // 设置RTC实例
    void SetRtc(Pcf8563Rtc* rtc);
    
    // 设置下一个闹钟
    void SetNextAlarm(const std::string& alarm_text);
    
    // 显示/隐藏闹钟通知
    void ShowAlarmNotification(const std::string& notification);
    void HideAlarmNotification();
    
    // 更新时钟显示
    void UpdateClockDisplay();
    
    // 新增：设置闹钟表情
    void SetAlarmEmotion(const std::string& emotion);

private:
    Display* display_;
    Pcf8563Rtc* rtc_;
    bool initialized_;
    bool is_visible_;
    bool notification_visible_;
    void* update_timer_;         // 更新定时器（简化版本）
    lv_obj_t* clock_container_;      // 时钟主容器  
    lv_obj_t* time_label_;          // 大字体时间标签
    lv_obj_t* date_label_;          // 日期标签
    lv_obj_t* alarm_label_;         // 闹钟标签
    lv_obj_t* notification_label_;  // 通知标签
    lv_obj_t* alarm_emotion_label_; // 闹钟表情标签
    lv_obj_t* alarm_icon_label_;    // 闹钟图标标签（仅显示铃铛图标）
    lv_obj_t* alarm_text_label_;    // 闹钟文字标签（仅显示文字）
    lv_obj_t* notification_icon_label_; // 通知图标标签
    lv_obj_t* notification_text_label_; // 通知文字标签
    
    // 字体指针
    const void* text_font_;
    const void* icon_font_;
    const void* emoji_font_;
    
    // 缓存的时间信息
    int last_displayed_hour_;
    int last_displayed_minute_;
    int last_displayed_day_;
    std::string last_alarm_text_;
    std::string last_notification_;
    bool last_notification_state_;
    
    // 当前显示的内容
    std::string current_notification_;
    std::string current_alarm_text_;
    std::string current_alarm_emotion_;  // 新增：当前闹钟表情
    
    // UI创建和管理方法
    void CreateClockUI();
    void DestroyClockUI();
    void UpdateTimeLabel();
    void UpdateDateLabel();
    void UpdateAlarmLabel();
    void UpdateNotificationLabel();
    void UpdateAlarmEmotionLabel();  // 新增：更新闹钟表情标签
    
    // 强制更新方法（用于首次显示）
    void ForceUpdateDisplay();
    void ForceUpdateTimeLabel();
    void ForceUpdateDateLabel();
    
    // 简化的方法声明
    static void UpdateTimerCallback(void* timer);
    void UpdateUI();
    bool GetCurrentTime(int& hour, int& minute, int& second, int& day, int& month, int& year);
    void RenderClockToCanvas();
    void CreateClockImage(unsigned char* buffer, int width, int height);
    void DrawText(unsigned char* buffer, int buf_width, int buf_height, 
                  const char* text, int x, int y, unsigned short color, int font_size);
    void DrawFilledRect(unsigned char* buffer, int buf_width, int buf_height,
                       int x, int y, int width, int height, unsigned short color, unsigned char alpha);
    unsigned short RGB888toRGB565(unsigned char r, unsigned char g, unsigned char b);
    
    // 新增：表情映射辅助方法
    std::string GetEmotionForAlarmType(const std::string& alarm_text);
};

#endif // CLOCK_UI_H 