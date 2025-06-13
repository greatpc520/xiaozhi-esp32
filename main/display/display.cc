#include <esp_log.h>
#include <esp_err.h>
#include <string>
#include <cstdlib>
#include <cstring>

#include "display.h"
#include "board.h"
#include "application.h"
#include "font_awesome_symbols.h"
#include "audio_codec.h"
#include "settings.h"
#include "assets/lang_config.h"

#define TAG "Display"

Display::Display() {
    // Notification timer
    esp_timer_create_args_t notification_timer_args = {
        .callback = [](void *arg) {
            Display *display = static_cast<Display*>(arg);
            DisplayLockGuard lock(display);
            lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "notification_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&notification_timer_args, &notification_timer_));

    // Create a power management lock
    auto ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_update", &pm_lock_);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "Power management not supported");
    } else {
        ESP_ERROR_CHECK(ret);
    }
}

Display::~Display() {
    if (notification_timer_ != nullptr) {
        esp_timer_stop(notification_timer_);
        esp_timer_delete(notification_timer_);
    }

    if (network_label_ != nullptr) {
        lv_obj_del(network_label_);
        lv_obj_del(notification_label_);
        lv_obj_del(status_label_);
        lv_obj_del(mute_label_);
        lv_obj_del(battery_label_);
        lv_obj_del(emotion_label_);
    }
    if( low_battery_popup_ != nullptr ) {
        lv_obj_del(low_battery_popup_);
    }
    if (pm_lock_ != nullptr) {
        esp_pm_lock_delete(pm_lock_);
    }
}

void Display::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    if (status_label_ == nullptr) {
        return;
    }
    lv_label_set_text(status_label_, status);
    lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
}

void Display::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void Display::ShowNotification(const char* notification, int duration_ms) {
    DisplayLockGuard lock(this);
    if (notification_label_ == nullptr) {
        return;
    }
    lv_label_set_text(notification_label_, notification);
    lv_obj_clear_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    esp_timer_stop(notification_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_, duration_ms * 1000));
}

void Display::UpdateStatusBar(bool update_all) {
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    {
        DisplayLockGuard lock(this);
        if (mute_label_ == nullptr) {
            return;
        }

        // 如果静音状态改变，则更新图标
        if (codec->output_volume() == 0 && !muted_) {
            muted_ = true;
            lv_label_set_text(mute_label_, FONT_AWESOME_VOLUME_MUTE);
        } else if (codec->output_volume() > 0 && muted_) {
            muted_ = false;
            lv_label_set_text(mute_label_, "");
        }
    }

    esp_pm_lock_acquire(pm_lock_);
    // 更新电池图标
    int battery_level;
    bool charging, discharging;
    const char* icon = nullptr;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        if (charging) {
            icon = FONT_AWESOME_BATTERY_CHARGING;
        } else {
            const char* levels[] = {
                FONT_AWESOME_BATTERY_EMPTY, // 0-19%
                FONT_AWESOME_BATTERY_1,    // 20-39%
                FONT_AWESOME_BATTERY_2,    // 40-59%
                FONT_AWESOME_BATTERY_3,    // 60-79%
                FONT_AWESOME_BATTERY_FULL, // 80-99%
                FONT_AWESOME_BATTERY_FULL, // 100%
            };
            icon = levels[battery_level / 20];
        }
        DisplayLockGuard lock(this);
        if (battery_label_ != nullptr && battery_icon_ != icon) {
            battery_icon_ = icon;
            lv_label_set_text(battery_label_, battery_icon_);
        }

        if (low_battery_popup_ != nullptr) {
            if (strcmp(icon, FONT_AWESOME_BATTERY_EMPTY) == 0 && discharging) {
                if (lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { // 如果低电量提示框隐藏，则显示
                    lv_obj_clear_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                    auto& app = Application::GetInstance();
                    app.PlaySound(Lang::Sounds::P3_LOW_BATTERY);
                }
            } else {
                // Hide the low battery popup when the battery is not empty
                if (!lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { // 如果低电量提示框显示，则隐藏
                    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }

    // 每 10 秒更新一次网络图标
    static int seconds_counter = 0;
    if (update_all || seconds_counter++ % 10 == 0) {
        // 升级固件时，不读取 4G 网络状态，避免占用 UART 资源
        auto device_state = Application::GetInstance().GetDeviceState();
        static const std::vector<DeviceState> allowed_states = {
            kDeviceStateIdle,
            kDeviceStateStarting,
            kDeviceStateWifiConfiguring,
            kDeviceStateListening,
            kDeviceStateActivating,
        };
        if (std::find(allowed_states.begin(), allowed_states.end(), device_state) != allowed_states.end()) {
            icon = board.GetNetworkStateIcon();
            if (network_label_ != nullptr && icon != nullptr && network_icon_ != icon) {
                DisplayLockGuard lock(this);
                network_icon_ = icon;
                lv_label_set_text(network_label_, network_icon_);
            }
        }
    }

    esp_pm_lock_release(pm_lock_);
}


void Display::SetEmotion(const char* emotion) {
    struct Emotion {
        const char* icon;
        const char* text;
    };

    static const std::vector<Emotion> emotions = {
        {FONT_AWESOME_EMOJI_NEUTRAL, "neutral"},
        {FONT_AWESOME_EMOJI_HAPPY, "happy"},
        {FONT_AWESOME_EMOJI_LAUGHING, "laughing"},
        {FONT_AWESOME_EMOJI_FUNNY, "funny"},
        {FONT_AWESOME_EMOJI_SAD, "sad"},
        {FONT_AWESOME_EMOJI_ANGRY, "angry"},
        {FONT_AWESOME_EMOJI_CRYING, "crying"},
        {FONT_AWESOME_EMOJI_LOVING, "loving"},
        {FONT_AWESOME_EMOJI_EMBARRASSED, "embarrassed"},
        {FONT_AWESOME_EMOJI_SURPRISED, "surprised"},
        {FONT_AWESOME_EMOJI_SHOCKED, "shocked"},
        {FONT_AWESOME_EMOJI_THINKING, "thinking"},
        {FONT_AWESOME_EMOJI_WINKING, "winking"},
        {FONT_AWESOME_EMOJI_COOL, "cool"},
        {FONT_AWESOME_EMOJI_RELAXED, "relaxed"},
        {FONT_AWESOME_EMOJI_DELICIOUS, "delicious"},
        {FONT_AWESOME_EMOJI_KISSY, "kissy"},
        {FONT_AWESOME_EMOJI_CONFIDENT, "confident"},
        {FONT_AWESOME_EMOJI_SLEEPY, "sleepy"},
        {FONT_AWESOME_EMOJI_SILLY, "silly"},
        {FONT_AWESOME_EMOJI_CONFUSED, "confused"}
    };
    
    // 查找匹配的表情
    std::string_view emotion_view(emotion);
    auto it = std::find_if(emotions.begin(), emotions.end(),
        [&emotion_view](const Emotion& e) { return e.text == emotion_view; });
    
    DisplayLockGuard lock(this);
    if (emotion_label_ == nullptr) {
        return;
    }

    // 如果找到匹配的表情就显示对应图标，否则显示默认的neutral表情
    if (it != emotions.end()) {
        lv_label_set_text(emotion_label_, it->icon);
    } else {
        lv_label_set_text(emotion_label_, FONT_AWESOME_EMOJI_NEUTRAL);
    }
}

void Display::SetIcon(const char* icon) {
    DisplayLockGuard lock(this);
    if (emotion_label_ == nullptr) {
        return;
    }
    lv_label_set_text(emotion_label_, icon);
}

void Display::SetPreviewImage(const lv_img_dsc_t* image) {
    // Do nothing
}

void Display::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }
    lv_label_set_text(chat_message_label_, content);
}

void Display::SetTheme(const std::string& theme_name) {
    current_theme_name_ = theme_name;
    Settings settings("display", true);
    settings.SetString("theme", theme_name);
}

void Display::CreateCanvas() {
    DisplayLockGuard lock(this);
    
    // 如果已经有画布，先销毁
    if (canvas_ != nullptr) {
        DestroyCanvas();
    }
    
    // 创建画布所需的缓冲区
    // 每个像素2字节(RGB565)
    size_t buf_size = width_ * height_ * 2;  // RGB565: 2 bytes per pixel
    
    // 分配内存，优先使用PSRAM
    canvas_buffer_ = heap_caps_malloc(buf_size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (canvas_buffer_ == nullptr) {
        ESP_LOGE("Display", "Failed to allocate canvas buffer");
        return;
    }
    
    // 获取活动屏幕
    lv_obj_t* screen = lv_screen_active();
    
    // 创建画布对象
    canvas_ = lv_canvas_create(screen);
    if (canvas_ == nullptr) {
        ESP_LOGE("Display", "Failed to create canvas");
        heap_caps_free(canvas_buffer_);
        canvas_buffer_ = nullptr;
        return;
    }
    
    // 初始化画布
    lv_canvas_set_buffer(canvas_, canvas_buffer_, width_, height_, LV_COLOR_FORMAT_RGB565);
    
    // 设置画布位置为全屏
    lv_obj_set_pos(canvas_, 0, 0);
    lv_obj_set_size(canvas_, width_, height_);
    
    // 设置画布为透明
    lv_canvas_fill_bg(canvas_, lv_color_make(0, 0, 0), LV_OPA_TRANSP);
    
    // 设置画布为顶层
    lv_obj_move_foreground(canvas_);
    
    ESP_LOGI("Display", "Canvas created successfully");
}

void Display::DestroyCanvas() {
    DisplayLockGuard lock(this);
    
    if (canvas_ != nullptr) {
        lv_obj_del(canvas_);
        canvas_ = nullptr;
    }
    
    if (canvas_buffer_ != nullptr) {
        heap_caps_free(canvas_buffer_);
        canvas_buffer_ = nullptr;
    }
    
    ESP_LOGI("Display", "Canvas destroyed");
}

void Display::DrawImageOnCanvas(int x, int y, int width, int height, const uint8_t* img_data) {
    DisplayLockGuard lock(this);
    
    // 确保有画布
    if (canvas_ == nullptr) {
        ESP_LOGE("Display", "Canvas not created");
        return;
    }

    // 目标显示区域
    const int target_dim = 240;
    int crop_x = 0, crop_y = 0, crop_w = width, crop_h = height;
    // 居中裁剪为正方形
    if (width > height) {
        crop_w = height;
        crop_x = (width - crop_w) / 2;
        crop_y = 0;
        crop_h = height;
    } else if (height > width) {
        crop_h = width;
        crop_y = (height - crop_h) / 2;
        crop_x = 0;
        crop_w = width;
    }
    // 现在裁剪区域是crop_x, crop_y, crop_w, crop_h，且crop_w==crop_h

    // 分配缩放后buffer
    uint8_t* scaled_buf = (uint8_t*)heap_caps_malloc(target_dim * target_dim * 2, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!scaled_buf) {
        ESP_LOGE("Display", "Failed to alloc scale buffer");
        return;
    }
    // 双线性插值缩放RGB565
    const uint16_t* src = (const uint16_t*)img_data;
    uint16_t* dst = (uint16_t*)scaled_buf;
    float scale = (float)crop_w / target_dim;
    for (int j = 0; j < target_dim; ++j) {
        float src_yf = crop_y + (j + 0.5f) * scale - 0.5f;
        int y0 = (int)src_yf;
        int y1 = y0 + 1;
        float wy = src_yf - y0;
        if (y1 >= crop_y + crop_h) y1 = crop_y + crop_h - 1;
        for (int i = 0; i < target_dim; ++i) {
            float src_xf = crop_x + (i + 0.5f) * scale - 0.5f;
            int x0 = (int)src_xf;
            int x1 = x0 + 1;
            float wx = src_xf - x0;
            if (x1 >= crop_x + crop_w) x1 = crop_x + crop_w - 1;
            // 取四个点
            uint16_t c00 = src[y0 * width + x0];
            uint16_t c10 = src[y0 * width + x1];
            uint16_t c01 = src[y1 * width + x0];
            uint16_t c11 = src[y1 * width + x1];
            // 分别拆分RGB565
            auto unpack = [](uint16_t c, int& r, int& g, int& b) {
                r = (c >> 11) & 0x1F;
                g = (c >> 5) & 0x3F;
                b = c & 0x1F;
            };
            int r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
            unpack(c00, r00, g00, b00);
            unpack(c10, r10, g10, b10);
            unpack(c01, r01, g01, b01);
            unpack(c11, r11, g11, b11);
            // 双线性插值
            float r0 = r00 * (1 - wx) + r10 * wx;
            float r1 = r01 * (1 - wx) + r11 * wx;
            float r = r0 * (1 - wy) + r1 * wy;
            float g0 = g00 * (1 - wx) + g10 * wx;
            float g1 = g01 * (1 - wx) + g11 * wx;
            float g = g0 * (1 - wy) + g1 * wy;
            float b0 = b00 * (1 - wx) + b10 * wx;
            float b1 = b01 * (1 - wx) + b11 * wx;
            float b = b0 * (1 - wy) + b1 * wy;
            // 合成RGB565
            int ri = (int)(r + 0.5f);
            int gi = (int)(g + 0.5f);
            int bi = (int)(b + 0.5f);
            if (ri > 31) ri = 31;
            if (gi > 63) gi = 63;
            if (bi > 31) bi = 31;
            dst[j * target_dim + i] = (ri << 11) | (gi << 5) | bi;
        }
    }
    // 检查参数是否有效
    if (x < 0 || y < 0 || (x + target_dim) > width_ || (y + target_dim) > height_) {
        ESP_LOGE("Display", "Invalid coordinates: x=%d, y=%d, w=%d, h=%d, screen: %dx%d", 
                x, y, target_dim, target_dim, width_, height_);
        heap_caps_free(scaled_buf);
        return;
    }
    // 创建一个描述器来映射图像数据
    const lv_image_dsc_t img_dsc = {
        .header = {
            .magic = LV_IMAGE_HEADER_MAGIC,
            .cf = LV_COLOR_FORMAT_RGB565,
            .flags = 0,
            .w = (uint32_t)target_dim,
            .h = (uint32_t)target_dim,
            .stride = (uint32_t)(target_dim * 2),  // RGB565: 2 bytes per pixel
            .reserved_2 = 0,
        },
        .data_size = (uint32_t)(target_dim * target_dim * 2),  // RGB565: 2 bytes per pixel
        .data = scaled_buf,
        .reserved = NULL
    };
    // 使用图层绘制图像到画布上
    lv_layer_t layer;
    lv_canvas_init_layer(canvas_, &layer);
    lv_draw_image_dsc_t draw_dsc;
    lv_draw_image_dsc_init(&draw_dsc);
    draw_dsc.src = &img_dsc;
    lv_area_t area;
    area.x1 = x;
    area.y1 = y;
    area.x2 = x + target_dim - 1;
    area.y2 = y + target_dim - 1;
    lv_draw_image(&layer, &draw_dsc, &area);
    lv_canvas_finish_layer(canvas_, &layer);
    // 确保画布在最上层
    lv_obj_move_foreground(canvas_);
    heap_caps_free(scaled_buf);
    // ESP_LOGI("Display", "Image drawn on canvas at x=%d, y=%d, w=%d, h=%d", x, y, target_dim, target_dim);
}
