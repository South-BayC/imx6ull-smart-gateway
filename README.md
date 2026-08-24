# imx6ull-drivers — i.MX6ULL 全外设驱动工程

基于正点原子 I.MX6ULL ALPHA 开发板的嵌入式 Linux 驱动综合项目（项目 B）。

## 项目结构

```
imx6ull-drivers/
├── Makefile               # 顶层编译调度（core + dts + test + protocol）
├── README.md              # 本文件
├── core/                  # 核心驱动模块（每个含五件套：源码/UAPI/Makefile/dts/测试）
│   ├── led/               # [P1] LED 字符设备驱动（miscdevice + gpiod，/dev/led）
│   ├── key_event/         # [P2] 按键中断驱动（阻塞/非阻塞/poll 三种读模式，/dev/key-event）
│   ├── gpio_event_capture/# [P3] 多通道事件采集驱动（kfifo + 时间戳 + 统计 + tracepoint，/dev/edt_capture0）
│   │   ├── uapi/edt_capture.h    # 用户态接口（ABI v2，8 通道，16 字节事件）
│   │   ├── trace/                # 模块内 tracepoint（irq/enqueue/drop/read）
│   │   └── test/edt_capture_test.c  # 语义断言 selftest（47 项）+ 交互读模式
│   ├── pwm_beep/         # [P4] PWM 蜂鸣器驱动（/dev/beep_pwm0）
│   ├── ap3216c/          # [P4] I2C 环境光/接近传感器驱动（/dev/ap3216c0）
│   ├── icm20608/         # [P4] SPI 六轴 IMU 驱动（/dev/icm20608）
│   └── key_input/        # [P6-1] input 子系统按键驱动（/dev/input/eventX）
├── protocol/             # [P6-2] SUMP 逻辑分析仪工具链（PulseView 连接）
│   ├── capture_source.h/.c       # 数据源抽象（edt_capture 后端 + sim 合成源）
│   ├── event_reconstructor.h/.c  # 边沿事件 → 固定采样率网格重建
│   ├── sump_server.c             # SUMP 协议服务器（TCP:9527）
│   ├── sump_selftest.c           # 协议自测客户端（模拟 sigrok 流程）
│   ├── Makefile                  # 交叉编译 + host（PC 自测）双目标
│   └── README.md                 # PulseView 连接指南 + 等效采样率限制声明
├── dts/                   # 设备树源码
│   └── imx6ull-southbay-emmc.dts
├── benchmark/             # [P6] 基准方法学（README.md，结果表待板端实测填写）
├── docs/                  # 每驱动设计文档 + 架构决策
└── build/                 # 编译产物（module/ test/ protocol/ dts/，git 忽略）
```

## 快速使用

```bash
make              # 一键全编：core 驱动 + dts + 测试程序 + protocol 工具链
make core         # 只编译核心驱动模块
make dts          # 只编译设备树
make test         # 编译用户态测试程序
make protocol     # 编译 SUMP 工具链（交叉版）
make -C protocol host  # SUMP 工具链 PC 自测版（--sim 无需板端）
make clean        # 清理全部编译产物
make send         # 部署：.ko/.test/sump 产物 → NFS rootfs，.dtb → TFTP
```

## 环境

- 内核：4.1.15（KDIR=/home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay）
- 交叉编译链：arm-linux-gnueabihf-
- 开发模式：NFS rootfs（/home/szh/linux/nfs/rootfs）+ TFTP（/home/szh/linux/tftp/）
- 更多环境信息见 `项目进度.md`（D:\Desktop\举目长安\嵌入式Linux\）

## 进度

- [x] P1 LED 字符设备驱动（/dev/led，ioctl 控制，验收 4/4）
- [x] P2 按键中断驱动（三种读模式，验收 5/5）
- [x] P3 gpio_event 企业级驱动（多通道/kfifo/统计/tracepoint，验收 7/7 + 内核重编 ftrace）
- [x] P4 PWM/I2C/SPI 子系统驱动（beep_pwm/ap3216c/icm20608，验收 8/8 + git d7b56ed）
- [ ] P5 OV5640 V4L2 subdev 驱动（待摄像头到货）
- [x] P6-1 input 子系统按键驱动（/dev/input/eventX，验收 PASS=10 FAIL=0，2026-08-20）
- [x] P6-2 SUMP 工具链开发完成（protocol/ 四件套 + 文档，2026-08-20；端到端板端验收 PASS=7 FAIL=0，2026-08-21）
- [x] P6-3 SUMP 板端验收（自研 selftest 实证：1M 样本零丢失 + ch0 方波频率精确匹配，PASS=7 FAIL=0；PulseView GUI 实时连接经用户决定取消——libsigrok 官方预编译包未编译串口 TCP 传输，属第三方软件缺陷，源码编译可解锁）
- [x] P6 最终交付（全驱动 insmod/rmmod 演示 + 干净卸载验证 + git 提交推送 origin/master@dd7f018，2026-08-21；基准报告待实测补充）
- [x] P7-1 GT9xx 电容触摸驱动（core/gt9147/ = 官方教程 64.8 指定驱动，板测中断模式验证通过，2026-08-21）
  - 定案：教程 23_multitouch/gt9147.c + 官方验证配置表（7寸 1024x600，modsw=0x0d 下降沿）+ 教程 dts 形态（0x14/0x79/reset-gpios）
  - 板适配补丁：芯片出厂配置区为空 → 检测空白后下载官方配置（含软复位后重读）
  - 板测实证：中断计数 7→515、坐标流平滑、1024x600 全范围；曾误判"INT 无信号"——根因是配置极性错（上升沿 vs 芯片下降沿）+ 缺软复位
- [x] P7-2 智能安防网关 LVGL 设计文档（docs/superpowers/specs/，2026-08-22）
- [x] P7-3 LVGL 安防网关 UI 完整实现（core/lvgl/，2026-08-23）
  - 主界面：状态栏（真实时间/联网/布防指示）/摄像头预览（V4L2 30fps）/1×4 分区卡片/事件时间轴/底栏
  - 分区级独立布防撤防（详情弹窗状态设置按钮）；预警设置（四位置×触发源×数据通道×阈值×比较方式×冷却）
  - 告警确认弹窗（长按 1s 消警防误触 + MUTE 静音）+ 告警分级声光（高/中/低 闪烁鸣叫分档）
  - 抓拍相册：真实画面缩略图 3×2（区域/级别/时间）+ 点击查看大图 + TF 卡 BMP 持久化 + 检测面板 TF 容量
  - 传感器 IIO 迁移（AP3216C/ICM20608 regmap+iio_dev，sysfs 接口）；GT9147 中断风暴自愈（软复位）
  - KEY0 屏幕唤醒（10 分钟无操作息屏）；亮度联动 LCD 背光；定时布防时间段可设自动执行
- [x] P7-4 显示优化（fbdev 双渲染缓冲 + 摄像头 canvas 局部刷新 + YUV 查表转换 30fps）

## 验收

各阶段验收脚本与清单见 `项目进度.md` 对应章节（P3/P4 验收脚本位于对应驱动 test/ 目录）。

## 文档

- `项目进度.md`（D:\Desktop\举目长安\嵌入式Linux\）—— 进度/验收/决策/问题记录
- `docs/` —— 每驱动设计文档 + 架构决策（自研 gpio_event vs input 子系统对比等）
- `protocol/README.md` —— SUMP 工具链使用指南
- `benchmark/README.md` —— 全量基准方法学