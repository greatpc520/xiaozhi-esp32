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
    void SetNextAlarm(const char* next_alarm_time);
    void SetNextAlarm(const std::string& alarm_text);  // 重载版本
    
    // 显示/隐藏闹钟通知
    void ShowAlarmNotification(const std::string& notification);
    void HideAlarmNotification();
    
    // 更新时钟显示
    void UpdateClockDisplay();
    
    // 刷新下一次闹钟显示（闹钟触发后调用）
    void RefreshNextAlarmDisplay();
    
    // 新增：设置闹钟表情
    void SetAlarmEmotion(const std::string& emotion);
    
    // 新增：壁纸和动画相关方法
    void SetWallpaper(const char* image_path_or_url);   // 设置壁纸（支持本地路径或URL）
    void SetAnimation(const char* animation_path);      // 设置动画内容
    void ShowAnimation(bool show);                      // 显示/隐藏动画
    void UpdateAnimation();                             // 更新动画帧
    void TestWallpaperWithColor();                      // 测试壁纸功能（纯色背景）
    
    // 新增：壁纸配置管理
    void SetSolidColorWallpaper(uint32_t color);        // 设置纯色壁纸
    void SetImageWallpaper(const char* image_name);     // 设置图片壁纸（SD卡）
    void SetNetworkWallpaper(const char* url);          // 设置网络壁纸
    void ClearWallpaper();                              // 清除壁纸
    void SaveWallpaperConfig();                         // 保存壁纸配置
    void LoadWallpaperConfig();                         // 加载壁纸配置
    
    // 新增：只保存配置而不立即应用的函数（用于MCP工具）
    void SaveSolidColorWallpaperConfig(uint32_t color);     // 只保存纯色壁纸配置
    void SaveImageWallpaperConfig(const char* image_name);  // 只保存图片壁纸配置  
    void SaveNetworkWallpaperConfig(const char* url);       // 只保存网络壁纸配置
    void SaveClearWallpaperConfig();                        // 只保存清除壁纸配置
    
    // 新增：动画管理
    void SetAnimationFromSD(const char* anim_name);     // 从SD卡设置动画
    void SetAnimationFromNetwork(const char* url);      // 从网络设置动画
    void ClearAnimation();                              // 清除动画
    void SaveAnimationConfig();                         // 保存动画配置
    void LoadAnimationConfig();                         // 加载动画配置
    
    // 新增：只保存动画配置而不立即应用的函数（用于MCP工具）
    void SaveAnimationFromSDConfig(const char* anim_name);    // 只保存SD动画配置
    void SaveAnimationFromNetworkConfig(const char* url);     // 只保存网络动画配置
    
    // 新增：显示听状态动画
    void ShowListenAnimation();                               // 在animation_label_中显示听状态图片0
    
    // 新增：JPG解码相关
    bool DecodeJpgFromSD(const char* filename, uint8_t** rgb565_data, int* width, int* height);
    bool DownloadAndDecodeJpg(const char* url, const char* local_filename);
    
    // 新增：RGB转换函数
    uint16_t RGB888ToRGB565(uint8_t r, uint8_t g, uint8_t b);
    
    // 新增：时间解析函数
    bool ParseTimeString(const char* time_str, struct tm* tm_out);
    
    // 时间显示更新函数
    void UpdateTimeDisplay();
    
    // 强制更新方法（用于NTP同步后立即更新）
    void ForceUpdateDisplay();

private:
    Display* display_;
    Pcf8563Rtc* rtc_;
    bool initialized_;
    bool is_visible_;
    bool notification_visible_;
    void* update_timer_;         // 更新定时器（简化版本）
    lv_obj_t* clock_container_;      // 时钟主容器  
    lv_obj_t* time_label_;          // 大字体时间标签
    lv_obj_t* time_am_pm_label_;    // 小字体时间标签
    lv_obj_t* date_label_;          // 日期标签
    lv_obj_t* alarm_label_;         // 闹钟标签
    lv_obj_t* notification_label_;  // 通知标签
    lv_obj_t* alarm_emotion_label_; // 闹钟表情标签
    lv_obj_t* alarm_icon_label_;    // 闹钟图标标签（仅显示铃铛图标）
    lv_obj_t* alarm_text_label_;    // 闹钟文字标签（仅显示文字）
    lv_obj_t* notification_icon_label_; // 通知图标标签
    lv_obj_t* notification_text_label_; // 通知文字标签
    
    // 新增：壁纸和动画标签
    lv_obj_t* wallpaper_img_;       // 全屏壁纸图片
    lv_obj_t* animation_label_;     // 64*64动画标签
    
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
    
    // 新增：壁纸和动画相关状态
    std::string current_wallpaper_;      // 当前壁纸路径
    std::string current_animation_;      // 当前动画路径
    bool animation_visible_;             // 动画是否可见
    int animation_frame_;                // 当前动画帧
    void* animation_timer_;              // 动画定时器
    
    // 新增：壁纸配置状态
    enum WallpaperType {
        WALLPAPER_NONE = 0,
        WALLPAPER_SOLID_COLOR,
        WALLPAPER_SD_IMAGE,
        WALLPAPER_NETWORK_IMAGE
    };
    WallpaperType wallpaper_type_;       // 壁纸类型
    uint32_t wallpaper_color_;           // 纯色壁纸颜色
    std::string wallpaper_image_name_;   // 图片壁纸文件名
    std::string wallpaper_network_url_;  // 网络壁纸URL
    
    // 配置文件路径
    static const char* WALLPAPER_CONFIG_FILE;
    static const char* ANIMATION_CONFIG_FILE;
    
    // UI创建和管理方法
    void CreateClockUI();
    void DestroyClockUI();
    void UpdateTimeLabel();
    void UpdateDateLabel();
    void UpdateAlarmLabel();
    void UpdateNotificationLabel();
    void UpdateAlarmEmotionLabel();  // 新增：更新闹钟表情标签
    
    // 强制更新方法（用于首次显示）
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