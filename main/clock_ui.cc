#include "clock_ui.h"
#include "time_sync_manager.h"
#include "display/spi_lcd_anim_display.h"  // 新增：用于资源管理
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <time.h>
#include <sys/time.h>
#include <algorithm>  // 新增：用于std::transform
#include <cctype>     // 新增：用于::tolower
#include <cstring>    // 新增：用于memcpy
#include "display.h"
#include "pcf8563_rtc.h"
#include "alarm_manager.h"
#include "font_awesome_symbols.h"  // 新增：包含图标符号定义
#include <lvgl.h>

static const char* TAG = "ClockUI";

// 声明可用的普惠字体
// LV_FONT_DECLARE(font_puhui_30_4);  // 最大字体，用于时间
LV_FONT_DECLARE(font_puhui_20_4);  // 中等字体
LV_FONT_DECLARE(font_puhui_40_4);  // 中等字体
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
    alarm_emotion_label_(nullptr),
    alarm_icon_label_(nullptr),
    alarm_text_label_(nullptr),
    notification_icon_label_(nullptr),
    notification_text_label_(nullptr),
    text_font_(nullptr),
    icon_font_(nullptr),
    emoji_font_(nullptr),
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

void ClockUI::SetFonts(const void* text_font, const void* icon_font, const void* emoji_font) {
    text_font_ = text_font;
    icon_font_ = icon_font;
    emoji_font_ = emoji_font;
    ESP_LOGI(TAG, "Fonts set for ClockUI - text: %p, icon: %p, emoji: %p", text_font, icon_font, emoji_font);
}

void ClockUI::CreateClockUI() {
    if (clock_container_) {
        return; // 已经创建
    }
    
    ESP_LOGI(TAG, "Creating lightweight clock UI components");
    
    // 获取当前屏幕
    lv_obj_t* screen = lv_screen_active();
    if (!screen) {
        ESP_LOGE(TAG, "Failed to get active screen");
        return;
    }
    
    // 创建时钟主容器（轻量级）
    lv_obj_t* container = lv_obj_create(screen);
    if (!container) {
        ESP_LOGE(TAG, "Failed to create clock container");
        return;
    }
    clock_container_ = container;
    
    // 设置容器为全屏，完全矩形背景
    lv_obj_set_size(container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    
    // 创建时间标签（简化版本）
    lv_obj_t* time_lbl = lv_label_create(container);
    if (time_lbl) {
        time_label_ = time_lbl;
        lv_obj_set_style_text_color(time_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(time_lbl, &font_puhui_40_4, 0);
        lv_obj_set_style_text_align(time_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(time_lbl, 0, LV_VER_RES / 2 - 40 -30);
        lv_obj_set_size(time_lbl, LV_HOR_RES, LV_SIZE_CONTENT);
        lv_label_set_text(time_lbl, "");

        lv_obj_t* time_am_pm = lv_label_create(container);
    if (time_am_pm) {
        time_am_pm_label_ = time_am_pm;
        lv_obj_set_style_text_color(time_am_pm, lv_color_white(), 0);
        lv_obj_set_style_text_font(time_am_pm, &font_puhui_20_4, 0);
        lv_obj_set_style_text_align(time_am_pm, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(time_am_pm,  LV_HOR_RES/2 + 58, LV_VER_RES / 2 - 30-30);
        lv_obj_set_size(time_am_pm, LV_HOR_RES, LV_SIZE_CONTENT);
        lv_label_set_text(time_am_pm, "上午");
    }
    }
    
    // 创建日期标签（简化版本）
    lv_obj_t* date_lbl = lv_label_create(container);
    if (date_lbl) {
        date_label_ = date_lbl;
        lv_obj_set_style_text_color(date_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(date_lbl, &font_puhui_20_4, 0);
        lv_obj_set_style_text_align(date_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(date_lbl, 0, 5);
        lv_obj_set_size(date_lbl, LV_HOR_RES, LV_SIZE_CONTENT);
        lv_label_set_text(date_lbl, "");
    }
    
    // 创建闹钟标签（显示下一个闹钟）- 现在改为创建容器
    // 创建闹钟容器（包含图标和文字）
    lv_obj_t* alarm_container = lv_obj_create(container);
    if (alarm_container) {
        alarm_icon_label_ = alarm_container; // 重用变量作为容器指针
        lv_obj_set_size(alarm_container, LV_HOR_RES, LV_SIZE_CONTENT);
        lv_obj_set_pos(alarm_container, 0, LV_VER_RES - 60);
        lv_obj_set_style_bg_opa(alarm_container, LV_OPA_TRANSP, 0); // 透明背景
        lv_obj_set_style_border_width(alarm_container, 0, 0);
        lv_obj_set_style_pad_all(alarm_container, 0, 0);
        lv_obj_set_style_radius(alarm_container, 0, 0);
        lv_obj_clear_flag(alarm_container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(alarm_container, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(alarm_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_flex_main_place(alarm_container, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_set_style_flex_cross_place(alarm_container, LV_FLEX_ALIGN_CENTER, 0);
        
        // 在容器内创建图标标签
        lv_obj_t* icon_lbl = lv_label_create(alarm_container);
        if (icon_lbl) {
            lv_obj_set_style_text_color(icon_lbl, lv_color_make(255, 165, 0), 0); // 橙色
            lv_obj_set_style_text_font(icon_lbl, icon_font_ ? (const lv_font_t*)icon_font_ : &font_puhui_20_4, 0);
            lv_label_set_text(icon_lbl, "\uF071"); // Font Awesome 闹钟图标
            lv_obj_set_style_pad_right(icon_lbl, 8, 0); // 与文字间距
        }
        
        // 在容器内创建文字标签
        lv_obj_t* text_lbl = lv_label_create(alarm_container);
        if (text_lbl) {
            alarm_text_label_ = text_lbl; // 存储文字标签指针
            lv_obj_set_style_text_color(text_lbl, lv_color_make(255, 165, 0), 0); // 橙色
            lv_obj_set_style_text_font(text_lbl, text_font_ ? (const lv_font_t*)text_font_ : &font_puhui_20_4, 0);
            lv_label_set_text(text_lbl, "");  // 初始为空
        }
        
        lv_obj_add_flag(alarm_container, LV_OBJ_FLAG_HIDDEN); // 初始隐藏
    }
    
    // 创建闹钟通知容器（居中显示，用于闹钟触发时的通知）
    lv_obj_t* notification_container = lv_obj_create(container);
    if (notification_container) {
        notification_icon_label_ = notification_container; // 重用变量作为容器指针
        lv_obj_set_style_bg_color(notification_container, lv_color_make(255, 100, 100), 0); // 浅红色背景
        lv_obj_set_style_bg_opa(notification_container, LV_OPA_90, 0); // 半透明背景
        lv_obj_set_style_pad_all(notification_container, 8, 0); // 添加内边距
        lv_obj_set_style_radius(notification_container, 8, 0); // 圆角
        lv_obj_set_style_border_width(notification_container, 0, 0);
        lv_obj_set_pos(notification_container, 10, LV_VER_RES-60);
        lv_obj_set_size(notification_container, LV_HOR_RES - 20, LV_SIZE_CONTENT);
        lv_obj_clear_flag(notification_container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(notification_container, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(notification_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_flex_main_place(notification_container, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_set_style_flex_cross_place(notification_container, LV_FLEX_ALIGN_CENTER, 0);
        
        // 在容器内创建图标标签
        lv_obj_t* notif_icon_lbl = lv_label_create(notification_container);
        if (notif_icon_lbl) {
            lv_obj_set_style_text_color(notif_icon_lbl, lv_color_white(), 0); // 白色
            lv_obj_set_style_text_font(notif_icon_lbl, icon_font_ ? (const lv_font_t*)icon_font_ : &font_puhui_20_4, 0);
            lv_label_set_text(notif_icon_lbl, "\uF0F3"); // Font Awesome 铃铛图标
            lv_obj_set_style_pad_right(notif_icon_lbl, 8, 0); // 与文字间距
        }
        
        // 在容器内创建文字标签
        lv_obj_t* notif_text_lbl = lv_label_create(notification_container);
        if (notif_text_lbl) {
            notification_text_label_ = notif_text_lbl; // 存储文字标签指针
            lv_obj_set_style_text_color(notif_text_lbl, lv_color_white(), 0); // 白色
            lv_obj_set_style_text_font(notif_text_lbl, text_font_ ? (const lv_font_t*)text_font_ : &font_puhui_20_4, 0);
            lv_label_set_text(notif_text_lbl, "");  // 初始为空
        }
        
        lv_obj_add_flag(notification_container, LV_OBJ_FLAG_HIDDEN); // 初始隐藏
    }
    
    ESP_LOGI(TAG, "Lightweight clock UI components created successfully");
}

void ClockUI::DestroyClockUI() {
    if (!clock_container_) {
        return; // 已经销毁或未创建
    }
    
    ESP_LOGI(TAG, "Starting to destroy Clock UI components");
    
    // 先将所有指针置空，避免在删除过程中被其他线程访问
    void* container = clock_container_;
    clock_container_ = nullptr;
    time_label_ = nullptr;
    time_am_pm_label_ = nullptr;
    date_label_ = nullptr;
    alarm_label_ = nullptr;  // 旧版本兼容
    notification_label_ = nullptr;  // 旧版本兼容
    alarm_emotion_label_ = nullptr;
    alarm_icon_label_ = nullptr;  // 现在指向闹钟容器
    alarm_text_label_ = nullptr;  // 指向容器内的文字标签
    notification_icon_label_ = nullptr;  // 现在指向通知容器
    notification_text_label_ = nullptr;  // 指向容器内的文字标签
    
    // 等待一帧时间，确保所有异步操作完成
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 最后删除容器对象
    if (container) {
        lv_obj_del((lv_obj_t*)container);
        ESP_LOGI(TAG, "Clock UI components destroyed safely");
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
    
    // 在显示时钟时，卸载SpiLcdAnimDisplay的UI以节约资源
    if (display_) {
        // 使用static_cast替代dynamic_cast，因为RTTI被禁用
        auto* spi_anim_display = static_cast<SpiLcdAnimDisplay*>(display_);
        // 注意：这里假设display_确实是SpiLcdAnimDisplay类型
        // 在实际使用中应该通过其他方式确认类型安全性
        if (spi_anim_display) {
            ESP_LOGI(TAG, "Tearing down SpiLcdAnimDisplay UI to save resources");
            spi_anim_display->TeardownUI();
        }
    }
    
    // 异步创建UI组件，避免阻塞主线程
    lv_async_call([](void* param) {
        ClockUI* self = static_cast<ClockUI*>(param);
        // 增加null检查，确保对象仍然有效
        if (self && self->is_visible_ && !self->clock_container_) {
            self->CreateClockUI();
            if (self->clock_container_) {
                // 强制重置显示，确保首次显示正确更新
                self->ForceUpdateDisplay();
                
                ESP_LOGI(TAG, "Clock UI created and shown asynchronously with forced update");
                ESP_LOGI(TAG, "Clock UI components ready for alarm display");
            }
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
    
    // 在隐藏时钟时，重新设置SpiLcdAnimDisplay的UI
    if (display_) {
        // 使用static_cast替代dynamic_cast，因为RTTI被禁用
        auto* spi_anim_display = static_cast<SpiLcdAnimDisplay*>(display_);
        // 注意：这里假设display_确实是SpiLcdAnimDisplay类型
        if (spi_anim_display) {
            ESP_LOGI(TAG, "Restoring SpiLcdAnimDisplay UI after hiding clock");
            spi_anim_display->SetupUI();
        }
    }
    
    ESP_LOGI(TAG, "Clock UI hidden");
}

void ClockUI::SetRtc(Pcf8563Rtc* rtc) {
    rtc_ = rtc;
}

void ClockUI::SetNextAlarm(const std::string& alarm_text) {
    // 新版本：使用容器结构，alarm_icon_label_现在指向容器
    if (!alarm_icon_label_) {
        ESP_LOGE(TAG, "SetNextAlarm: alarm container is null!");
        return;
    }
    
    ESP_LOGI(TAG, "SetNextAlarm called with text: '%s'", alarm_text.c_str());
    
    if (alarm_text.empty()) {
        // 隐藏闹钟容器
        lv_obj_add_flag(alarm_icon_label_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "SetNextAlarm: Alarm container hidden (empty text)");
    } else {
        // 更新文字标签内容（闹钟时间）
        if (alarm_text_label_ && lv_obj_is_valid(alarm_text_label_)) {
            static char display_text[128];
            snprintf(display_text, sizeof(display_text), " %s", alarm_text.c_str());
            lv_label_set_text(alarm_text_label_, display_text);
        }
        
        // 显示闹钟容器
        if (lv_obj_is_valid(alarm_icon_label_)) {
            lv_obj_clear_flag(alarm_icon_label_, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI(TAG, "SetNextAlarm: Alarm container shown with text: '%s'", alarm_text.c_str());
        } else {
            ESP_LOGE(TAG, "SetNextAlarm: alarm container is not a valid LVGL object!");
        }
    }
}

void ClockUI::ShowAlarmNotification(const std::string& notification) {
    // 新版本：使用容器结构，notification_icon_label_现在指向容器
    if (!notification_icon_label_) {
        ESP_LOGE(TAG, "ShowAlarmNotification: notification container is null!");
        return;
    }
    
    if (notification.empty()) {
        ESP_LOGW(TAG, "ShowAlarmNotification: notification text is empty");
        return;
    }
    
    ESP_LOGI(TAG, "ShowAlarmNotification called with: '%s'", notification.c_str());
    
    // 更新文字标签内容（通知文本）
    if (notification_text_label_ && lv_obj_is_valid(notification_text_label_)) {
        lv_label_set_text(notification_text_label_, notification.c_str());
    }
    
    // 显示通知容器
    if (lv_obj_is_valid(notification_icon_label_)) {
        lv_obj_clear_flag(notification_icon_label_, LV_OBJ_FLAG_HIDDEN);
        notification_visible_ = true;
        ESP_LOGI(TAG, "ShowAlarmNotification: Notification container shown with text: '%s'", notification.c_str());
    } else {
        ESP_LOGE(TAG, "ShowAlarmNotification: notification container is not a valid LVGL object!");
    }
}

void ClockUI::HideAlarmNotification() {
    // 新版本：使用容器结构，notification_icon_label_现在指向容器
    if (!notification_icon_label_) {
        return;
    }
    
    // 隐藏通知容器
    lv_obj_add_flag(notification_icon_label_, LV_OBJ_FLAG_HIDDEN);
    notification_visible_ = false;
    
    ESP_LOGI(TAG, "Alarm notification container hidden");
    
    // 闹钟通知隐藏后，刷新下一次闹钟显示
    RefreshNextAlarmDisplay();
}

void ClockUI::RefreshNextAlarmDisplay() {
    if (!is_visible_) {
        return;
    }
    
    ESP_LOGI(TAG, "RefreshNextAlarmDisplay: Updating next alarm information");
    
    // 获取下一个闹钟信息
    auto& alarm_manager = AlarmManager::GetInstance();
    AlarmInfo next_alarm = alarm_manager.GetNextAlarm();
    
    if (next_alarm.id > 0) {
        // 构建闹钟显示文本
        time_t current_time;
        struct tm alarm_tm;
        
        // 获取当前时间用于构建闹钟时间戳
        auto& time_sync_manager = TimeSyncManager::GetInstance();
        auto* rtc = time_sync_manager.GetRtc();
        
        if (rtc && rtc->GetTime(&current_time)) {
            localtime_r(&current_time, &alarm_tm);
        } else {
            time(&current_time);
            localtime_r(&current_time, &alarm_tm);
        }
        
        // 设置闹钟时间
        alarm_tm.tm_hour = next_alarm.hour;
        alarm_tm.tm_min = next_alarm.minute;
        alarm_tm.tm_sec = 0;
        
        // 转换为12小时制显示
        int display_hour = alarm_tm.tm_hour;
        const char* am_pm = "AM";
        if (display_hour >= 12) {
            am_pm = "PM";
            if (display_hour > 12) {
                display_hour -= 12;
            }
        } else if (display_hour == 0) {
            display_hour = 12;
        }
        
        char alarm_text[64];
        snprintf(alarm_text, sizeof(alarm_text), "%d:%02d %s", 
                display_hour, alarm_tm.tm_min, am_pm);
        
        SetNextAlarm(alarm_text);
        
        // 设置闹钟表情
        if (!next_alarm.description.empty()) {
            SetAlarmEmotion(next_alarm.description);
        }
        
        ESP_LOGI(TAG, "RefreshNextAlarmDisplay: Updated to show next alarm: %s", alarm_text);
    } else {
        // 没有闹钟，隐藏闹钟显示
        SetNextAlarm("");
        SetAlarmEmotion("");
        ESP_LOGI(TAG, "RefreshNextAlarmDisplay: No next alarm, hiding alarm display");
    }
}

void ClockUI::UpdateTimerCallback(void* timer) {
    // Simplified - do nothing
}

void ClockUI::UpdateClockDisplay() {
    // 增加更严格的安全检查
    if (!is_visible_ || !clock_container_ || !time_label_ || !date_label_) {
        ESP_LOGD(TAG, "UpdateClockDisplay: UI components not ready");
        return;
    }
    
    // 检查LVGL对象有效性
    if (!lv_obj_is_valid(clock_container_) || !lv_obj_is_valid(time_label_) || !lv_obj_is_valid(date_label_)) {
        ESP_LOGW(TAG, "UpdateClockDisplay: LVGL objects not valid");
        return;
    }
    
    // 简化的更新逻辑，避免复杂操作，减少函数调用层次
    try {
        UpdateTimeLabel();
        UpdateDateLabel();
        // 注意：闹钟信息由ShowClock()方法单独更新，这里不重复更新
    } catch (...) {
        ESP_LOGE(TAG, "UpdateClockDisplay: Exception during update");
    }
}

void ClockUI::ForceUpdateDisplay() {
    if (!is_visible_ || !clock_container_ || !time_label_ || !date_label_) {
        return;
    }
    
    ESP_LOGI(TAG, "ForceUpdateDisplay: Forcing immediate time and date refresh");
    
    // 强制更新时间标签
    ForceUpdateTimeLabel();
    
    // 强制更新日期标签  
    ForceUpdateDateLabel();
}

void ClockUI::UpdateTimeLabel() {
    if (!time_label_ || !is_visible_) return;
    
    // 检查LVGL对象有效性
    if (!lv_obj_is_valid(time_label_)) {
        ESP_LOGW(TAG, "UpdateTimeLabel: time_label not valid");
        return;
    }
    
    // 使用静态变量减少栈使用，改用分钟级别的比较避免时区问题
    static int last_update_minute = -1;
    static char time_str[32] = {0};  // 预初始化，增加缓冲区大小以避免截断警告
    static char time_am_pm_str[32] = {0};
    
    // 简化时间获取逻辑，减少函数调用层次
    struct tm timeinfo = {0};
    bool time_valid = false;
    
    try {
        // 使用TimeSyncManager的统一时间获取函数
        auto& time_sync_manager = TimeSyncManager::GetInstance();
        time_valid = time_sync_manager.GetUnifiedTime(&timeinfo);
        
        if (!time_valid) {
            ESP_LOGW(TAG, "UpdateTimeLabel: Failed to get unified time");
            return; // 时间无效，不更新
        }
        
        // 使用分钟级别的比较，避免时区时间戳转换问题
        int current_minute = timeinfo.tm_hour * 60 + timeinfo.tm_min;
        if (current_minute == last_update_minute) {
            return; // 时间没变化，不需要更新
        }
        
        // 转换为12小时制 - 简化逻辑
        int hour = timeinfo.tm_hour;
        int minute = timeinfo.tm_min;
        // const char* am_pm = (hour >= 12) ? "PM" : "AM";
        const char* am_pm = (hour >= 12) ? "下午" : "上午";
        
        if (hour == 0) {
            hour = 12;
        } else if (hour > 12) {
            hour -= 12;
        }
        
        // 更新时间显示 - 使用安全的snprintf
        // int ret = snprintf(time_str, sizeof(time_str), "%d:%02d %s", hour, minute, am_pm);
        int ret = snprintf(time_str, sizeof(time_str), "%02d:%02d", hour, minute);
        int ret2 = snprintf(time_am_pm_str, sizeof(time_am_pm_str), "%s", am_pm);
        if (ret >= sizeof(time_str)) {
            ESP_LOGW(TAG, "UpdateTimeLabel: Time string truncated");
            return;
        }
        
        // 确保在主线程中更新LVGL组件
        lv_label_set_text(time_label_, time_str);
        lv_label_set_text(time_am_pm_label_, time_am_pm_str);
        last_update_minute = current_minute;
    } catch (...) {
        ESP_LOGE(TAG, "UpdateTimeLabel: Exception during time update");
    }
}

void ClockUI::UpdateDateLabel() {
    if (!date_label_ || !is_visible_) return;
    
    // 检查LVGL对象有效性
    if (!lv_obj_is_valid(date_label_)) {
        ESP_LOGW(TAG, "UpdateDateLabel: date_label not valid");
        return;
    }
    
    // 使用静态变量减少栈使用，改用日期值比较避免时区问题
    static int last_date_update = -1;
    static char date_str[32] = {0};
    
    // 简化时间获取逻辑，减少函数调用层次
    struct tm timeinfo = {0};
    bool time_valid = false;
    
    try {
        // 使用TimeSyncManager的统一时间获取函数
        auto& time_sync_manager = TimeSyncManager::GetInstance();
        time_valid = time_sync_manager.GetUnifiedTime(&timeinfo);
        
        if (!time_valid) {
            ESP_LOGW(TAG, "UpdateDateLabel: Failed to get unified time");
            return;
        }
        
        // 使用年月日组合值比较，避免时区时间戳转换问题
        int current_date = (timeinfo.tm_year + 1900) * 10000 + (timeinfo.tm_mon + 1) * 100 + timeinfo.tm_mday;
        if (current_date == last_date_update) {
            return;
        }
        
        // 使用静态数组减少栈使用
        // static const char* weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char* weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
        int weekday = (timeinfo.tm_wday >= 0 && timeinfo.tm_wday <= 6) ? timeinfo.tm_wday : 0;
        
        // 格式化日期显示，确保月份和日期的正确性 - 使用安全的snprintf
        int ret = snprintf(date_str, sizeof(date_str), "%02d月%02d日 %s", 
                 timeinfo.tm_mon + 1, timeinfo.tm_mday, weekdays[weekday]);
        if (ret >= sizeof(date_str)) {
            ESP_LOGW(TAG, "UpdateDateLabel: Date string truncated");
            return;
        }
        
        // 添加LVGL对象有效性检查，确保在主线程中更新
        lv_label_set_text(date_label_, date_str);
        ESP_LOGI(TAG, "Date updated: %s (tm_mon=%d, tm_mday=%d, tm_wday=%d)", 
                 date_str, timeinfo.tm_mon, timeinfo.tm_mday, timeinfo.tm_wday);
        
        last_date_update = current_date;
        
    } catch (...) {
        ESP_LOGE(TAG, "UpdateDateLabel: Exception during date update");
    }
}

void ClockUI::ForceUpdateTimeLabel() {
    if (!time_label_ || !is_visible_) return;
    
    static char time_str[32];
    static char time_am_pm_str[32];
    
    // 使用TimeSyncManager的统一时间获取函数
    struct tm timeinfo;
    auto& time_sync_manager = TimeSyncManager::GetInstance();
    
    if (!time_sync_manager.GetUnifiedTime(&timeinfo)) {
        ESP_LOGW(TAG, "ForceUpdateTimeLabel: Failed to get unified time");
        return;
    }
    
    // 转换为12小时制
    int hour = timeinfo.tm_hour;
    int minute = timeinfo.tm_min;
    bool is_pm = hour >= 12;
    
    if (hour == 0) {
        hour = 12;
    } else if (hour > 12) {
        hour -= 12;
    }
    
    // 强制更新时间显示
    // snprintf(time_str, sizeof(time_str), "%d:%02d %s", hour, minute, is_pm ? "下午" : "上午");
    snprintf(time_str, sizeof(time_str), "%02d:%02d", hour, minute);
    snprintf(time_am_pm_str, sizeof(time_am_pm_str), "%s", is_pm ? "下午" : "上午");
    
    if (lv_obj_is_valid(time_label_) && lv_obj_is_valid(time_am_pm_label_)) {
        lv_label_set_text(time_label_, time_str);
        lv_label_set_text(time_am_pm_label_, time_am_pm_str);
        ESP_LOGI(TAG, "Force updated time: %s %s", time_str, time_am_pm_str);
    }
}

void ClockUI::ForceUpdateDateLabel() {
    if (!date_label_ || !is_visible_) return;
    
    static char date_str[32];
    
    // 使用TimeSyncManager的统一时间获取函数
    struct tm timeinfo;
    auto& time_sync_manager = TimeSyncManager::GetInstance();
    
    if (!time_sync_manager.GetUnifiedTime(&timeinfo)) {
        ESP_LOGW(TAG, "ForceUpdateDateLabel: Failed to get unified time");
        return;
    }
    
    // const char* weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    int weekday = timeinfo.tm_wday;
    
    // 强制格式化日期显示
    snprintf(date_str, sizeof(date_str), "%02d月%02d日 %s", 
             timeinfo.tm_mon + 1, timeinfo.tm_mday, weekdays[weekday]);
    
    if (lv_obj_is_valid(date_label_)) {
        lv_label_set_text(date_label_, date_str);
        ESP_LOGI(TAG, "Force updated date: %s (mon=%d, mday=%d, wday=%d, year=%d)", 
                 date_str, timeinfo.tm_mon, timeinfo.tm_mday, timeinfo.tm_wday, timeinfo.tm_year + 1900);
    }
}

void ClockUI::UpdateAlarmLabel() {
    // 简化实现，暂时移除复杂的闹钟显示逻辑
    return;
}

void ClockUI::UpdateNotificationLabel() {
    // 简化实现，暂时移除复杂的通知显示逻辑  
    return;
}

void ClockUI::UpdateAlarmEmotionLabel() {
    // 简化实现，暂时移除表情显示逻辑
    return;
}

void ClockUI::SetAlarmEmotion(const std::string& emotion) {
    // 简化实现，暂时移除表情设置逻辑
    return;
}

std::string ClockUI::GetEmotionForAlarmType(const std::string& alarm_text) {
    // 将闹钟描述转换为小写以便匹配
    std::string lower_text = alarm_text;
    std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(), ::tolower);
    
    // 根据闹钟描述中的关键词选择合适的表情
    if (lower_text.find("起床") != std::string::npos || 
        lower_text.find("morning") != std::string::npos ||
        lower_text.find("wake") != std::string::npos) {
        return "😴"; // 睡觉表情 - 起床相关
    }
    else if (lower_text.find("吃饭") != std::string::npos || 
             lower_text.find("用餐") != std::string::npos ||
             lower_text.find("lunch") != std::string::npos ||
             lower_text.find("dinner") != std::string::npos ||
             lower_text.find("breakfast") != std::string::npos ||
             lower_text.find("meal") != std::string::npos) {
        return "🤤"; // 流口水表情 - 吃饭相关
    }
    else if (lower_text.find("锻炼") != std::string::npos || 
             lower_text.find("运动") != std::string::npos ||
             lower_text.find("健身") != std::string::npos ||
             lower_text.find("exercise") != std::string::npos ||
             lower_text.find("workout") != std::string::npos ||
             lower_text.find("gym") != std::string::npos) {
        return "😎"; // 酷表情 - 运动相关
    }
    else if (lower_text.find("工作") != std::string::npos || 
             lower_text.find("上班") != std::string::npos ||
             lower_text.find("会议") != std::string::npos ||
             lower_text.find("work") != std::string::npos ||
             lower_text.find("meeting") != std::string::npos ||
             lower_text.find("office") != std::string::npos) {
        return "🤔"; // 思考表情 - 工作相关
    }
    else if (lower_text.find("学习") != std::string::npos || 
             lower_text.find("复习") != std::string::npos ||
             lower_text.find("考试") != std::string::npos ||
             lower_text.find("study") != std::string::npos ||
             lower_text.find("exam") != std::string::npos ||
             lower_text.find("homework") != std::string::npos) {
        return "🤔"; // 思考表情 - 学习相关
    }
    else if (lower_text.find("生日") != std::string::npos || 
             lower_text.find("庆祝") != std::string::npos ||
             lower_text.find("party") != std::string::npos ||
             lower_text.find("birthday") != std::string::npos ||
             lower_text.find("celebrate") != std::string::npos) {
        return "😆"; // 开心表情 - 庆祝相关
    }
    else if (lower_text.find("医院") != std::string::npos || 
             lower_text.find("看病") != std::string::npos ||
             lower_text.find("医生") != std::string::npos ||
             lower_text.find("doctor") != std::string::npos ||
             lower_text.find("hospital") != std::string::npos ||
             lower_text.find("medicine") != std::string::npos) {
        return "😔"; // 难过表情 - 医疗相关
    }
    else if (lower_text.find("约会") != std::string::npos || 
             lower_text.find("聚会") != std::string::npos ||
             lower_text.find("朋友") != std::string::npos ||
             lower_text.find("date") != std::string::npos ||
             lower_text.find("friend") != std::string::npos ||
             lower_text.find("social") != std::string::npos) {
        return "😍"; // 爱心表情 - 社交相关
    }
    else if (lower_text.find("睡觉") != std::string::npos || 
             lower_text.find("休息") != std::string::npos ||
             lower_text.find("sleep") != std::string::npos ||
             lower_text.find("rest") != std::string::npos ||
             lower_text.find("bedtime") != std::string::npos) {
        return "😌"; // 放松表情 - 睡觉相关
    }
    else if (lower_text.find("购物") != std::string::npos || 
             lower_text.find("买") != std::string::npos ||
             lower_text.find("shopping") != std::string::npos ||
             lower_text.find("buy") != std::string::npos ||
             lower_text.find("shop") != std::string::npos) {
        return "😉"; // 眨眼表情 - 购物相关
    }
    else {
        return "🙂"; // 默认开心表情
    }
}

bool ClockUI::GetCurrentTime(int& hour, int& minute, int& second, int& day, int& month, int& year) {
    // 使用TimeSyncManager的统一时间获取函数
    struct tm timeinfo;
    auto& time_sync_manager = TimeSyncManager::GetInstance();
    
    if (!time_sync_manager.GetUnifiedTime(&timeinfo)) {
        ESP_LOGW(TAG, "GetCurrentTime: Failed to get unified time");
        return false;
    }
    
    hour = timeinfo.tm_hour;
    minute = timeinfo.tm_min;
    second = timeinfo.tm_sec;
    day = timeinfo.tm_mday;
    month = timeinfo.tm_mon + 1; // tm_mon is 0-based
    year = timeinfo.tm_year + 1900;
    
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