#include <lvgl.h>
#include "spi_lcd_anim_display.h"
// #include "lcd_display.h"
#include "font_awesome_symbols.h"
#include "assets/lang_config.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <algorithm>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "lodepng.h"
#include "tjpgd.h" // 假设你已集成tjpgd库
static uint8_t *rgb565a8_data = (uint8_t *)heap_caps_malloc(240 * 240 * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
// uint16_t *rgb565_data = (uint16_t *)heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
static uint8_t *httpbuffer = NULL; //(uint8_t *)heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
static size_t image_buffer_size = 0;
#include "esp_http_client.h"

#include "camera_service.h"

#define TAG "SpiLcdAnimDisplay"


#define FRAME_WIDTH  240
#define FRAME_HEIGHT 240
#define FRAME_SIZE   (FRAME_WIDTH * FRAME_HEIGHT * 2)
#define MAX_SPEAK_FRAMES 40
#define MAX_LISTEN_FRAMES 10

static uint8_t* speak_anim_cache[MAX_SPEAK_FRAMES] = {nullptr};
static int speak_anim_cache_count = 0;
static uint8_t* listen_anim_cache[MAX_LISTEN_FRAMES] = {nullptr};
static int listen_anim_cache_count = 0;
static uint8_t* idle_img_cache = nullptr;

// Color definitions for dark theme
#define DARK_BACKGROUND_COLOR       lv_color_hex(0x121212)     // Dark background
#define DARK_TEXT_COLOR             lv_color_white()           // White text
#define DARK_CHAT_BACKGROUND_COLOR  lv_color_hex(0x1E1E1E)     // Slightly lighter than background
#define DARK_USER_BUBBLE_COLOR      lv_color_hex(0x1A6C37)     // Dark green
#define DARK_ASSISTANT_BUBBLE_COLOR lv_color_hex(0x333333)     // Dark gray
#define DARK_SYSTEM_BUBBLE_COLOR    lv_color_hex(0x2A2A2A)     // Medium gray
#define DARK_SYSTEM_TEXT_COLOR      lv_color_hex(0xAAAAAA)     // Light gray text
#define DARK_BORDER_COLOR           lv_color_hex(0x333333)     // Dark gray border
#define DARK_LOW_BATTERY_COLOR      lv_color_hex(0xFF0000)     // Red for dark mode

// Color definitions for light theme
#define LIGHT_BACKGROUND_COLOR       lv_color_white()           // White background
#define LIGHT_TEXT_COLOR             lv_color_black()           // Black text
#define LIGHT_CHAT_BACKGROUND_COLOR  lv_color_hex(0xE0E0E0)     // Light gray background
#define LIGHT_USER_BUBBLE_COLOR      lv_color_hex(0x95EC69)     // WeChat green
#define LIGHT_ASSISTANT_BUBBLE_COLOR lv_color_white()           // White
#define LIGHT_SYSTEM_BUBBLE_COLOR    lv_color_hex(0xE0E0E0)     // Light gray
#define LIGHT_SYSTEM_TEXT_COLOR      lv_color_hex(0x666666)     // Dark gray text
#define LIGHT_BORDER_COLOR           lv_color_hex(0xE0E0E0)     // Light gray border
#define LIGHT_LOW_BATTERY_COLOR      lv_color_black()           // Black for light mode

// Theme color structure
struct ThemeColors {
    lv_color_t background;
    lv_color_t text;
    lv_color_t chat_background;
    lv_color_t user_bubble;
    lv_color_t assistant_bubble;
    lv_color_t system_bubble;
    lv_color_t system_text;
    lv_color_t border;
    lv_color_t low_battery;
};
// Define dark theme colors
static const ThemeColors DARK_THEME = {
    .background = DARK_BACKGROUND_COLOR,
    .text = DARK_TEXT_COLOR,
    .chat_background = DARK_CHAT_BACKGROUND_COLOR,
    .user_bubble = DARK_USER_BUBBLE_COLOR,
    .assistant_bubble = DARK_ASSISTANT_BUBBLE_COLOR,
    .system_bubble = DARK_SYSTEM_BUBBLE_COLOR,
    .system_text = DARK_SYSTEM_TEXT_COLOR,
    .border = DARK_BORDER_COLOR,
    .low_battery = DARK_LOW_BATTERY_COLOR
};

// Define light theme colors
static const ThemeColors LIGHT_THEME = {
    .background = LIGHT_BACKGROUND_COLOR,
    .text = LIGHT_TEXT_COLOR,
    .chat_background = LIGHT_CHAT_BACKGROUND_COLOR,
    .user_bubble = LIGHT_USER_BUBBLE_COLOR,
    .assistant_bubble = LIGHT_ASSISTANT_BUBBLE_COLOR,
    .system_bubble = LIGHT_SYSTEM_BUBBLE_COLOR,
    .system_text = LIGHT_SYSTEM_TEXT_COLOR,
    .border = LIGHT_BORDER_COLOR,
    .low_battery = LIGHT_LOW_BATTERY_COLOR
};

// Current theme - initialize based on default config
static ThemeColors current_theme = LIGHT_THEME;


LV_FONT_DECLARE(font_awesome_30_4);
struct AnimFramesTaskParam {
    int role_id;
    SpiLcdAnimDisplay* display;
};

static void FreeCache(uint8_t** cache, int max_count) {
    for (int i = 0; i < max_count; ++i) {
        if (cache[i]) {
            heap_caps_free(cache[i]);
            cache[i] = nullptr;
        }
    }
}

static void LoadRawFrames(const std::string& dir, uint8_t** cache, int max_count, int& out_count) {
    FreeCache(cache, max_count);
    out_count = 0;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    std::vector<std::string> files;
    while ((ent = readdir(d)) != nullptr) {
        if (strstr(ent->d_name, ".raw") || strstr(ent->d_name, ".RAW")) {
            files.push_back(dir + "/" + ent->d_name);
        }
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    int count = std::min((int)files.size(), max_count);
    for (int i = 0; i < count; ++i) {
        FILE* fp = fopen(files[i].c_str(), "rb");
        if (fp) {
            cache[i] = (uint8_t*)heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (cache[i]) {
                size_t read_bytes = fread(cache[i], 1, FRAME_SIZE, fp);
                if (read_bytes == FRAME_SIZE) {
                    ++out_count;
                } else {
                    heap_caps_free(cache[i]);
                    cache[i] = nullptr;
                }
            }
            fclose(fp);
        }
    }
    ESP_LOGI(TAG, "LoadRawFrames: %s, count: %d", dir.c_str(), out_count);
}

static void LoadIdleRaw(const std::string& dir) {
    if (idle_img_cache) {
        heap_caps_free(idle_img_cache);
        idle_img_cache = nullptr;
    }
    DIR* d = opendir(dir.c_str());
    if (!d) {
        ESP_LOGE(TAG, "LoadIdleRaw: %s,opendir failed", dir.c_str());
        return;
    }
    struct dirent* ent;
    std::vector<std::string> files;
    while ((ent = readdir(d)) != nullptr) {
        if (strstr(ent->d_name, ".raw") || strstr(ent->d_name, ".RAW")) {
            files.push_back(dir + "/" + ent->d_name);
        }
    }
    closedir(d);
    if (!files.empty()) {
        FILE* fp = fopen(files[0].c_str(), "rb");
        if (fp) {
            idle_img_cache = (uint8_t*)heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (idle_img_cache) {
                // fread(idle_img_cache, 1, FRAME_SIZE, fp);
                size_t read_bytes = fread(idle_img_cache, 1, FRAME_SIZE, fp);
                if (read_bytes == FRAME_SIZE) {
                    ESP_LOGI(TAG, "LoadIdleRaw: %s,fread success", files[0].c_str());
                } else {
                    heap_caps_free(idle_img_cache);
                    idle_img_cache = nullptr;
                    ESP_LOGE(TAG, "LoadIdleRaw: %s,fread failed", files[0].c_str());
                }
            }else{
                ESP_LOGE(TAG, "LoadIdleRaw: %s,idle_img_cache failed", files[0].c_str());
            }
            fclose(fp);
        }else{
            ESP_LOGE(TAG, "LoadIdleRaw: %s,fopen failed", files[0].c_str());
        }
    }else{        
        ESP_LOGE(TAG, "LoadIdleRaw: %s,files.empty failed", dir.c_str());
    }
}

static void anim_frames_ready_cb(void* param) {
    SpiLcdAnimDisplay* display = (SpiLcdAnimDisplay*)param;
    display->OnFramesLoaded();
    ESP_LOGI(TAG, "anim_frames_ready_cb");
}

static void anim_frames_load_task(void* arg) {
    AnimFramesTaskParam* param = (AnimFramesTaskParam*)arg;
    int role_id = param->role_id;
    SpiLcdAnimDisplay* display = param->display;
    char base[128];
    snprintf(base, sizeof(base), "/sdcard/%d", role_id);
    // LoadIdleRaw(std::string(base) + "/standby");
    LoadRawFrames(std::string(base) + "/listen", listen_anim_cache, MAX_LISTEN_FRAMES, listen_anim_cache_count);
    LoadRawFrames(std::string(base) + "/speak", speak_anim_cache, MAX_SPEAK_FRAMES, speak_anim_cache_count);
    
    ESP_LOGI(TAG, "anim_frames_load_task: %d", role_id);
    lv_async_call(anim_frames_ready_cb, display);
    delete param;

    vTaskDelete(NULL);
}

SpiLcdAnimDisplay::SpiLcdAnimDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy,
    DisplayFonts fonts)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy, fonts) {
    role_id_ = 0;
    anim_img_obj_ = nullptr;
    current_state_ = "idle";
    SetupUI();
}

void SpiLcdAnimDisplay::SetupUI() {
    DisplayLockGuard lock(this);
    auto screen = lv_screen_active();
    // 1. 创建动画容器（最底层）
    // lv_obj_t* anim_bg_container_ = lv_obj_create(screen);
    // lv_obj_set_size(anim_bg_container_, width_, height_);
    // lv_obj_clear_flag(anim_bg_container_, LV_OBJ_FLAG_SCROLLABLE);
    // lv_obj_set_style_bg_color(anim_bg_container_, lv_color_black(), 0);
    // lv_obj_set_style_pad_all(anim_bg_container_, 0, 0);
    // lv_obj_set_style_bg_opa(anim_bg_container_, LV_OPA_TRANSP, 0); // 透明背景

    // 2. 创建动画图片对象，挂到动画容器
    anim_img_obj_ = lv_img_create(screen);
    lv_obj_set_size(anim_img_obj_, width_, height_);
    lv_obj_align(anim_img_obj_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(anim_img_obj_, LV_OPA_TRANSP, 0); // 透明背景
    lv_obj_set_style_text_font(anim_img_obj_, fonts_.text_font, 0);
    lv_obj_set_style_text_color(anim_img_obj_, current_theme.text, 0);
    lv_obj_set_style_bg_color(anim_img_obj_, current_theme.background, 0);
    lv_obj_set_style_pad_all(anim_img_obj_, 0, 0);
    lv_anim_init(&anim_);
    lv_anim_set_var(&anim_, anim_img_obj_);
    // return;
    // 3. 创建表情图片对象，挂到动画容器
    emotion_label_img = lv_img_create(anim_img_obj_);
    // 设置图片控件大小为自适应内容
    lv_obj_set_size(emotion_label_img, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    // 设置在父容器中的位置（左上角）
    lv_obj_set_pos(emotion_label_img, 0, 0);
    // 禁用自动布局和滚动
    lv_obj_add_flag(emotion_label_img, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(emotion_label_img, LV_OBJ_FLAG_SCROLLABLE);
    // 移除所有内边距和外边距
    lv_obj_set_style_pad_all(emotion_label_img, 0, 0);
    lv_obj_set_style_margin_all(emotion_label_img, 0, 0);
    lv_obj_set_style_bg_opa(emotion_label_img, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(emotion_label_img, 0, 0);

/* Container */
    container_ = lv_obj_create(anim_img_obj_);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, current_theme.background, 0);
    lv_obj_set_style_border_color(container_, current_theme.border, 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_TRANSP, 0); // 透明背景

    /* Status bar */
    status_bar_ = lv_obj_create(container_);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_color(status_bar_, current_theme.background, 0);
    lv_obj_set_style_text_color(status_bar_, current_theme.text, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0); // 透明背景
    
    
    /* Content - Chat area */
    // content_ = lv_obj_create(container_);
    // lv_obj_set_style_radius(content_, 0, 0);
    // lv_obj_set_width(content_, LV_HOR_RES);
    // lv_obj_set_flex_grow(content_, 1);
    // lv_obj_set_style_pad_all(content_, 10, 0);
    // lv_obj_set_style_bg_color(content_, current_theme.chat_background, 0); // Background for chat area
    // lv_obj_set_style_border_color(content_, current_theme.border, 0); // Border color for chat area

    // // Enable scrolling for chat content
    // lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    // lv_obj_set_scroll_dir(content_, LV_DIR_VER);
    
    // // Create a flex container for chat messages
    // lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    // lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // lv_obj_set_style_pad_row(content_, 10, 0); // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    /* Status bar */
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);
    lv_obj_set_style_pad_left(status_bar_, 10, 0);
    lv_obj_set_style_pad_right(status_bar_, 10, 0);
    lv_obj_set_style_pad_top(status_bar_, 2, 0);
    lv_obj_set_style_pad_bottom(status_bar_, 2, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    // 设置状态栏的内容垂直居中
    lv_obj_set_flex_align(status_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 创建emotion_label_在状态栏最左侧
    // emotion_label_ = lv_label_create(status_bar_);
    // lv_obj_set_style_text_font(emotion_label_, &font_awesome_30_4, 0);
    // lv_obj_set_style_text_color(emotion_label_, current_theme.text, 0);
    // lv_label_set_text(emotion_label_, FONT_AWESOME_AI_CHIP);
    // lv_obj_set_style_margin_right(emotion_label_, 5, 0); // 添加右边距，与后面的元素分隔

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, current_theme.text, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, current_theme.text, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    
    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, current_theme.text, 0);

    // network_label_ = lv_label_create(status_bar_);
    // lv_label_set_text(network_label_, "");
    // lv_obj_set_style_text_font(network_label_, fonts_.icon_font, 0);
    // lv_obj_set_style_text_color(network_label_, current_theme.text, 0);
    // lv_obj_set_style_margin_left(network_label_, 5, 0); // 添加左边距，与前面的元素分隔
    // lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);

    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, current_theme.text, 0);
    lv_obj_set_style_margin_left(battery_label_, 5, 0); // 添加左边距，与前面的元素分隔

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, fonts_.text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, current_theme.low_battery, 0);
    lv_obj_set_style_radius(low_battery_popup_, 10, 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    // LoadFrames();
    LoadIdleRaw(std::string("/sdcard/1/standby"));
    // ShowIdleImage();
    // SetRoleId(1);
    // SetAnimState("idle");
    
    ESP_LOGI(TAG, "SetupUI: %s", current_state_.c_str());
    // StartFrameQueueTask();
}

void SpiLcdAnimDisplay::SetRoleId(int role_id) {
    if (role_id_ == role_id || role_id <= 0) return;
    ESP_LOGI(TAG, "SetRoleId: %d", role_id);
    role_id_ = role_id;
    LoadFrames();
    SetAnimState("idle");

}

void SpiLcdAnimDisplay::SetAnimState(const std::string& state) {
    // static uint32_t last_change_time = 0;
    // uint32_t now = lv_tick_get();
    // if (state == current_state_ && state !="idle") return;
    // if (state == current_state_ && (now - last_change_time) < 1000) {
    //     // 状态相同且小于1秒，不执行
    //     return;
    // }
    // last_change_time = now;
    if (state == current_state_) return;
    // StopAnim();
    current_state_ = state;
    // if (state == "speak" && speak_anim_cache_count > 0) {
    //     frame_count_ = speak_anim_cache_count;
    //     anim_fps_ = 8;
    //     StartAnim();
    // } else if (state == "listen" && listen_anim_cache_count > 0) {
    //     frame_count_ = listen_anim_cache_count;
    //     anim_fps_ = 8;
    //     StartAnim();
    // } else {
    //     ShowIdleImage();
    // }
    if (state == "idle" && idle_img_cache) {
        lv_async_call([](void* param){
            SpiLcdAnimDisplay* self = static_cast<SpiLcdAnimDisplay*>(param);
            self->ShowIdleImage();
        }, this);
    } else {
        OnFramesLoaded();
    }
    ESP_LOGI(TAG, "SetAnimState: %s", current_state_.c_str());
}

void SpiLcdAnimDisplay::LoadFrames() {
    static int last_role_id = -1;
    if (last_role_id == role_id_) return;
    last_role_id = role_id_;
    // 启动异步任务加载动画帧
    AnimFramesTaskParam* param = new AnimFramesTaskParam{role_id_, this};
    xTaskCreate(anim_frames_load_task, "anim_load", 8192, param, 1, NULL);
}

void SpiLcdAnimDisplay::OnFramesLoaded() {
    // return;
    // LVGL主线程回调，加载完成后重启动画或显示idle
    if (current_state_ == "speak" && speak_anim_cache_count > 0) {
        frame_count_ = speak_anim_cache_count;
        anim_fps_ = 8;
        StartAnim();
    } else if (current_state_ == "listen" && listen_anim_cache_count > 0) {
        frame_count_ = listen_anim_cache_count;
        anim_fps_ = 8;
        StartAnim();
    } else {
        // ShowIdleImage();
    }
    ESP_LOGI(TAG, "OnFramesLoaded: %s", current_state_.c_str());
}

static void anim_exec_cb(void* obj, int32_t frame) {
    SpiLcdAnimDisplay* self = static_cast<SpiLcdAnimDisplay*>(lv_obj_get_user_data((lv_obj_t*)obj));
        if (!self) return;
    // 判断帧缓存是否加载完成
    if (self->current_state_ == "speak" && (frame >= speak_anim_cache_count || !speak_anim_cache[frame])) return;
    if (self->current_state_ == "listen" && (frame >= listen_anim_cache_count || !listen_anim_cache[frame])) return;
    
    static lv_img_dsc_t img_desc;
    img_desc.header.cf = LV_COLOR_FORMAT_RGB565;
    img_desc.header.w = FRAME_WIDTH;
    img_desc.header.h = FRAME_HEIGHT;
    img_desc.data_size = FRAME_SIZE;
    if (self->current_state_ == "speak" && frame < speak_anim_cache_count && speak_anim_cache[frame]) {
        img_desc.data = speak_anim_cache[frame];
    } else if (self->current_state_ == "listen" && frame < listen_anim_cache_count && listen_anim_cache[frame]) {
        img_desc.data = listen_anim_cache[frame];
    } else {
        return;
    }
    lv_img_set_src((lv_obj_t*)obj, &img_desc);
}

void SpiLcdAnimDisplay::StartAnim() {
    if (frame_count_ <= 0) return;
    StopAnim();
    // lv_anim_init(&anim_);
    // lv_anim_set_var(&anim_, anim_img_obj_);
    lv_obj_set_user_data(anim_img_obj_, this);
    lv_anim_set_exec_cb(&anim_, anim_exec_cb);
    lv_anim_set_values(&anim_, 0, frame_count_ - 1);
    lv_anim_set_time(&anim_, frame_count_ * 1000 / anim_fps_);
    lv_anim_set_repeat_count(&anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim_, lv_anim_path_linear);
    if (current_state_ == "listen")
        lv_anim_set_repeat_delay(&anim_, 12000); // 每次重复之间延迟1000ms
    else
        lv_anim_set_repeat_delay(&anim_, 100);
    lv_anim_start(&anim_);
    ESP_LOGI(TAG, "StartAnim: %s", current_state_.c_str());
}

void SpiLcdAnimDisplay::StopAnim() {
    lv_anim_del(anim_img_obj_, nullptr);
    ESP_LOGI(TAG, "StopAnim: %s", current_state_.c_str());
}

void SpiLcdAnimDisplay::ShowIdleImage() {
    // return;
    if (!idle_img_cache)
    {
        ESP_LOGE(TAG, "idle_img_cache is NULL");
        return;
    }
    // StopAnim();
    static lv_img_dsc_t img_desc;
    img_desc.header.cf = LV_COLOR_FORMAT_RGB565;
    img_desc.header.w = FRAME_WIDTH;
    img_desc.header.h = FRAME_HEIGHT;
    img_desc.data_size = FRAME_SIZE;
    img_desc.data = idle_img_cache;
    ESP_LOGI(TAG, "ShowIdleImage: img_desc=%p, data=%p", &img_desc, idle_img_cache);
    lv_img_set_src(anim_img_obj_, &img_desc);
    ESP_LOGI(TAG, "ShowIdleImage: %s", current_state_.c_str());
}

// 定义图片下载任务的结构体
struct DownloadImageParams
{
    const char *url;
    lv_obj_t *target_obj;
    LcdDisplay *display;
};
// 定义消息结构体
typedef struct
{
    lv_obj_t *obj;
    lv_img_dsc_t *img_desc;
} lvgl_img_update_t;

// LVGL更新回调函数
static void lvgl_update_cb(void *data)
{
    lvgl_img_update_t *update = (lvgl_img_update_t *)data;
    if (update && update->obj && update->img_desc)
    {
        lv_img_set_src(update->obj, update->img_desc);
        // lv_img_set_src(update->obj, update->img_desc);
        // lv_obj_set_style_img_recolor_opa(update->obj, LV_OPA_TRANSP, 0);  // 设置重新着色的透明度
    }
    free(data);
}
// RGB888转RGB565函数
uint16_t RGB888ToRGB565(uint8_t r, uint8_t g, uint8_t b)
{
    // 先转换为RGB565格式
    uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);

    // 如果颜色看起来不对，可以尝试以下几种字节序:

    // 方案1: 不交换字节序
    return color;

    // 方案2: 交换字节序
    // return (color >> 8) | (color << 8);

    // 方案3: 反转RGB顺序
    // return ((b & 0xF8) << 8) | ((g & 0xFC) << 3) | (r >> 3);
}
// 添加一个判断是否接近黑色的辅助函数
bool isNearBlack(uint8_t r, uint8_t g, uint8_t b, uint8_t threshold)
{
    return (r <= threshold && g <= threshold && b <= threshold);
}

esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    // static uint8_t *buffer = NULL;
    static size_t buffer_len = 0;

    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_DATA:
        if (!httpbuffer)
        {
            // 分配内部RAM内存来存储下载的数据
            httpbuffer = (uint8_t *)heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!httpbuffer)
            {
                ESP_LOGE(TAG, "Failed to allocate httpbuffer in internal RAM");
                return ESP_FAIL;
            }
            buffer_len = 0;
            ESP_LOGI(TAG, "httpbuffer allocated for HTTP data, size: %zu bytes", evt->data_len);
        }
        memcpy(httpbuffer + buffer_len, evt->data, evt->data_len);
        buffer_len += evt->data_len;
        // ESP_LOGI(TAG, "Received %zu bytes, total httpbuffer length: %zu bytes", evt->data_len, buffer_len);
        image_buffer_size = buffer_len;
        break;

    case HTTP_EVENT_DISCONNECTED:
        if (httpbuffer)
        {
            image_buffer_size = buffer_len;
            ESP_LOGE(TAG, "Download completed, httpbuffer length: %zu", buffer_len);
            // 下载完成，调用LoadPngToRgb565进行解码和显示
            // if (LoadPngToRgb565(httpbuffer, buffer_len)) {
            //     // 显示图片
            //     esp_lcd_panel_draw_bitmap(panel, 0, 0, 240, 240, rgb565_data);
            //     ESP_LOGI(TAG, "Image displayed successfully");
            // } else {
            //     ESP_LOGE(TAG, "Failed to load PNG image");
            // }
            // 释放下载缓冲区
            // heap_caps_free(httpbuffer);
            // httpbuffer = NULL;
            // ESP_LOGI(TAG, "Download httpbuffer freed");
        }
        break;
    default:
        // 处理其他事件
        break;
    }
    return ESP_OK;
}
void download_image(const char *url)
{
    esp_http_client_config_t config = {
        .url = url, //"http://d.schbcq.com/tpad/espk/happy.png",
        .event_handler = http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "HTTP client cleaned up");
}

// 添加重试下载函数
bool downloadImageWithRetry(const char *url, int max_retries = 3, int retry_delay_ms = 1000)
{
    for (int i = 0; i < max_retries; i++)
    {
        if (i > 0)
        {
            ESP_LOGI(TAG, "Retrying download attempt %d/%d", i + 1, max_retries);
            vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
        }

        download_image(url);
        if (httpbuffer && image_buffer_size > 0)
        {
            return true;
        }

        if (httpbuffer)
        {
            heap_caps_free(httpbuffer);
            httpbuffer = NULL;
        }
    }
    return false;
}







// 添加新的函数用于下载和解码图片
bool downloadAndDecodeImage(const char *url, lv_img_dsc_t &img_desc)
{
    ESP_LOGI(TAG, "Starting image download from URL: %s", url);

    // 下载图片（带重试机制）
    if (!downloadImageWithRetry(url))
    {
        ESP_LOGE(TAG, "Failed to download image after all retries");
        return false;
    }

    // 解码PNG
    unsigned width, height;
    std::vector<unsigned char> image;

    try
    {
        // 添加数据完整性检查
        if (image_buffer_size < 8 || httpbuffer[0] != 0x89 || httpbuffer[1] != 'P' ||
            httpbuffer[2] != 'N' || httpbuffer[3] != 'G')
        {
            ESP_LOGE(TAG, "Invalid PNG header or incomplete download");
            heap_caps_free(httpbuffer);
            httpbuffer = NULL;
            return false;
        }
        // unsigned error = lodepng::decode(image, width, height, httpbuffer, image_buffer_size, LCT_RGB);
        // 在解码前设置状态，禁用CRC检查
        lodepng::State state;
        state.decoder.ignore_crc = 1;
        // 修改解码参数，使用 RGBA 格式
        unsigned error = lodepng::decode(image, width, height, httpbuffer, image_buffer_size, LCT_RGBA);

        // 释放下载缓冲区
        if (error)
        {
            ESP_LOGE(TAG, "Error decoding PNG: %s", lodepng_error_text(error));
            heap_caps_free(httpbuffer);
            httpbuffer = NULL;
            return false;
        }
    }
    catch (const std::exception &e)
    {
        ESP_LOGE(TAG, "Exception during PNG decoding: %s", e.what());
        heap_caps_free(httpbuffer);
        httpbuffer = NULL;
        return false;
    }

    // 检查图片尺寸是否合理
    if (width == 0 || height == 0 || width > 1024 || height > 1024)
    {
        ESP_LOGE(TAG, "Invalid image dimensions: %ux%u", width, height);
        heap_caps_free(httpbuffer);
        httpbuffer = NULL;
        return false;
    }

    // 分配RGB565缓冲区到SPIRAM
    // size_t rgb565_size = width * height * sizeof(uint16_t);
    size_t rgb565a8_size = width * height * (sizeof(uint16_t) + sizeof(uint8_t)); // RGB565 + A8
    if (!rgb565a8_data)
    {
        ESP_LOGE(TAG, "Failed to allocate RGB565 buffer in SPIRAM");
        heap_caps_free(httpbuffer);
        httpbuffer = NULL;
        return false;
    }

    // 转换颜色格式
    try
    {
        // for (size_t i = 0; i < width * height; i++) {
        //     uint8_t r = image[i * 3 + 0];
        //     uint8_t g = image[i * 3 + 1];
        //     uint8_t b = image[i * 3 + 2];
        //     rgb565_data[i] = RGB888ToRGB565(r, g, b);
        // }
        // 在颜色转换时处理 alpha 通道
        for (size_t i = 0; i < width * height; i++)
        {
            uint8_t r = image[i * 4 + 0];
            uint8_t g = image[i * 4 + 1];
            uint8_t b = image[i * 4 + 2];
            uint8_t a = image[i * 4 + 3]; // alpha 通道

            // RGB565 部分
            ((uint16_t *)rgb565a8_data)[i] = RGB888ToRGB565(r, g, b);
            // Alpha 部分
            rgb565a8_data[width * height * 2 + i] = a; // 存储在RGB565数据之后
        }
    }
    catch (const std::exception &e)
    {
        ESP_LOGE(TAG, "Exception during color conversion: %s", e.what());
        heap_caps_free(httpbuffer);
        httpbuffer = NULL;
        return false;
    }

    // 更新图像描述符
    img_desc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    img_desc.header.w = width;
    img_desc.header.h = height;
    img_desc.data_size = rgb565a8_size;
    img_desc.data = rgb565a8_data;
    // // 设置LVGL图像描述符
    // img_desc.header.cf = LV_COLOR_FORMAT_RGB565;
    // img_desc.header.w = width;
    // img_desc.header.h = height;
    // img_desc.data_size = rgb565_size;
    // img_desc.data = (uint8_t*)rgb565_data;

    // 清理下载缓冲区
    heap_caps_free(httpbuffer);
    httpbuffer = NULL;
    ESP_LOGI(TAG, "Image downloaded and decoded successfully: %ux%u", width, height);

    return true;
}
// 图片下载和处理任务
static void download_image_task(void *arg)
{
    // 记录初始可用栈空间
    UBaseType_t initialStackHighWater = uxTaskGetStackHighWaterMark(NULL);

    DownloadImageParams *params = (DownloadImageParams *)arg;
    static lv_img_dsc_t img_desc;
    if (downloadAndDecodeImage(params->url, img_desc))
    {
        // 创建更新消息
        lvgl_img_update_t *update = (lvgl_img_update_t *)malloc(sizeof(lvgl_img_update_t));
        if (update)
        {
            update->obj = params->target_obj;
            update->img_desc = &img_desc;

            // 发送到LVGL任务队列
            lv_async_call(lvgl_update_cb, update);
        }
    }

    // 释放参数内存
    free((void *)params->url);
    free(params);
    // 记录任务结束时的可用栈空间
    UBaseType_t finalStackHighWater = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "Stack usage: %u bytes",
             (initialStackHighWater - finalStackHighWater) * sizeof(StackType_t));

    vTaskDelete(NULL);
}

void SpiLcdAnimDisplay::SetEmotion(const char *emotion)
{
    // return;
    if (strstr(emotion, "http") != nullptr)
    {
        ESP_LOGI(TAG, "SetEmotion: %s", emotion);

        // 创建参数结构体
        DownloadImageParams *params = (DownloadImageParams *)malloc(sizeof(DownloadImageParams));
        if (params == nullptr)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for download params");
            return;
        }

        params->url = strdup(emotion);          // 复制URL字符串
        params->target_obj = emotion_label_img; // chat_message_label_tool;
        params->display = this;

        // 创建图片下载任务
        TaskHandle_t task_handle;
        BaseType_t ret = xTaskCreate(
            download_image_task,
            "img_download",
            8192, // 增加栈大小
            params,
            5,
            &task_handle);

        if (ret != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create image download task");
            free((void *)params->url);
            free(params);
        }
    }
}

void SpiLcdAnimDisplay::ShowRgb565Frame(const uint8_t* buf, int w, int h) {
    EnqueueFrame(buf, w, h);
}

void SpiLcdAnimDisplay::EnqueueFrame(const uint8_t* buf, int w, int h) {
    if (!frame_queue_) return;
    CameraFrame frame;
    frame.w = w;
    frame.h = h;
    frame.buf = (uint8_t*)heap_caps_malloc(w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame.buf) return;
    memcpy(frame.buf, buf, w * h * 2);
    // 若队列满，丢弃最旧帧
    if (xQueueSend(frame_queue_, &frame, 0) != pdTRUE) {
        CameraFrame old;
        if (xQueueReceive(frame_queue_, &old, 0) == pdTRUE) {
            if (old.buf) heap_caps_free(old.buf);
        }
        xQueueSend(frame_queue_, &frame, 0);
    }
}
// 交换每个像素的高低字节
void swap_rgb565_bytes(uint8_t* buf, size_t len) {
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint8_t tmp = buf[i];
        buf[i] = buf[i + 1];
        buf[i + 1] = tmp;
    }
}
void SpiLcdAnimDisplay::StartFrameQueueTask() {
    if (!frame_queue_) {
        frame_queue_ = xQueueCreate(2, sizeof(CameraFrame)); // 最多缓存2帧
    }
    if (!frame_task_handle_) {
        xTaskCreate([](void* arg) {
            SpiLcdAnimDisplay* self = (SpiLcdAnimDisplay*)arg;
            CameraFrame frame;
            while (1) {
                if (xQueueReceive(self->frame_queue_, &frame, portMAX_DELAY) == pdTRUE) {
                    ESP_LOGI(TAG, "ShowRgb565Frame: 收到RGB565数据，宽度=%d，高度=%d", frame.w, frame.h);
                    if (self->emotion_label_img && frame.buf) {
                        // 结构体用于传递参数到lv_async_call
                        struct LvglRgb565Param {
                            SpiLcdAnimDisplay* self;
                            uint8_t* buf;
                            int w;
                            int h;
                        };
                        auto* param = new LvglRgb565Param{self, frame.buf, frame.w, frame.h};
                        lv_async_call([](void* user_data){
                            auto* p = (LvglRgb565Param*)user_data;
                            swap_rgb565_bytes((uint8_t*)p->buf, p->w * p->h * 2);
                            static lv_img_dsc_t img_dsc;
                            img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
                            img_dsc.header.w = p->w;
                            img_dsc.header.h = p->h;
                            img_dsc.data_size = p->w * p->h * 2;
                            img_dsc.data = p->buf;
                            if (p->self->emotion_label_img) {
                                lv_img_set_src(p->self->anim_img_obj_, &img_dsc);
                                // lv_img_set_src(p->self->emotion_label_img, &img_dsc);
                                // lv_obj_set_size(p->self->emotion_label_img, p->w, p->h);
                                // lv_obj_set_pos(p->self->emotion_label_img, 0, 0);

                                ESP_LOGI(TAG, "ShowRgb565Frame: 显示图片");
                                // vTaskDelay(pdMS_TO_TICKS(3000));
                                // 将图片对象内容置空并隐藏，释放资源
                                // lv_img_set_src(p->self->emotion_label_img, NULL);
                                // lv_obj_add_flag(p->self->emotion_label_img, LV_OBJ_FLAG_HIDDEN);
                                // lv_obj_clear_flag(p->self->emotion_label_img, LV_OBJ_FLAG_HIDDEN);
                                
                            }
                            heap_caps_free(p->buf);
                            delete p;
                        }, param);
                    // self->StopFrameQueueTask();
                    // 退出任务
                    vTaskDelete(NULL);
                    } else {
                        if (frame.buf) heap_caps_free(frame.buf);
                    }
                }
            }
        }, "lcd_frame_task", 4096, this, 1, &frame_task_handle_);
    }
}

void SpiLcdAnimDisplay::StopFrameQueueTask() {
    if (frame_task_handle_) {
        vTaskDelete(frame_task_handle_);
        frame_task_handle_ = nullptr;
    }
    if (frame_queue_) {
        CameraFrame frame;
        while (xQueueReceive(frame_queue_, &frame, 0) == pdTRUE) {
            if (frame.buf) heap_caps_free(frame.buf);
        }
        vQueueDelete(frame_queue_);
        frame_queue_ = nullptr;
    }
}

// JPEG解码回调上下文结构体
struct JpegIoContext {
    const uint8_t* jpg;
    size_t len;
    size_t pos;
    uint8_t* buf;
    int w, h;
};

static UINT tjpgd_input_func(JDEC* jd, BYTE* buff, UINT nbyte) {
    JpegIoContext* io = (JpegIoContext*)jd->device;
    if (io->pos + nbyte > io->len) nbyte = io->len - io->pos;
    if (buff && nbyte) memcpy(buff, io->jpg + io->pos, nbyte);
    io->pos += nbyte;
    return nbyte;
}

static UINT tjpgd_output_func(JDEC* jd, void* bitmap, JRECT* rect) {
    JpegIoContext* io = (JpegIoContext*)jd->device;
    int w = rect->right - rect->left + 1;
    int h = rect->bottom - rect->top + 1;
    for (int y = 0; y < h; ++y) {
        memcpy(io->buf + ((rect->top + y) * io->w + rect->left) * 2,
               (uint8_t*)bitmap + y * w * 2, w * 2);
    }
    return 1;
}

void SpiLcdAnimDisplay::ShowJpeg(const uint8_t* jpg, size_t len) {
    ESP_LOGI(TAG, "ShowJpeg: 收到JPEG数据，长度=%d", (int)len);
    JDEC jd;
    void* work = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    JpegIoContext io = { jpg, len, 0, nullptr, 0, 0 };
    JRESULT res = jd_prepare(&jd, tjpgd_input_func, work, 4096, &io);
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "ShowJpeg: jd_prepare失败: %d", res);
        heap_caps_free(work);
        return;
    }
    int w = jd.width, h = jd.height;
    ESP_LOGI(TAG, "解码JPEG尺寸: %d x %d", w, h);
    static uint8_t* rgb565_buf = (uint8_t*)heap_caps_malloc(w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb565_buf) {
        ESP_LOGE(TAG, "ShowJpeg: 分配RGB565缓冲区失败");
        heap_caps_free(work);
        return;
    }
    io.buf = rgb565_buf;
    io.w = w;
    io.h = h;
    res = jd_decomp(&jd, tjpgd_output_func, 0);
    heap_caps_free(work);
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "ShowJpeg: jd_decomp失败: %d", res);
        heap_caps_free(rgb565_buf);
        return;
    }
    swap_rgb565_bytes(rgb565_buf, w * h * 2);
    static lv_img_dsc_t img_dsc;
    img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    img_dsc.header.w = w;
    img_dsc.header.h = h;
    img_dsc.data_size = w * h * 2;
    // ESP_LOGI(TAG, "ShowJpeg: 图片大小=%d*%d", w, h);
    img_dsc.data = rgb565_buf;
    if (anim_img_obj_) {
        DisplayLockGuard lock(this);
        lv_img_set_src(anim_img_obj_, &img_dsc);
    }
    // 不要立即释放 rgb565_buf
    // heap_caps_free(rgb565_buf);
}

void SpiLcdAnimDisplay::ShowRgb565(const uint8_t* buf, int w, int h) {
    if (!buf || !anim_img_obj_) return;
    // 分配一块缓冲区用于显示
    static uint8_t* rgb565_buf = (uint8_t*)heap_caps_malloc(w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb565_buf) return;
    memcpy(rgb565_buf, buf, w * h * 2);
    swap_rgb565_bytes(rgb565_buf, w * h * 2);
    static lv_img_dsc_t img_dsc;
    img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    img_dsc.header.w = w;
    img_dsc.header.h = h;
    img_dsc.data_size = w * h * 2;
    img_dsc.data = rgb565_buf;
    {
        DisplayLockGuard lock(this);
        lv_img_set_src(anim_img_obj_, &img_dsc);
    }
    // heap_caps_free(rgb565_buf);
}

void SpiLcdAnimDisplay::CaptureAndShowPhoto() {
    DisplayLockGuard lock(this);
    CameraService::GetInstance().ShowPhotoToLvgl(anim_img_obj_);
}
