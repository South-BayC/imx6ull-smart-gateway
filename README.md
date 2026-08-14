# INDAQ — i.MX6ULL 多传感器融合驱动子系统

基于正点原子 I.MX6U 开发板的嵌入式 Linux 驱动综合项目。**ICM-20608 6轴 IMU（SPI） + AP3216C 环境光/接近传感器（I2C）**，通过无锁环形缓冲区实现零拷贝数据交付，并部署 LVGL 图形界面实现实时监控。

## 项目结构

```
imx6ul-indaq/
├── Makefile               # 顶层编译调度
├── README.md              # 本文件
├── LEARN.md               # 学习指南（架构、数据流、功能框架详解）
│
├── driver/                # 内核模块源码
│   ├── Makefile
│   ├── indaq_core.c/h     # [核心] Platform 驱动，字符设备 /dev/indaq
│   ├── indaq_i2csens.c/h  # [传感器] AP3216C I2C 驱动（Regmap）
│   ├── indaq_imu.c/h      # [传感器] ICM-20608 IMU SPI 驱动
│   ├── indaq_ringbuf.c/h  # [数据] 无锁环形缓冲区（SPSC）
│   ├── indaq_calib.c/h    # [校准] IMU 零偏标定 + scale 校正
│   ├── indaq_input.c/h    # [事件] Input 子系统（加速度 + tap 检测）
│   ├── indaq_gpioctrl.c/h # [触发] GPIO 外部触发采集
│   ├── indaq_iio.c/h      # [IIO] IIO 子系统接入（6轴 + 温度）
│   ├── indaq_debug.c/h    # [调试] DebugFS 寄存器透读透写
│   ├── indaq_pm.c/h       # [电源] Runtime PM + regcache
│   └── indaq_selftest.c   # 自测模块（独立 .ko，非默认编译）
│
├── app/                   # 用户态程序
│   ├── Makefile
│   ├── libdaq.h/c         # C API 用户空间库
│   ├── indaq_test.c       # 传感器数据采集测试
│   ├── test_libdaq.c      # libdaq 链接测试
│   ├── indaq_test.py      # Python 绑定（ctypes）
│   └── indaq_ui/          # LVGL 图形界面监控
│       ├── main.c         # 主程序：显示传感器数据 + 系统状态
│       ├── Makefile       # 交叉编译 Makefile
│       ├── lv_conf.h      # LVGL 配置
│       ├── lv_drv_conf.h  # LVGL 驱动配置（fbdev + evdev）
│       ├── S99indaq       # 开机自启动脚本（参考）
│       ├── TEST.md        # 测试指南
│       ├── lvgl/          # LVGL 图形库源码（v8.3）
│       └── lv_drivers/    # LVGL 显示/输入驱动（仅 fbdev + evdev）
│
├── dts/                   # 设备树源码
│   ├── Makefile
│   └── imx6ull-southbay-indaq.dts
│
└── .omo/plans/            # 开发规划文档（可忽略）
```

## 快速使用

```bash
make              # 编译驱动模块 + 用户程序
make driver       # 只编译驱动
make app          # 只编译应用程序
make bspcp        # 将 .ko 复制到 NFS 根文件系统
make dts          # 单独编译设备树 .dtb
make clean        # 清理全部编译产物

# 板子上加载
insmod /lib/modules/4.1.15/indaq.ko
./indaq_ui        # 启动 LVGL 图形界面
```

## 功能特性

| 特性 | 框架/方式 | 用途 |
|------|----------|------|
| ICM-20608 6轴 IMU | SPI + 延迟工作队列 | 100 Hz 连续采集加速度、角速度、温度 |
| AP3216C 环境光/接近 | I2C + Regmap | ALS/PS/IR 三通道数据 |
| 无锁环形缓冲区 | SPSC + memory barrier | 零拷贝数据交付 |
| IIO 框架 | read_raw | 精确传感器读数 |
| Input 子系统 | EV_ABS + EV_KEY | 运动事件 + tap 检测 |
| IMU 校准 | DebugFS 触发 | 陀螺零偏标定、scale 校正 |
| DebugFS | 寄存器透读透写 | 运行态硬件调试 |
| Runtime PM | autosuspend + regcache | 低功耗管理 |
| LVGL 图形界面 | fbdev + evdev | 传感器数据 + 系统状态实时监控 |

## 进度

### ✅ Phase 1–6 — 基础骨架和周边功能
- [x] Platform 驱动，字符设备，DTS
- [x] AP3216C I2C 驱动 + Regmap 缓存
- [x] `/dev/indaq` 数据连接 + 测试程序
- [x] 无锁环形缓冲区，IOCTL 启停控制
- [x] IR 数据 + 可配置采样率
- [x] GPIO 触发 / IIO / DebugFS / PM

### ✅ Phase 7 — ICM-20608 IMU 驱动
- [x] SPI 驱动，WHO_AM_I 校验 0xAE
- [x] 100 Hz 延迟工作队列连续采集
- [x] 环形缓冲区 IMU 数据推送
- [x] 用户态测试验证
- [x] IIO 集成（IMU 6轴 IIO 通道）
- [x] IMU 校准子系统（零偏/scale）
- [x] DebugFS 寄存器透读透写

### ✅ Phase 8 — 工程化增强
- [x] Input 子系统 + tap 检测
- [x] libdaq C API + Python 绑定
- [x] LVGL 图形界面监控
- [x] 开机自启动

### ⚠️ 未完成
- [ ] IIO Trigger Buffer（hrtimer 批量采集）— 板端内核缺少 IIO buffer EXPORT_SYMBOL，无法实现
- [ ] KUnit 测试 — 嵌入式项目，板端测试已覆盖

> 详细架构说明、数据流、功能框架见 **[LEARN.md](./LEARN.md)**
