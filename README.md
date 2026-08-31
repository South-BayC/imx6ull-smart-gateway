# i.MX6ULL 智能安防网关

基于 i.MX6ULL 的智能安防网关，采用 LVGL 图形界面 + Linux 设备驱动，实现多防区入侵检测、摄像头监控与实时告警。

## 项目简介

本网关运行于正点原子 i.MX6ULL 开发板（1024x600 LCD + GT911 触摸屏），实现以下核心功能：

- **LVGL 图形界面**（v9.2.2）：主界面、弹窗系统、4 防区状态机
- **双模入侵检测**：支持多种传感器组合的布防/撤防/告警逻辑
- **摄像头监控**：OV5640 摄像头经 CSI 接口采集，V4L2 输出 YUYV 格式
- **以太网通信**：fec1 RMII 接口，支持网络音视频/数据回传

## 目录结构

```
project/
├── Makefile                    # 顶层构建脚本（含 lvgl 目标）
├── deploy_lvgl.sh              # 板端 NFS 部署脚本
├── VERIFICATION.md             # 板端验收文档
├── gateway-mockup.html         # 界面原型（浏览器预览）
├── imx6ull-alientek-emmc.dts   # 设备树（CSI/OV5640/SPI4/fec1 等）
├── core/lvgl/
│   ├── Makefile                # LVGL 交叉编译脚本
│   ├── lv_conf.h               # LVGL v9.2.2 配置
│   ├── lvgl/                   # LVGL 库源码（编译必需部分）
│   └── src/
│       ├── main.c              # 应用入口
│       ├── lvgl_port.c/.h      # fbdev + evdev 移植层
│       ├── font/               # 中文字体源文件
│       └── ui/                 # 界面模块
│           ├── ui_home.c/.h    # 主界面
│           ├── ui_dialogs.c/.h # 弹窗系统
│           └── state_machine.c/.h # 状态机
└── imx6ull_双项目设计/         # 项目设计与交接文档
```

## 硬件平台

- **主控**：NXP i.MX6ULL（Cortex-A7 @ 528MHz）
- **显示**：1024x600 RGB565 LCD（mxsfb fbdev `/dev/fb0`）
- **触摸**：GT911 电容触摸（evdev `/dev/input/event1`）
- **摄像头**：OV5640 500 万像素（CSI 接口 + I2C 控制）
- **网络**：fec1 RMII 以太网

## 交叉编译

```bash
# 需要 arm-linux-gnueabihf 工具链
make lvgl KDIR=/path/to/kernel \
          CROSS_COMPILE=arm-linux-gnueabihf- \
          BUILD_DIR=$(pwd)/build
```

详见 `VERIFICATION.md` 的完整验收流程。

## 相机子系统（内核侧）

相机驱动位于内核源码树 `drivers/media/platform/mxc/subdev/`：
- `ov5640.c`：OV5640 传感器驱动（v4l2_subdev，PLL 寄存器已针对 30fps 优化）
- `mx6s_capture.c`：CSI 采集驱动（MCLKDIV/像素格式配置）
- 采集模式：YUYV 640x480，含垂直翻转与字节对齐修正