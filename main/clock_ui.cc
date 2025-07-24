#include "clock_ui.h"
#include "time_sync_manager.h"
#include "display/spi_lcd_anim_display.h"  // 新增：用于资源管理
#include "board.h"  // 新增：用于Board类访问
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <time.h>
#include <sys/time.h>
#include <algorithm>  // 新增：用于std::transform
#include <cctype>     // 新增：用于::tolower
#include <cstring>    // 新增：用于memcpy
#include <cinttypes>  // 新增：用于PRIx32
#include "display.h"
#include "pcf8563_rtc.h"
#include "alarm_manager.h"
#include "font_awesome_symbols.h"  // 新增：包含图标符号定义
#include <lvgl.h>
#include "tjpgd.h"              // 新增：TJPGD JPG解码库
#include "esp_http_client.h"     // 新增：HTTP客户端

static const char* TAG = "ClockUI";

// 声明可用的普惠字体
// LV_FONT_DECLARE(font_puhui_30_4);  // 最大字体，用于时间
LV_FONT_DECLARE(font_puhui_20_4);  // 中等字体
LV_FONT_DECLARE(font_puhui_80_4);  // 中等字体
// LV_FONT_DECLARE(font_puhui_16_4);  // 用于日期
// LV_FONT_DECLARE(font_puhui_14_1);  // 最小字体，用于AM/PM

// 新增：JPG解码上下文结构
struct ClockJpegDecodeContext {
    const uint8_t* src_data;
    size_t src_size;
    size_t src_pos;
    uint8_t* output_buffer;
    size_t output_size;
    size_t output_pos;
};

// 新增：TJPGD输入回调函数
static UINT clock_tjpgd_input_callback(JDEC* jd, BYTE* buff, UINT nbyte) {
    ClockJpegDecodeContext* ctx = (ClockJpegDecodeContext*)jd->device;
    
    if (buff) {
        // 读取数据
        UINT bytes_to_read = (UINT)std::min((size_t)nbyte, ctx->src_size - ctx->src_pos);
        memcpy(buff, ctx->src_data + ctx->src_pos, bytes_to_read);
        ctx->src_pos += bytes_to_read;
        return bytes_to_read;
    } else {
        // 跳过数据
        ctx->src_pos = std::min(ctx->src_pos + (size_t)nbyte, ctx->src_size);
        return nbyte;
    }
}

// 新增：TJPGD输出回调函数
static UINT clock_tjpgd_output_callback(JDEC* jd, void* bitmap, JRECT* rect) {
    ClockJpegDecodeContext* ctx = (ClockJpegDecodeContext*)jd->device;
    
    if (!bitmap || !ctx->output_buffer) {
        return 1;
    }
    
    // 计算输出区域
    int rect_width = rect->right - rect->left + 1;
    int rect_height = rect->bottom - rect->top + 1;
    
    // 将RGB数据复制到输出缓冲区
    BYTE* src_line = (BYTE*)bitmap;
    
    for (int y = 0; y < rect_height; y++) {
        int dst_y = rect->top + y;
        if (dst_y >= 0 && dst_y < (int)jd->height) {
            size_t dst_offset = (dst_y * jd->width + rect->left) * 3;
            size_t src_offset = y * rect_width * 3;
            
            if (dst_offset + rect_width * 3 <= ctx->output_size) {
                memcpy(ctx->output_buffer + dst_offset, src_line + src_offset, rect_width * 3);
            }
        }
    }
    
    return 1;
}

// 新增：RGB888转RGB565函数
uint16_t ClockUI::RGB888ToRGB565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

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
    wallpaper_img_(nullptr),
    animation_label_(nullptr),
    text_font_(nullptr),
    icon_font_(nullptr),
    emoji_font_(nullptr),
    last_displayed_hour_(-1),
    last_displayed_minute_(-1),
    last_displayed_day_(-1),
    last_notification_state_(false),
    animation_visible_(false),
    animation_frame_(0),
    animation_timer_(nullptr),
    wallpaper_type_(WALLPAPER_NONE),
    wallpaper_color_(0x000000),
    wallpaper_image_name_(""),
    wallpaper_network_url_("") {
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
    
    // 设置容器为全屏，透明背景以显示壁纸
    lv_obj_set_size(container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0); // 改为透明，让壁纸显示
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    
    // 创建全屏壁纸图片（作为背景层）
    lv_obj_t* wallpaper = lv_img_create(container);
    if (wallpaper) {
        wallpaper_img_ = wallpaper;
        lv_obj_set_size(wallpaper, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(wallpaper, 0, 0);
        lv_obj_set_style_bg_opa(wallpaper, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wallpaper, 0, 0);
        lv_obj_set_style_pad_all(wallpaper, 0, 0);
        lv_obj_add_flag(wallpaper, LV_OBJ_FLAG_HIDDEN); // 默认隐藏，等待设置壁纸
        ESP_LOGI(TAG, "Wallpaper image created");
    }
    
    // 创建时间标签（简化版本）
    lv_obj_t* time_lbl = lv_label_create(container);
    if (time_lbl) {
        time_label_ = time_lbl;
        lv_obj_set_style_text_color(time_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(time_lbl, &font_puhui_80_4, 0);
        lv_obj_set_style_text_align(time_lbl, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(time_lbl, 5, 40);  // 调整时间位置，稍微上移
        lv_obj_set_size(time_lbl, LV_HOR_RES, LV_SIZE_CONTENT);
        lv_label_set_text(time_lbl, "");

        lv_obj_t* time_am_pm = lv_label_create(container);
    if (time_am_pm) {
        time_am_pm_label_ = time_am_pm;
        lv_obj_set_style_text_color(time_am_pm, lv_color_white(), 0);
        lv_obj_set_style_text_font(time_am_pm, &font_puhui_20_4, 0);
        lv_obj_set_style_text_align(time_am_pm, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(time_am_pm,  LV_HOR_RES-25, 50);  // 调整AM/PM位置
        lv_obj_set_size(time_am_pm, LV_HOR_RES, LV_SIZE_CONTENT);
        lv_label_set_text(time_am_pm, "");
    }
    }
    
    // 创建80*80动画标签（在时间下面，240*240图片的1/3大小）
    lv_obj_t* anim_lbl = lv_img_create(container);
    if (anim_lbl) {
        animation_label_ = anim_lbl;
        lv_obj_set_size(anim_lbl, 120, 120);
        // 居中显示，位置在时间标签下方，与表情位置一致
        lv_obj_set_pos(anim_lbl, (LV_HOR_RES - 120) / 2, LV_VER_RES / 2 );  // 改为100，与表情位置保持一致
        lv_obj_set_style_bg_opa(anim_lbl, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(anim_lbl, 0, 0);
        lv_obj_set_style_pad_all(anim_lbl, 0, 0);
        lv_obj_set_style_radius(anim_lbl, 8, 0); // 轻微的圆角
        lv_obj_add_flag(anim_lbl, LV_OBJ_FLAG_HIDDEN); // 默认隐藏
        ESP_LOGI(TAG, "80x80 animation label created at position (x=%ld, y=%ld)", (LV_HOR_RES - 120) / 2, LV_VER_RES / 2 + 20);
    } 
    
    // 创建日期标签（简化版本）
    lv_obj_t* date_lbl = lv_label_create(container);
    if (date_lbl) {
        date_label_ = date_lbl;
        lv_obj_set_style_text_color(date_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(date_lbl, &font_puhui_20_4, 0);
        lv_obj_set_style_text_align(date_lbl, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(date_lbl, 5, 5);  // 日期位置稍微下调
        lv_obj_set_size(date_lbl, LV_HOR_RES, LV_SIZE_CONTENT);
        lv_label_set_text(date_lbl, "");
    }
    
    // 创建闹钟容器（包含图标和文字）
    lv_obj_t* alarm_container = lv_obj_create(container);
    if (alarm_container) {
        alarm_icon_label_ = alarm_container; // 重用变量作为容器指针
        lv_obj_set_size(alarm_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_pos(alarm_container, 120, 5);  // 放在日期标签后面（日期大约占用100像素）
        lv_obj_set_style_bg_opa(alarm_container, LV_OPA_TRANSP, 0); // 透明背景
        lv_obj_set_style_border_width(alarm_container, 0, 0);
        lv_obj_set_style_pad_all(alarm_container, 0, 0);
        lv_obj_set_style_radius(alarm_container, 0, 0);
        lv_obj_clear_flag(alarm_container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(alarm_container, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(alarm_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_flex_main_place(alarm_container, LV_FLEX_ALIGN_START, 0);  // 从左开始排列
        lv_obj_set_style_flex_cross_place(alarm_container, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_column(alarm_container, 1 , 0);  // 设置flex项目间的列间距为1像素
        
        // 在容器内创建图标标签
        lv_obj_t* icon_lbl = lv_label_create(alarm_container);
        if (icon_lbl) {
            lv_obj_set_style_text_color(icon_lbl, lv_color_make(255, 165, 0), 0); // 橙色
            lv_obj_set_style_text_font(icon_lbl, icon_font_ ? (const lv_font_t*)icon_font_ : &font_puhui_20_4, 0);
            lv_label_set_text(icon_lbl, "\uF071"); // Font Awesome 闹钟图标
            lv_obj_set_style_pad_right(icon_lbl, 1, 0); // 与文字间距
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
        lv_obj_set_pos(notification_container, 10, LV_VER_RES - 45);  // 通知容器位置稍微上调
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
            lv_obj_set_style_pad_right(notif_icon_lbl, 5, 0); // 与文字间距
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
    wallpaper_img_ = nullptr;  // 壁纸图片
    animation_label_ = nullptr;  // 动画标签
    
    // 停止动画定时器
    if (animation_timer_) {
        // 这里应该停止定时器，具体实现取决于定时器类型
        animation_timer_ = nullptr;
    }
    
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
                
                // 加载保存的壁纸和动画配置
                self->LoadWallpaperConfig();
                self->LoadAnimationConfig();
                
                // 显示听状态动画
                self->ShowListenAnimation();
                
                // 先测试纯色背景
                //   self->TestWallpaperWithColor();
                  
                //   // 延迟3秒后尝试加载图片壁纸
                //   vTaskDelay(pdMS_TO_TICKS(3000));
                //   self->SetWallpaper("/sdcard/BJ.JPG");
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
    // 调用const char*版本
    SetNextAlarm(alarm_text.c_str());
}

void ClockUI::SetNextAlarm(const char* next_alarm_time) {
    // 新版本：使用容器结构，alarm_icon_label_现在指向容器
    if (!alarm_icon_label_) {
        ESP_LOGE(TAG, "SetNextAlarm: alarm container is null!");
        return;
    }
    
    ESP_LOGI(TAG, "SetNextAlarm called with text: '%s'", next_alarm_time ? next_alarm_time : "NULL");
    
    if (!next_alarm_time || strlen(next_alarm_time) == 0) {
        // 隐藏闹钟容器
        lv_obj_add_flag(alarm_icon_label_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "SetNextAlarm: Alarm container hidden (empty text)");
    } else {
        // 更新文字标签内容（闹钟时间）
        if (alarm_text_label_ && lv_obj_is_valid(alarm_text_label_)) {
            static char display_text[128];
            snprintf(display_text, sizeof(display_text), "%s", next_alarm_time);
            lv_label_set_text(alarm_text_label_, display_text);
        }
        
        // 显示闹钟容器
        if (lv_obj_is_valid(alarm_icon_label_)) {
            lv_obj_clear_flag(alarm_icon_label_, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI(TAG, "SetNextAlarm: Alarm container shown with text: '%s'", next_alarm_time);
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
    //隐藏80*80表情动画标签
    lv_obj_add_flag(animation_label_, LV_OBJ_FLAG_HIDDEN);
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
    
    // 重置时间缓存，确保下次正常更新能正确工作
    last_displayed_hour_ = -1;
    last_displayed_minute_ = -1;
    last_displayed_day_ = -1;
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
        if (current_minute == last_update_minute && last_update_minute != -1) {
            return; // 时间没变化，不需要更新
        }
        
        // 转换为12小时制 - 简化逻辑
        int hour = timeinfo.tm_hour;
        int minute = timeinfo.tm_min;
        // const char* am_pm = (hour >= 12) ? "PM" : "AM";
        const char* am_pm = (hour >= 12) ? "下" : "上";
        
        if (hour == 0) {
            hour = 12;
            lv_obj_set_pos(time_am_pm_label_,  LV_HOR_RES-25, 50);
        } else if (hour > 12) {
            hour -= 12;
            lv_obj_set_pos(time_am_pm_label_,  LV_HOR_RES-25, 80);
        }
        
        // 更新时间显示 - 使用安全的snprintf
        // int ret = snprintf(time_str, sizeof(time_str), "%d:%02d %s", hour, minute, am_pm);
        int ret = snprintf(time_str, sizeof(time_str), "%02d:%02d", hour, minute);
        snprintf(time_am_pm_str, sizeof(time_am_pm_str), "%s", am_pm);
        if (ret >= sizeof(time_str)) {
            ESP_LOGW(TAG, "UpdateTimeLabel: Time string truncated");
            return;
        }
        
        // 确保在主线程中更新LVGL组件
        lv_label_set_text(time_label_, time_str);
        lv_label_set_text(time_am_pm_label_, time_am_pm_str);
        last_update_minute = current_minute;
        ESP_LOGD(TAG, "Time updated to: %s %s", time_str, time_am_pm_str);
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
        if (current_date == last_date_update && last_date_update != -1) {
            return;
        }
        
        // 使用静态数组减少栈使用
        // static const char* weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char* weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
        int weekday = (timeinfo.tm_wday >= 0 && timeinfo.tm_wday <= 6) ? timeinfo.tm_wday : 0;
        
        // 格式化日期显示，确保月份和日期的正确性 - 使用安全的snprintf
        int ret = snprintf(date_str, sizeof(date_str), "%02d/%02d%s", 
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
    snprintf(date_str, sizeof(date_str), "%02d/%02d %s", 
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
    return;//去掉闹钟表情的显示
    if (!animation_label_ || !is_visible_) {
        ESP_LOGD(TAG, "SetAlarmEmotion: animation_label_ is null or not visible");
        return;
    }
    
    // 检查LVGL对象有效性
    if (!lv_obj_is_valid(animation_label_)) {
        ESP_LOGW(TAG, "SetAlarmEmotion: animation_label_ object is invalid");
        return;
    }
    
    ESP_LOGI(TAG, "Setting alarm emotion: %s", emotion.c_str());
    
    if (emotion.empty()) {
        // 隐藏表情动画
        lv_obj_add_flag(animation_label_, LV_OBJ_FLAG_HIDDEN);
        current_alarm_emotion_ = "";
        ESP_LOGI(TAG, "Alarm emotion hidden (empty emotion)");
        return;
    }
    
    // 根据闹钟描述获取合适的表情
    std::string emotion_char = GetEmotionForAlarmType(emotion);
    current_alarm_emotion_ = emotion_char;
    
    // 将动画标签临时用作表情显示（将来可以替换为真正的动画）
    // 首先确保动画标签变成文本标签模式
    
    // 删除当前的图片源（如果有的话）
    lv_img_set_src(animation_label_, nullptr);
    
    // 创建一个文本标签来显示表情
    static lv_obj_t* emotion_text_label = nullptr;
    
    // 如果已经有表情文字标签，先删除它
    if (emotion_text_label && lv_obj_is_valid(emotion_text_label)) {
        lv_obj_del(emotion_text_label);
        emotion_text_label = nullptr;
    }
    
    // 在动画标签的父容器中创建新的表情文字标签
    lv_obj_t* parent = lv_obj_get_parent(animation_label_);
    if (parent && lv_obj_is_valid(parent)) {
        emotion_text_label = lv_label_create(parent);
        if (emotion_text_label) {
            // 设置表情文字的样式和位置
            lv_obj_set_style_text_color(emotion_text_label, lv_color_white(), 0);
            
            // 使用表情字体（如果可用），否则使用默认字体
            if (emoji_font_) {
                lv_obj_set_style_text_font(emotion_text_label, (const lv_font_t*)emoji_font_, 0);
            } else {
                // 如果没有表情字体，使用大一点的文字字体
                lv_obj_set_style_text_font(emotion_text_label, &font_puhui_20_4, 0);
            }
            
            lv_obj_set_style_text_align(emotion_text_label, LV_TEXT_ALIGN_CENTER, 0);
            
            // 设置位置和大小，确保表情在时间下方不重叠的位置
            lv_obj_set_size(emotion_text_label, 80, 80);
            lv_obj_set_pos(emotion_text_label, (LV_HOR_RES - 80) / 2, LV_VER_RES / 2 );  // 改为100，进一步下移避免重叠
            
            // 设置表情文字
            lv_label_set_text(emotion_text_label, emotion_char.c_str());
            
            // 设置文本对齐方式，但不调用lv_obj_center()避免位置冲突
            lv_obj_set_style_text_align(emotion_text_label, LV_TEXT_ALIGN_CENTER, 0);
            
            ESP_LOGI(TAG, "Alarm emotion displayed: %s (character: %s) at position (x=%ld, y=%ld)", 
                     emotion.c_str(), emotion_char.c_str(), (LV_HOR_RES - 80) / 2, LV_VER_RES / 2 + 100);
        } else {
            ESP_LOGE(TAG, "Failed to create emotion text label");
        }
    }
    
    // 隐藏原来的80*80动画标签（避免冲突）
    lv_obj_add_flag(animation_label_, LV_OBJ_FLAG_HIDDEN);
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

// 设置壁纸
// clock_ui->SetWallpaper("/sdcard/wallpaper.jpg");

// // 设置并显示动画
// clock_ui->SetAnimation("/sdcard/heart_animation");
// clock_ui->ShowAnimation(true);
// 新增：设置壁纸
void ClockUI::SetWallpaper(const char* image_path_or_url) {
    if (!wallpaper_img_ || !image_path_or_url) {
        ESP_LOGE(TAG, "SetWallpaper: wallpaper_img_ is null or invalid path");
        return;
    }
    
    ESP_LOGI(TAG, "Setting wallpaper: %s", image_path_or_url);
    
    // 检查壁纸对象是否有效
    if (!lv_obj_is_valid(wallpaper_img_)) {
        ESP_LOGE(TAG, "SetWallpaper: wallpaper_img_ object is invalid");
        return;
    }
    
    // 检查是否是URL（以http开头）
    if (strncmp(image_path_or_url, "http", 4) == 0) {
        // URL壁纸，这里需要下载图片并设置
        // 可以参考SpiLcdAnimDisplay中的下载逻辑
        ESP_LOGI(TAG, "URL wallpaper not implemented yet: %s", image_path_or_url);
        return;
    }
    
    // 检查文件是否存在
    FILE* file = fopen(image_path_or_url, "rb");
    if (!file) {
        ESP_LOGE(TAG, "SetWallpaper: File does not exist: %s", image_path_or_url);
        return;
    }
    
    // 获取文件大小
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fclose(file);
    
    if (file_size <= 0) {
        ESP_LOGE(TAG, "SetWallpaper: File is empty or invalid: %s (size: %ld)", image_path_or_url, file_size);
        return;
    }
    
    ESP_LOGI(TAG, "SetWallpaper: File found, size: %ld bytes", file_size);
    
    // 本地文件路径
    current_wallpaper_ = image_path_or_url;
    
    // 设置壁纸位置和尺寸信息（调试） - 修复格式化问题
    ESP_LOGI(TAG, "SetWallpaper: Object position before: x=%ld, y=%ld, w=%ld, h=%ld", 
             (long)lv_obj_get_x(wallpaper_img_), (long)lv_obj_get_y(wallpaper_img_), 
             (long)lv_obj_get_width(wallpaper_img_), (long)lv_obj_get_height(wallpaper_img_));
    
    // 确保壁纸在最底层
    lv_obj_move_to_index(wallpaper_img_, 0);
    
    // 重新设置尺寸和位置，确保覆盖全屏
    lv_obj_set_size(wallpaper_img_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(wallpaper_img_, 0, 0);
    
    // 检查文件扩展名，LVGL可能不支持JPG
    const char* file_ext = strrchr(image_path_or_url, '.');
    bool is_jpg = file_ext && (strcasecmp(file_ext, ".jpg") == 0 || strcasecmp(file_ext, ".jpeg") == 0);
    
    if (is_jpg) {
        ESP_LOGW(TAG, "JPG files may not be supported by LVGL, using fallback test");
        // 使用纯色背景作为测试
        lv_obj_set_style_bg_color(wallpaper_img_, lv_color_hex(0xFF0000), 0); // 红色背景
        lv_obj_set_style_bg_opa(wallpaper_img_, LV_OPA_COVER, 0);
    } else {
        // 尝试加载本地图片文件（非JPG格式）
        lv_img_set_src(wallpaper_img_, image_path_or_url);
        
        // 检查图片是否加载成功
        const void* img_src = lv_img_get_src(wallpaper_img_);
        if (!img_src) {
            ESP_LOGW(TAG, "Failed to load image, using fallback color");
            // 使用纯色背景作为备用
            lv_obj_set_style_bg_color(wallpaper_img_, lv_color_hex(0x00FF00), 0); // 绿色背景
            lv_obj_set_style_bg_opa(wallpaper_img_, LV_OPA_COVER, 0);
        } else {
            ESP_LOGI(TAG, "SetWallpaper: Image source loaded successfully");
        }
    }
    
    // 显示壁纸
    lv_obj_clear_flag(wallpaper_img_, LV_OBJ_FLAG_HIDDEN);
    
    // 确保对象类型设置正确
    lv_obj_set_style_bg_opa(wallpaper_img_, LV_OPA_COVER, 0);
    
    // 强制刷新显示
    lv_obj_invalidate(wallpaper_img_);
    
    ESP_LOGI(TAG, "SetWallpaper: Object position after: x=%ld, y=%ld, w=%ld, h=%ld, visible=%d", 
             (long)lv_obj_get_x(wallpaper_img_), (long)lv_obj_get_y(wallpaper_img_), 
             (long)lv_obj_get_width(wallpaper_img_), (long)lv_obj_get_height(wallpaper_img_),
             !lv_obj_has_flag(wallpaper_img_, LV_OBJ_FLAG_HIDDEN));
    
    ESP_LOGI(TAG, "Wallpaper set successfully: %s", image_path_or_url);
}

// 新增：测试壁纸功能的辅助方法
void ClockUI::TestWallpaperWithColor() {
    if (!wallpaper_img_) {
        ESP_LOGE(TAG, "TestWallpaperWithColor: wallpaper_img_ is null");
        return;
    }
    
    ESP_LOGI(TAG, "Testing wallpaper with solid color");
    
    // 创建一个简单的纯色图片作为测试
    lv_obj_set_size(wallpaper_img_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(wallpaper_img_, 0, 0);
    lv_obj_set_style_bg_color(wallpaper_img_, lv_color_hex(0x0000FF), 0); // 蓝色
    lv_obj_set_style_bg_opa(wallpaper_img_, LV_OPA_COVER, 0); // 不透明
    lv_obj_clear_flag(wallpaper_img_, LV_OBJ_FLAG_HIDDEN);
    
    // 确保在最底层
    lv_obj_move_to_index(wallpaper_img_, 0);
    
    ESP_LOGI(TAG, "Test wallpaper (blue background) set");
}

// 新增：设置动画内容
void ClockUI::SetAnimation(const char* animation_path) {
    if (!animation_label_ || !animation_path) {
        ESP_LOGE(TAG, "SetAnimation: animation_label_ is null or invalid path");
        return;
    }
    
    ESP_LOGI(TAG, "Setting animation: %s", animation_path);
    
    current_animation_ = animation_path;
    animation_frame_ = 0;
    
    // 设置初始帧
    lv_img_set_src(animation_label_, animation_path);
    
    ESP_LOGI(TAG, "Animation set successfully: %s", animation_path);
}

// 新增：显示/隐藏动画
void ClockUI::ShowAnimation(bool show) {
    if (!animation_label_) {
        ESP_LOGE(TAG, "ShowAnimation: animation_label_ is null");
        return;
    }
    
    animation_visible_ = show;
    
    if (show) {
        lv_obj_clear_flag(animation_label_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "Animation shown");
    } else {
        lv_obj_add_flag(animation_label_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "Animation hidden");
    }
}

// 新增：更新动画帧
void ClockUI::UpdateAnimation() {
    if (!animation_label_ || !animation_visible_ || current_animation_.empty()) {
        return;
    }
    
    // 这里可以实现动画帧的更新逻辑
    // 例如：切换到下一帧，循环播放等
    animation_frame_++;
    
    // 构建帧文件路径（假设动画文件以数字命名）
    char frame_path[256];
    snprintf(frame_path, sizeof(frame_path), "%s_%03d.bin", current_animation_.c_str(), animation_frame_);
    
    // 尝试加载帧文件
    // 这里需要根据实际的动画文件格式来实现
    
    ESP_LOGD(TAG, "Animation frame updated: %d", animation_frame_);
}

// 配置文件路径定义
const char* ClockUI::WALLPAPER_CONFIG_FILE = "/sdcard/WALL.CFG";
const char* ClockUI::ANIMATION_CONFIG_FILE = "/sdcard/ANIM.CFG";

// 新增：设置纯色壁纸
void ClockUI::SetSolidColorWallpaper(uint32_t color) {
    if (!wallpaper_img_) {
        ESP_LOGE(TAG, "SetSolidColorWallpaper: wallpaper_img_ is null");
        return;
    }
    
    ESP_LOGI(TAG, "Setting solid color wallpaper: 0x%06" PRIx32, color);
    
    wallpaper_type_ = WALLPAPER_SOLID_COLOR;
    wallpaper_color_ = color;
    wallpaper_image_name_ = "";
    wallpaper_network_url_ = "";
    
    // 设置纯色背景
    lv_obj_set_size(wallpaper_img_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(wallpaper_img_, 0, 0);
    lv_obj_set_style_bg_color(wallpaper_img_, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(wallpaper_img_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(wallpaper_img_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(wallpaper_img_, 0);
    
    // 保存配置
    SaveWallpaperConfig();
    
    ESP_LOGI(TAG, "Solid color wallpaper set successfully");
}

// 新增：设置图片壁纸（SD卡）
void ClockUI::SetImageWallpaper(const char* image_name) {
    if (!wallpaper_img_ || !image_name) {
        ESP_LOGE(TAG, "SetImageWallpaper: invalid parameters");
        return;
    }
    
    ESP_LOGI(TAG, "Setting image wallpaper from SD: %s", image_name);
    
    // 构建完整文件路径（支持8位大写文件名）
    char file_path[64];
    snprintf(file_path, sizeof(file_path), "/sdcard/%s.JPG", image_name);
    
    // 检查文件是否存在
    FILE* file = fopen(file_path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Image file not found: %s", file_path);
        return;
    }
    fclose(file);
    
    // 解码JPG图片
    uint8_t* rgb565_data = nullptr;
    int width = 0, height = 0;
    
    if (DecodeJpgFromSD(file_path, &rgb565_data, &width, &height)) {
        wallpaper_type_ = WALLPAPER_SD_IMAGE;
        wallpaper_image_name_ = image_name;
        wallpaper_color_ = 0;
        wallpaper_network_url_ = "";
        
        // 创建LVGL图像描述符
        static lv_img_dsc_t img_dsc;
        img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        img_dsc.header.w = width;
        img_dsc.header.h = height;
        img_dsc.data_size = width * height * 2;
        img_dsc.data = rgb565_data;
        
        // 设置图片
        lv_obj_set_size(wallpaper_img_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(wallpaper_img_, 0, 0);
        lv_img_set_src(wallpaper_img_, &img_dsc);
        lv_obj_clear_flag(wallpaper_img_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_to_index(wallpaper_img_, 0);
        
        // 保存配置
        SaveWallpaperConfig();
        
        ESP_LOGI(TAG, "Image wallpaper set successfully: %s (%dx%d)", image_name, width, height);
    } else {
        ESP_LOGE(TAG, "Failed to decode JPG image: %s", file_path);
    }
}

// 新增：设置网络壁纸
void ClockUI::SetNetworkWallpaper(const char* url) {
    if (!wallpaper_img_ || !url) {
        ESP_LOGE(TAG, "SetNetworkWallpaper: invalid parameters");
        return;
    }
    
    ESP_LOGI(TAG, "Setting network wallpaper: %s", url);
    
    // 生成本地文件名（从URL中提取文件名，去掉扩展名）
    std::string url_str(url);
    std::string filename = "HAPPY";  // 默认文件名
    
    size_t pos = url_str.find_last_of('/');
    if (pos != std::string::npos && pos + 1 < url_str.length()) {
        std::string full_name = url_str.substr(pos + 1);
        
        // 去掉文件扩展名（.jpg或.jpeg）
        size_t dot_pos = full_name.find_last_of('.');
        if (dot_pos != std::string::npos) {
            filename = full_name.substr(0, dot_pos);
        } else {
            filename = full_name;
        }
        
        // 限制文件名长度为8个字符
        if (filename.length() > 8) {
            filename = filename.substr(0, 8);
        }
    }
    
    // 转换为大写
    std::transform(filename.begin(), filename.end(), filename.begin(), ::toupper);
    
    ESP_LOGI(TAG, "Generated local filename: %s", filename.c_str());
    
    // 先尝试从SD卡加载（不带扩展名，因为SetImageWallpaper会添加）
    char local_path[64];
    snprintf(local_path, sizeof(local_path), "/sdcard/%s.JPG", filename.c_str());
    
    FILE* file = fopen(local_path, "rb");
    if (file) {
        fclose(file);
        ESP_LOGI(TAG, "Found cached image, using local file: %s", local_path);
        SetImageWallpaper(filename.c_str());
        return;
    }
    
    // 从网络下载
    if (DownloadAndDecodeJpg(url, filename.c_str())) {
        wallpaper_type_ = WALLPAPER_NETWORK_IMAGE;
        wallpaper_network_url_ = url;
        wallpaper_image_name_ = filename;
        wallpaper_color_ = 0;
        
        // 下载成功后加载图片
        SetImageWallpaper(filename.c_str());
        
        ESP_LOGI(TAG, "Network wallpaper downloaded and set successfully");
    } else {
        ESP_LOGE(TAG, "Failed to download network wallpaper: %s", url);
    }
}

// 新增：清除壁纸
void ClockUI::ClearWallpaper() {
    if (!wallpaper_img_) {
        return;
    }
    
    ESP_LOGI(TAG, "Clearing wallpaper");
    
    wallpaper_type_ = WALLPAPER_NONE;
    wallpaper_color_ = 0;
    wallpaper_image_name_ = "";
    wallpaper_network_url_ = "";
    
    // 隐藏壁纸
    lv_obj_add_flag(wallpaper_img_, LV_OBJ_FLAG_HIDDEN);
    
    // 保存配置
    SaveWallpaperConfig();
    
    ESP_LOGI(TAG, "Wallpaper cleared successfully");
}

// 新增：保存壁纸配置
void ClockUI::SaveWallpaperConfig() {
    FILE* file = fopen(WALLPAPER_CONFIG_FILE, "w");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open wallpaper config file for writing");
        return;
    }
    
    fprintf(file, "type=%d\n", (int)wallpaper_type_);
    fprintf(file, "color=0x%06" PRIx32 "\n", wallpaper_color_);
    fprintf(file, "image_name=%s\n", wallpaper_image_name_.c_str());
    fprintf(file, "network_url=%s\n", wallpaper_network_url_.c_str());
    
    fclose(file);
    ESP_LOGI(TAG, "Wallpaper config saved");
}

// 新增：加载壁纸配置
void ClockUI::LoadWallpaperConfig() {
    FILE* file = fopen(WALLPAPER_CONFIG_FILE, "r");
    if (!file) {
        ESP_LOGI(TAG, "No wallpaper config file found, using defaults");
        return;
    }
    
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        // 移除换行符
        line[strcspn(line, "\n")] = 0;
        
        if (strncmp(line, "type=", 5) == 0) {
            wallpaper_type_ = (WallpaperType)atoi(line + 5);
        } else if (strncmp(line, "color=", 6) == 0) {
            wallpaper_color_ = strtol(line + 6, nullptr, 16);
        } else if (strncmp(line, "image_name=", 11) == 0) {
            wallpaper_image_name_ = std::string(line + 11);
        } else if (strncmp(line, "network_url=", 12) == 0) {
            wallpaper_network_url_ = std::string(line + 12);
        }
    }
    
    fclose(file);
    
    // 根据配置应用壁纸
    switch (wallpaper_type_) {
        case WALLPAPER_SOLID_COLOR:
            SetSolidColorWallpaper(wallpaper_color_);
            break;
        case WALLPAPER_SD_IMAGE:
            if (!wallpaper_image_name_.empty()) {
                SetImageWallpaper(wallpaper_image_name_.c_str());
            }
            break;
        case WALLPAPER_NETWORK_IMAGE:
            if (!wallpaper_network_url_.empty()) {
                SetNetworkWallpaper(wallpaper_network_url_.c_str());
            }
            break;
        default:
            break;
    }
    
    ESP_LOGI(TAG, "Wallpaper config loaded and applied");
}

// 新增：只保存纯色壁纸配置而不立即应用
void ClockUI::SaveSolidColorWallpaperConfig(uint32_t color) {
    ESP_LOGI(TAG, "Saving solid color wallpaper config without applying: 0x%06" PRIx32, color);
    
    wallpaper_type_ = WALLPAPER_SOLID_COLOR;
    wallpaper_color_ = color;
    wallpaper_image_name_ = "";
    wallpaper_network_url_ = "";
    
    // 只保存配置，不应用
    SaveWallpaperConfig();
    
    ESP_LOGI(TAG, "Solid color wallpaper config saved (not applied)");
}

// 新增：只保存图片壁纸配置而不立即应用
void ClockUI::SaveImageWallpaperConfig(const char* image_name) {
    if (!image_name) {
        ESP_LOGE(TAG, "SaveImageWallpaperConfig: invalid image_name");
        return;
    }
    
    ESP_LOGI(TAG, "Saving image wallpaper config without applying: %s", image_name);
    
    wallpaper_type_ = WALLPAPER_SD_IMAGE;
    wallpaper_image_name_ = image_name;
    wallpaper_color_ = 0;
    wallpaper_network_url_ = "";
    
    // 只保存配置，不应用
    SaveWallpaperConfig();
    
    ESP_LOGI(TAG, "Image wallpaper config saved (not applied)");
}

// 新增：只保存网络壁纸配置而不立即应用
void ClockUI::SaveNetworkWallpaperConfig(const char* url) {
    if (!url) {
        ESP_LOGE(TAG, "SaveNetworkWallpaperConfig: invalid url");
        return;
    }
    
    ESP_LOGI(TAG, "Saving network wallpaper config without applying: %s", url);
    
    wallpaper_type_ = WALLPAPER_NETWORK_IMAGE;
    wallpaper_network_url_ = url;
    wallpaper_color_ = 0;
    wallpaper_image_name_ = "";
    
    // 只保存配置，不应用
    SaveWallpaperConfig();
    
    ESP_LOGI(TAG, "Network wallpaper config saved (not applied)");
}

// 新增：只保存清除壁纸配置而不立即应用
void ClockUI::SaveClearWallpaperConfig() {
    ESP_LOGI(TAG, "Saving clear wallpaper config without applying");
    
    wallpaper_type_ = WALLPAPER_NONE;
    wallpaper_color_ = 0;
    wallpaper_image_name_ = "";
    wallpaper_network_url_ = "";
    
    // 只保存配置，不应用
    SaveWallpaperConfig();
    
    ESP_LOGI(TAG, "Clear wallpaper config saved (not applied)");
}

// 新增：从SD卡设置动画
void ClockUI::SetAnimationFromSD(const char* anim_name) {
    if (!animation_label_ || !anim_name) {
        ESP_LOGE(TAG, "SetAnimationFromSD: invalid parameters");
        return;
    }
    
    ESP_LOGI(TAG, "Setting animation from SD: %s", anim_name);
    
    current_animation_ = std::string("/sdcard/") + anim_name;
    animation_frame_ = 0;
    
    // 设置动画并显示
    SetAnimation(current_animation_.c_str());
    ShowAnimation(true);
    
    // 保存配置
    SaveAnimationConfig();
}

// 新增：从网络设置动画  
void ClockUI::SetAnimationFromNetwork(const char* url) {
    if (!animation_label_ || !url) {
        ESP_LOGE(TAG, "SetAnimationFromNetwork: invalid parameters");
        return;
    }
    
    ESP_LOGI(TAG, "Setting animation from network: %s", url);
    
    // 这里可以添加从网络下载动画的逻辑
    // 暂时使用URL作为动画路径
    current_animation_ = url;
    animation_frame_ = 0;
    
    SetAnimation(current_animation_.c_str());
    ShowAnimation(true);
    
    SaveAnimationConfig();
}

// 新增：清除动画
void ClockUI::ClearAnimation() {
    if (!animation_label_) {
        return;
    }
    
    ESP_LOGI(TAG, "Clearing animation");
    
    current_animation_ = "";
    animation_frame_ = 0;
    ShowAnimation(false);
    
    SaveAnimationConfig();
}

// 新增：保存动画配置
void ClockUI::SaveAnimationConfig() {
    FILE* file = fopen(ANIMATION_CONFIG_FILE, "w");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open animation config file for writing");
        return;
    }
    
    fprintf(file, "animation_path=%s\n", current_animation_.c_str());
    fprintf(file, "animation_visible=%d\n", animation_visible_ ? 1 : 0);
    
    fclose(file);
    ESP_LOGI(TAG, "Animation config saved");
}

// 新增：加载动画配置
void ClockUI::LoadAnimationConfig() {
    FILE* file = fopen(ANIMATION_CONFIG_FILE, "r");
    if (!file) {
        ESP_LOGI(TAG, "No animation config file found, using defaults");
        return;
    }
    
    char line[256];
    bool should_show_animation = false;
    
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = 0;
        
        if (strncmp(line, "animation_path=", 15) == 0) {
            current_animation_ = std::string(line + 15);
        } else if (strncmp(line, "animation_visible=", 18) == 0) {
            should_show_animation = (atoi(line + 18) == 1);
        }
    }
    
    fclose(file);
    
    // 应用动画配置
    if (!current_animation_.empty()) {
        SetAnimation(current_animation_.c_str());
        ShowAnimation(should_show_animation);
        ESP_LOGI(TAG, "Animation config loaded and applied: %s", current_animation_.c_str());
    }
}

// 新增：只保存SD动画配置而不立即应用
void ClockUI::SaveAnimationFromSDConfig(const char* anim_name) {
    if (!anim_name) {
        ESP_LOGE(TAG, "SaveAnimationFromSDConfig: invalid anim_name");
        return;
    }
    
    ESP_LOGI(TAG, "Saving SD animation config without applying: %s", anim_name);
    
    current_animation_ = std::string("/sdcard/") + anim_name;
    animation_frame_ = 0;
    
    // 只保存配置，不应用
    SaveAnimationConfig();
    
    ESP_LOGI(TAG, "SD animation config saved (not applied)");
}

// 新增：只保存网络动画配置而不立即应用
void ClockUI::SaveAnimationFromNetworkConfig(const char* url) {
    if (!url) {
        ESP_LOGE(TAG, "SaveAnimationFromNetworkConfig: invalid url");
        return;
    }
    
    ESP_LOGI(TAG, "Saving network animation config without applying: %s", url);
    
    current_animation_ = url;
    animation_frame_ = 0;
    
    // 只保存配置，不应用
    SaveAnimationConfig();
    
    ESP_LOGI(TAG, "Network animation config saved (not applied)");
}

// 新增：显示听状态动画
void ClockUI::ShowListenAnimation() {
    if (!animation_label_ || !is_visible_) {
        ESP_LOGE(TAG, "ShowListenAnimation: animation_label_ is null or not visible");
        return;
    }
    
    // 检查LVGL对象有效性
    if (!lv_obj_is_valid(animation_label_)) {
        ESP_LOGE(TAG, "ShowListenAnimation: animation_label_ object is invalid");
        return;
    }
    
    // 检查display_是否为SpiLcdAnimDisplay类型
    if (!display_) {
        ESP_LOGE(TAG, "ShowListenAnimation: display_ is null");
        return;
    }
    
    // 使用static_cast转换为SpiLcdAnimDisplay
    auto* spi_anim_display = static_cast<SpiLcdAnimDisplay*>(display_);
    if (!spi_anim_display) {
        ESP_LOGE(TAG, "ShowListenAnimation: failed to cast display_ to SpiLcdAnimDisplay");
        return;
    }
    
    ESP_LOGI(TAG, "Showing listen animation on animation_label_");
    
    // 显示animation_label_（取消隐藏）
    lv_obj_clear_flag(animation_label_, LV_OBJ_FLAG_HIDDEN);
    
    // 调用SpiLcdAnimDisplay::ShowEmotionImage显示听状态图片0
    spi_anim_display->ShowEmotionImage(animation_label_, 0);
    
    // 设置图片缩放为1/3大小（原图240*240，缩放到80*80）
    // LVGL的zoom值：256表示1倍，85表示约1/3倍 (256/3≈85)
    lv_img_set_zoom(animation_label_, 128);
    
    ESP_LOGI(TAG, "Listen animation displayed successfully with 1/3 scale");
}

// 新增：从SD卡解码JPG图片（完整实现）
bool ClockUI::DecodeJpgFromSD(const char* filename, uint8_t** rgb565_data, int* width, int* height) {
    ESP_LOGI(TAG, "Decoding JPG from SD: %s", filename);
    
    FILE* file = fopen(filename, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open JPG file: %s", filename);
        return false;
    }
    
    // 获取文件大小
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (file_size <= 0) {
        ESP_LOGE(TAG, "Invalid JPG file size: %ld", file_size);
        fclose(file);
        return false;
    }
    
    // 读取文件数据
    uint8_t* jpg_data = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpg_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPG data");
        fclose(file);
        return false;
    }
    
    size_t read_size = fread(jpg_data, 1, file_size, file);
    fclose(file);
    
    if (read_size != file_size) {
        ESP_LOGE(TAG, "Failed to read complete JPG file");
        heap_caps_free(jpg_data);
        return false;
    }
    
    // 检查JPG头部
    if (jpg_data[0] != 0xFF || jpg_data[1] != 0xD8) {
        ESP_LOGE(TAG, "Invalid JPG header: expected FF D8, got %02X %02X", jpg_data[0], jpg_data[1]);
        heap_caps_free(jpg_data);
        return false;
    }
    
    ESP_LOGI(TAG, "Valid JPG header detected");
    
    // 分配TJPGD工作缓冲区
    const size_t work_size = 3100;
    uint8_t* work_buffer = (uint8_t*)heap_caps_malloc(work_size, MALLOC_CAP_INTERNAL);
    if (!work_buffer) {
        ESP_LOGE(TAG, "Failed to allocate TJPGD work buffer");
        heap_caps_free(jpg_data);
        return false;
    }
    
    // 设置解码上下文
    ClockJpegDecodeContext decode_ctx;
    decode_ctx.src_data = jpg_data;
    decode_ctx.src_size = file_size;
    decode_ctx.src_pos = 0;
    decode_ctx.output_buffer = nullptr;
    decode_ctx.output_size = 0;
    decode_ctx.output_pos = 0;
    
    // 初始化TJPGD
    JDEC jdec;
    JRESULT res = jd_prepare(&jdec, clock_tjpgd_input_callback, work_buffer, work_size, &decode_ctx);
    
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "TJPGD prepare failed: %d", res);
        heap_caps_free(work_buffer);
        heap_caps_free(jpg_data);
        return false;
    }
    
    *width = jdec.width;
    *height = jdec.height;
    ESP_LOGI(TAG, "JPG dimensions: %dx%d", *width, *height);
    
    // 检查图片尺寸是否合理
    if (*width == 0 || *height == 0 || *width > 1024 || *height > 1024) {
        ESP_LOGE(TAG, "Invalid JPG dimensions: %dx%d", *width, *height);
        heap_caps_free(work_buffer);
        heap_caps_free(jpg_data);
        return false;
    }
    
    // 分配RGB输出缓冲区
    size_t rgb_size = (*width) * (*height) * 3;
    uint8_t* rgb_buffer = (uint8_t*)heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb_buffer) {
        ESP_LOGE(TAG, "Failed to allocate RGB buffer: %zu bytes", rgb_size);
        heap_caps_free(work_buffer);
        heap_caps_free(jpg_data);
        return false;
    }
    
    // 设置输出缓冲区
    decode_ctx.output_buffer = rgb_buffer;
    decode_ctx.output_size = rgb_size;
    
    // 执行JPG解码
    res = jd_decomp(&jdec, clock_tjpgd_output_callback, 0);
    
    // 清理资源
    heap_caps_free(work_buffer);
    heap_caps_free(jpg_data);
    
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "TJPGD decompress failed: %d", res);
        heap_caps_free(rgb_buffer);
        return false;
    }
    
    ESP_LOGI(TAG, "JPG decoded successfully");
    
    // 转换RGB888到RGB565
    size_t rgb565_size = (*width) * (*height) * 2;
    *rgb565_data = (uint8_t*)heap_caps_malloc(rgb565_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!*rgb565_data) {
        ESP_LOGE(TAG, "Failed to allocate RGB565 buffer: %zu bytes", rgb565_size);
        heap_caps_free(rgb_buffer);
        return false;
    }
    
    uint16_t* rgb565_ptr = (uint16_t*)*rgb565_data;
    for (size_t i = 0; i < (*width) * (*height); i++) {
        uint8_t r = rgb_buffer[i * 3 + 0];
        uint8_t g = rgb_buffer[i * 3 + 1]; 
        uint8_t b = rgb_buffer[i * 3 + 2];
        rgb565_ptr[i] = RGB888ToRGB565(r, g, b);
    }
    
    heap_caps_free(rgb_buffer);
    ESP_LOGI(TAG, "RGB888 to RGB565 conversion completed");
    return true;
}

// 新增：下载并解码JPG（改进实现）
bool ClockUI::DownloadAndDecodeJpg(const char* url, const char* local_filename) {
    ESP_LOGI(TAG, "Downloading and decoding JPG: %s -> %s", url, local_filename);
    
    // 创建HTTP客户端配置
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 30000;
    config.buffer_size = 4096;
    config.buffer_size_tx = 1024;
    config.user_agent = "ESP32-ClockUI/1.0";
    config.method = HTTP_METHOD_GET;
    config.skip_cert_common_name_check = true;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 3;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return false;
    }
    
    esp_http_client_set_header(client, "Accept", "image/jpeg, image/jpg, image/*");
    esp_http_client_set_header(client, "Connection", "close");
    esp_http_client_set_header(client, "Cache-Control", "no-cache");
    
    // 开始下载
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    
    ESP_LOGI(TAG, "HTTP Status: %d, Content-Length: %d", status_code, content_length);
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status: %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    
    // 分配下载缓冲区
    size_t download_size = (content_length > 0) ? content_length : 512 * 1024; // 默认512KB
    uint8_t* download_buffer = (uint8_t*)heap_caps_malloc(download_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!download_buffer) {
        ESP_LOGE(TAG, "Failed to allocate download buffer: %zu bytes", download_size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    
    // 读取数据
    size_t total_read = 0;
    while (total_read < download_size) {
        int data_read = esp_http_client_read(client, (char*)(download_buffer + total_read), 
                                             download_size - total_read);
        if (data_read < 0) {
            ESP_LOGE(TAG, "Error reading HTTP data");
            break;
        } else if (data_read == 0) {
            ESP_LOGI(TAG, "Download completed");
            break;
        }
        total_read += data_read;
    }
    
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    
    if (total_read == 0) {
        ESP_LOGE(TAG, "No data downloaded");
        heap_caps_free(download_buffer);
        return false;
    }
    
    ESP_LOGI(TAG, "Downloaded %zu bytes", total_read);
    
    // 保存到SD卡
    char local_path[64];
    snprintf(local_path, sizeof(local_path), "/sdcard/%s.JPG", local_filename);
    
    FILE* file = fopen(local_path, "wb");
    if (file) {
        size_t written = fwrite(download_buffer, 1, total_read, file);
        fclose(file);
        
        if (written == total_read) {
            ESP_LOGI(TAG, "File saved to SD card: %s", local_path);
            heap_caps_free(download_buffer);
            return true;
        } else {
            ESP_LOGE(TAG, "Failed to write complete file to SD card");
        }
    } else {
        ESP_LOGE(TAG, "Failed to create file on SD card: %s", local_path);
    }
    
    heap_caps_free(download_buffer);
    return false;
}

