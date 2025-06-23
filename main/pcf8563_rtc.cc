#include "pcf8563_rtc.h"
#include <esp_log.h>
#include <string.h>
#include <sstream>
#include <iomanip>

#define TAG "PCF8563_RTC"

Pcf8563Rtc::Pcf8563Rtc(i2c_master_bus_handle_t i2c_bus) 
    : i2c_bus_(i2c_bus), i2c_dev_(nullptr) {
}

Pcf8563Rtc::~Pcf8563Rtc() {
    if (i2c_dev_) {
        i2c_master_bus_rm_device(i2c_dev_);
    }
}

bool Pcf8563Rtc::Initialize() {
    ESP_LOGI(TAG, "Initializing PCF8563 RTC");
    
    // 配置I2C设备
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF8563_I2C_ADDRESS,
        .scl_speed_hz = 100000,  // 100kHz
    };
    
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &i2c_dev_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(ret));
        return false;
    }
    
    // 检查芯片是否存在
    uint8_t test_data;
    if (!ReadRegister(PCF8563_REG_CONTROL1, &test_data)) {
        ESP_LOGE(TAG, "Failed to communicate with PCF8563");
        return false;
    }
    
    // 清除停止位，启动时钟
    if (!StartClock()) {
        ESP_LOGE(TAG, "Failed to start RTC clock");
        return false;
    }
    
    // 清除控制寄存器中的中断标志
    WriteRegister(PCF8563_REG_CONTROL2, 0x00);
    
    ESP_LOGI(TAG, "PCF8563 RTC initialized successfully");
    return true;
}

bool Pcf8563Rtc::SetTime(const struct tm* time_tm) {
    if (!IsValidTime(time_tm)) {
        ESP_LOGE(TAG, "Invalid time provided");
        return false;
    }
    
    uint8_t time_data[7];
    
    // 转换为BCD格式
    time_data[0] = DecToBcd(time_tm->tm_sec);        // 秒
    time_data[1] = DecToBcd(time_tm->tm_min);        // 分
    time_data[2] = DecToBcd(time_tm->tm_hour);       // 时（24小时制）
    time_data[3] = DecToBcd(time_tm->tm_mday);       // 日
    time_data[4] = DecToBcd(time_tm->tm_wday);       // 星期
    time_data[5] = DecToBcd(time_tm->tm_mon + 1);    // 月（1-12）
    time_data[6] = DecToBcd(time_tm->tm_year % 100); // 年（00-99）
    
    // 写入时间寄存器
    for (int i = 0; i < 7; i++) {
        if (!WriteRegister(PCF8563_REG_SECONDS + i, time_data[i])) {
            ESP_LOGE(TAG, "Failed to write time register %d", i);
            return false;
        }
    }
    
    ESP_LOGI(TAG, "RTC time set: %04d-%02d-%02d %02d:%02d:%02d", 
             time_tm->tm_year + 1900, time_tm->tm_mon + 1, time_tm->tm_mday,
             time_tm->tm_hour, time_tm->tm_min, time_tm->tm_sec);
    
    return true;
}

bool Pcf8563Rtc::SetTime(time_t timestamp) {
    struct tm* time_tm = localtime(&timestamp);
    return SetTime(time_tm);
}

bool Pcf8563Rtc::GetTime(struct tm* time_tm) {
    uint8_t time_data[7];
    
    // 读取时间寄存器
    if (!ReadRegisters(PCF8563_REG_SECONDS, time_data, 7)) {
        ESP_LOGE(TAG, "Failed to read time registers");
        return false;
    }
    
    // 转换为十进制格式
    time_tm->tm_sec = BcdToDec(time_data[0] & 0x7F);    // 清除VL位
    time_tm->tm_min = BcdToDec(time_data[1] & 0x7F);
    time_tm->tm_hour = BcdToDec(time_data[2] & 0x3F);
    time_tm->tm_mday = BcdToDec(time_data[3] & 0x3F);
    time_tm->tm_wday = BcdToDec(time_data[4] & 0x07);
    time_tm->tm_mon = BcdToDec(time_data[5] & 0x1F) - 1; // 转换为0-11
    time_tm->tm_year = BcdToDec(time_data[6]) + 100;     // 转换为年数（从1900年开始）
    
    // 检查VL位（电压低指示）
    if (time_data[0] & 0x80) {
        ESP_LOGW(TAG, "RTC voltage low flag set");
    }
    
    return true;
}

bool Pcf8563Rtc::GetTime(time_t* timestamp) {
    struct tm time_tm;
    if (!GetTime(&time_tm)) {
        return false;
    }
    
    *timestamp = mktime(&time_tm);
    return true;
}

std::string Pcf8563Rtc::GetTimeString() {
    struct tm time_tm;
    if (!GetTime(&time_tm)) {
        return "RTC读取失败";
    }
    
    std::ostringstream oss;
    oss << std::setfill('0') 
        << std::setw(4) << (time_tm.tm_year + 1900) << "-"
        << std::setw(2) << (time_tm.tm_mon + 1) << "-"
        << std::setw(2) << time_tm.tm_mday << " "
        << std::setw(2) << time_tm.tm_hour << ":"
        << std::setw(2) << time_tm.tm_min << ":"
        << std::setw(2) << time_tm.tm_sec;
    
    return oss.str();
}

bool Pcf8563Rtc::IsRunning() {
    uint8_t control1;
    if (!ReadRegister(PCF8563_REG_CONTROL1, &control1)) {
        return false;
    }
    
    // STOP位为0表示时钟运行
    return !(control1 & PCF8563_CONTROL1_STOP);
}

bool Pcf8563Rtc::StartClock() {
    uint8_t control1;
    if (!ReadRegister(PCF8563_REG_CONTROL1, &control1)) {
        return false;
    }
    
    // 清除STOP位
    control1 &= ~PCF8563_CONTROL1_STOP;
    return WriteRegister(PCF8563_REG_CONTROL1, control1);
}

bool Pcf8563Rtc::StopClock() {
    uint8_t control1;
    if (!ReadRegister(PCF8563_REG_CONTROL1, &control1)) {
        return false;
    }
    
    // 设置STOP位
    control1 |= PCF8563_CONTROL1_STOP;
    return WriteRegister(PCF8563_REG_CONTROL1, control1);
}

bool Pcf8563Rtc::SyncSystemTimeToRtc() {
    time_t now;
    time(&now);
    return SetTime(now);
}

bool Pcf8563Rtc::SyncRtcToSystemTime() {
    time_t rtc_time;
    if (!GetTime(&rtc_time)) {
        return false;
    }
    
    struct timeval tv = { .tv_sec = rtc_time, .tv_usec = 0 };
    if (settimeofday(&tv, nullptr) != 0) {
        ESP_LOGE(TAG, "Failed to set system time");
        return false;
    }
    
    ESP_LOGI(TAG, "System time synced from RTC: %s", GetTimeString().c_str());
    return true;
}

bool Pcf8563Rtc::WriteRegister(uint8_t reg, uint8_t data) {
    uint8_t write_buf[2] = {reg, data};
    esp_err_t ret = i2c_master_transmit(i2c_dev_, write_buf, 2, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write register 0x%02X: %s", reg, esp_err_to_name(ret));
        return false;
    }
    return true;
}

bool Pcf8563Rtc::ReadRegister(uint8_t reg, uint8_t* data) {
    return ReadRegisters(reg, data, 1);
}

bool Pcf8563Rtc::ReadRegisters(uint8_t reg, uint8_t* data, size_t len) {
    esp_err_t ret = i2c_master_transmit_receive(i2c_dev_, &reg, 1, data, len, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read register 0x%02X: %s", reg, esp_err_to_name(ret));
        return false;
    }
    return true;
}

uint8_t Pcf8563Rtc::DecToBcd(uint8_t dec) {
    return ((dec / 10) << 4) | (dec % 10);
}

uint8_t Pcf8563Rtc::BcdToDec(uint8_t bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

bool Pcf8563Rtc::IsValidTime(const struct tm* time_tm) {
    if (!time_tm) return false;
    
    return (time_tm->tm_sec >= 0 && time_tm->tm_sec <= 59) &&
           (time_tm->tm_min >= 0 && time_tm->tm_min <= 59) &&
           (time_tm->tm_hour >= 0 && time_tm->tm_hour <= 23) &&
           (time_tm->tm_mday >= 1 && time_tm->tm_mday <= 31) &&
           (time_tm->tm_mon >= 0 && time_tm->tm_mon <= 11) &&
           (time_tm->tm_year >= 100); // 年份从1900开始，所以2000年是100
} 