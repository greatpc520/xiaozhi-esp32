#include <lvgl.h>
#include "spi_lcd_anim_display.h"
// #include "lcd_display.h"
#include "font_awesome_symbols.h"
#include "assets/lang_config.h"
#include "application.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>
#include <cmath>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "lodepng.h"
#include "tjpgd.h" // 假设你已集成tjpgd库
#include "esp_http_client.h"
#include "esp_ping.h"
#include <arpa/inet.h>

// #include "camera_service.h"

#define TAG "SpiLcdAnimDisplay"

// 函数声明
bool decodePngImage(lv_img_dsc_t &img_desc, unsigned &width, unsigned &height);
bool decodeJpgImage(lv_img_dsc_t &img_desc, unsigned &width, unsigned &height);
bool processRawImage(lv_img_dsc_t &img_desc, unsigned &width, unsigned &height);
bool convertToRgb565A8(const std::vector<unsigned char> &image_rgba, unsigned width, unsigned height,
                       lv_img_dsc_t &img_desc, bool has_alpha);

#define FRAME_WIDTH 240
#define FRAME_HEIGHT 240
#define FRAME_SIZE (FRAME_WIDTH * FRAME_HEIGHT * 2)
#define MAX_SPEAK_FRAMES 40
#define MAX_LISTEN_FRAMES 10

static uint8_t *speak_anim_cache[MAX_SPEAK_FRAMES] = {nullptr};
static int speak_anim_cache_count = 0;
static uint8_t *listen_anim_cache[MAX_LISTEN_FRAMES] = {nullptr};
static int listen_anim_cache_count = 0;
static uint8_t *idle_img_cache = nullptr;

static uint8_t *rgb565a8_data = (uint8_t *)heap_caps_malloc(240 * 240 * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
// uint16_t *rgb565_data = (uint16_t *)heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
static uint8_t *httpbuffer = NULL; //(uint8_t *)heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
static size_t image_buffer_size = 0;

// Color definitions for dark theme
#define DARK_BACKGROUND_COLOR lv_color_hex(0x121212)       // Dark background
#define DARK_TEXT_COLOR lv_color_white()                   // White text
#define DARK_CHAT_BACKGROUND_COLOR lv_color_hex(0x1E1E1E)  // Slightly lighter than background
#define DARK_USER_BUBBLE_COLOR lv_color_hex(0x1A6C37)      // Dark green
#define DARK_ASSISTANT_BUBBLE_COLOR lv_color_hex(0x333333) // Dark gray
#define DARK_SYSTEM_BUBBLE_COLOR lv_color_hex(0x2A2A2A)    // Medium gray
#define DARK_SYSTEM_TEXT_COLOR lv_color_hex(0xAAAAAA)      // Light gray text
#define DARK_BORDER_COLOR lv_color_hex(0x333333)           // Dark gray border
#define DARK_LOW_BATTERY_COLOR lv_color_hex(0xFF0000)      // Red for dark mode

// Color definitions for light theme
#define LIGHT_BACKGROUND_COLOR lv_color_white()            // White background
#define LIGHT_TEXT_COLOR lv_color_black()                  // Black text
#define LIGHT_CHAT_BACKGROUND_COLOR lv_color_hex(0xE0E0E0) // Light gray background
#define LIGHT_USER_BUBBLE_COLOR lv_color_hex(0x95EC69)     // WeChat green
#define LIGHT_ASSISTANT_BUBBLE_COLOR lv_color_white()      // White
#define LIGHT_SYSTEM_BUBBLE_COLOR lv_color_hex(0xE0E0E0)   // Light gray
#define LIGHT_SYSTEM_TEXT_COLOR lv_color_hex(0x666666)     // Dark gray text
#define LIGHT_BORDER_COLOR lv_color_hex(0xE0E0E0)          // Light gray border
#define LIGHT_LOW_BATTERY_COLOR lv_color_black()           // Black for light mode

// Theme color structure
// struct ThemeColors {
//     lv_color_t background;
//     lv_color_t text;
//     lv_color_t chat_background;
//     lv_color_t user_bubble;
//     lv_color_t assistant_bubble;
//     lv_color_t system_bubble;
//     lv_color_t system_text;
//     lv_color_t border;
//     lv_color_t low_battery;
// };
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
    .low_battery = DARK_LOW_BATTERY_COLOR};

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
    .low_battery = LIGHT_LOW_BATTERY_COLOR};

// Current theme - initialize based on default config
static ThemeColors current_theme = LIGHT_THEME;

LV_FONT_DECLARE(font_awesome_30_4);
struct AnimFramesTaskParam
{
    int role_id;
    SpiLcdAnimDisplay *display;
};

static void FreeCache(uint8_t **cache, int max_count)
{
    for (int i = 0; i < max_count; ++i)
    {
        if (cache[i])
        {
            heap_caps_free(cache[i]);
            cache[i] = nullptr;
        }
    }
}

static void LoadRawFrames(const std::string &dir, uint8_t **cache, int max_count, int &out_count)
{
    FreeCache(cache, max_count);
    out_count = 0;
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    struct dirent *ent;
    std::vector<std::string> files;
    while ((ent = readdir(d)) != nullptr)
    {
        if (strstr(ent->d_name, ".raw") || strstr(ent->d_name, ".RAW"))
        {
            files.push_back(dir + "/" + ent->d_name);
        }
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    int count = std::min((int)files.size(), max_count);
    for (int i = 0; i < count; ++i)
    {
        FILE *fp = fopen(files[i].c_str(), "rb");
        if (fp)
        {
            cache[i] = (uint8_t *)heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (cache[i])
            {
                size_t read_bytes = fread(cache[i], 1, FRAME_SIZE, fp);
                if (read_bytes == FRAME_SIZE)
                {
                    ++out_count;
                }
                else
                {
                    heap_caps_free(cache[i]);
                    cache[i] = nullptr;
                }
            }
            fclose(fp);
        }
    }
    ESP_LOGI(TAG, "LoadRawFrames: %s, count: %d", dir.c_str(), out_count);
}

static void LoadIdleRaw(const std::string &dir)
{
    if (idle_img_cache)
    {
        heap_caps_free(idle_img_cache);
        idle_img_cache = nullptr;
    }
    DIR *d = opendir(dir.c_str());
    if (!d)
    {
        ESP_LOGE(TAG, "LoadIdleRaw: %s,opendir failed", dir.c_str());
        return;
    }
    struct dirent *ent;
    std::vector<std::string> files;
    while ((ent = readdir(d)) != nullptr)
    {
        if (strstr(ent->d_name, ".raw") || strstr(ent->d_name, ".RAW"))
        {
            files.push_back(dir + "/" + ent->d_name);
        }
    }
    closedir(d);
    if (!files.empty())
    {
        FILE *fp = fopen(files[0].c_str(), "rb");
        if (fp)
        {
            idle_img_cache = (uint8_t *)heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (idle_img_cache)
            {
                // fread(idle_img_cache, 1, FRAME_SIZE, fp);
                size_t read_bytes = fread(idle_img_cache, 1, FRAME_SIZE, fp);
                if (read_bytes == FRAME_SIZE)
                {
                    ESP_LOGI(TAG, "LoadIdleRaw: %s,fread success", files[0].c_str());
                }
                else
                {
                    heap_caps_free(idle_img_cache);
                    idle_img_cache = nullptr;
                    ESP_LOGE(TAG, "LoadIdleRaw: %s,fread failed", files[0].c_str());
                }
            }
            else
            {
                ESP_LOGE(TAG, "LoadIdleRaw: %s,idle_img_cache failed", files[0].c_str());
            }
            fclose(fp);
        }
        else
        {
            ESP_LOGE(TAG, "LoadIdleRaw: %s,fopen failed", files[0].c_str());
        }
    }
    else
    {
        ESP_LOGE(TAG, "LoadIdleRaw: %s,files.empty failed", dir.c_str());
    }
}

static void anim_frames_ready_cb(void *param)
{
    SpiLcdAnimDisplay *display = (SpiLcdAnimDisplay *)param;
    display->OnFramesLoaded();
    ESP_LOGI(TAG, "anim_frames_ready_cb");
}

static void anim_frames_load_task(void *arg)
{
    AnimFramesTaskParam *param = (AnimFramesTaskParam *)arg;
    int role_id = param->role_id;
    SpiLcdAnimDisplay *display = param->display;
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
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy, fonts)
{
    role_id_ = 0;
    anim_img_obj_ = nullptr;
    current_state_ = "idle";
    // SpiLcdDisplay::SetupUI();
    SetupUI();
}

void SpiLcdAnimDisplay::SetupUI()
{
    DisplayLockGuard lock(this);
    auto screen = lv_screen_active();
    
    // 确保屏幕没有圆角和裁剪
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_style_clip_corner(screen, false, 0);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
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
    SetRoleId(1);
    // SetAnimState("idle");
     lv_async_call([](void* param){
            SpiLcdAnimDisplay* self = static_cast<SpiLcdAnimDisplay*>(param);
            self->ShowIdleImage();
        }, this);

    ESP_LOGI(TAG, "SetupUI: %s", current_state_.c_str());
    // StartFrameQueueTask();
}

void SpiLcdAnimDisplay::SetRoleId(int role_id)
{
    if (role_id_ == role_id || role_id <= 0)
        return;
    ESP_LOGI(TAG, "SetRoleId: %d", role_id);
    if (HasCanvas())
    {
        DestroyCanvas();
        ESP_LOGI(TAG, "已关闭画布显示");
    }
    role_id_ = role_id;
    LoadFrames();
    SetAnimState("idle");
}

void SpiLcdAnimDisplay::SetAnimState(const std::string &state)
{
    // static uint32_t last_change_time = 0;
    // uint32_t now = lv_tick_get();
    // if (state == current_state_ && state !="idle") return;
    // if (state == current_state_ && (now - last_change_time) < 1000) {
    //     // 状态相同且小于1秒，不执行
    //     return;
    // }
    // last_change_time = now;
    //  ESP_LOGI(TAG, "SetAnimState: %s",state.c_str());
    if (state == current_state_)
        return;
    // StopAnim();
    current_state_ = state;
    if (HasCanvas())
    {
        DestroyCanvas();
        ESP_LOGI(TAG, "已关闭画布显示");
    }
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
    if (state == "idle" && idle_img_cache)
    {
        // lv_async_call([](void* param){
        //     SpiLcdAnimDisplay* self = static_cast<SpiLcdAnimDisplay*>(param);
        //     self->ShowIdleImage();
        // }, this);
    }
    else
    {
        // OnFramesLoaded();
        //         xTaskCreate([](void* param){
        //     SpiLcdAnimDisplay* self = static_cast<SpiLcdAnimDisplay*>(param);
        //     if (self) {
        //         self->OnFramesLoaded();
        //     }
        //     vTaskDelete(NULL);
        // }, "on_frames_loaded", 4096, this, 1, NULL);
        // 正确做法：用 lv_async_call 让 OnFramesLoaded 在 LVGL 线程执行
        lv_async_call([](void *param)
                      {
            SpiLcdAnimDisplay* self = static_cast<SpiLcdAnimDisplay*>(param);
            if (self) self->OnFramesLoaded(); }, this);
    }

    ESP_LOGI(TAG, "SetAnimStated: %s", current_state_.c_str());
}

void SpiLcdAnimDisplay::LoadFrames()
{
    static int last_role_id = -1;
    if (last_role_id == role_id_)
        return;
    last_role_id = role_id_;
    // 启动异步任务加载动画帧
    AnimFramesTaskParam *param = new AnimFramesTaskParam{role_id_, this};
    xTaskCreate(anim_frames_load_task, "anim_load", 8192, param, 1, NULL);
}

void SpiLcdAnimDisplay::OnFramesLoaded()
{
    // return;
    // LVGL主线程回调，加载完成后重启动画或显示idle
    if (current_state_ == "speak" && speak_anim_cache_count > 0)
    {
        frame_count_ = speak_anim_cache_count;
        anim_fps_ = 8;
        StartAnim();
    }
    else if (current_state_ == "listen" && listen_anim_cache_count > 0)
    {
        frame_count_ = listen_anim_cache_count;
        anim_fps_ = 8;
        StartAnim();
    }
    else
    {
        // ShowIdleImage();
    }
    ESP_LOGI(TAG, "OnFramesLoaded: %s", current_state_.c_str());
}

static void anim_exec_cb(void *obj, int32_t frame)
{
    SpiLcdAnimDisplay *self = static_cast<SpiLcdAnimDisplay *>(lv_obj_get_user_data((lv_obj_t *)obj));
    if (!self)
        return;
    // 判断帧缓存是否加载完成
    if (self->current_state_ == "speak" && (frame >= speak_anim_cache_count || !speak_anim_cache[frame]))
        return;
    if (self->current_state_ == "listen" && (frame >= listen_anim_cache_count || !listen_anim_cache[frame]))
        return;

    static lv_img_dsc_t img_desc;
    img_desc.header.cf = LV_COLOR_FORMAT_RGB565;
    img_desc.header.w = FRAME_WIDTH;
    img_desc.header.h = FRAME_HEIGHT;
    img_desc.data_size = FRAME_SIZE;
    if (self->current_state_ == "speak" && frame < speak_anim_cache_count && speak_anim_cache[frame])
    {
        img_desc.data = speak_anim_cache[frame];
    }
    else if (self->current_state_ == "listen" && frame < listen_anim_cache_count && listen_anim_cache[frame])
    {
        img_desc.data = listen_anim_cache[frame];
    }
    else
    {
        return;
    }
    lv_img_set_src((lv_obj_t *)obj, &img_desc);
}

void SpiLcdAnimDisplay::StartAnim()
{
    if (frame_count_ <= 0)
        return;
    if (anim_img_obj_  == nullptr)return;
        
    StopAnim();
    vTaskDelay(pdMS_TO_TICKS(100));
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

void SpiLcdAnimDisplay::StopAnim()
{
    if (anim_img_obj_  == nullptr)return;
    lv_anim_del(anim_img_obj_, nullptr);
    ESP_LOGI(TAG, "StopAnim: %s", current_state_.c_str());
}

void SpiLcdAnimDisplay::ShowIdleImage()
{
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
    LcdDisplay *display;
};

// 定义消息结构体
typedef struct
{
    LcdDisplay *display;
    lv_img_dsc_t *img_desc;
    unsigned width;
    unsigned height;
} lvgl_canvas_update_t;

// LVGL更新回调函数 - 修改为使用画布显示
static void lvgl_update_cb(void *data)
{
    lvgl_canvas_update_t *update = (lvgl_canvas_update_t *)data;
    if (update && update->display && update->img_desc)
    {
        // 检查画布是否存在，如果不存在则创建
        if (!update->display->HasCanvas())
        {
            update->display->CreateCanvas();
            // ESP_LOGI(TAG, "创建画布用于显示表情图片");
        }

        // 根据图片格式处理数据
        const uint8_t *image_data = update->img_desc->data;

        if (update->img_desc->header.cf == LV_COLOR_FORMAT_RGB565)
        {
            // 直接使用RGB565数据绘制到画布
            update->display->DrawImageOnCanvas(0, 0, update->width, update->height, image_data);
            ESP_LOGI(TAG, "RGB565图片已绘制到画布: %ux%u", update->width, update->height);
        }
        else if (update->img_desc->header.cf == LV_COLOR_FORMAT_RGB565A8)
        {
            // RGB565A8格式：前面是RGB565数据，后面是Alpha数据
            // 这里只使用RGB565部分绘制到画布（画布不支持透明度）
            update->display->DrawImageOnCanvas(0, 0, update->width, update->height, image_data);
            ESP_LOGI(TAG, "RGB565A8图片已绘制到画布: %ux%u (忽略Alpha通道)", update->width, update->height);
        }
        else
        {
            ESP_LOGW(TAG, "不支持的图片格式，无法绘制到画布: %d", update->img_desc->header.cf);
        }

        // 创建一个 FreeRTOS 任务，在 3 秒后销毁画布
        struct HideTaskParam
        {
            LcdDisplay *display;
        };
        auto *hide_param = (HideTaskParam *)malloc(sizeof(HideTaskParam));
        hide_param->display = update->display;
        xTaskCreate([](void *arg)
                    {
            auto* hp = (HideTaskParam*)arg;
            vTaskDelay(pdMS_TO_TICKS(1000));
            lv_async_call([](void* display_ptr){
                LcdDisplay* display = (LcdDisplay*)display_ptr;
                if (display && display->HasCanvas()) {
                    display->DestroyCanvas();
                    // ESP_LOGI(TAG, "已销毁画布，表情图片已隐藏");
                }
            }, hp->display);
            free(hp);
            vTaskDelete(NULL); }, "hide_emotion", 2048, hide_param, 1, NULL);
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
    static size_t buffer_len = 0;
    static size_t allocated_buffer_size = 0; // 实际分配的缓冲区大小
    static bool download_completed = false;  // 防止多次DISCONNECTED事件
    static bool download_started = false;    // 防止重复下载

    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "HTTP client connected");
        // 重置所有状态
        download_completed = false;
        download_started = false;
        buffer_len = 0;
        allocated_buffer_size = 0;

        // 清理旧的缓冲区
        if (httpbuffer)
        {
            heap_caps_free(httpbuffer);
            httpbuffer = NULL;
        }
        image_buffer_size = 0;
        break;

    case HTTP_EVENT_ON_HEADER:
        // 只处理Content-Length头部一次
        if (!download_started && strcasecmp(evt->header_key, "Content-Length") == 0)
        {
            size_t content_length = atoi(evt->header_value);
            ESP_LOGI(TAG, "Content-Length: %zu bytes", content_length);

            // 检查可用内存
            size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            ESP_LOGI(TAG, "Free SPIRAM: %zu bytes", free_spiram);

            if (content_length > free_spiram / 2)
            {
                ESP_LOGE(TAG, "文件太大，可能导致内存不足: %zu bytes", content_length);
                return ESP_FAIL;
            }

            // 预分配精确大小的缓冲区
            if (content_length > 0)
            {
                allocated_buffer_size = content_length; // 不再添加额外缓冲
                httpbuffer = (uint8_t *)heap_caps_malloc(allocated_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!httpbuffer)
                {
                    ESP_LOGE(TAG, "Failed to allocate httpbuffer: %zu bytes", allocated_buffer_size);
                    return ESP_FAIL;
                }
                buffer_len = 0;
                image_buffer_size = 0;
                download_completed = false;
                download_started = true;
                ESP_LOGI(TAG, "Pre-allocated httpbuffer: %zu bytes", allocated_buffer_size);
            }
        }
        break;

    case HTTP_EVENT_ON_DATA:
        // 确保只处理一次下载的数据
        if (download_completed)
        {
            ESP_LOGW(TAG, "Ignoring additional data after download completed");
            return ESP_OK;
        }

        if (!httpbuffer)
        {
            // 如果没有预分配，使用默认大小
            allocated_buffer_size = 512 * 1024; // 默认512KB
            httpbuffer = (uint8_t *)heap_caps_malloc(allocated_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!httpbuffer)
            {
                ESP_LOGE(TAG, "Failed to allocate httpbuffer: %zu bytes", allocated_buffer_size);
                return ESP_FAIL;
            }
            buffer_len = 0;
            image_buffer_size = 0;
            download_completed = false;
            download_started = true;
            ESP_LOGI(TAG, "Default allocated httpbuffer: %zu bytes", allocated_buffer_size);
        }

        // 检查缓冲区是否足够
        if (buffer_len + evt->data_len > allocated_buffer_size)
        {
            ESP_LOGE(TAG, "Buffer overflow! Current: %zu, adding: %d, allocated: %zu",
                     buffer_len, evt->data_len, allocated_buffer_size);
            return ESP_FAIL; // 不再自动扩展，严格按Content-Length分配
        }

        memcpy(httpbuffer + buffer_len, evt->data, evt->data_len);
        buffer_len += evt->data_len;

        // 每接收10KB数据打印一次进度
        if (buffer_len % (10 * 1024) == 0 || buffer_len < 10 * 1024)
        {
            ESP_LOGI(TAG, "Download progress: %zu/%zu bytes", buffer_len, allocated_buffer_size);
        }
        break;

    case HTTP_EVENT_DISCONNECTED:
        // 防止多次DISCONNECTED事件重置数据
        if (!download_completed && buffer_len > 0)
        {
            // 只在第一次DISCONNECTED时保存数据
            image_buffer_size = buffer_len;
            download_completed = true;
            ESP_LOGI(TAG, "Download completed, total size: %zu bytes", image_buffer_size);
        }
        else if (download_completed)
        {
            ESP_LOGI(TAG, "Additional DISCONNECTED event ignored (already completed)");
        }
        else
        {
            ESP_LOGW(TAG, "DISCONNECTED event with no data (buffer_len: %zu)", buffer_len);
        }

        // 重置静态变量为下次下载准备
        buffer_len = 0;
        allocated_buffer_size = 0;
        download_started = false;
        break;

    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP download error");
        if (httpbuffer)
        {
            heap_caps_free(httpbuffer);
            httpbuffer = NULL;
        }
        image_buffer_size = 0;
        buffer_len = 0;
        allocated_buffer_size = 0;
        download_completed = false;
        download_started = false;
        break;

    default:
        break;
    }
    return ESP_OK;
}

void download_image(const char *url)
{
    ESP_LOGI(TAG, "Starting image download from: %s", url);

    // 重置全局状态 - 确保每次下载都是干净的状态
    image_buffer_size = 0;
    if (httpbuffer)
    {
        heap_caps_free(httpbuffer);
        httpbuffer = NULL;
    }

    // 检查可用内存
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Free SPIRAM before download: %zu bytes", free_spiram);

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler;
    config.timeout_ms = 30000;    // 30秒超时
    config.buffer_size = 4096;    // 4KB缓冲区
    config.buffer_size_tx = 1024; // 1KB发送缓冲区
    config.user_agent = "ESP32-ImageDownloader/1.0";
    config.method = HTTP_METHOD_GET;
    config.skip_cert_common_name_check = true; // 跳过证书检查（如果是HTTPS）
    config.disable_auto_redirect = false;      // 允许自动重定向
    config.max_redirection_count = 3;          // 最多3次重定向

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return;
    }

    // 设置HTTP头部
    esp_http_client_set_header(client, "Accept", "image/png,image/jpeg,image/*,*/*");
    esp_http_client_set_header(client, "Connection", "close");
    esp_http_client_set_header(client, "Cache-Control", "no-cache");

    esp_err_t err = esp_http_client_perform(client);

    // 获取响应信息
    int status_code = esp_http_client_get_status_code(client);
    int content_length = esp_http_client_get_content_length(client);

    ESP_LOGI(TAG, "HTTP Status: %d, Content-Length: %d, downloaded: %zu",
             status_code, content_length, image_buffer_size);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
        // 如果是超时错误，给出特殊提示
        if (err == ESP_ERR_HTTP_EAGAIN)
        {
            ESP_LOGE(TAG, "HTTP request timed out");
        }
    }
    else if (status_code >= 400)
    {
        ESP_LOGE(TAG, "HTTP error status: %d", status_code);
        // 清理错误状态下的数据
        if (httpbuffer)
        {
            heap_caps_free(httpbuffer);
            httpbuffer = NULL;
        }
        image_buffer_size = 0;
    }
    else if (status_code == 200)
    {
        // 验证下载的数据大小
        if (content_length > 0 && image_buffer_size != (size_t)content_length)
        {
            ESP_LOGW(TAG, "Size mismatch: expected %d, got %zu bytes", content_length, image_buffer_size);
        }
        else
        {
            ESP_LOGI(TAG, "Download successful: %zu bytes", image_buffer_size);
        }
    }

    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "HTTP client cleaned up");
}

// 检查网络连接状态
bool check_network_connectivity()
{
    // 简单检查：尝试HTTP请求到一个可靠的服务器
    esp_http_client_config_t config = {};
    config.url = "http://httpbin.org/status/200";
    config.timeout_ms = 5000;         // 5秒超时
    config.method = HTTP_METHOD_HEAD; // 只获取头部信息

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to create HTTP client for connectivity check");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    bool connected = (err == ESP_OK && status_code == 200);
    ESP_LOGI(TAG, "Network connectivity check: %s (status: %d)",
             connected ? "SUCCESS" : "FAILED", status_code);

    return connected;
}

// 分块下载大文件的函数 - 修复版本
bool download_image_chunked(const char *url, size_t expected_size)
{
    // 重置全局状态
    image_buffer_size = 0;
    if (httpbuffer)
    {
        heap_caps_free(httpbuffer);
        httpbuffer = NULL;
    }

    // 检查可用内存
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Available SPIRAM for chunked download: %zu bytes", free_spiram);

    // 确保不超过可用内存的一半
    if (expected_size > free_spiram / 2)
    {
        expected_size = free_spiram / 2;
        ESP_LOGW(TAG, "Adjusted expected size to %zu bytes", expected_size);
    }

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler; // 关键：添加事件处理函数
    config.timeout_ms = 60000;                 // 增加到60秒用于大文件
    config.buffer_size = 8192;                 // 增大缓冲区到8KB
    config.buffer_size_tx = 1024;
    config.user_agent = "ESP32-ImageDownloader/1.0";
    config.method = HTTP_METHOD_GET;
    config.skip_cert_common_name_check = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to init HTTP client for chunked download");
        return false;
    }

    // 设置HTTP头
    esp_http_client_set_header(client, "Accept", "image/png,image/jpeg,image/*,*/*");
    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    int content_length = esp_http_client_get_content_length(client);

    ESP_LOGI(TAG, "Chunked download status: %d, Content-Length: %d, error: %s, downloaded: %zu",
             status_code, content_length, esp_err_to_name(err), image_buffer_size);

    esp_http_client_cleanup(client);

    bool success = (err == ESP_OK && status_code == 200 && image_buffer_size > 0);

    if (!success && httpbuffer)
    {
        heap_caps_free(httpbuffer);
        httpbuffer = NULL;
        image_buffer_size = 0;
    }

    ESP_LOGI(TAG, "Chunked download %s, downloaded size: %zu",
             success ? "succeeded" : "failed", image_buffer_size);
    return success;
}

bool downloadImageWithRetry(const char *url, int max_retries = 3, int retry_delay_ms = 1000)
{
    // 可选的网络连接检查
    if (!check_network_connectivity())
    {
        ESP_LOGW(TAG, "Network connectivity check failed, but continuing anyway");
    }

    for (int i = 0; i < max_retries; i++)
    {
        if (i > 0)
        {
            ESP_LOGI(TAG, "Retrying download attempt %d/%d", i + 1, max_retries);
            vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
        }

        // 尝试普通下载
        download_image(url);
        if (httpbuffer && image_buffer_size > 0)
        {
            ESP_LOGI(TAG, "Normal download succeeded with %zu bytes", image_buffer_size);
            return true;
        }

        // 如果普通下载失败且是最后一次尝试，尝试分块下载
        if (i == max_retries - 1)
        {
            ESP_LOGI(TAG, "Trying chunked download as last resort");
            size_t expected_size = 512 * 1024; // 假设最大512KB
            if (download_image_chunked(url, expected_size))
            {
                ESP_LOGI(TAG, "Chunked download succeeded with %zu bytes", image_buffer_size);
                return true;
            }
        }

        // 只有在确实失败时才清理缓冲区
        if (httpbuffer && image_buffer_size == 0)
        {
            ESP_LOGI(TAG, "Cleaning up failed download attempt");
            heap_caps_free(httpbuffer);
            httpbuffer = NULL;
        }

        // 递增延迟重试
        retry_delay_ms = retry_delay_ms + 1000;
    }
    return false;
}

// 获取文件扩展名的辅助函数
std::string getFileExtension(const char *url)
{
    std::string url_str(url);
    size_t pos = url_str.rfind('.');
    if (pos == std::string::npos)
    {
        return "";
    }
    std::string ext = url_str.substr(pos + 1);
    // 转换为小写
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

// 添加新的函数用于下载和解码图片（支持多种格式）
bool downloadAndDecodeImage(const char *url, lv_img_dsc_t &img_desc)
{
    ESP_LOGI(TAG, "Starting image download from URL: %s", url);

    // 检查可用内存
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Free SPIRAM before download: %zu bytes", free_spiram);

    // 获取文件扩展名
    std::string ext = getFileExtension(url);
    ESP_LOGI(TAG, "Detected file extension: %s", ext.c_str());

    // 下载图片（带重试机制）
    if (!downloadImageWithRetry(url))
    {
        ESP_LOGE(TAG, "Failed to download image after all retries");
        return false;
    }

    ESP_LOGI(TAG, "Downloaded %zu bytes, processing as %s format", image_buffer_size, ext.c_str());

    unsigned width = 0, height = 0;

    // 根据文件扩展名处理不同格式
    if (ext == "png")
    {
        return decodePngImage(img_desc, width, height);
    }
    else if (ext == "jpg" || ext == "jpeg")
    {
        return decodeJpgImage(img_desc, width, height);
    }
    else if (ext == "raw")
    {
        return processRawImage(img_desc, width, height);
    }
    else
    {
        ESP_LOGE(TAG, "Unsupported image format: %s", ext.c_str());
        if (httpbuffer)
        {
            heap_caps_free(httpbuffer);
            httpbuffer = NULL;
        }
        return false;
    }
}

// PNG解码函数
bool decodePngImage(lv_img_dsc_t &img_desc, unsigned &width, unsigned &height)
{
    ESP_LOGI(TAG, "Decoding PNG image...");

    std::vector<unsigned char> image;

    try
    {
        // 检查基本数据有效性
        if (image_buffer_size < 8 || !httpbuffer)
        {
            ESP_LOGE(TAG, "Insufficient data for PNG: %zu bytes", image_buffer_size);
            if (httpbuffer)
            {
                heap_caps_free(httpbuffer);
                httpbuffer = NULL;
            }
            return false;
        }

        // 打印前几个字节用于调试
        ESP_LOGI(TAG, "PNG file header: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X (size: %zu)",
                 httpbuffer[0], httpbuffer[1], httpbuffer[2], httpbuffer[3],
                 httpbuffer[4], httpbuffer[5], httpbuffer[6], httpbuffer[7], image_buffer_size);

        // 添加数据完整性检查 - 使用更宽松的检查
        bool is_png = false;
        if (image_buffer_size >= 8)
        {
            // 标准PNG头部: 89 50 4E 47 0D 0A 1A 0A
            if (httpbuffer[0] == 0x89 && httpbuffer[1] == 'P' &&
                httpbuffer[2] == 'N' && httpbuffer[3] == 'G')
            {
                is_png = true;
                ESP_LOGI(TAG, "Valid PNG header detected");
            }
            else
            {
                ESP_LOGW(TAG, "Non-standard PNG header, attempting to decode anyway");
                // 尝试解码，LodePNG可能仍能处理
                is_png = true;
            }
        }

        if (!is_png)
        {
            ESP_LOGE(TAG, "Invalid PNG format");
            heap_caps_free(httpbuffer);
            httpbuffer = NULL;
            return false;
        }

        // 在解码前设置状态，禁用CRC检查
        lodepng::State state;
        state.decoder.ignore_crc = 1;
        state.decoder.zlibsettings.ignore_adler32 = 1; // 也忽略adler32检查

        // 修改解码参数，使用 RGBA 格式
        unsigned error = lodepng::decode(image, width, height, state, httpbuffer, image_buffer_size);

        // 释放下载缓冲区
        heap_caps_free(httpbuffer);
        httpbuffer = NULL;

        if (error)
        {
            ESP_LOGE(TAG, "Error decoding PNG: %s", lodepng_error_text(error));
            return false;
        }

        ESP_LOGI(TAG, "PNG decoded successfully: %ux%u, %zu bytes", width, height, image.size());
    }
    catch (const std::exception &e)
    {
        ESP_LOGE(TAG, "Exception during PNG decoding: %s", e.what());
        if (httpbuffer)
        {
            heap_caps_free(httpbuffer);
            httpbuffer = NULL;
        }
        return false;
    }

    return convertToRgb565A8(image, width, height, img_desc, true); // PNG有alpha通道
}

// JPG解码上下文结构
struct JpegDecodeContext
{
    const uint8_t *src_data;
    size_t src_size;
    size_t src_pos;
    uint8_t *output_buffer;
    size_t output_size;
    size_t output_pos;
};

// TJPGD输入回调函数
static UINT tjpgd_input_callback(JDEC *jd, BYTE *buff, UINT nbyte)
{
    JpegDecodeContext *ctx = (JpegDecodeContext *)jd->device;

    if (buff)
    {
        // 读取数据
        UINT bytes_to_read = (UINT)std::min((size_t)nbyte, ctx->src_size - ctx->src_pos);
        memcpy(buff, ctx->src_data + ctx->src_pos, bytes_to_read);
        ctx->src_pos += bytes_to_read;
        return bytes_to_read;
    }
    else
    {
        // 跳过数据
        ctx->src_pos = std::min(ctx->src_pos + (size_t)nbyte, ctx->src_size);
        return nbyte;
    }
}

// TJPGD输出回调函数
static UINT tjpgd_output_callback(JDEC *jd, void *bitmap, JRECT *rect)
{
    JpegDecodeContext *ctx = (JpegDecodeContext *)jd->device;

    if (!bitmap || !ctx->output_buffer)
    {
        return 1;
    }

    // 计算输出区域
    int rect_width = rect->right - rect->left + 1;
    int rect_height = rect->bottom - rect->top + 1;

    // 将RGB数据复制到输出缓冲区
    BYTE *src_line = (BYTE *)bitmap;

    for (int y = 0; y < rect_height; y++)
    {
        int dst_y = rect->top + y;
        if (dst_y >= 0 && dst_y < (int)jd->height)
        {
            size_t dst_offset = (dst_y * jd->width + rect->left) * 3;
            size_t src_offset = y * rect_width * 3;

            if (dst_offset + rect_width * 3 <= ctx->output_size)
            {
                memcpy(ctx->output_buffer + dst_offset, src_line + src_offset, rect_width * 3);
            }
        }
    }

    return 1;
}

// JPG解码函数（真实实现）
bool decodeJpgImage(lv_img_dsc_t &img_desc, unsigned &width, unsigned &height)
{
    ESP_LOGI(TAG, "Decoding real JPG image...");

    try
    {
        // 检查下载的数据
        if (image_buffer_size < 4 || !httpbuffer)
        {
            ESP_LOGE(TAG, "Insufficient data for JPG: %zu bytes", image_buffer_size);
            if (httpbuffer)
            {
                heap_caps_free(httpbuffer);
                httpbuffer = NULL;
            }
            return false;
        }

        // 打印前几个字节用于调试
        ESP_LOGI(TAG, "JPG file header: 0x%02X 0x%02X 0x%02X 0x%02X (size: %zu)",
                 httpbuffer[0], httpbuffer[1], httpbuffer[2], httpbuffer[3], image_buffer_size);

        // 检查JPG头部
        if (httpbuffer[0] != 0xFF || httpbuffer[1] != 0xD8)
        {
            ESP_LOGE(TAG, "Invalid JPG header: expected FF D8, got %02X %02X",
                     httpbuffer[0], httpbuffer[1]);
            heap_caps_free(httpbuffer);
            httpbuffer = NULL;
            return false;
        }

        ESP_LOGI(TAG, "Valid JPG header detected");

        // 分配TJPGD工作缓冲区
        const size_t work_size = 3100;
        uint8_t *work_buffer = (uint8_t *)heap_caps_malloc(work_size, MALLOC_CAP_INTERNAL);
        if (!work_buffer)
        {
            ESP_LOGE(TAG, "Failed to allocate TJPGD work buffer");
            heap_caps_free(httpbuffer);
            httpbuffer = NULL;
            return false;
        }

        // 设置解码上下文
        JpegDecodeContext decode_ctx;
        decode_ctx.src_data = httpbuffer;
        decode_ctx.src_size = image_buffer_size;
        decode_ctx.src_pos = 0;
        decode_ctx.output_buffer = nullptr;
        decode_ctx.output_size = 0;
        decode_ctx.output_pos = 0;

        // 初始化TJPGD
        JDEC jdec;
        JRESULT res = jd_prepare(&jdec, tjpgd_input_callback, work_buffer, work_size, &decode_ctx);

        if (res != JDR_OK)
        {
            ESP_LOGE(TAG, "TJPGD prepare failed: %d", res);
            heap_caps_free(work_buffer);
            heap_caps_free(httpbuffer);
            httpbuffer = NULL;
            return false;
        }

        width = jdec.width;
        height = jdec.height;
        ESP_LOGI(TAG, "JPG dimensions: %ux%u", width, height);

        // 检查图片尺寸是否合理
        if (width == 0 || height == 0 || width > 1024 || height > 1024)
        {
            ESP_LOGE(TAG, "Invalid JPG dimensions: %ux%u", width, height);
            heap_caps_free(work_buffer);
            heap_caps_free(httpbuffer);
            httpbuffer = NULL;
            return false;
        }

        // 分配RGB输出缓冲区
        size_t rgb_size = width * height * 3;
        uint8_t *rgb_buffer = (uint8_t *)heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!rgb_buffer)
        {
            ESP_LOGE(TAG, "Failed to allocate RGB buffer: %zu bytes", rgb_size);
            heap_caps_free(work_buffer);
            heap_caps_free(httpbuffer);
            httpbuffer = NULL;
            return false;
        }

        // 设置输出缓冲区
        decode_ctx.output_buffer = rgb_buffer;
        decode_ctx.output_size = rgb_size;

        // 执行JPG解码
        res = jd_decomp(&jdec, tjpgd_output_callback, 0);

        // 清理资源
        heap_caps_free(work_buffer);
        heap_caps_free(httpbuffer);
        httpbuffer = NULL;

        if (res != JDR_OK)
        {
            ESP_LOGE(TAG, "TJPGD decompress failed: %d", res);
            heap_caps_free(rgb_buffer);
            return false;
        }

        ESP_LOGI(TAG, "JPG decoded successfully");

        // 转换RGB888到RGBA格式
        std::vector<unsigned char> image_rgba(width * height * 4);
        for (size_t i = 0; i < width * height; i++)
        {
            image_rgba[i * 4 + 0] = rgb_buffer[i * 3 + 0]; // R
            image_rgba[i * 4 + 1] = rgb_buffer[i * 3 + 1]; // G
            image_rgba[i * 4 + 2] = rgb_buffer[i * 3 + 2]; // B
            image_rgba[i * 4 + 3] = 255;                   // A (不透明)
        }

        heap_caps_free(rgb_buffer);

        return convertToRgb565A8(image_rgba, width, height, img_desc, false); // JPG没有alpha通道
    }
    catch (const std::exception &e)
    {
        ESP_LOGE(TAG, "Exception during JPG decoding: %s", e.what());
        if (httpbuffer)
        {
            heap_caps_free(httpbuffer);
            httpbuffer = NULL;
        }
        return false;
    }
}

// RAW数据处理函数
bool processRawImage(lv_img_dsc_t &img_desc, unsigned &width, unsigned &height)
{
    ESP_LOGI(TAG, "Processing RAW RGB565 image...");

    // RAW文件需要从URL中推断尺寸，或者使用默认尺寸
    // 这里我们假设是240x240的标准尺寸
    size_t expected_size_240 = 240 * 240 * 2;
    size_t expected_size_320 = 320 * 240 * 2;
    size_t expected_size_480 = 480 * 320 * 2;

    if (image_buffer_size == expected_size_240)
    {
        width = height = 240;
    }
    else if (image_buffer_size == expected_size_320)
    {
        width = 320;
        height = 240;
    }
    else if (image_buffer_size == expected_size_480)
    {
        width = 480;
        height = 320;
    }
    else
    {
        // 尝试根据大小推算正方形图片
        size_t pixels = image_buffer_size / 2;
        width = height = (unsigned)sqrt(pixels);
        ESP_LOGW(TAG, "Unknown RAW size %zu, assuming %ux%u", image_buffer_size, width, height);
    }

    ESP_LOGI(TAG, "RAW dimensions: %ux%u (%zu bytes)", width, height, image_buffer_size);

    // 检查尺寸是否合理
    if (width == 0 || height == 0 || width > 1024 || height > 1024)
    {
        ESP_LOGE(TAG, "Invalid RAW dimensions: %ux%u", width, height);
        heap_caps_free(httpbuffer);
        httpbuffer = NULL;
        return false;
    }

    if (width * height * 2 != image_buffer_size)
    {
        ESP_LOGE(TAG, "RAW size mismatch: expected %zu, got %zu", width * height * 2, image_buffer_size);
        heap_caps_free(httpbuffer);
        httpbuffer = NULL;
        return false;
    }

    // 释放旧的rgb565a8_data
    if (rgb565a8_data)
    {
        heap_caps_free(rgb565a8_data);
        rgb565a8_data = NULL;
    }

    // 对于RAW数据，直接使用下载的数据作为RGB565，不需要alpha通道
    img_desc.header.cf = LV_COLOR_FORMAT_RGB565;
    img_desc.header.w = width;
    img_desc.header.h = height;
    img_desc.data_size = image_buffer_size;
    img_desc.data = httpbuffer; // 直接使用下载的数据

    // 注意：这里不释放httpbuffer，因为它被用作图像数据
    // rgb565a8_data = httpbuffer; // 保存指针用于后续清理

    ESP_LOGI(TAG, "RAW image processed successfully: %ux%u", width, height);
    return true;
}

// 转换为RGB565A8格式的辅助函数
bool convertToRgb565A8(const std::vector<unsigned char> &image_rgba, unsigned width, unsigned height,
                       lv_img_dsc_t &img_desc, bool has_alpha)
{
    // 释放旧的rgb565a8_data
    if (rgb565a8_data)
    {
        heap_caps_free(rgb565a8_data);
        rgb565a8_data = NULL;
    }

    // 分配RGB565缓冲区到SPIRAM
    size_t rgb565a8_size;
    if (has_alpha)
    {
        rgb565a8_size = width * height * (sizeof(uint16_t) + sizeof(uint8_t)); // RGB565 + A8
        img_desc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    }
    else
    {
        rgb565a8_size = width * height * sizeof(uint16_t); // 仅RGB565
        img_desc.header.cf = LV_COLOR_FORMAT_RGB565;
    }

    rgb565a8_data = (uint8_t *)heap_caps_malloc(rgb565a8_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb565a8_data)
    {
        ESP_LOGE(TAG, "Failed to allocate RGB565 buffer in SPIRAM, size: %zu", rgb565a8_size);
        return false;
    }

    // 转换颜色格式
    try
    {
        for (size_t i = 0; i < width * height; i++)
        {
            uint8_t r = image_rgba[i * 4 + 0];
            uint8_t g = image_rgba[i * 4 + 1];
            uint8_t b = image_rgba[i * 4 + 2];
            uint8_t a = has_alpha ? image_rgba[i * 4 + 3] : 255;

            // RGB565 部分
            ((uint16_t *)rgb565a8_data)[i] = RGB888ToRGB565(r, g, b);

            // Alpha 部分（如果需要）
            if (has_alpha)
            {
                rgb565a8_data[width * height * 2 + i] = a; // 存储在RGB565数据之后
            }
        }
    }
    catch (const std::exception &e)
    {
        ESP_LOGE(TAG, "Exception during color conversion: %s", e.what());
        heap_caps_free(rgb565a8_data);
        rgb565a8_data = NULL;
        return false;
    }

    // 更新图像描述符
    img_desc.header.w = width;
    img_desc.header.h = height;
    img_desc.data_size = rgb565a8_size;
    img_desc.data = rgb565a8_data;

    // 检查剩余内存
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Free SPIRAM after decode: %zu bytes", free_spiram);
    ESP_LOGI(TAG, "Image converted successfully: %ux%u, format: %s",
             width, height, has_alpha ? "RGB565A8" : "RGB565");

    return true;
}

// 图片下载和处理任务
static void download_image_task(void *arg)
{
    DownloadImageParams *params = (DownloadImageParams *)arg;
    static lv_img_dsc_t img_desc;

    // 检查可用内存
    size_t free_heap = esp_get_free_heap_size();
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Before download - Free heap: %zu, Free SPIRAM: %zu", free_heap, free_spiram);

    // 内存不足检查
    if (free_spiram < 100 * 1024)
    { // 少于100KB SPIRAM
        ESP_LOGE(TAG, "Insufficient SPIRAM memory for download");
        goto cleanup;
    }

    if (downloadAndDecodeImage(params->url, img_desc))
    {
        // 创建更新消息
        lvgl_canvas_update_t *update = (lvgl_canvas_update_t *)malloc(sizeof(lvgl_canvas_update_t));
        if (update)
        {
            update->display = params->display;
            update->img_desc = &img_desc;
            update->width = img_desc.header.w;
            update->height = img_desc.header.h;
            // 发送到LVGL任务队列
            lv_async_call(lvgl_update_cb, update);
        }
        ESP_LOGI(TAG, "Image download successful: %s", params->url);
    }
    else
    {
        ESP_LOGE(TAG, "Image download failed: %s", params->url);
    }

cleanup:
    // 释放参数内存
    free((void *)params->url);
    free(params);

    vTaskDelete(NULL);
}
void SpiLcdAnimDisplay::showurl(const char *url) {
     if (url && strncmp(url, "http", 4) == 0)
    {
        // 如果是URL，下载图片并显示到画布
        ESP_LOGI(TAG, "SetEmotion: downloading image from URL: %s", url);

        // 创建下载参数
        DownloadImageParams *params = (DownloadImageParams *)malloc(sizeof(DownloadImageParams));
        if (!params)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for download params");
            return;
        }

        // 复制URL字符串
        params->url = strdup(url);
        params->display = this;

        // 启动下载任务
        xTaskCreate(download_image_task, "download_url", 8192, params, 2, NULL);
    }
}
void SpiLcdAnimDisplay::SetEmotion(const char *emotion)
{
    return;
    DisplayLockGuard lock(this);

    // 检查是否是URL（以http开头）
    if (emotion && strncmp(emotion, "http", 4) == 0)
    {
        // 如果是URL，下载图片并显示到画布
        ESP_LOGI(TAG, "SetEmotion: downloading image from URL: %s", emotion);

        // 创建下载参数
        DownloadImageParams *params = (DownloadImageParams *)malloc(sizeof(DownloadImageParams));
        if (!params)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for download params");
            return;
        }

        // 复制URL字符串
        params->url = strdup(emotion);
        params->display = this;

        // 启动下载任务
        xTaskCreate(download_image_task, "download_emotion", 8192, params, 2, NULL);
    }
    else
    {
        // 如果不是URL，调用父类的实现（显示表情符号）
        ESP_LOGI(TAG, "SetEmotion: displaying emoji: %s", emotion ? emotion : "null");

        
        auto status = Application::GetInstance().GetDeviceState();
        if (status != kDeviceStateSpeaking && status != kDeviceStateListening) {
            ESP_LOGE(TAG, "chat status is idle");
            return;
        }
        // auto protocol = Application::GetInstance().GetProtocolPtr();
        // if (!protocol) {
        //     ESP_LOGE(TAG, "protocol is null");
        //     return;
        // }
        // static int num = 0;
        // num++;
        // if (num < 3) {
        //     // num = 0;
        //     ESP_LOGE(TAG, "SetEmotion num: %d", num);
        //     return;
        // }


        // 如果有画布在显示图片，先销毁它
        if (HasCanvas())
        {
            DestroyCanvas();
            ESP_LOGI(TAG, "销毁画布以显示表情符号");
        }

        // 先隐藏图片对象
        if (emotion_label_img)
        {
            lv_img_set_src(emotion_label_img, NULL);
            lv_obj_add_flag(emotion_label_img, LV_OBJ_FLAG_HIDDEN);
        }

        struct Emotion
        {
            const char *icon;
            const char *text;
        };

        static const std::vector<Emotion> emotions = {
            {"😶", "neutral"},
            {"🙂", "happy"},
            {"😆", "laughing"},
            {"😂", "funny"},
            {"😔", "sad"},
            {"😠", "angry"},
            {"😭", "crying"},
            {"😍", "loving"},
            {"😳", "embarrassed"},
            {"😯", "surprised"},
            {"😱", "shocked"},
            {"🤔", "thinking"},
            {"😉", "winking"},
            {"😎", "cool"},
            {"😌", "relaxed"},
            {"🤤", "delicious"},
            {"😘", "kissy"},
            {"😏", "confident"},
            {"😴", "sleepy"},
            {"😜", "silly"},
            {"🙄", "confused"}};

        // 查找匹配的表情
        std::string_view emotion_view(emotion);
        auto it = std::find_if(emotions.begin(), emotions.end(),
                               [&emotion_view](const Emotion &e)
                               { return e.text == emotion_view; });

        // DisplayLockGuard lock(this);
        // if (emotion_label_ == nullptr)
        // {
        //     return;
        // }

        // 如果找到匹配的表情就显示对应图标，否则显示默认的neutral表情
        // lv_obj_set_style_text_font(emotion_label_, fonts_.emoji_font, 0);
        if (it != emotions.end())
        {
            ESP_LOGI(TAG, "it != emotions.end(): %s", it->text);
            // lv_label_set_text(emotion_label_, it->icon);
            // 如果是URL，下载图片并显示到画布
            ESP_LOGI(TAG, "SetEmotion: downloading image from URL: %s", emotion);

            // 创建下载参数
            DownloadImageParams *params = (DownloadImageParams *)malloc(sizeof(DownloadImageParams));
            if (!params)
            {
                ESP_LOGE(TAG, "Failed to allocate memory for download params");
                return;
            }

            // 复制URL字符串
            // 这行代码是错误的，strdup 只能复制 const char*，不能直接拼接 std::string 或 std::string_view
            // 正确做法如下：
            std::string url = "http://www.replime.cn/ejpg/" + std::string(it->text) + ".jpg";
            params->url = strdup(url.c_str());
            params->display = this;

            // 启动下载任务
            xTaskCreate(download_image_task, "download_emotion", 8192, params, 2, NULL);
        }
        else
        {
            // lv_label_set_text(emotion_label_, "😶");
            ESP_LOGI(TAG, "emotion_label_ is null");
        }

        // 调用父类实现
        // LcdDisplay::SetEmotion(emotion);
    }
}



