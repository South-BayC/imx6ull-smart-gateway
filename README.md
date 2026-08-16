# imx6ull-drivers — i.MX6ULL 全外设驱动工程

基于正点原子 I.MX6ULL ALPHA 开发板的嵌入式 Linux 驱动综合项目（项目 B）。

## 项目结构

```
imx6ull-drivers/
├── Makefile               # 顶层编译调度（core + dts）
├── README.md              # 本文件
├── core/                  # 核心驱动模块
│   └── led/               # [P1] LED 字符设备驱动（miscdevice + gpiod）
│       ├── led_drv.c      # 驱动源码
│       ├── Makefile
│       ├── uapi/led_uapi.h  # 用户态接口头文件（ioctl 定义）
│       └── test/led_test.c  # 测试程序
└── dts/                   # 设备树源码
    ├── Makefile
    └── imx6ull-southbay-emmc.dts
```

## 快速使用

```bash
make              # 编译驱动模块 + 设备树
make core         # 只编译核心驱动
make dts          # 只编译设备树
make clean        # 清理全部编译产物
```

## 环境

- 内核：4.1.15（KDIR=/home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay）
- 交叉编译链：arm-linux-gnueabihf-
- 开发模式：NFS rootfs（/home/szh/linux/nfs/rootfs）+ TFTP（/home/szh/linux/tftp/）

## 进度

- [x] P1 LED 字符设备驱动（/dev/led，ioctl 控制）
- [ ] P2 按键中断驱动
- [ ] P3 gpio_event 企业级驱动
- [ ] P4 PWM/I2C/SPI 子系统驱动
- [ ] P5 OV5640 V4L2 subdev 驱动
- [ ] P6 input 子系统 + SUMP 工具链