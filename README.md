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
project/                          # 本仓库：仅包含 LVGL 应用层代码
├── Makefile                    # 顶层构建脚本（默认只构建 LVGL 应用层）
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

# 不在本仓库：内核驱动 (imx6ull-drivers)
# gt9147 触摸驱动、mxsfb 显示驱动等在 Linux 开发机的内核源码树中
# (drivers/media/platform/mxc/subdev/ 及对应目录)，独立构建。
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

---

## 快速开始（面向新手）

从零到在板端跑起来，按下面 4 步走。完整验收细节见 [`VERIFICATION.md`](VERIFICATION.md)。

### 第 1 步：准备交叉编译环境（Ubuntu 主机）

```bash
# 1) 交叉编译链（与出厂内核配套的 Linaro 4.9.4）
export PATH=/opt/gcc-linaro-4.9.4-2017.01/bin:$PATH
arm-linux-gnueabihf-gcc --version     # 应显示 4.9.4

# 2) 内核源码目录（用于头文件 linux/fb.h、linux/input.h）
export KDIR=/home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay
ls $KDIR/Makefile                      # 必须存在
```

### 第 2 步：交叉编译

```bash
cd /path/to/project
make lvgl KDIR=$KDIR \
          CROSS_COMPILE=arm-linux-gnueabihf- \
          BUILD_DIR=$(pwd)/build

# 验收检查（<5MB、ARM、可执行）
make lvgl-check
# 产物：build/lvgl/lvgl_gateway（约 1.4MB，静态链接）
```

### 第 3 步：部署到板端（NFS）

```bash
./deploy_lvgl.sh 192.168.1.100 /nfs/rootfs
# 或手动：
cp build/lvgl/lvgl_gateway /nfs/rootfs/usr/bin/
chmod +x /nfs/rootfs/usr/bin/lvgl_gateway
```

### 第 4 步：板端运行

```bash
# 板端登录后先确认设备节点
ls -l /dev/fb0            # mxsfb 帧缓冲
ls -l /dev/input/event1   # GT911 触摸

# 运行（Ctrl+C 退出）
/usr/bin/lvgl_gateway
```

正常会看到：1024x600 中文主界面、4 防区卡片、事件时间轴、底部 布防/撤防/消警/设置 按钮全部可触摸响应。

> **只想要"能跑"的捷径**：执行 `make lvgl` → `make lvgl-check` → `./deploy_lvgl.sh <IP> <NFS>` → 板端跑 `lvgl_gateway` 即可。其余文档都是你想深入时再看的。

---

## 学习路径（推荐阅读顺序）

这个项目刻意把**业务逻辑**、**显示对接**、**界面绘制**拆成了独立模块，正好是一条可循序渐进的学习路线。建议按下面顺序读源码：

| 步骤 | 模块 | 学习重点 |
|------|------|----------|
| ① 入口 | `src/main.c` | 程序启动流程：初始化 → 构建界面 → 主循环。看它是怎么把下面各模块串起来的 |
| ② 移植层 | `src/lvgl_port.c/.h` | **LVGL 怎么跑到 Linux 上**：fbdev 显示对接、evdev 触摸对接、错误码处理 |
| ③ 业务层 | `src/ui/state_machine.c/.h` | **最值得学的设计**：纯 C 逻辑层，**完全不依赖 LVGL**，通过回调通知 UI。学会"逻辑与界面解耦" |
| ④ 界面层 | `src/ui/ui_home.c/.h` | LVGL 控件树的搭建：顶栏/防区卡片/事件时间轴/底栏，设计令牌（颜色/字体/布局宏）统一管理 |
| ⑤ 弹窗 | `src/ui/ui_dialogs.c/.h` | 弹窗系统：遮罩 + 对话框、详情/相册/设置三类弹窗复用同一套框架 |
| ⑥ 构建 | `core/lvgl/Makefile` | 交叉编译、静态链接、`-gc-sections` 裁剪体积、`<5MB` 验收 |
| ⑦ 进阶 | `imx6ull-alientek-emmc.dts`、`VERIFICATION.md` | 设备树、板端验收流程、相机子系统（需 Linux 源码树） |

**给新手的一句话**：先理解 `①→②→③`，你就掌握了"嵌入式 GUI 应用"的主干——**画界面是表象，业务逻辑与显示解耦才是核心**。`③ 状态机` 不碰任何 LVGL 函数，是理解分层设计的绝佳范例。

---

## 模块依赖关系（架构速览）

```
main.c  (程序入口)
  │
  ├── lvgl_port  (移植层: fbdev 显示 + evdev 触摸，依赖 lvgl 库)
  │
  └── ui_home / ui_dialogs  (界面层: 只负责画，不碰业务)
        │                        │
        │  (查询状态 + 调用命令)  │  (回调通知: 状态变化 → 刷新界面)
        ▼                        ▼
      state_machine  (业务层: 纯 C 逻辑，零 LVGL 依赖)
```

设计要点：
- **单向依赖**：`state_machine`（业务）← `ui_*`（界面）← `main`（入口）。业务层永远不被界面层"牵着走"。
- **回调解耦**：界面层通过 `sm_register_event_cb`/`sm_register_state_cb` 订阅状态机变化，状态机不知道也不关心谁在听。
- **可测试**：因为业务层不依赖显示，可以直接写单元测试验证布防/撤防/告警逻辑，无需屏幕。