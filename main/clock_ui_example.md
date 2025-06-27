# Clock UI 壁纸和动画功能使用示例

本文档展示了时钟界面的壁纸和动画功能的使用方法。这些功能通过MCP工具提供，可以通过AI助手或直接调用来使用。

## 壁纸功能

### 1. 设置纯色壁纸

```json
{
  "tool": "clock.set_solid_wallpaper",
  "arguments": {
    "color": "blue"
  }
}
```

支持的颜色格式：
- 颜色名称: `"red"`, `"green"`, `"blue"`, `"yellow"`, `"purple"`, `"cyan"`, `"white"`, `"black"`, `"orange"`, `"pink"`
- 十六进制格式: `"#FF0000"` (红色)
- 0x格式: `"0xFF0000"` (红色)

### 2. 设置SD卡图片壁纸

```json
{
  "tool": "clock.set_image_wallpaper", 
  "arguments": {
    "image_name": "WALLPPR1"
  }
}
```

要求：
- 图片文件名不超过8个字符，自动转换为大写
- 文件格式：JPG
- 存储位置：`/sdcard/WALLPPR1.JPG`

### 3. 设置网络壁纸

```json
{
  "tool": "clock.set_network_wallpaper",
  "arguments": {
    "url": "http://example.com/wallpaper.jpg"
  }
}
```

功能：
- 自动下载网络图片
- 缓存到SD卡避免重复下载
- 支持JPG格式

### 4. 清除壁纸

```json
{
  "tool": "clock.clear_wallpaper",
  "arguments": {}
}
```

## 动画功能

### 1. 设置SD卡动画

```json
{
  "tool": "clock.set_animation",
  "arguments": {
    "animation_name": "heart_beat"
  }
}
```

### 2. 设置网络动画

```json
{
  "tool": "clock.set_network_animation",
  "arguments": {
    "url": "http://example.com/animation.bin"
  }
}
```

### 3. 显示/隐藏动画

```json
{
  "tool": "clock.show_animation",
  "arguments": {
    "show": true
  }
}
```

### 4. 清除动画

```json
{
  "tool": "clock.clear_animation",
  "arguments": {}
}
```

## 使用场景示例

### 场景1：设置蓝色背景
用户：请把时钟背景设置为蓝色
AI助手会调用：`clock.set_solid_wallpaper` with `color: "blue"`

### 场景2：使用SD卡壁纸
用户：请使用SD卡里的BJ.JPG作为时钟壁纸
AI助手会调用：`clock.set_image_wallpaper` with `image_name: "BJ"`

### 场景3：从网络下载壁纸
用户：请从网络下载这个图片作为壁纸：http://example.com/nature.jpg
AI助手会调用：`clock.set_network_wallpaper` with `url: "http://example.com/nature.jpg"`

## 技术特性

### 壁纸功能
- 全屏尺寸：240x240像素（根据设备屏幕调整）
- 透明背景容器确保壁纸正确显示
- 自动配置持久化到`/sdcard/wallpaper.cfg`
- 智能缓存避免重复下载网络资源

### 动画功能
- 动画标签尺寸：64x64像素
- 位置：居中显示在时间下方
- 自动配置持久化到`/sdcard/animation.cfg`
- 支持显示/隐藏控制

### 系统集成
- 与时钟UI完全集成，不影响时间和日期显示
- 自动配置加载，重启后保持设置
- 内存优化，使用SPIRAM存储图像数据
- 错误处理和日志记录完善

## 文件格式支持

### 图片格式
- JPG：主要支持格式，推荐使用
- 文件命名：8字符大写，如`WALLPPR1.JPG`

### 存储位置
- SD卡根目录：`/sdcard/`
- 配置文件：`/sdcard/wallpaper.cfg`, `/sdcard/animation.cfg`

## 注意事项

1. 确保SD卡已正确挂载
2. 网络功能需要WiFi连接
3. 大图片可能影响内存使用
4. JPG解码需要足够的SPIRAM
5. 配置文件自动管理，无需手动编辑

# 时钟界面表情显示功能

## 功能说明

时钟界面现在支持在中央位置显示表情字符，用于显示与闹钟相关的情感表达。表情显示在64x64像素的区域内，位于时间显示下方。

## 使用方法

### 1. 通过MCP工具测试表情显示

使用 `test.show_alarm_emotion` 工具来测试表情显示功能：

```json
{
    "name": "test.show_alarm_emotion",
    "arguments": {
        "emotion_type": "起床"
    }
}
```

### 2. 支持的表情类型和对应字符

| 类型关键词 | 显示表情 | 说明 |
|-----------|---------|------|
| 起床、morning、wake | 😴 | 睡觉表情 |
| 吃饭、用餐、lunch、dinner、breakfast | 🤤 | 流口水表情 |
| 锻炼、运动、健身、exercise、workout | 😎 | 酷表情 |
| 工作、上班、会议、work、meeting | 🤔 | 思考表情 |
| 学习、复习、考试、study | 🤔 | 思考表情 |
| 生日、庆祝、party、birthday | 😆 | 开心表情 |
| 医院、看病、医生、doctor | 😔 | 难过表情 |
| 约会、聚会、朋友、date、friend | 😍 | 爱心表情 |
| 睡觉、休息、sleep、rest | 😌 | 放松表情 |
| 购物、买、shopping | 😉 | 眨眼表情 |
| 其他 | 🙂 | 默认开心表情 |

### 3. 在闹钟触发时自动显示表情

当闹钟触发时，系统会根据闹钟描述自动选择合适的表情。

## 技术实现

### 表情显示位置
- 位置：屏幕中央，时间显示下方
- 大小：64x64 像素
- 字体：优先使用表情字体，备选为大号文字字体

### 表情映射算法
`GetEmotionForAlarmType()` 函数通过关键词匹配来选择合适的表情字符。支持中英文关键词，不区分大小写。

## 测试步骤

1. **显示时钟界面**：使用 `clock.show` 工具
2. **测试不同表情**：使用 `test.show_alarm_emotion` 工具
3. **清除表情**：传入空的 `emotion_type`

## 未来扩展

- 替换文字表情为真正的动画文件
- 支持自定义表情映射规则
- 添加表情动画效果 