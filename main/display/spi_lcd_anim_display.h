#ifndef SPI_LCD_ANIM_DISPLAY_H
#define SPI_LCD_ANIM_DISPLAY_H

#include "lcd_display.h"
#include <vector>
#include <string>
#include <lvgl.h>
#include <freertos/queue.h>

struct CameraFrame {
    uint8_t* buf;
    int w;
    int h;
};

// SPI LCD 动画显示器，支持角色ID切换，三种状态动画，.raw图片，lv_anim_t动画
class SpiLcdAnimDisplay : public SpiLcdDisplay {
public:
    SpiLcdAnimDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                      int width, int height, int offset_x, int offset_y,
                      bool mirror_x, bool mirror_y, bool swap_xy,
                      DisplayFonts fonts);

    virtual void SetupUI() override;
    // void SetRoleId(int role_id); // 切换角色ID，自动加载动画帧
    // void SetAnimState(const std::string& state); // "listen"/"speak"/"idle"
    void SetRoleId(int role_id) override;
    void SetAnimState(const std::string& state) override;
    void SetEmotion(const char *emotion) override;

    std::string current_state_;

    void OnFramesLoaded();

    void ShowRgb565(const uint8_t* buf, int w, int h);
    void ShowJpeg(const uint8_t* jpg, size_t len);
    void ShowRgb565Frame(const uint8_t* buf, int w, int h);

    void StartFrameQueueTask();
    void StopFrameQueueTask();
    void EnqueueFrame(const uint8_t* buf, int w, int h);

    // 拍照并显示到动画图片对象
    void CaptureAndShowPhoto();
    void showurl(const char *url);

protected:
    int role_id_ = 0;
    std::vector<std::string> speak_frames_;
    std::vector<std::string> listen_frames_;
    std::string idle_image_;
    lv_obj_t* anim_img_obj_ = nullptr;
    lv_obj_t* emotion_label_img = nullptr;
    lv_anim_t anim_;
    int current_frame_idx_ = 0;
    int frame_count_ = 0;
    int anim_fps_ = 8;

    QueueHandle_t frame_queue_ = nullptr;
    TaskHandle_t frame_task_handle_ = nullptr;

    void LoadFrames();
    void StartAnim();
    void StopAnim();
    // static void AnimCustomExecCb(lv_anim_t* a, void* obj, int32_t value);
    void ShowIdleImage();
};

#endif // SPI_LCD_ANIM_DISPLAY_H 