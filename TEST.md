# INDAQ Monitor UI — 数据测试指南

## 环境准备

```bash
# 板子上加载驱动
insmod /lib/modules/4.1.15/indaq.ko

# 挂载 debugfs（UI 状态显示需要）
mount -t debugfs none /sys/kernel/debug

# 启动 UI
./indaq_ui
```

---

## 左栏 — 传感器数据

### ALS / PS / IR（环境光 / 接近 / 红外）

**数据流：** AP3216C（I2C）→ 内核驱动 → ring buffer → UI

| 字段 | 含义 | 典型值 |
|------|------|--------|
| ALS | 环境光照度 (lux) | 0.01 × raw，室内 ~50-500 |
| PS | 接近检测 (counts) | 0~1023，靠近物体增大 |
| IR | 红外强度 (counts) | 0~1023 |
| OBJ | 物体是否靠近 | PS>100 → YES |

**测试方法：**

```bash
# 1. 通过 indaq_test 读环缓冲数据
./indaq_test -b -c 20

# 2. 直接读 AP3216C 寄存器（I2C 地址 0x1e）
i2cget -y 0 0x1e 0x0c w    # ALS 数据
i2cget -y 0 0x1e 0x10 w    # PS + IR 数据

# 3. 用手遮挡测试
#   - 遮住传感器（板子上小窗口）→ ALS 下降，PS 上升，OBJ → YES
#   - 移开手掌 → ALS 回升，PS 下降，OBJ → NO
#   - 用手电筒照传感器 → ALS 大幅升高
```

---

### Accelerometer（加速度计 X/Y/Z）

**数据流：** ICM-20608（SPI）→ 内核驱动 → ring buffer → UI

**显示格式：** `X: +0.023(g)[12345]` — 换算值(单位)[原始寄存器值]

| 状态 | AX | AY | AZ |
|------|----|----|----|
| 水平桌面静止（正面朝上）| ~0 g | ~0 g | ~+1.0 g (+2048) |
| 水平桌面静止（正面朝下）| ~0 g | ~0 g | ~-1.0 g (-2048) |
| 竖立（X轴垂直）| ~±1.0 g | ~0 g | ~0 g |
| 竖立（Y轴垂直）| ~0 g | ~±1.0 g | ~0 g |

**测试方法：**

```bash
# 1. 静止观察
#    板子平放桌面 → az ≈ +1.0 g，ax/ay ≈ 0

# 2. 旋转验证
#    板子左边缘立起 → ax → +1.0 g
#    板子下边缘立起 → ay → +1.0 g

# 3. 看原始数据
cat /sys/kernel/debug/indaq/imu_raw
# 输出: accel: 12  -5  2048

./indaq_test -b -c 20
```

---

### Gyroscope（陀螺仪 GX/GY/GZ）

**数据流：** ICM-20608（SPI）→ 内核驱动 → ring buffer → UI

**显示格式：** `GX: 0.7(dps)[12]` — 度/秒[原始值]

**测试方法：**

```bash
# 1. 静止检查
#    所有轴应 < ±1.0 dps，正常 ±0.5 以内

# 2. 旋转测试
#    绕 X 轴快速旋转 → GX 读数跳变（可达 ±2000 dps）
#    绕 Y 轴快速旋转 → GY 读数跳变
#    绕 Z 轴快速旋转 → GZ 读数跳变

# 3. 原始数据
cat /sys/kernel/debug/indaq/imu_raw
# 输出: gyro: -3  2  0

./indaq_test -b -c 20
```

---

### Temperature（温度）

**数据流：** ICM-20608 内置温度传感器 → ring buffer → UI

**显示格式：** `25.0(C)[-521]`

**换算公式：** `(raw - 521) / 340 + 35`

| 状态 | 显示值 |
|------|--------|
| 室温（约 25°C） | raw ≈ -521，显示 ~25.0°C |
| 手握芯片 10 秒 | 上升 1-3°C |
| 吹热气 | 迅速上升 |

**测试方法：**

```bash
cat /sys/kernel/debug/indaq/imu_raw | grep temp
# temp: -521  → 25.0°C

# 用手捏住 ICM-20608 芯片十几秒，数值缓慢上升
```

---

## 右栏 — 系统状态

### Capture（采集状态）

| 状态 | 含义 |
|------|------|
| `RUNNING` | 采集进行中，ring buffer 接收传感器数据 |
| `N/A` | 采集停止或设备未打开 |

**控制方法：**

```bash
# 启动采集
./indaq_test                               # 打开设备 = 启动
./indaq_test -r 100 -b                     # 启动并读数据

# 停止采集（/dev/indaq IOCTL）
cat /sys/kernel/debug/indaq/capture_active  # 1=运行 0=停止

# GPIO 按键切换（需 DT 配置 indaq,trigger-gpios）
# 按一下 GPIO 按键，capture 在 RUNNING ↔ N/A 之间切换

# 停止 indaq_ui 进程 → capture 显示 N/A
# 重新运行 indaq_ui → capture 恢复 RUNNING
```

---

### Rate（采样率）/ Samples（总样本数）

**控制方法：**

```bash
# 改变采样率
./indaq_test -r 500    # 设为 500 Hz
./indaq_test -r 100    # 设为 100 Hz
./indaq_test -r 10     # 设为 10 Hz（低速测试）

# 查看当前值
cat /sys/kernel/debug/indaq/sampling_rate   # 当前采样率
cat /sys/kernel/debug/indaq/total_samples   # 总样本数

# 验证采样率
# 100 Hz 时每秒约 100 个样本
./indaq_test -r 100 -b -c 100 | wc -l       # 应约 100 行
time ./indaq_test -r 100 -b -c 100          # 应约 1 秒
```

---

### Ring Buffer（环形缓冲）

| 字段 | 含义 | 测试方法 |
|------|------|----------|
| Count | 当前缓冲内样本数 | `cat /sys/kernel/debug/indaq/ringbuf_count` |
| Capacity | 缓冲容量 | `cat /sys/kernel/debug/indaq/ringbuf_capacity` |
| Usage | 使用率 % | count / capacity × 100 |

**测试方法：**

```bash
# 查看完整环缓冲状态
cat /sys/kernel/debug/indaq/ringbuf
# 输出:
#   capacity: 4096
#   count:    128
#   head:     128
#   tail:     0
#   usage:    3%

# 启动采集后 count 持续增长
# 停止采集后 count 固定不变
# 重新启动采集 count 从 0 重新计数

# 从 UI 上观察：Usage 百分比随采集进行逐渐升高
```

---

### Calibration（校准状态）

| 字段 | 显示 | 含义 |
|------|------|------|
| Gyro | CALIBRATED / not cal | 陀螺零偏是否已校准 |
| Off | x/y/z | 三个轴的零偏值（LSB） |
| Accel | CALIBRATED / not cal | 加速度 scale 是否已校准 |

**校准流程：**

```bash
# === 步骤1: 陀螺零偏校准 ===
# 前提：板子静止放置在水平桌面
echo 1 > /sys/kernel/debug/indaq/calib_gyro
# 采集 100 个样本取平均（约 1-2 秒）
# 校准后 Gyro → CALIBRATED，Off 显示偏移值（如 -3/2/0）

# === 步骤2: 加速度 scale 校准 ===
# 前提：板子水平静止放置
echo 1 > /sys/kernel/debug/indaq/calib_accel
# 以 Z 轴实测值（期望 ~2048）计算 scale 修正因子
# 校准后 Accel → CALIBRATED

# === 查看校准参数 ===
cat /sys/kernel/debug/indaq/calib_params
# 输出:
#   calibrated:            yes
#   gyro_samples:          100
#   gyro_offset.x:         -3
#   gyro_offset.y:         2
#   gyro_offset.z:         0
#   accel_calibrated:      yes
#   accel_z_ref:           2048
#   accel_scale_correction: 2048/2045

# 观察 UI：Cal 行从 "not cal" 变为 "CALIBRATED"
```

---

### PM（电源管理）

| 状态 | 含义 |
|------|------|
| `active` | 正常运行 |
| `SUSPENDED` | 系统已挂起 |
| `N/A` | debugfs 不可读 |

**测试方法：**

```bash
# 查看当前状态
cat /sys/kernel/debug/indaq/stats | grep pm_suspended
# pm_suspended: no  → UI 显示 active
# pm_suspended: yes → UI 显示 SUSPENDED

# 触发挂起（需要 root）
echo mem > /sys/power/state
# 系统挂起 → UI 显示 SUSPENDED（如果背光还亮着）
# 按开发板按键唤醒 → 恢复显示 active
```

---

### GPIO（GPIO 触发采集）

| 状态 | 含义 |
|------|------|
| `active` | GPIO 触发已注册（DT 配置有效） |
| `N/A` | 未配置 GPIO 触发 |
| `--` | 读不到数据 |

**测试方法：**

```bash
cat /sys/kernel/debug/indaq/stats | grep gpio_trigger
# gpio_trigger: yes → active
# gpio_trigger: no  → N/A

# 如果有硬件按键（DT 中 indaq,trigger-gpios 已配置）：
#   按下按键 → capture 运行状态切换
#   观察 UI：Capture 行从 RUNNING ↔ N/A 变化

# 如果没有硬件按键，UI 始终显示 N/A（正常）
```

---

### Tap（敲击检测）

| 状态 | 含义 |
|------|------|
| 数字 | 累计敲击计数 |
| N/A | input 子系统未初始化 |

**测试方法：**

```bash
# 查看当前计数
cat /sys/kernel/debug/indaq/stats | grep tap_count

# 敲击测试（用手指在板子 ICM-20608 附近轻敲）：
#   快速敲 3 下，每次间隔 < 200ms
#   UI 上 Tap 计数会递增

# 触发三连击后，还会产生 BTN_TL 按键事件
evtest /dev/input/event0
# 快速敲击三下，应看到 type 1 (EV_KEY), code 274 (BTN_TL)

# 注意：敲击力度要足够大（Z 轴变化 >500 LSB ≈ 0.25g）
#        敲击时间间隔要短（<200ms）
```

---

### IIO（IIO 子系统）

| 状态 | 含义 |
|------|------|
| `registered` | IIO 设备注册成功 |
| `N/A` | 未找到 IIO 设备 |

**测试方法：**

```bash
# 查看 IIO 设备列表
ls /sys/bus/iio/devices/
# 应有 iio:device0

# 确认设备名称
cat /sys/bus/iio/devices/iio:device0/name
# 应输出 "indaq-imu" 或含 "indaq"

# 读取 IIO 通道数据
cat /sys/bus/iio/devices/iio:device0/in_accel_x_raw
cat /sys/bus/iio/devices/iio:device0/in_accel_y_raw
cat /sys/bus/iio/devices/iio:device0/in_accel_z_raw
cat /sys/bus/iio/devices/iio:device0/in_gyro_x_raw
cat /sys/bus/iio/devices/iio:device0/in_gyro_y_raw
cat /sys/bus/iio/devices/iio:device0/in_gyro_z_raw
cat /sys/bus/iio/devices/iio:device0/in_temp_raw
```

---

### DebugFS（调试文件系统）

| 状态 | 含义 |
|------|------|
| `OK` | debugfs 已挂载，stats 文件可读 |
| `N/A` | debugfs 未挂载 |

**测试方法：**

```bash
# 确保 debugfs 已挂载
mount -t debugfs none /sys/kernel/debug

# 验证
cat /sys/kernel/debug/indaq/stats > /dev/null 2>&1 && echo OK

# 模拟 N/A：卸载 debugfs
umount /sys/kernel/debug
# UI 显示 DebugFS: N/A

# 恢复
mount -t debugfs none /sys/kernel/debug
# UI 恢复 DebugFS: OK
```

---

### Health（健康状态）

| 状态 | 含义 |
|------|------|
| `IMU OK` | ICM-20608 初始化完成，可以读取 |
| `N/A` | IMU 未就绪 |

**测试方法：**

```bash
cat /sys/kernel/debug/indaq/stats | grep imu_ready
# imu_ready: yes → IMU OK
# imu_ready: no  → N/A

# 模拟 N/A：卸载驱动
rmmod indaq
# UI 显示 Health: N/A

# 恢复
insmod /lib/modules/4.1.15/indaq.ko
# UI 恢复 Health: IMU OK
```

---

## 快速状态切换一览

| UI 字段 | 切换方式 | 预期变化 |
|---------|----------|----------|
| ALS/PS/IR | 手遮挡/手电筒照射传感器 | 数值实时变化，OBJ 在 YES/NO 切换 |
| Accel X/Y/Z | 旋转/翻转板子 | 重力分量在各轴之间转移 |
| Gyro GX/GY/GZ | 快速旋转板子 | 对应轴 dps 跳变 |
| 温度 | 手握芯片 | 缓慢上升 1-3°C |
| Capture | `ioctl` 或 GPIO 按键 | RUNNING ↔ N/A |
| Rate | `./indaq_test -r <Hz>` | 改变采样率 |
| Ringbuf Usage | 启动/停止采集 | 采集时增长，停止时固定 |
| Gyro Cal | `echo 1 > calib_gyro` | not cal → CALIBRATED |
| Accel Cal | `echo 1 > calib_accel` | not cal → CALIBRATED |
| PM | `echo mem > /sys/power/state` | active → SUSPENDED |
| GPIO | 查看 DT 配置 | active 或 N/A |
| Tap | 快速敲击板子 3 次 | 计数递增 |
| IIO | 查看 iio:device0 是否存在 | registered 或 N/A |
| DebugFS | `mount/umount debugfs` | OK ↔ N/A |
| Health | `rmmod/insmod` 驱动 | IMU OK ↔ N/A |
