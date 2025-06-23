#ifndef PCF8563_RTC_H
#define PCF8563_RTC_H

#include <driver/i2c_master.h>
#include <time.h>
#include <sys/time.h>
#include <string>

// PCF8563 I2C地址
#define PCF8563_I2C_ADDRESS     0x51

// PCF8563寄存器地址
#define PCF8563_REG_CONTROL1    0x00
#define PCF8563_REG_CONTROL2    0x01
#define PCF8563_REG_SECONDS     0x02
#define PCF8563_REG_MINUTES     0x03
#define PCF8563_REG_HOURS       0x04
#define PCF8563_REG_DAYS        0x05
#define PCF8563_REG_WEEKDAYS    0x06
#define PCF8563_REG_MONTHS      0x07
#define PCF8563_REG_YEARS       0x08

// PCF8563控制位定义
#define PCF8563_CONTROL1_STOP   0x20
#define PCF8563_CONTROL1_TESTC  0x08
#define PCF8563_CONTROL2_TIE    0x01
#define PCF8563_CONTROL2_AIE    0x02
#define PCF8563_CONTROL2_TF     0x04
#define PCF8563_CONTROL2_AF     0x08
#define PCF8563_CONTROL2_TI_TP  0x10

// PCF8563 RTC时钟驱动类
class Pcf8563Rtc {
public:
    Pcf8563Rtc(i2c_master_bus_handle_t i2c_bus);
    ~Pcf8563Rtc();

    // 初始化RTC
    bool Initialize();
    
    // 设置时间
    bool SetTime(const struct tm* time_tm);
    bool SetTime(time_t timestamp);
    
    // 读取时间
    bool GetTime(struct tm* time_tm);
    bool GetTime(time_t* timestamp);
    
    // 获取当前时间字符串
    std::string GetTimeString();
    
    // 检查时钟是否运行
    bool IsRunning();
    
    // 启动/停止时钟
    bool StartClock();
    bool StopClock();
    
    // 同步系统时间到RTC
    bool SyncSystemTimeToRtc();
    
    // 从RTC同步到系统时间
    bool SyncRtcToSystemTime();

private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t i2c_dev_;
    
    // I2C读写函数
    bool WriteRegister(uint8_t reg, uint8_t data);
    bool ReadRegister(uint8_t reg, uint8_t* data);
    bool ReadRegisters(uint8_t reg, uint8_t* data, size_t len);
    
    // BCD转换函数
    uint8_t DecToBcd(uint8_t dec);
    uint8_t BcdToDec(uint8_t bcd);
    
    // 验证时间有效性
    bool IsValidTime(const struct tm* time_tm);
};

#endif // PCF8563_RTC_H 