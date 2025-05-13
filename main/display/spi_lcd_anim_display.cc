#include <lvgl.h>
#include "spi_lcd_anim_display.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <algorithm>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
    if (!d) return;
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
                fread(idle_img_cache, 1, FRAME_SIZE, fp);
            }
            fclose(fp);
        }
    }
    ESP_LOGI(TAG, "LoadIdleRaw: %s", dir.c_str());
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
    LoadIdleRaw(std::string(base) + "/standby");
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
    anim_img_obj_ = lv_img_create(screen);
    lv_obj_set_size(anim_img_obj_, width_, height_);
    lv_obj_align(anim_img_obj_, LV_ALIGN_CENTER, 0, 0);
    LoadFrames();
    SetAnimState("idle");
    ESP_LOGI(TAG, "SetupUI: %s", current_state_.c_str());
}

void SpiLcdAnimDisplay::SetRoleId(int role_id) {
    if (role_id_ == role_id) return;
    ESP_LOGI(TAG, "SetRoleId: %d", role_id);
    role_id_ = role_id;
    LoadFrames();
    // SetAnimState("idle");

}

void SpiLcdAnimDisplay::SetAnimState(const std::string& state) {
   
    if (state == current_state_) return;
    StopAnim();
    current_state_ = state;
    if (state == "speak" && speak_anim_cache_count > 0) {
        frame_count_ = speak_anim_cache_count;
        anim_fps_ = 8;
        StartAnim();
    } else if (state == "listen" && listen_anim_cache_count > 0) {
        frame_count_ = listen_anim_cache_count;
        anim_fps_ = 8;
        StartAnim();
    } else {
        ShowIdleImage();
    }
    ESP_LOGI(TAG, "SetAnimState: %s", current_state_.c_str());
}

void SpiLcdAnimDisplay::LoadFrames() {
    static int last_role_id = -1;
    if (last_role_id == role_id_) return;
    last_role_id = role_id_;
    // 启动异步任务加载动画帧
    AnimFramesTaskParam* param = new AnimFramesTaskParam{role_id_, this};
    xTaskCreate(anim_frames_load_task, "anim_load", 8192, param, 5, NULL);
}

void SpiLcdAnimDisplay::OnFramesLoaded() {
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
        ShowIdleImage();
    }
    ESP_LOGI(TAG, "OnFramesLoaded: %s", current_state_.c_str());
}

static void anim_exec_cb(void* obj, int32_t frame) {
    SpiLcdAnimDisplay* self = static_cast<SpiLcdAnimDisplay*>(lv_obj_get_user_data((lv_obj_t*)obj));
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

    lv_anim_init(&anim_);
    lv_anim_set_var(&anim_, anim_img_obj_);
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
    if (!idle_img_cache) return;
    static lv_img_dsc_t img_desc;
    img_desc.header.cf = LV_COLOR_FORMAT_RGB565;
    img_desc.header.w = FRAME_WIDTH;
    img_desc.header.h = FRAME_HEIGHT;
    img_desc.data_size = FRAME_SIZE;
    img_desc.data = idle_img_cache;
    lv_img_set_src(anim_img_obj_, &img_desc);
    ESP_LOGI(TAG, "ShowIdleImage: %s", current_state_.c_str());
} 