#pragma once
#include <vector>
#include <functional>
#include <cstdint>
#include <lvgl.h>
class CameraService {
public:
    static CameraService& GetInstance();
    bool Init(); // 初始化摄像头
    // 实时预览，回调返回RGB565数据（宽、高、数据指针、字节数）
    void StartPreview(std::function<void(int, int, const uint8_t*, size_t)> on_frame);
    void StopPreview();
    // 拍照，返回JPEG数据
    bool TakePhoto(std::vector<uint8_t>& jpg_data);
    bool TakePhotoToBuffer(uint8_t* buf, size_t buf_size, size_t& out_len);
    // 拍照并将RGB565数据写入外部缓冲区
    bool TakePhotoRgb565ToBuffer(uint8_t* buf, size_t buf_size, int& w, int& h);
    // 拍照并直接显示到LVGL图片对象（JPEG->RGB888）
    bool ShowPhotoToLvgl(lv_obj_t* img_obj);
}; 