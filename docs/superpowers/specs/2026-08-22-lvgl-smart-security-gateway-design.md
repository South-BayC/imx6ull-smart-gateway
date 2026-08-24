# 智能安防边缘网关 LVGL 设计文档

**日期**: 2026-08-22  
**作者**: SouthBay · Sisyphus  
**状态**: 已实现（P7-3/P7-4，2026-08-23；实现差异见文末）  
**关联**: P7-2 LVGL 移植，基于 P7-1 GT911 触摸（中断模式）与 mxsfb 1024x600

---

## 1. 概述

### 1.1 目标
在 i.MX6ULL（528MHz 单核无GPU，Buildroot，Linux 4.1.15）上，用 LVGL v9 构建**可落地的企业级智能安防边缘网关界面**，实现本地中文触摸交互（布防/撤防/告警/抓拍）与云端精判协同，单 OV5640 摄像头通过模拟切换覆盖多防区。

### 1.2 硬件基线（已验证）
- **显示**: 7寸 1024x600 RGB，mxsfb `/dev/fb0`，bits-per-pixel 24，pwm-backlight 背光
- **触摸**: GT911 `gt9147.ko` 中断模式，`X_MAX 1024 Y_MAX 600 TRIGGER FALLING`，`/dev/input/event1`，板测 515次中断触发
- **外设可复用**: `led`/`beep_pwm`（声光告警）、`gpio_event_capture`（防区脉冲）、`ap3216c`（环境）、`key_input`（备用）
- **存储/网络**: TF 卡（抓拍）、NFS/TFTP（开发期）、MQTT（云端）

### 1.3 成功标准
- 中文全量显示（常用3500字，16/20px）无方块
- 触摸点击延迟 < 100ms，滑动帧率 ≥ 25 FPS（局部刷新）
- 布防/撤防/消警/抓拍/切换 5 类按钮 100% 触摸可用
- 抓拍相册、定时布防、告警分级、系统设置 4 项增强功能可用
- 交叉编译产物 < 5MB，内存占用 < 2MB（含字库）

---

## 2. 需求

### 2.1 核心场景
**中小商铺/仓库夜间守护**：店主下班一键布防，4防区（前门/后门/窗/仓库）实时在线，异常声光告警+弹窗+时间轴，单摄像头分时查看各防区，抓拍存档。

### 2.2 功能清单（首版）

| 模块 | 功能 | 交互 | 优先级 |
|------|------|------|--------|
| **布撤防** | 一键布防/撤防（全区）、分区状态卡片（在线·布防中·异常·离线） | 底栏大按钮（56px高），布防后卡片绿光呼吸 | P0 |
| **告警** | 弹窗（时间/位置/类型）+ 蜂鸣长鸣 + LED 快闪 | 需长按1秒“确认/消警”防误触，一键静音 | P0 |
| **时间轴** | 近20条事件（触发→确认→撤防）可滚动，最新在上，按类型着色 | 右侧 360 宽列表，触摸滑动 | P0 |
| **摄像头** | 单 OV5640 实时预览占位（640x360）+ 4 位置模拟切换（前门/后门/仓库/周界）+ 抓拍 | 顶部4切换钮，预览区右下角“抓拍” | P0 |
| **抓拍相册** | 抓拍 JPG 存 TF 卡，点击时间轴条目查看缩略图 | 相册弹窗，支持删除 | P1 |
| **定时布防** | 22:00-06:00 自动布防开关（演示模式：10秒后自动布防） | 设置页开关 | P1 |
| **告警分级** | 周界=高（红闪+长鸣）、仓库=中（琥珀）、门=低；级别影响弹窗与声光策略 | 设置页可配 | P1 |
| **系统设置** | 背光亮度滑条（pwm-backlight）、音量、WiFi/网络状态、关于 | 设置弹窗 | P1 |

### 2.3 非功能
- 中文：GB2312 常用字 + 业务字（报警布防撤防通道异常离线在线等）
- 性能：1024x600 16bpp，PARTIAL 60行，LV_MEM 64KB，单核 DRAW_CNT 1
- 可靠：断网本地存图，联网补传；10秒 LCD 熄屏可唤醒

---

## 3. 架构

```
[传感器层] 门磁/红外/烟感 --GPIO/I2C--> [边缘网关 i.MX6ULL]
                                          ├─ 本地：LVGL UI(1024x600) + 状态机 + 本地日志（TF卡）
                                          ├─ 驱动：fbdev /dev/fb0 + evdev /dev/input/event1 + beep/led/gpio
                                          └─ 云端：MQTT 上报事件+抓拍 → 云精判 → 下发确认
[执行层] 蜂鸣/LED <--PWM/GPIO-- 网关
[交互层] 人 <--触摸--> LVGL <--事件--> 网关 <--MQTT--> 云/App
[视觉层] OV5640 --V4L2--> 预览（占位→真实流，单路分时切换）
```

**技术栈**：LVGL v9（内置 fbdev/evdev 驱动，无需 lv_drivers 外部仓）+ C + Buildroot + arm-linux-gnueabihf

---

## 4. 组件分解（高内聚、低耦合）

| 组件 | 职责 | 接口 | 依赖 |
|------|------|------|------|
| **lvgl-port** | 初始化 lv_init、tick、fbdev/evdev 创建 | `lv_display_t *disp = lv_linux_fbdev_create()` <br> `lv_indev_t *indev = lv_evdev_create(POINTER, "/dev/input/event1")` | fbdev, evdev, lv_conf.h |
| **display-mgr** | 管理 1024x600 刷新策略、背光亮度 | `set_brightness(level)` | /dev/fb0, pwm-backlight |
| **touch-mgr** | 触摸校准（swap_axes/calibration 若需） | `lv_evdev_set_calibration()` | /dev/input/event1 |
| **font-mgr** | 中文字库加载与回退 | `lv_font_SHSC_16/20` + Fallback 链 | lv_font_conv 生成的 C 数组 |
| **state-machine** | 防区状态（在线/布防/异常/离线）、布撤防、告警分级 | `arm_all()`, `disarm_all()`, `ack_alarm(id)` | gpio_event_capture（后续） |
| **ui-home** | 主屏：预览+4卡片+时间轴+底栏 | LVGL objects | state-machine, display-mgr |
| **ui-dialogs** | 弹窗：告警确认、分区详情、相册、设置 | `show_alarm_dialog()`, `show_gallery()` | ui-home |
| **storage-mgr** | 抓拍 JPG 存取、事件日志 | `capture_save()`, `log_append()` | TF 卡 /tmp |

每个组件可独立理解与测试，LVGL 对象通过回调与状态机交互，不直接操作驱动。

---

## 5. UI 设计（企业级个性化）

### 5.1 设计令牌
- **色彩**：背景 #0B1220，深卡 #141E33，卡片 #1B2A4A，边框 #253656；文字主 #E6EDF7 次 #8A9AB5；强调 琥珀 #FFB020（告警）、青 #00D1FF（交互）、绿 #20C997（布防）、红 #FF4D4F（高危）
- **字体**：中文 Noto Sans SC（16px 正文/20px 标题，bpp4 抗锯齿），数字 JetBrains Mono，时间戳 Space Mono
- **布局**：1024x600 固定画布，顶栏32px（状态）+ 主区（左640预览+2x2卡片 / 右360时间轴）+ 底栏56px（大按钮），8px 间距体系
- **签名**：**周界呼吸光**——分区卡片边框按状态微光呼吸（布防绿脉冲/告警红闪），摄像头扫描线持续动效，工业控制台辨识度

### 5.2 交互
- 布防/撤防：底栏大按钮，按压缩放反馈，布防后全卡片绿光呼吸，15-30秒随机告警演示（可关）
- 告警：卡片变红闪 + 弹窗（需长按1秒确认），蜂鸣/LED 联动，一键消警
- 预览切换：顶部4钮（前门/后门/仓库/周界）切占位图，点卡片高亮对应钮
- 时间轴：触摸滑动，点条目看抓拍缩略图

### 5.3 原型
已生成高保真 Mockup：`C:\Users\Southbay\AppData\Local\Temp\opencode\gateway-mockup.html`（1024x600，单文件，可交互预览）

---

## 6. 数据流

1. 触摸 → evdev `/dev/input/event1` → `lv_evdev` → LVGL indev → 按钮回调
2. 按钮回调 → state-machine（布防状态变更）→ 更新卡片样式 + 触发蜂鸣/LED + 追加时间轴 + MQTT 上报（预留）
3. 摄像头 → V4L2（占位期为静态图）→ 预览区 `lv_img` 刷新
4. 抓拍 → storage-mgr 存 JPG → 时间轴条目关联缩略图

---

## 7. 错误处理
- fbdev 打开失败 → 日志提示，降级黑屏占位，不崩溃
- input 打开失败 → 提示“触摸未就绪”，重试 3 次
- 字库缺字 → Fallback 链回退至 Montserrat，不显示方块
- TF 卡满 → 循环覆盖最旧抓拍，提示
- 网络断 → 本地存图，联网补传

---

## 8. 中文方案
- **主选**：`lv_font_conv` 离线点阵（C 数组），3500 常用字，16px/20px bpp4，`--no-compress` 调试、`--compress` 量产
- 流程：`fonttools pyftsubset` 子集化 SourceHanSansSC → `lv_font_conv` 转 C → `LV_FONT_CUSTOM_DECLARE` 引入 → `lv_style_set_text_font`
- 备用：`binfont` 文件（OTA 换字库时）、`TinyTTF`（多字号动态缩放，i.MX6ULL 慎用）

---

## 9. 构建与部署
- **源码位置**：`core/lvgl/`（新建，遵循 core/ 目录模式）
- **配置**：`lv_conf.h` 基于 `lv_conf_template.h`，仅改 §5.1 段（COLOR_DEPTH 16, MEM_SIZE 64KB, USE_LINUX_FBDEV/EVDEV 1）
- **编译**：复用顶层 `KDIR/CROSS_COMPILE/BUILD_DIR` 约定，`make -C core/lvgl BUILD_DIR=$(pwd)/build`
- **部署**：`build/lvgl/` → NFS rootfs，`make send` 同步
- **启动**：`/etc/init.d/rcS` 自启 `lvgl_gateway &`

---

## 10. 测试

| 层级 | 用例 | 断言 |
|------|------|------|
| 单元 | 字体渲染：中文“布防”显示无方块 | 像素比对 |
| 集成 | fbdev 刷新：画红块 → 读 /dev/fb0 | 颜色一致 |
| 集成 | evdev 触摸：点击布防 → 状态变绿 | 事件回调 |
| 端到端 | 布防→触发→弹窗→长按确认→时间轴+1 | 全链路 |
| 性能 | 滑动时间轴 30s，帧率 ≥25 FPS，内存水位 <80% | LVGL perf monitor |

---

## 11. 演进
- P5 摄像头到货：V4L2 真实流替换占位图，4 切换钮接真实多路或同一路多预设
- AI 扩展：云端精判结果回显（人形/车辆标签）
- 多用户：权限管理、日志导出 USB

---

**待用户确认后**：进入 `writing-plans` 生成详细实施计划。

---

## 12. 实现记录（2026-08-23，P7-3/P7-4）

### 已实现（全部 P0/P1 + 部分 P2）
- 全部功能清单项（2.2）已实现，并有多处超出设计：
  - 分区级独立布防/撤防（详情弹窗状态按钮，预警门槛 per-zone）
  - 预警配置化：四位置×触发源×数据通道×阈值×比较方式（GT/EQ/LT）×冷却时间
  - 告警确认弹窗：长按 1s 消警（进度条）+ MUTE 静音
  - 告警分级声光：高/中/低 三档 LED 闪烁与蜂鸣节奏
  - 抓拍：真实画面全图 + TF 卡 BMP 持久化 + 检测面板 TF 容量显示
  - 摄像头：真实 V4L2 流（640×480→630×340 查表缩放转换，30fps 实测显示）
  - 真实时间显示、KEY0 唤屏（10 分钟息屏）、亮度联动背光

### 实现差异（相对本设计）
| 设计 | 实际 | 原因 |
|---|---|---|
| 中文字库 3500 字 | 147 字符业务子集 | lv_font_conv 工具链离线不可用；缺字以文案规避 |
| 抓拍 JPG 存 TF | BMP 存 TF（内存相册重启丢失）| 无 JPG 编码库；BMP 无依赖通用可读 |
| storage-mgr 独立组件 | 抓拍逻辑内聚 ui_home + cam_feed | 单文件内聚足够，未独立模块 |
| LV_MEM 64KB | 256KB | 对象数超预期（弹窗/相册/测试面板）|
| 告警长按确认 | 已实现 ✓ | — |
| 10 秒熄屏 | 10 分钟（用户调整）| 用户需求 |
