# ESP32S3 KORVO2 V3 RTC闹钟系统集成说明

## 概述

本文档说明了如何在esp32s3_korvo2_v3硬件上使用完整的RTC闹钟系统，包括PCF8563 RTC芯片、NTP时间同步、时钟界面显示和语音闹钟功能。

## 系统架构

### 1. 硬件组件
- **PCF8563 RTC芯片**: 提供精确的实时时钟，地址0x51
- **I2C总线**: GPIO17(SDA), GPIO18(SCL)，用于RTC通信
- **240x240圆形显示屏**: 显示任天堂风格的时钟界面
- **WiFi模块**: 用于NTP时间同步

### 2. 软件组件
- **TimeSyncManager**: 统一管理RTC、NTP、时钟UI和闹钟
- **Pcf8563Rtc**: PCF8563芯片驱动
- **NtpSync**: 中国NTP服务器时间同步
- **ClockUI**: 任天堂风格的圆形时钟界面
- **AlarmManager**: 语音闹钟管理器
- **AlarmMcpTools**: MCP工具接口

## 功能特性

### 1. RTC时钟管理
- PCF8563芯片初始化和通信
- BCD时间格式转换
- 时钟启停控制
- 低电压检测
- 系统时间与RTC双向同步

### 2. NTP时间同步
- 中国NTP服务器（阿里云、中国NTP池）
- 北京时间时区设置（UTC+8）
- 异步同步机制
- 自动将NTP时间同步到RTC

### 3. 时钟界面显示
- 任天堂风格红色圆形背景
- 白色时间文字显示（HH:MM AM/PM）
- 日期显示（YYYY-MM-DD）
- 下一个闹钟时间指示
- 闹钟触发时的黄色通知面板

### 4. 语音闹钟功能
- 中文语音命令识别
- 相对时间设置："2小时后提醒我吃药"
- 绝对时间设置："明天8点提醒我开会"
- 每日重复闹钟："每天7点提醒我起床"
- 闹钟管理："取消闹钟"、"列出闹钟"
- NVS持久化存储

### 5. MCP工具接口
- set_alarm: 设置闹钟
- list_alarms: 列出所有闹钟
- cancel_alarm: 取消闹钟
- get_current_time: 获取当前时间
- rtc.get_status: 获取RTC状态
- rtc.sync_time: 手动触发NTP同步

## 使用方法

### 1. 系统启动流程
```
1. 板级初始化 → InitializeClockAndAlarm()
2. TimeSyncManager初始化 → PCF8563 RTC初始化
3. 时钟UI和闹钟管理器初始化
4. 开机时间同步 → 从RTC读取时间到系统
5. WiFi连接成功后 → 自动NTP同步到RTC
```

### 2. 语音命令示例
```
"请设置2小时后提醒我吃药"
"明天早上7点提醒我起床"
"每天下午5点提醒我下班"
"30分钟后提醒我开会"
"取消所有闹钟"
"列出我的闹钟"
```

### 3. MCP工具调用示例
```json
// 设置闹钟
{
  "tool": "set_alarm",
  "arguments": {
    "time": "2小时后",
    "description": "吃药提醒"
  }
}

// 获取RTC状态
{
  "tool": "rtc.get_status",
  "arguments": {}
}

// 手动同步时间
{
  "tool": "rtc.sync_time",
  "arguments": {}
}
```

### 4. 时钟界面控制
```cpp
// 显示时钟
Board::GetInstance().ShowClock();

// 隐藏时钟（对话状态时）
Board::GetInstance().HideClock();

// 检查时钟是否可见
bool visible = Board::GetInstance().IsClockVisible();
```

## 配置说明

### 1. I2C配置
- **总线**: I2C_NUM_1
- **SDA**: GPIO_NUM_17
- **SCL**: GPIO_NUM_18
- **频率**: 100kHz
- **地址**: 0x51 (PCF8563)

### 2. NTP服务器配置
- 主服务器: ntp.aliyun.com
- 备用服务器: time.pool.aliyun.com, cn.pool.ntp.org
- 时区: CST-8 (北京时间)
- 同步间隔: 1小时

### 3. 显示配置
- 分辨率: 240x240
- 格式: RGB565
- 字体: font_puhui_20_4
- 背景色: 红色 (Nintendo风格)
- 文字色: 白色

## 故障排除

### 1. RTC通信失败
- 检查I2C连接和地址
- 确认PCF8563芯片电源
- 检查I2C总线初始化

### 2. 时间同步失败
- 确认WiFi网络连接
- 检查NTP服务器可达性
- 验证时区设置

### 3. 闹钟不触发
- 检查RTC时间是否正确
- 确认闹钟数据存储
- 验证闹钟检查定时器

### 4. 时钟界面不显示
- 检查显示屏初始化
- 确认字体资源加载
- 验证UI创建和显示调用

## 开发注意事项

1. **电源管理**: PCF8563需要持续供电以保持时间
2. **时间精度**: RTC芯片精度约±20ppm
3. **存储限制**: NVS闹钟数据有大小限制
4. **任务优先级**: 时钟更新任务优先级设置合理
5. **内存使用**: 注意LVGL图形对象的内存管理

## 示例代码

参考main/boards/esp32s3-korvo2-v3/esp32s3_korvo2_v3_board.cc中的完整实现。

所有相关文件已集成到项目中，开机即可使用完整的RTC闹钟功能。 