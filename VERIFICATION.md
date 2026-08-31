# LVGL 智能安防网关 - 板端验收文档

> **项目**：i.MX6ULL 智能安防网关 (P7-2 最终任务)
> **目标**：交叉编译产物 <5MB，板端 NFS 部署，全链路验收
> **硬件**：1024x600 LCD (mxsfb fbdev) + GT911 触摸 (/dev/input/event1)
> **日期**：2026-08-22

---

## 1. 编译环境准备

### 1.1 交叉编译链
```bash
# 推荐：Linaro 4.9.4 (与出厂 4.9.88 内核配套)
export PATH=/opt/gcc-linaro-4.9.4-2017.01/bin:$PATH
arm-linux-gnueabihf-gcc --version
# 输出应包含: arm-linux-gnueabihf-gcc (Linaro GCC 4.9-2017.01) 4.9.4
```

### 1.2 内核源码
```bash
# 内核源码目录 (用于头文件: linux/fb.h, linux/input.h 等)
export KDIR=/home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay
ls $KDIR/Makefile  # 必须存在
```

### 1.3 NFS / TFTP 服务 (Ubuntu 主机)
```bash
# NFS 配置 (/etc/exports)
/nfs/rootfs *(rw,sync,no_root_squash,no_subtree_check)

# 重启 NFS 服务
sudo systemctl restart nfs-kernel-server
showmount -e localhost  # 验证导出

# TFTP 配置 (/etc/default/tftp-hpa)
TFTP_DIRECTORY="/tftpboot"
sudo systemctl restart tftp-hpa
```

---

## 2. 交叉编译

### 2.1 编译命令
```bash
cd /path/to/project

# 方式 1: 顶层 Makefile (推荐)
make lvgl KDIR=/home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay \
          CROSS_COMPILE=arm-linux-gnueabihf- \
          BUILD_DIR=$(pwd)/build

# 方式 2: 直接进入子目录
make -C core/lvgl KDIR=/home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay \
                  CROSS_COMPILE=arm-linux-gnueabihf- \
                  BUILD_DIR=$(pwd)/build
```

### 2.2 预期输出
```
==========================================
Building LVGL Gateway...
==========================================
  CC      lvgl/src/core/lv_global.c
  CC      lvgl/src/core/lv_obj.c
  ...
  CC      src/main.c
  CC      src/lvgl_port.c
  CC      src/ui/ui_home.c
  CC      src/ui/ui_dialogs.c
  CC      src/ui/state_machine.c
  CC      src/font/lv_font_SHSC_16.c
  CC      src/font/lv_font_SHSC_20.c
  LD      /path/to/project/build/lvgl/lvgl_gateway
  STRIP   /path/to/project/build/lvgl/lvgl_gateway

==========================================
Build successful: /path/to/project/build/lvgl/lvgl_gateway
   text    data     bss     dec     hex filename
  1245184   123456   56789  1425429  15c115 /path/to/project/build/lvgl/lvgl_gateway
==========================================
```

### 2.3 产物验收检查
```bash
make lvgl-check
# 或手动检查：
ls -lh build/lvgl/lvgl_gateway
# -rwxr-xr-x 1 user user 1.4M Aug 22 10:00 build/lvgl/lvgl_gateway  (< 5MB ✓)

file build/lvgl/lvgl_gateway
# build/lvgl/lvgl_gateway: ELF 32-bit LSB executable, ARM, EABI5 version 1 (SYSV), statically linked, stripped (< 5MB ✓, ARM ✓, 可执行 ✓)

arm-linux-gnueabihf-readelf -d build/lvgl/lvgl_gateway | grep NEEDED
# 静态链接应无动态依赖 (或仅 libc/pthread/rt/m)
```

---

## 3. 板端部署

### 3.1 NFS Rootfs 准备
```bash
# 确保 NFS rootfs 已挂载且可写
mount | grep nfs
# 192.168.1.100:/nfs/rootfs on /mnt/nfs type nfs (rw,relatime,...)

# 或使用部署脚本 (自动复制 + 生成验证脚本)
./deploy_lvgl.sh 192.168.1.100 /nfs/rootfs
```

### 3.2 手动部署步骤
```bash
# 1. 复制可执行文件
cp build/lvgl/lvgl_gateway /nfs/rootfs/usr/bin/
chmod +x /nfs/rootfs/usr/bin/lvgl_gateway

# 2. 验证复制成功
ls -lh /nfs/rootfs/usr/bin/lvgl_gateway
```

### 3.3 板端网络配置
```bash
# 板端设置静态 IP (与 NFS 服务器同网段)
# /etc/network/interfaces
auto eth0
iface eth0 inet static
    address 192.168.1.101
    netmask 255.255.255.0
    gateway 192.168.1.1

# 重启网络
/etc/init.d/networking restart
ping 192.168.1.100  # 测试连通性
```

---

## 4. 板端验收步骤

### 4.1 设备节点检查
```bash
# 登录板端
ssh root@192.168.1.101

# 检查帧缓冲设备
ls -l /dev/fb0
# crw-rw---- 1 root video 29, 0 Aug 22 10:00 /dev/fb0

cat /sys/class/graphics/fb0/virtual_size
# 1024,600  (必须匹配 1024x600)

cat /sys/class/graphics/fb0/bits_per_pixel
# 16  (必须匹配 RGB565)

# 检查触摸设备
ls -l /dev/input/event1
# crw-rw---- 1 root input 13, 65 Aug 22 10:00 /dev/input/event1

cat /proc/bus/input/devices
# 应包含 GT911 设备信息，Handlers=event1

# 权限修正 (如需要)
chmod 666 /dev/fb0 /dev/input/event1
```

### 4.2 运行主程序
```bash
# 直接运行 (前台，Ctrl+C 退出)
/usr/bin/lvgl_gateway

# 预期输出：
# ========================================
#   智能安防网关 - LVGL Gateway v1.0
#   i.MX6ULL + mxsfb fbdev + GT911
#   1024x600 16bpp RGB565
# ========================================
# [main] INFO: LVGL 移植层初始化成功
# [main] INFO: 状态机初始化完成
# [main] INFO: 主界面构建完成
# [main] INFO: 进入主循环 (Ctrl+C 退出)
# [main] INFO: 目标帧率 >= 25 FPS (周期 <= 40ms)
```

### 4.3 界面验收清单

| 检查项 | 预期结果 | 验收标准 |
|--------|----------|----------|
| **分辨率** | 1024x600 全屏显示 | 无黑边、无拉伸、像素对齐 |
| **色深** | 16bpp RGB565 | 颜色正常，无色块、无花屏 |
| **中文显示** | 所有中文正常渲染 | 标题、按钮、状态文字均为中文，无乱码/方块字 |
| **顶栏** | 系统名·时钟·联网/布防状态 | 时钟实时更新，状态图标显示 |
| **摄像头预览区** | 4通道切换按钮 (前门/后门/仓库/周界) | 点击切换，标题联动，十字线居中 |
| **防区卡片** | 2x2 网格 (前门/后门/窗户/仓库) | 状态点颜色正确，点击弹出详情弹窗 |
| **事件时间轴** | 右侧滚动列表，最新在上 | 新事件自动添加，颜色区分类型 |
| **底栏按钮** | **5 类按钮**：布防/撤防/消警/设置/分区卡片 | 全部可点击，触摸响应灵敏 |
| **布防按钮** | 青色边框，点击后防区变绿/呼吸 | 状态机联动，顶栏显示"已布防" |
| **撤防按钮** | 灰色边框，点击后防区变青色 | 状态机联动，顶栏显示"已撤防" |
| **消警按钮** | 琥珀色边框，点击后告警消除 | 红色闪烁停止，恢复布防/在线态 |
| **设置按钮** | 灰色边框，点击弹出设置弹窗 | 亮度/音量滑块、网络状态、关于信息 |
| **分区卡片** | 点击弹出详情弹窗 | 显示 ID、名称、状态、传感器、IP、固件、RSSI |
| **动画效果** | 布防绿光呼吸、告警红色闪烁 | 50ms 定时器驱动，流畅无卡顿 |
| **抓拍提示** | 底部绿色 Toast 1.5s 自动消失 | 点击摄像头区域触发 (演示) |

### 4.4 触摸测试 (5 类按钮 100% 可用)

```bash
# 手动测试每个按钮，记录结果
# 1. 布防按钮 (底栏左1) - 点击 → 所有在线防区变绿/呼吸，顶栏"已布防"
# 2. 撤防按钮 (底栏左2) - 点击 → 所有防区变青色，顶栏"已撤防"
# 3. 消警按钮 (底栏左3) - 布防+触发告警后点击 → 红色闪烁停止
# 4. 设置按钮 (底栏左4) - 点击 → 弹出设置弹窗，滑块可拖动
# 5. 分区卡片 (左下 2x2) - 点击任一 → 弹出详情弹窗，信息完整

# 触摸坐标校验 (可选)
cat /sys/class/input/event1/device/name
# Goodix Capacitive TouchScreen (GT911)

# 触摸事件监测 (调试用)
evtest /dev/input/event1
# 触摸屏幕应输出 ABS_X, ABS_Y, BTN_TOUCH 事件
```

### 4.5 帧率验证 (≥ 25 FPS)

```bash
# 方法 1: 目测 (主观)
# - 界面切换、动画、滚动无明显卡顿
# - 50ms 定时器驱动的呼吸/闪烁动画流畅

# 方法 2: LVGL 内置性能监控 (需在 lv_conf.h 开启)
# #define LV_USE_PERF_MONITOR 1
# 重新编译后，界面右上角显示 FPS

# 方法 3: 简单计时测试
# 在主循环添加帧计数，每秒打印一次
# 预期: 25-60 FPS (受限于 fbdev 刷新率和 SPI/I2C 触摸上报率)

# 硬件限制说明:
# - mxsfb fbdev: 通常 60Hz 刷新
# - GT911 I2C: ~100Hz 触摸上报
# - LVGL 部分刷新 (60行/帧): 约 120KB/帧传输
# - 理论上限: 60 FPS，实际 25-40 FPS 为合格
```

---

## 5. 自启动配置

### 5.1 修改 /etc/init.d/rcS
```bash
# 在板端执行
cat >> /etc/init.d/rcS << 'EOF'

# LVGL 智能安防网关自启动
# 等待设备节点就绪
for i in $(seq 1 10); do
    [ -c /dev/fb0 ] && [ -c /dev/input/event1 ] && break
    sleep 1
done

# 启动 LVGL 网关 (后台运行)
/usr/bin/lvgl_gateway &
echo "LVGL Gateway started in background (PID: $!)"
EOF

chmod +x /etc/init.d/rcS
```

### 5.2 验证自启动
```bash
# 重启板端
reboot

# 启动后检查进程
ps | grep lvgl_gateway
# 应显示后台运行进程

# 检查屏幕显示
# 应自动显示主界面，无需手动运行
```

---

## 6. 故障排查

### 6.1 常见问题

| 现象 | 可能原因 | 解决方案 |
|------|----------|----------|
| `/dev/fb0` 不存在 | mxsfb 驱动未加载 | `modprobe mxsfb` 或检查设备树 `status="okay"` |
| `/dev/input/event1` 不存在 | GT911 驱动未加载 | `modprobe gt9147` 检查 I2C 地址 0x5D/0x14 |
| 启动报 `lv_linux_fbdev_set_file 失败` | 权限不足或设备节点错误 | `chmod 666 /dev/fb0` 确认路径正确 |
| 启动报 `evdev 触摸初始化失败` | 触摸设备路径错误 | 确认 `/dev/input/event1` 是 GT911，非其他设备 |
| 界面花屏/颜色异常 | 色深不匹配 | 确认 `LV_COLOR_DEPTH=16` 且 fbdev 报告 16bpp |
| 中文显示方块字 | 字体未编入/路径错误 | 确认 `lv_font_SHSC_16/20.c` 参与编译，`lv_conf.h` 声明正确 |
| 触摸坐标偏移/反向 | 校准值错误 | GT911 驱动 ioctl 自动读取 min/max，检查驱动上报 |
| 程序崩溃/Segfault | 栈溢出/空指针 | 增加栈大小 `ulimit -s 1024`，检查日志定位 |
| 帧率过低 (<20 FPS) | 刷新模式/缓冲区问题 | 确认 `LV_DRAW_BUF_PARTIAL_MAX_ROWS=60`，检查 fbdev 性能 |

### 6.2 调试技巧
```bash
# 启用 LVGL 日志 (lv_conf.h: LV_USE_LOG=1, LV_LOG_LEVEL=LV_LOG_LEVEL_INFO)
# 程序会输出详细初始化日志

# 使用 strace 追踪系统调用
strace -f /usr/bin/lvgl_gateway 2>&1 | head -50

# 检查内存使用
cat /proc/$(pidof lvgl_gateway)/status | grep VmRSS

# 查看内核日志
dmesg | grep -E "mxsfb|gt9147|fbdev|input"
```

---

## 7. 验收签收

| 验收项 | 标准 | 实测结果 | 通过/失败 | 备注 |
|--------|------|----------|-----------|------|
| 交叉编译产物大小 | < 5 MB | ______ KB | ☐ / ☐ | |
| 产物架构 | ARM 32-bit (EABI5) | ______ | ☐ / ☐ | |
| 产物类型 | 静态链接可执行文件 | ______ | ☐ / ☐ | |
| /dev/fb0 存在且可读写 | 是 | 是/否 | ☐ / ☐ | |
| /dev/input/event1 存在且可读 | 是 | 是/否 | ☐ / ☐ | |
| 1024x600 中文界面显示 | 正常无乱码 | 是/否 | ☐ / ☐ | |
| 5 类按钮触摸响应 | 100% 可用 | 是/否 | ☐ / ☐ | |
| 帧率 | ≥ 25 FPS | ______ FPS | ☐ / ☐ | |
| 自启动配置 | rcS 追加生效 | 是/否 | ☐ / ☐ | |
| 运行稳定性 | 30 分钟无崩溃 | 是/否 | ☐ / ☐ | |

**验收人**：_______________  **日期**：_______________  **签名**：_______________

---

## 8. 附录：关键文件清单

```
project/
├── Make                          # 顶层 Makefile (含 lvgl 目标)
├── deploy_lvgl.sh               # 部署脚本
├── core/lvgl/
│   ├── Makefile                 # LVGL 交叉编译 Makefile
│   ├── lv_conf.h                # LVGL v9.2.2 配置 (fbdev+evdev, 16bpp, 部分刷新)
│   ├── lvgl/                    # LVGL v9.2.2 源码 (git tag v9.2.2)
│   └── src/
│       ├── main.c               # 主程序入口
│       ├── lvgl_port.h/.c       # 移植层 (fbdev+evdev 对接)
│       ├── font/
│       │   ├── lv_font_SHSC_16.c  # 中文字体 16px
│       │   └── lv_font_SHSC_20.c  # 中文字体 20px
│       └── ui/
│           ├── ui_home.h/.c     # 主界面 (顶栏/预览/防区/事件/底栏)
│           ├── ui_dialogs.h/.c  # 弹窗系统 (详情/相册/设置)
│           └── state_machine.h/.c # 状态机 (4防区/布防/告警/事件)
├── build/lvgl/lvgl_gateway      # 编译产物 (<5MB)
└── /nfs/rootfs/usr/bin/
    ├── lvgl_gateway             # 部署后可执行文件
    └── verify_lvgl.sh           # 板端验证脚本
```

---

**文档版本**：v1.0  
**维护者**：显示驱动专家  
**更新日期**：2026-08-22