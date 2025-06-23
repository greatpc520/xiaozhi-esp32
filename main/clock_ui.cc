#include "clock_ui.h"
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <time.h>
#include <sys/time.h>
#include "display.h"
#include "pcf8563_rtc.h"
#include "alarm_manager.h"
#include <lvgl.h>

static const char* TAG = "ClockUI";

// 声明可用的普惠字体
// LV_FONT_DECLARE(font_puhui_30_4);  // 最大字体，用于时间
LV_FONT_DECLARE(font_puhui_20_4);  // 中等字体
// LV_FONT_DECLARE(font_puhui_16_4);  // 用于日期
// LV_FONT_DECLARE(font_puhui_14_1);  // 最小字体，用于AM/PM

ClockUI::ClockUI() : 
    display_(nullptr),
    rtc_(nullptr),
    initialized_(false),
    is_visible_(false),
    notification_visible_(false),
    update_timer_(nullptr),
    clock_container_(nullptr),
    time_label_(nullptr),
    date_label_(nullptr),
    alarm_label_(nullptr),
    notification_label_(nullptr),
    last_displayed_hour_(-1),
    last_displayed_minute_(-1),
    last_displayed_day_(-1),
    last_notification_state_(false) {
    ESP_LOGI(TAG, "ClockUI created (graphical version)");
}

ClockUI::~ClockUI() {
    Hide();
    DestroyClockUI();
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

void ClockUI::CreateClockUI() {
    if (clock_container_) {
        return; // 已经创建
    }
    
    ESP_LOGI(TAG, "Creating lightweight clock UI components");
    
    // 重置看门狗定时器
    esp_task_wdt_reset();
    
    // 获取当前屏幕
    lv_obj_t* screen = lv_screen_active();
    
    // 创建时钟主容器（轻量级）
    lv_obj_t* container = lv_obj_create(screen);
    clock_container_ = container;
    
    // 设置容器为全屏，但使用更少的样式
    lv_obj_set_size(container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    
    // 让出CPU时间给其他任务
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(1));
    
    // 创建时间标签（简化版本）
    lv_obj_t* time_lbl = lv_label_create(container);
    time_label_ = time_lbl;
    lv_obj_set_style_text_color(time_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(time_lbl, &font_puhui_20_4, 0);
    lv_obj_set_style_text_align(time_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(time_lbl, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(time_lbl, LV_OPA_COVER, 0);
    lv_obj_set_pos(time_lbl, 0, LV_VER_RES / 3 - 40);
    lv_obj_set_size(time_lbl, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_label_set_text(time_lbl, "9:23");
    
    esp_task_wdt_reset();
    
    // 创建AM/PM标签（简化版本）
    lv_obj_t* ampm_lbl = lv_label_create(container);
    lv_obj_set_style_text_color(ampm_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ampm_lbl, &font_puhui_20_4, 0);
    lv_obj_set_style_bg_color(ampm_lbl, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ampm_lbl, LV_OPA_COVER, 0);
    lv_obj_set_pos(ampm_lbl, LV_HOR_RES - 70, LV_VER_RES / 3 - 20);
    lv_label_set_text(ampm_lbl, "AM");
    
    // 创建日期标签（简化版本）
    lv_obj_t* date_lbl = lv_label_create(container);
    date_label_ = date_lbl;
    lv_obj_set_style_text_color(date_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(date_lbl, &font_puhui_20_4, 0);
    lv_obj_set_style_text_align(date_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(date_lbl, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(date_lbl, LV_OPA_COVER, 0);
    lv_obj_set_pos(date_lbl, 0, LV_VER_RES / 3 + 40);
    lv_obj_set_size(date_lbl, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_label_set_text(date_lbl, "3/10 Mon");
    
    esp_task_wdt_reset();
    
    // 创建闹钟标签（简化版本，仅在需要时创建复杂样式）
    lv_obj_t* alarm_lbl = lv_label_create(container);
    alarm_label_ = alarm_lbl;
    lv_obj_set_style_text_color(alarm_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(alarm_lbl, &font_puhui_20_4, 0);
    lv_obj_set_style_text_align(alarm_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(alarm_lbl, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_opa(alarm_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(alarm_lbl, 10, 0);
    lv_obj_set_style_pad_all(alarm_lbl, 10, 0);
    lv_obj_set_pos(alarm_lbl, (LV_HOR_RES - 150) / 2, LV_VER_RES / 3 + 90);
    lv_obj_set_size(alarm_lbl, 150, LV_SIZE_CONTENT);
    lv_obj_add_flag(alarm_lbl, LV_OBJ_FLAG_HIDDEN); // 默认隐藏
    
    esp_task_wdt_reset();
    
    // 创建通知标签（简化版本）
    lv_obj_t* notification_lbl = lv_label_create(container);
    notification_label_ = notification_lbl;
    lv_obj_set_style_text_color(notification_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(notification_lbl, &font_puhui_20_4, 0);
    lv_obj_set_style_text_align(notification_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(notification_lbl, lv_color_hex(0xFF8C00), 0);
    lv_obj_set_style_bg_opa(notification_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(notification_lbl, 10, 0);
    lv_obj_set_style_pad_all(notification_lbl, 10, 0);
    lv_obj_set_pos(notification_lbl, (LV_HOR_RES - 180) / 2, LV_VER_RES / 3 + 90);
    lv_obj_set_size(notification_lbl, 180, LV_SIZE_CONTENT);
    lv_obj_add_flag(notification_lbl, LV_OBJ_FLAG_HIDDEN); // 默认隐藏
    
    esp_task_wdt_reset();
    
    ESP_LOGI(TAG, "Lightweight clock UI components created successfully");
}

void ClockUI::DestroyClockUI() {
    if (clock_container_) {
        lv_obj_del((lv_obj_t*)clock_container_);
        clock_container_ = nullptr;
        time_label_ = nullptr;
        date_label_ = nullptr;
        alarm_label_ = nullptr;
        notification_label_ = nullptr;
        ESP_LOGI(TAG, "Clock UI components destroyed");
    }
}

void ClockUI::Show() {
    if (!initialized_) {
        ESP_LOGE(TAG, "ClockUI not initialized");
        return;
    }
    
    if (is_visible_) {
        ESP_LOGD(TAG, "ClockUI already visible, skipping");
        return;
    }
    
    // 立即设置为可见状态，避免重复调用
    is_visible_ = true;
    
    // 异步创建UI组件，避免阻塞主线程
    lv_async_call([](void* param) {
        ClockUI* self = static_cast<ClockUI*>(param);
        if (self && self->is_visible_) {
            self->CreateClockUI();
            self->UpdateClockDisplay();
            ESP_LOGI(TAG, "Clock UI created and shown asynchronously");
        }
    }, this);
}

void ClockUI::Hide() {
    if (!is_visible_) {
        return;
    }
    
    is_visible_ = false;
    notification_visible_ = false;
    
    // 销毁UI组件
    DestroyClockUI();
    
    ESP_LOGI(TAG, "Clock UI hidden");
}

void ClockUI::SetRtc(Pcf8563Rtc* rtc) {
    rtc_ = rtc;
}

void ClockUI::SetNextAlarm(const std::string& alarm_text) {
    current_alarm_text_ = alarm_text;
    if (is_visible_) {
        UpdateAlarmLabel();
    }
}

void ClockUI::ShowAlarmNotification(const std::string& notification) {
    ESP_LOGI(TAG, "ShowAlarmNotification: %s", notification.c_str());
    current_notification_ = notification;
    notification_visible_ = true;
    if (is_visible_) {
        UpdateNotificationLabel();
    }
}

void ClockUI::HideAlarmNotification() {
    ESP_LOGI(TAG, "HideAlarmNotification");
    notification_visible_ = false;
    current_notification_.clear();
    if (is_visible_) {
        UpdateNotificationLabel();
    }
}

void ClockUI::UpdateTimerCallback(void* timer) {
    // Simplified - do nothing
}

void ClockUI::UpdateClockDisplay() {
    if (!is_visible_ || !clock_container_) {
        return;
    }
    
    // 异步更新，避免阻塞主线程
    lv_async_call([](void* param) {
        ClockUI* self = static_cast<ClockUI*>(param);
        if (self && self->is_visible_ && self->clock_container_) {
            self->UpdateTimeLabel();
            self->UpdateDateLabel();
            self->UpdateAlarmLabel();
            self->UpdateNotificationLabel();
            ESP_LOGD(TAG, "Clock display updated asynchronously");
        }
    }, this);
}

void ClockUI::UpdateTimeLabel() {
    if (!time_label_) return;
    
    int hour, minute, second, day, month, year;
    if (!GetCurrentTime(hour, minute, second, day, month, year)) {
        ESP_LOGW(TAG, "Failed to get current time");
        return;
    }
    
    // 检查是否需要更新（避免不必要的UI操作）
    if (last_displayed_hour_ == hour && last_displayed_minute_ == minute) {
        return; // 时间没变化，不需要更新
    }
    
    last_displayed_hour_ = hour;
    last_displayed_minute_ = minute;
    
    // 转换为12小时制
    bool is_pm = hour >= 12;
    int display_hour = hour;
    if (display_hour == 0) {
        display_hour = 12;
    } else if (display_hour > 12) {
        display_hour -= 12;
    }
    
    // 更新时间显示
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%d:%02d", display_hour, minute);
    lv_label_set_text((lv_obj_t*)time_label_, time_str);
    
    // 查找AM/PM标签并更新
    lv_obj_t* container = (lv_obj_t*)clock_container_;
    lv_obj_t* child = lv_obj_get_child(container, 1); // AM/PM标签是第二个子对象
    if (child) {
        lv_label_set_text(child, is_pm ? "PM" : "AM");
    }
}

void ClockUI::UpdateDateLabel() {
    if (!date_label_) return;
    
    int hour, minute, second, day, month, year;
    if (!GetCurrentTime(hour, minute, second, day, month, year)) {
        return;
    }
    
    // 检查日期是否需要更新（避免不必要的UI操作）
    if (last_displayed_day_ == day) {
        return; // 日期没变化，不需要更新
    }
    
    last_displayed_day_ = day;
    
    // 计算星期几
    time_t now = time(NULL);
    struct tm *timeinfo = localtime(&now);
    const char* weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    int weekday = timeinfo->tm_wday;
    
    // 更新日期显示
    char date_str[32];
    snprintf(date_str, sizeof(date_str), "%d/%d %s", month, day, weekdays[weekday]);
    lv_label_set_text((lv_obj_t*)date_label_, date_str);
}

void ClockUI::UpdateAlarmLabel() {
    if (!alarm_label_) return;
    
    bool should_show = !current_alarm_text_.empty() && !notification_visible_;
    
    // 检查状态是否需要更新
    if (last_alarm_text_ == current_alarm_text_ && last_notification_state_ == notification_visible_) {
        return; // 状态没变化，不需要更新
    }
    
    last_alarm_text_ = current_alarm_text_;
    last_notification_state_ = notification_visible_;
    
    if (should_show) {
        // 显示闹钟信息
        char alarm_str[64];
        snprintf(alarm_str, sizeof(alarm_str), "🔔%s", current_alarm_text_.c_str());
        lv_label_set_text((lv_obj_t*)alarm_label_, alarm_str);
        lv_obj_clear_flag((lv_obj_t*)alarm_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        // 隐藏闹钟信息
        lv_obj_add_flag((lv_obj_t*)alarm_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void ClockUI::UpdateNotificationLabel() {
    if (!notification_label_) return;
    
    // 检查通知状态是否需要更新
    if (last_notification_ == current_notification_ && last_notification_state_ == notification_visible_) {
        return; // 状态没变化，不需要更新
    }
    
    last_notification_ = current_notification_;
    
    if (notification_visible_ && !current_notification_.empty()) {
        // 显示通知信息
        char notification_str[128];
        snprintf(notification_str, sizeof(notification_str), "🔔%s", current_notification_.c_str());
        lv_label_set_text((lv_obj_t*)notification_label_, notification_str);
        lv_obj_clear_flag((lv_obj_t*)notification_label_, LV_OBJ_FLAG_HIDDEN);
        
        // 隐藏闹钟信息（通知优先级更高）
        if (alarm_label_) {
            lv_obj_add_flag((lv_obj_t*)alarm_label_, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        // 隐藏通知信息
        lv_obj_add_flag((lv_obj_t*)notification_label_, LV_OBJ_FLAG_HIDDEN);
    }
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