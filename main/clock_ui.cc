#include "clock_ui.h"
#include <esp_log.h>
#include <time.h>
#include <sys/time.h>
#include "display.h"
#include "pcf8563_rtc.h"
#include "alarm_manager.h"

static const char* TAG = "ClockUI";

ClockUI::ClockUI() : 
    display_(nullptr),
    rtc_(nullptr),
    initialized_(false),
    is_visible_(false),
    notification_visible_(false),
    update_timer_(nullptr),
    last_displayed_hour_(-1),
    last_displayed_minute_(-1),
    last_displayed_day_(-1),
    last_notification_state_(false) {
    ESP_LOGI(TAG, "ClockUI created (simplified version)");
}

ClockUI::~ClockUI() {
    Hide();
}

bool ClockUI::Initialize(Display* display) {
    if (!display) {
        ESP_LOGE(TAG, "Display pointer is null");
        return false;
    }
    
    display_ = display;
    initialized_ = true;
    
    ESP_LOGI(TAG, "ClockUI initialized successfully");
    return true;
}

void ClockUI::Show() {
    if (!initialized_) {
        ESP_LOGE(TAG, "ClockUI not initialized");
        return;
    }
    
    if (is_visible_) {
        ESP_LOGW(TAG, "ClockUI already visible");
        return;
    }
    
    is_visible_ = true;
    UpdateClockDisplay();
    ESP_LOGI(TAG, "Clock UI shown");
}

void ClockUI::Hide() {
    if (!is_visible_) {
        return;
    }
    
    is_visible_ = false;
    notification_visible_ = false;
    
    ESP_LOGI(TAG, "Clock UI hidden");
}

void ClockUI::SetRtc(Pcf8563Rtc* rtc) {
    rtc_ = rtc;
}

void ClockUI::SetNextAlarm(const std::string& alarm_text) {
    current_alarm_text_ = alarm_text;
    if (is_visible_) {
        UpdateClockDisplay();
    }
}

void ClockUI::ShowAlarmNotification(const std::string& notification) {
    ESP_LOGI(TAG, "ShowAlarmNotification: %s", notification.c_str());
    current_notification_ = notification;
    notification_visible_ = true;
    if (is_visible_) {
        UpdateClockDisplay();
    }
}

void ClockUI::HideAlarmNotification() {
    ESP_LOGI(TAG, "HideAlarmNotification");
    notification_visible_ = false;
    current_notification_.clear();
    if (is_visible_) {
        UpdateClockDisplay();
    }
}

void ClockUI::UpdateTimerCallback(void* timer) {
    // Simplified - do nothing
}

void ClockUI::UpdateClockDisplay() {
    if (!display_ || !is_visible_) {
        return;
    }
    
    int hour, minute, second, day, month, year;
    if (!GetCurrentTime(hour, minute, second, day, month, year)) {
        ESP_LOGW(TAG, "Failed to get current time");
        return;
    }
    
    // 格式化时间显示 - 参考图片格式
    char time_str[64];
    bool is_pm = hour >= 12;
    int display_hour = hour;
    
    // 转换为12小时制
    if (display_hour == 0) {
        display_hour = 12;
    } else if (display_hour > 12) {
        display_hour -= 12;
    }
    
    // 格式化日期显示
    char date_str[32];
    const char* weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    
    // 计算星期几
    time_t now = time(NULL);
    struct tm *timeinfo = localtime(&now);
    int weekday = timeinfo->tm_wday;
    
    snprintf(date_str, sizeof(date_str), "%d/%d %s", month, day, weekdays[weekday]);
    
    // 构建完整的时钟显示文本 - 模仿图片中的格式
    if (notification_visible_ && !current_notification_.empty()) {
        // 有通知时的显示格式
        snprintf(time_str, sizeof(time_str), "%d:%02d %s | %s | 🔔%s", 
                display_hour, minute, is_pm ? "PM" : "AM", 
                date_str, current_notification_.c_str());
    } else if (!current_alarm_text_.empty()) {
        // 有闹钟时的显示格式
        snprintf(time_str, sizeof(time_str), "%d:%02d %s | %s | ⏰%s", 
                display_hour, minute, is_pm ? "PM" : "AM", 
                date_str, current_alarm_text_.c_str());
    } else {
        // 标准时钟显示格式
        snprintf(time_str, sizeof(time_str), "%d:%02d %s | %s", 
                display_hour, minute, is_pm ? "PM" : "AM", date_str);
    }
    
    // 使用SetStatus方法显示时钟（这是application.cc中使用的方法）
    display_->SetStatus(time_str);
    
    ESP_LOGI(TAG, "Clock display updated: %s", time_str);
}

bool ClockUI::GetCurrentTime(int& hour, int& minute, int& second, int& day, int& month, int& year) {
    // 优先使用RTC时间
    if (rtc_) {
        struct tm time_tm;
        if (rtc_->GetTime(&time_tm)) {
            hour = time_tm.tm_hour;
            minute = time_tm.tm_min;
            second = time_tm.tm_sec;
            day = time_tm.tm_mday;
            month = time_tm.tm_mon + 1; // tm_mon is 0-based
            year = time_tm.tm_year + 1900;
            return true;
        }
    }
    
    // 备用：使用系统时间
    time_t now = time(NULL);
    if (now == (time_t)-1) {
        return false;
    }
    
    struct tm *timeinfo = localtime(&now);
    if (!timeinfo) {
        return false;
    }
    
    hour = timeinfo->tm_hour;
    minute = timeinfo->tm_min;
    second = timeinfo->tm_sec;
    day = timeinfo->tm_mday;
    month = timeinfo->tm_mon + 1; // tm_mon is 0-based
    year = timeinfo->tm_year + 1900;
    
    return true;
}

void ClockUI::RenderClockToCanvas() {
    // Simplified - do nothing
}

void ClockUI::CreateClockImage(unsigned char* buffer, int width, int height) {
    // Simplified - do nothing
}

void ClockUI::DrawText(unsigned char* buffer, int buf_width, int buf_height, 
                      const char* text, int x, int y, unsigned short color, int font_size) {
    // Simplified - do nothing
}

void ClockUI::DrawFilledRect(unsigned char* buffer, int buf_width, int buf_height,
                            int x, int y, int width, int height, unsigned short color, unsigned char alpha) {
    // Simplified - do nothing
}

unsigned short ClockUI::RGB888toRGB565(unsigned char r, unsigned char g, unsigned char b) {
    return 0; // Simplified
} 