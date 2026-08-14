# INDAQ 学习指南——全工作流函数级详解

> i.MX6ULL 多传感器融合驱动子系统 — 从 DTS 编译到 LVGL 界面的每一个函数调用与数据流向

---

## 目录

1. [项目全景与文件依赖关系](#1-项目全景与文件依赖关系)
2. [编译构建流程](#2-编译构建流程)
3. [系统启动：设备树匹配 → 模块加载 → probe 调用链](#3-系统启动设备树匹配--模块加载--probe-调用链)
4. [内核模块初始化：module_init 调用链](#4-内核模块初始化module_init-调用链)
5. [数据采集流程：传感器 → 环形缓冲区](#5-数据采集流程传感器--环形缓冲区)
6. [数据输出流程：环形缓冲区 → 用户态](#6-数据输出流程环形缓冲区--用户态)
7. [IOCTL 控制接口](#7-ioctl-控制接口)
8. [IIO 子系统：sysfs 读数路径](#8-iio-子系统sysfs-读数路径)
9. [Input 子系统：加速度上报与 Tap 检测](#9-input-子系统加速度上报与-tap-检测)
10. [GPIO 触发控制](#10-gpio-触发控制)
11. [IMU 校准子系统](#11-imu-校准子系统)
12. [Power Management 电源管理](#12-power-management-电源管理)
13. [DebugFS 调试接口](#13-debugfs-调试接口)
14. [libdaq 用户态 C API](#14-libdaq-用户态-c-api)
15. [indaq_test CLI 测试程序](#15-indaq_test-cli-测试程序)
16. [Python 绑定 indaq_test.py](#16-python-绑定-indaq_testpy)
17. [LVGL 图形界面 indaq_ui](#17-lvgl-图形界面-indaq_ui)
18. [自测试模块 indaq_selftest](#18-自测试模块-indaq_selftest)
19. [完整调用链汇总](#19-完整调用链汇总)

---

## 1. 项目全景与文件依赖关系

### 1.1 内核模块文件依赖图

```
                    ┌──────────────────┐
                    │  indaq_core.h    │ (核心头文件: 数据结构, IOCTL 定义, 子设备接口声明)
                    └────────┬─────────┘
          ┌──────────────────┼──────────────────┐
          ▼                  ▼                   ▼
  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐
  │indaq_ringbuf │  │indaq_i2csens │  │  indaq_imu       │
  │.h/.c         │  │.h/.c         │  │  .h/.c           │
  │环形缓冲区     │  │AP3216C I2C   │  │ICM-20608 SPI    │
  │SPSC spinlock │  │regmap RBTREE │  │burst read 14B   │
  └──────────────┘  └──────┬───────┘  └────────┬─────────┘
                           │                   │
          ┌────────────────┼───────────────────┼──────────┐
          ▼                ▼                   ▼          ▼
  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
  │indaq_input   │  │indaq_calib   │  │  indaq_iio   │  │indaq_debug   │
  │.h/.c         │  │.h/.c         │  │  .h/.c       │  │.h/.c         │
  │evdev tap检测 │  │IMU 校准      │  │IIO sysfs接口 │  │DebugFS接口   │
  └──────────────┘  └──────────────┘  └──────────────┘  └──────┬───────┘
                                                               │
                                                      ┌────────▼───────┐
                                                      │  indaq_pm     │
                                                      │  .h/.c        │
                                                      │  电源管理     │
                                                      └────────────────┘
```

### 1.2 所有 .c 文件间函数调用关系

| 源文件 | 调用的外部函数 | 被谁调用 |
|--------|---------------|----------|
| `indaq_core.c` | `indaq_ringbuf_create/destroy/push/read/reset` | 用户态 syscall (read/ioctl), I2C/IMU worker |
| | `indaq_i2csens_init/exit`, `indaq_imu_init/exit` | probe/remove |
| | `indaq_gpioctrl_init/exit`, `indaq_iio_init/exit` | probe/remove |
| | `indaq_debug_init/exit`, `indaq_pm_init/exit` | probe/remove |
| | `indaq_calib_init/exit`, `indaq_input_init/exit` | probe/remove |
| | `indaq_register_i2c_driver/imu_driver` | module_init |
| `indaq_i2csens.c` | `indaq_push_sample()` → core | worker 定时推送 |
| | `indaq_push_imu_sample()` → core | (间接, IMU 用) |
| `indaq_imu.c` | `indaq_push_imu_sample()` → core | worker 定时推送 |
| | `icm20608_read_regs/read_reg/write_reg` → SPI | 自身 init/read |
| `indaq_ringbuf.c` | 无（纯算法） | core, input, selftest |
| `indaq_input.c` | `icm20608_get_imu()` → imu | worker 读取 IMU 缓存 |
| `indaq_iio.c` | `indaq_i2csens_get_data()` → i2csens | read_raw 回调 |
| | `icm20608_get_imu()` → imu | read_raw 回调 |
| `indaq_calib.c` | `icm20608_get_imu()` → imu | 校准操作 |
| `indaq_debug.c` | `icm20608_read_reg/write_reg` → imu | reg_peek/poke |
| | `icm20608_get_imu()` → imu | reg_peek/poke, stats |
| | `indaq_pm_is_suspended()` → pm | stats |
| | `indaq_input_get_tap_count()` → input | stats |
| `indaq_gpioctrl.c` | `indaq_ringbuf_reset()` → ringbuf | gpio 中断 |
| `indaq_pm.c` | 无（纯 notifier） | core, debug |
| `indaq_selftest.c` | `indaq_ringbuf_create/destroy/push/read/reset` | 自身 init |

---

## 2. 编译构建流程

### 2.1 顶层 Makefile 目标

```
make            → driver + app + dts + ui   (全部构建)
make driver     → 编译 indaq.ko             (内核模块)
make app        → 编译 indaq_test, libdaq   (用户态程序)
make dts        → 编译 .dtb                 (设备树)
make ui         → 编译 indaq_ui             (LVGL 图形界面)
make bspcp      → 复制 .ko 到 NFS 根文件系统
make install    → bspcp + appcp + uicp      (全部复制)
```

### 2.2 driver/Makefile 详细流程

```makefile
# 变量:
KERNELDIR  = /home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay
CROSS_COMPILE = arm-linux-gnueabihf-
ARCH = arm

# 目标文件:
indaq-objs := indaq_core.o indaq_i2csens.o indaq_imu.o indaq_ringbuf.o \
              indaq_gpioctrl.o indaq_calib.o indaq_input.o \
              indaq_debug.o indaq_pm.o
# 条件编译:
ifdef CONFIG_IIO
indaq-objs += indaq_iio.o
endif

# 编译命令:
$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) ARCH=arm \
        CROSS_COMPILE=$(CROSS_COMPILE) modules
# 输出: indaq.ko → build/driver/indaq.ko
```

**编译流程：**
1. `make` 进入 `driver/` 目录
2. 调用内核 Kbuild 系统，`KERNELDIR` 指向板端内核源码
3. Kbuild 逐个编译 `indaq-objs` 中的 `.c` → `.o`
4. 链接所有 `.o` 为 `indaq.ko`
5. 复制到 `build/driver/` 目录，清理中间文件

### 2.3 app/Makefile

arm-linux-gnueabihf-gcc 交叉编译：
- `libdaq.c` → `libdaq.o`（可打包为 `libdaq.so`）
- `indaq_test.c` → `indaq_test`（链接 `libdaq.o` 或直接使用原始 syscall）
- `test_libdaq.c` → `test_libdaq`

### 2.4 dts/Makefile

`dtc` 编译 `imx6ull-southbay-indaq.dts` → `imx6ull-southbay-indaq.dtb`

### 2.5 app/indaq_ui/Makefile

arm-linux-gnueabihf-gcc 编译：
- `main.c` + `../libdaq.c` + LVGL 全部源码 + lv_drivers(fbdev + evdev) → `indaq_ui`
- 链接 `-lm -lrt`

---

## 3. 系统启动：设备树匹配 → 模块加载 → probe 调用链

### 3.1 DTS 关键节点

```
/ {
    indaq {
        compatible = "atomic,imx6ul-indaq";          // ← 匹配 platform_driver
        indaq,sampling-rate = <1000>;
        indaq,trigger-gpios = <&gpio1 18 GPIO_ACTIVE_LOW>;  // KEY0 按钮
    };
};

&i2c1 {
    ap3216c@1e {
        compatible = "alientek,ap3216c";              // ← 匹配 i2c_driver
        reg = <0x1e>;
    };
};

&ecspi3 {
    spidev: icm20608@0 {
        compatible = "alientek,icm20608";             // ← 匹配 spi_driver
        spi-max-frequency = <8000000>;
        reg = <0>;
    };
};
```

### 3.2 模块加载 → probe 全调用链

```
insmod indaq.ko
  │
  ├── module_init(indaq_module_init)                          [indaq_core.c:386]
  │     │
  │     ├── platform_driver_register(&indaq_driver)           [indaq_core.c:390]
  │     │     └── of_match_table = { .compatible = "atomic,imx6ul-indaq" }
  │     │     └── 触发 indaq_probe() 回调                     [indaq_core.c:230]
  │     │           │
  │     │           ├── [1] devm_kzalloc → struct indaq_device
  │     │           ├── [2] indev->sampling_rate = 1000
  │     │           ├── [3] atomic_set(&capture_active, 0)
  │     │           ├── [4] mutex_init(&indev->lock)
  │     │           ├── [5] init_waitqueue_head(&indev->read_wq)
  │     │           │
  │     │           ├── [6] indaq_ringbuf_create(4096)       [indaq_ringbuf.c:12]
  │     │           │     ├── kmalloc(sizeof(struct indaq_ringbuf))
  │     │           │     ├── kmalloc_array(4096, 28)  ← 4096 × 28 = 114688 bytes
  │     │           │     ├── rb->capacity = 4096, head=0, tail=0, count=0
  │     │           │     └── spin_lock_init(&rb->lock)
  │     │           │
  │     │           ├── platform_set_drvdata(pdev, indev)
  │     │           ├── g_indev = indev                     ← 全局指针
  │     │           │
  │     │           ├── [7] of_property_read_u32("indaq,sampling-rate")
  │     │           │
  │     │           ├── [8] alloc_chrdev_region(&devno, 0, 1, "indaq")
  │     │           │     └── 动态分配主设备号（如 242）
  │     │           ├── [9] cdev_init(&cdev, &indaq_fops)    [indaq_core.c:163]
  │     │           │     └── fops: .open=indaq_open, .read=indaq_read,
  │     │           │            .unlocked_ioctl=indaq_ioctl
  │     │           ├── [10] cdev_add(&cdev, devno, 1)
  │     │           ├── [11] class_create(THIS_MODULE, "indaq")
  │     │           ├── [12] device_create(cls, NULL, devno, NULL, "indaq")
  │     │           │     └── 创建 /dev/indaq 设备节点
  │     │           │
  │     │           ├── [13] indaq_i2csens_init(indev)       [indaq_i2csens.c:266]
  │     │           │     └── dev_info("I2C sensor subsystem ready")
  │     │           │     └── 注意: I2C 设备的实际 probe 在 i2c_driver 匹配时独立触发
  │     │           │
  │     │           ├── [14] indaq_imu_init(indev)           [indaq_imu.c:357]
  │     │           │     ├── imu = icm20608_global_imu    ← 从 SPI probe 获取
  │     │           │     ├── imu->core_dev = indev
  │     │           │     └── indev->imu = imu
  │     │           │
  │     │           ├── [15] indaq_gpioctrl_init(indev)      [indaq_gpioctrl.c:71]
  │     │           │     ├── of_get_named_gpio_flags("indaq,trigger-gpios")
  │     │           │     ├── gpio_request / gpio_direction_input
  │     │           │     ├── gpio_to_irq
  │     │           │     └── request_threaded_irq(indaq_gpio_irq_hard,
  │     │           │                              indaq_gpio_irq_thread)
  │     │           │
  │     │           ├── [16] indaq_iio_init(indev)           [indaq_iio.c:342]
  │     │           │     ├── iio_device_alloc(sizeof(struct indaq_iio))
  │     │           │     ├── indio_dev->name = "indaq"
  │     │           │     ├── indio_dev->channels = indaq_iio_channels (10 通道)
  │     │           │     ├── indio_dev->info = &indaq_iio_info
  │     │           │     │     └── .read_raw = indaq_iio_read_raw
  │     │           │     ├── iio_device_register(indio_dev)
  │     │           │     └── indev->iio = iio
  │     │           │
  │     │           ├── [17] indaq_debug_init(indev)         [indaq_debug.c:382]
  │     │           │     ├── debugfs_create_dir("indaq", NULL)
  │     │           │     ├── capture_active, total_samples, sampling_rate
  │     │           │     ├── ringbuf / ringbuf_capacity / ringbuf_count / head / tail
  │     │           │     ├── reg_peek / reg_poke / imu_raw / stats
  │     │           │     └── indev->debug_dir = dir
  │     │           │
  │     │           ├── [18] indaq_pm_init(indev)            [indaq_pm.c:45]
  │     │           │     ├── devm_kzalloc → struct indaq_pm
  │     │           │     ├── pm->pm_nb.notifier_call = indaq_pm_notifier
  │     │           │     └── register_pm_notifier(&pm->pm_nb)
  │     │           │
  │     │           ├── [19] indaq_calib_init(indev)         [indaq_calib.c:222]
  │     │           │     ├── devm_kzalloc → struct indaq_calib_params
  │     │           │     ├── indev->calib = cal
  │     │           │     └── debugfs_create_file("calib_params/calib_gyro/calib_accel")
  │     │           │
  │     │           └── [20] indaq_input_init(indev)         [indaq_input.c:117]
  │     │                 ├── devm_kzalloc → struct indaq_input_priv
  │     │                 ├── input_allocate_device()
  │     │                 ├── input->name = "INDAQ IMU Accel"
  │     │                 ├── set_bit(EV_ABS) + input_set_abs_params(X/Y/Z)
  │     │                 ├── set_bit(EV_KEY) + set_bit(BTN_TL)
  │     │                 ├── input_register_device(input)
  │     │                 ├── INIT_DELAYED_WORK(&priv->work, indaq_input_worker)
  │     │                 └── schedule_delayed_work(20ms)
  │     │
  │     ├── indaq_register_i2c_driver()                      [indaq_i2csens.c:249]
  │     │     └── i2c_add_driver(&ap3216c_i2c_driver)
  │     │           └── of_match_table = { "alientek,ap3216c", "sallenkey,ap3216c" }
  │     │           └── 触发 ap3216c_probe()                  [indaq_i2csens.c:156]
  │     │                 │
  │     │                 ├── [1] devm_kzalloc → struct indaq_i2csens
  │     │                 ├── [2] sens->read_interval_ms = 250
  │     │                 ├── [3] devm_regmap_init_i2c(client, &ap3216c_regmap_config)
  │     │                 │     └── regmap: reg_bits=8, val_bits=8, cache=RBTREE
  │     │                 │
  │     │                 ├── [4] regmap_write(SYS_CONFIG, AP3216C_SYS_RESET)  // 0x04
  │     │                 │     └── I2C 写: addr=0x1E, reg=0x00, val=0x04
  │     │                 ├── [5] msleep(10)
  │     │                 │
  │     │                 ├── [6] regmap_write(SYS_CONFIG, AP3216C_SYS_ENABLE)  // 0x03
  │     │                 │     └── I2C 写: addr=0x1E, reg=0x00, val=0x03
  │     │                 ├── [7] msleep(10)
  │     │                 │
  │     │                 ├── [8] INIT_DELAYED_WORK(&read_work, ap3216c_read_worker)
  │     │                 ├── [9] schedule_delayed_work(250ms)    ← 启动周期采集
  │     │                 └── [10] g_ap3216c_sens = sens        ← 全局指针
  │     │
  │     └── indaq_register_imu_driver()                       [indaq_imu.c:336]
  │           └── spi_register_driver(&icm20608_spi_driver)
  │                 └── of_match_table = { "alientek,icm20608", "invensense,icm20608" }
  │                 └── 触发 icm20608_probe()                   [indaq_imu.c:234]
  │                       │
  │                       ├── [1] devm_kzalloc → struct indaq_imu
  │                       ├── [2] imu->interval_ms = 10
  │                       ├── [3] spi->mode = SPI_MODE_0
  │                       ├── [4] spi->bits_per_word = 8
  │                       ├── [5] spi->max_speed_hz = min(8MHz, 10MHz) = 8MHz
  │                       ├── [6] spi_setup(spi)
  │                       │
  │                       ├── [7] icm20608_init(imu)              [indaq_imu.c:155]
  │                       │     ├── icm20608_read_reg(WHO_AM_I)   [SPI 读 0x75]
  │                       │     │     └── icm20608_read_regs(spi, 0x75, &whoami, 1)
  │                       │     │           └── SPI: 发送 0xF5 (读命令), 接收 1 字节
  │                       │     │           └── 预期 whoami = 0xAF (或 0xAE)
  │                       │     │
  │                       │     ├── icm20608_write_reg(PWR_MGMT_1, DEVICE_RESET)  // 0x80
  │                       │     │     └── SPI: 发送 [0x6B, 0x80]
  │                       │     ├── msleep(50)
  │                       │     │
  │                       │     ├── icm20608_write_reg(PWR_MGMT_1, CLKSEL_PLL)   // 0x03
  │                       │     │     └── SPI: 发送 [0x6B, 0x03]
  │                       │     ├── msleep(10)
  │                       │     │
  │                       │     ├── icm20608_write_reg(INT_PIN_CFG, 0x02)  // 禁用 I2C
  │                       │     │     └── SPI: 发送 [0x37, 0x02]
  │                       │     │
  │                       │     ├── icm20608_write_reg(CONFIG, 0x02)      // DLPF ~184Hz
  │                       │     │     └── SPI: 发送 [0x1A, 0x02]
  │                       │     │
  │                       │     ├── icm20608_write_reg(GYRO_CONFIG, FS_SEL_2000DPS) // ±2000
  │                       │     │     └── SPI: 发送 [0x1B, 0x18]
  │                       │     │
  │                       │     ├── icm20608_write_reg(ACCEL_CONFIG, FS_SEL_16G) // ±16g
  │                       │     │     └── SPI: 发送 [0x1C, 0x18]
  │                       │     │
  │                       │     ├── icm20608_write_reg(ACCEL_CONFIG2, DLPF_218HZ)  // 0x01
  │                       │     │     └── SPI: 发送 [0x1D, 0x01]
  │                       │     │
  │                       │     └── icm20608_write_reg(SMPLRT_DIV, 9)  // 100Hz
  │                       │           └── SPI: 发送 [0x19, 0x09]
  │                       │
  │                       ├── [8] INIT_DELAYED_WORK(&imu->work, icm20608_read_worker)
  │                       ├── [9] schedule_delayed_work(10ms)      ← 启动周期采集
  │                       └── [10] icm20608_global_imu = imu      ← 全局指针
  │
  └── pr_info("INDAQ: module loaded (v%s)")
```

---

## 4. 内核模块初始化：module_init 调用链

```
indaq_module_init()                        [indaq_core.c:386]
  ├── platform_driver_register(&indaq_driver)  → indaq_probe()
  ├── indaq_register_i2c_driver()              → i2c_add_driver → ap3216c_probe()
  └── indaq_register_imu_driver()              → spi_register_driver → icm20608_probe()
```

### module_exit 反向调用链

```
indaq_module_exit()                          [indaq_core.c:412]
  ├── indaq_unregister_imu_driver()          → spi_unregister_driver → icm20608_remove()
  │     ├── cancel_delayed_work_sync(&imu->work)
  │     ├── icm20608_global_imu = NULL
  │     └── icm20608_write_reg(PWR_MGMT_1, SLEEP)
  │
  ├── indaq_unregister_i2c_driver()          → i2c_del_driver → ap3216c_remove()
  │     ├── cancel_delayed_work_sync(&sens->read_work)
  │     └── regmap_write(SYS_CONFIG, 0x00)   // 禁用传感器
  │
  └── platform_driver_unregister(&indaq_driver) → indaq_remove()
        ├── indaq_i2csens_exit / imu_exit / gpioctrl_exit / iio_exit
        ├── indaq_input_exit / calib_exit / debug_exit / pm_exit
        ├── indaq_ringbuf_destroy()
        ├── device_destroy / class_destroy / cdev_del / unregister_chrdev_region
        └── pr_info("INDAQ removed")
```

---

## 5. 数据采集流程：传感器 → 环形缓冲区

### 5.1 ICM-20608 IMU 数据流 (100Hz)

```
时间轴每 10ms:
                    │
icm20608_read_worker()                          [indaq_imu.c:133]
  │ 容器: container_of(work, struct indaq_imu, work.work)
  │
  ├── icm20608_read_all(imu)                    [indaq_imu.c:102]
  │     │
  │     ├── icm20608_read_regs(imu->spi, 0x3B, buf, 14)    [indaq_imu.c:46]
  │     │     │
  │     │     ├── addr = 0x3B | 0x80 = 0xBB    ← R/W#=1
  │     │     ├── spi_message_init(&msg)
  │     │     ├── xfer[0]: tx_buf=&addr, len=1  ← 发送地址字节
  │     │     ├── xfer[1]: rx_buf=buf, len=14   ← 接收 14 字节
  │     │     ├── spi_message_add_tail(x2)
  │     │     └── spi_sync(spi, &msg)           ← SPI 同步传输
  │     │
  │     ├── imu->ax = (buf[0] << 8) | buf[1]    ← ACCEL_XOUT_H/L (0x3B-0x3C)
  │     ├── imu->ay = (buf[2] << 8) | buf[3]    ← ACCEL_YOUT_H/L (0x3D-0x3E)
  │     ├── imu->az = (buf[4] << 8) | buf[5]    ← ACCEL_ZOUT_H/L (0x3F-0x40)
  │     ├── imu->temp = (buf[6] << 8) | buf[7]  ← TEMP_OUT_H/L (0x41-0x42)
  │     ├── imu->gx = (buf[8] << 8) | buf[9]    ← GYRO_XOUT_H/L (0x43-0x44)
  │     ├── imu->gy = (buf[10] << 8) | buf[11]  ← GYRO_YOUT_H/L (0x45-0x46)
  │     └── imu->gz = (buf[12] << 8) | buf[13]  ← GYRO_ZOUT_H/L (0x47-0x48)
  │
  ├── indaq_push_imu_sample(imu->ax, ay, az, gx, gy, gz, temp)  [indaq_core.c:203]
  │     │
  │     ├── [check] indev && capture_active
  │     ├── s = indev->latest_sample           ← 继承上次 AP3216C 的 als/ps/ir 值
  │     ├── s.ts_ns = ktime_get_ns()           ← 当前时间戳 (纳秒)
  │     ├── s.ax = ax, s.ay = ay, s.az = az    ← 更新 IMU 字段
  │     ├── s.temp = temp
  │     ├── s.gx = gx, s.gy = gy, s.gz = gz
  │     ├── indev->latest_sample = s           ← 更新融合样本
  │     │
  │     ├── indaq_ringbuf_push(indev->ringbuf, &s)   [indaq_ringbuf.c:53]
  │     │     ├── spin_lock_irqsave(&rb->lock, flags)
  │     │     ├── memcpy(&rb->buf[rb->head], &s, 28)  ← 写入 head 位置
  │     │     ├── rb->head = (head + 1) % capacity     ← 循环推进 head
  │     │     ├── if (count < capacity) count++
  │     │     ├── else: tail = (tail + 1) % capacity   ← 覆盖最旧样本
  │     │     └── spin_unlock_irqrestore(&rb->lock, flags)
  │     │
  │     └── wake_up_interruptible(&indev->read_wq)    ← 唤醒阻塞的 read()
  │
  └── schedule_delayed_work(&imu->work, 10ms)          ← 重新调度
```

### 5.2 AP3216C 数据流 (~4Hz)

```
时间轴每 250ms:
                    │
ap3216c_read_worker()                           [indaq_i2csens.c:50]
  │ 容器: container_of(work, struct indaq_i2csens, read_work.work)
  │
  ├── [1] regmap_read(sens->regmap, ALS_DATA_HI, &als_hi)    [寄存器 0x0D]
  │     └── regmap: cache hit? → 若未缓存则 I2C 读取
  │     └── I2C: addr=0x1E, reg=0x0D, read 1 byte
  ├── [2] regmap_read(sens->regmap, ALS_DATA_LO, &als_lo)    [寄存器 0x0C]
  ├── [3] regmap_read(sens->regmap, IR_DATA_HI, &ir_hi)      [寄存器 0x0B]
  ├── [4] regmap_read(sens->regmap, IR_DATA_LO, &ir_lo)      [寄存器 0x0A]
  ├── [5] regmap_read(sens->regmap, PS_DATA_HI, &ps_hi)      [寄存器 0x0F]
  ├── [6] regmap_read(sens->regmap, PS_DATA_LO, &ps_lo)      [寄存器 0x0E]
  │
  ├── sens->als_value = (als_hi << 8) | als_lo               ← 16-bit ALS
  │
  ├── if (ir_lo & BIT(7))                                    ← IR_OF 溢出标志
  │     sens->ir_value = 0
  │   else
  │     sens->ir_value = (ir_hi << 2) | (ir_lo & 0x03)      ← 10-bit IR
  │
  ├── if (ps_lo & BIT(6))                                    ← PS_IR_OF 溢出标志
  │     sens->ps_value = 0
  │   else
  │     sens->ps_value = ((ps_hi & 0x3F) << 4) | (ps_lo & 0x0F)  ← 10-bit PS
  │
  ├── indaq_push_sample(als_value, ps_value, ir_value)       [indaq_core.c:178]
  │     ├── [check] indev && capture_active
  │     ├── s = indev->latest_sample           ← 继承上次 IMU 的 ax/ay/az/gx/gy/gz/temp
  │     ├── s.ts_ns = ktime_get_ns()
  │     ├── s.als = als, s.ps = ps, s.ir = ir  ← 更新 AP3216C 字段
  │     ├── indev->latest_sample = s
  │     ├── indaq_ringbuf_push(indev->ringbuf, &s)          [同上]
  │     └── wake_up_interruptible(&indev->read_wq)
  │
  └── schedule_delayed_work(&sens->read_work, 250ms)         ← 重新调度
```

### 5.3 融合样本机制详解

`latest_sample` 在内核中是一个**持续更新的合并结构体**：

```
┌────────────────────────────────────────────────────┐
│ struct indaq_sample (28 字节 packed)                │
│                                                     │
│  IMU 推送时 (100Hz):                                │
│    ts_ns = ktime_get_ns()                           │
│    als/ps/ir = 保留上次 AP3216C 写入的值             │
│    ax/ay/az/gx/gy/gz/temp = 本次 IMU 新值          │
│                                                     │
│  AP3216C 推送时 (~4Hz):                             │
│    ts_ns = ktime_get_ns()                           │
│    als/ps/ir = 本次 AP3216C 新值                   │
│    ax/ay/az/gx/gy/gz/temp = 保留上次 IMU 写入的值    │
└────────────────────────────────────────────────────┘
```

**效果**：用户态每次从环形缓冲区读取的样本，都是包含**所有传感器最新数据**的对齐样本。

### 5.4 环形缓冲区状态变化示例

```
初始状态:  head=0, tail=0, count=0        [空]
  buf: [  ][  ][  ][  ]...[  ]

IMU 推第 1 次: head=0→1, tail=0, count=1
  buf: [S0][  ][  ][  ]...[  ]

IMU 推第 2 次: head=1→2, tail=0, count=2
  buf: [S0][S1][  ][  ]...[  ]

... 持续写入 ...
  head 一直推进, tail 不动, count 增加

当 count = 4096 (满): head=0, tail=0, count=4096
  buf: [S4095][S0][S1]...[S4094]    ← 已循环覆盖

再推入: head=0→1, tail=0→1, count=4096  [满, 覆盖最旧]
  buf: [S_NEW][S0][S1]...[S4094]

用户 read() 消费: 从 tail 读取 N 个, tail 推进, count 减少
  └─ 批量读取 32 个: tail=1→33, count=4096→4064
```

---

## 6. 数据输出流程：环形缓冲区 → 用户态

### 6.1 用户态调用链

```
用户程序:
  fd = open("/dev/indaq", O_RDONLY)
  ioctl(fd, INDAQ_IOCTL_START_CAPTURE)
  read(fd, buf, sizeof(struct indaq_sample) * N)
  ioctl(fd, INDAQ_IOCTL_STOP_CAPTURE)
  close(fd)
```

### 6.2 内核侧 read() 全流程

```
系统调用 read(fd, buf, count)
  │
  └── vfs_read() → → → indaq_read()               [indaq_core.c:49]
        │
        ├── filp->private_data → indev
        ├── max_samples = count / sizeof(struct indaq_sample)  ← 计算样本数
        ├── if (max_samples > 64) max_samples = 64             ← 限幅
        ├── samples = kmalloc_array(max_samples, 28, GFP_KERNEL)
        │
        ├── [检查 O_NONBLOCK]
        │   ├── 非阻塞且无数据 → ret = -EAGAIN, goto out_free
        │   └── 阻塞模式 → wait_event_interruptible(read_wq, count > 0)
        │         └── 睡眠等待, 被 wake_up_interruptible() 唤醒
        │
        ├── n = indaq_ringbuf_read(indev->ringbuf, samples, max_samples)
        │     [indaq_ringbuf.c:80]
        │     ├── spin_lock_irqsave(&rb->lock, flags)
        │     ├── for (i=0; i<max && rb->count>0; i++)
        │     │     memcpy(&buf[i], &rb->buf[rb->tail], 28)  ← 读取
        │     │     rb->tail = (tail + 1) % capacity          ← 推进 tail
        │     │     rb->count--
        │     └── spin_unlock_irqrestore(&rb->lock, flags)
        │     └── return i  ← 实际读取的样本数
        │
        ├── copy_to_user(buf, samples, n * 28)
        │     └── 若失败 → ret = -EFAULT, goto out_free
        │
        ├── indev->total_samples += n
        ├── ret = n * sizeof(struct indaq_sample)            ← 返回字节数
        │
        └── kfree(samples)
```

### 6.3 阻塞等待机制

```
  生产者 (传感器 worker)                消费者 (用户态 read)
         │                                   │
         │                                   │ wait_event_interruptible()
         │                                   │   └─ 进程进入睡眠
         │                                   │
  indaq_ringbuf_push()                       │
         │                                   │
  wake_up_interruptible(&read_wq) ───────────┤
                                             │ 被唤醒
                                             │ indaq_ringbuf_read()
```

### 6.4 批量读取 + 去重（UI 场景）

LVGL 中的 `update_sensor_data()` 使用**批量读取取最新**策略：

```c
struct indaq_sample batch[32];                 // 32 个样本的批量缓冲区
ssize_t n = indaq_read(c->daq_fd, batch, 32); // 一次性全部取出
if (n > 0) {
    c->sample = batch[n - 1];                  // 只取最后一个（最新）
}
```

**原因**：IMU 以 100Hz 产生数据，UI 每 200ms 刷新一次。在这 200ms 内可能堆积了 ~20 个样本。如果逐个读取，UI 会滞后。批量读取并取最新样本，保证显示实时性。

---

## 7. IOCTL 控制接口

### 7.1 所有 IOCTL 命令

| 命令 | 方向 | 编号 | 功能 | 实现函数 |
|------|------|------|------|----------|
| `GET_INFO` | R | 0 | 获取驱动信息 | `indaq_ioctl()` case 0 |
| `START_CAPTURE` | - | 3 | 开始采集 | `indaq_ioctl()` case 3 |
| `STOP_CAPTURE` | - | 4 | 停止采集 | `indaq_ioctl()` case 4 |
| `SET_SAMPLING_RATE` | W | 5 | 设置采样率 | `indaq_ioctl()` case 5 |

### 7.2 IOCTL 实现详解

```c
static long indaq_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct indaq_device *indev = filp->private_data;

    switch (cmd) {
    case INDAQ_IOCTL_GET_INFO:     // 0x80104900  _IOR('I', 0, struct indaq_info)
        info.version = 0x0100;                    // 版本 1.0
        info.sampling_rate = indev->sampling_rate; // 当前采样率 (Hz)
        info.total_samples = indev->total_samples;  // 总样本数
        info.errors = 0;
        copy_to_user((void __user *)arg, &info, sizeof(info));
        break;

    case INDAQ_IOCTL_START_CAPTURE:  // 0x00004903  _IO('I', 3)
        indaq_ringbuf_reset(indev->ringbuf);      // 清空环形缓冲区
        atomic_set(&indev->capture_active, 1);    // 设置采集激活标志
        break;

    case INDAQ_IOCTL_STOP_CAPTURE:   // 0x00004904  _IO('I', 4)
        atomic_set(&indev->capture_active, 0);    // 清除采集激活标志
        break;

    case INDAQ_IOCTL_SET_SAMPLING_RATE:  // 0x40044905  _IOW('I', 5, u32)
        copy_from_user(&rate_hz, (u32 __user *)arg, sizeof(rate_hz));
        if (rate_hz < 1 || rate_hz > 100) return -EINVAL;
        indev->sampling_rate = rate_hz;
        indaq_i2csens_set_interval(1000 / rate_hz);  // 转换为 ms
        // rate_hz=100 → interval=10ms (AP3216C 最快 10ms)
        // rate_hz=4   → interval=250ms (默认)
        break;
    }
}
```

### 7.3 capture_active 标志的作用

`atomic_t capture_active` 控制传感器数据是否写入环形缓冲区：

```
在 indaq_push_sample() 和 indaq_push_imu_sample() 入口：
    if (!atomic_read(&indev->capture_active))
        return;              // ← 直接丢弃，不入队

这意味着：
  START 后：数据从 worker → ringbuf → 用户态可读
  STOP 后：  worker 仍在运行（读 SPI/I2C），但数据不入 ringbuf
            用户态 read() 看到 ringbuf 为空 → 阻塞或 -EAGAIN
```

---

## 8. IIO 子系统：sysfs 读数路径

### 8.1 IIO 通道布局

```
/sys/bus/iio/devices/iio:device0/
  ├── in_illuminance_raw         → AP3216C ALS  (16-bit)
  ├── in_proximity_raw           → AP3216C PS   (10-bit)
  ├── in_intensity_raw           → AP3216C IR   (10-bit)
  ├── in_accel_x_raw             → ICM-20608 Accel X
  ├── in_accel_y_raw             → ICM-20608 Accel Y
  ├── in_accel_z_raw             → ICM-20608 Accel Z
  ├── in_anglvel_x_raw           → ICM-20608 Gyro X
  ├── in_anglvel_y_raw           → ICM-20608 Gyro Y
  ├── in_anglvel_z_raw           → ICM-20608 Gyro Z
  ├── in_temp_raw                → ICM-20608 Temperature
  ├── in_accel_scale             → 加速度 scale: 0.000488 (16g)
  ├── in_anglvel_scale           → 角速度 scale: 0.060976 (2000dps)
  └── in_temp_scale              → 温度 scale: 0.002941
```

### 8.2 read_raw 回调详解

```c
static int indaq_iio_read_raw(iio_dev, chan, *val, *val2, mask)
{
    iio = iio_priv(indio_dev);
    indev = iio->indev;
    imu = indev->imu ?: icm20608_get_imu();
    cal = indev->calib;

    switch (mask) {
    case IIO_CHAN_INFO_RAW:
        switch (chan->address) {
        // --- AP3216C 通道: 调用 i2csens_get_data() ---
        case IIO_CHAN_ALS:
        case IIO_CHAN_PS:
        case IIO_CHAN_IR:
            indaq_i2csens_get_data(&als, &ps, &ir);   // 返回缓存的传感器值
            switch (chan->address) {
            case IIO_CHAN_ALS: *val = als; break;     // IIO_LIGHT
            case IIO_CHAN_PS:  *val = ps;  break;     // IIO_PROXIMITY
            case IIO_CHAN_IR:  *val = ir;  break;     // IIO_INTENSITY
            }
            return IIO_VAL_INT;                        // 返回整数

        // --- IMU Accel: 直接从 imu 结构体读取缓存 ---
        case IIO_CHAN_ACCEL_X: *val = imu->ax; return IIO_VAL_INT;  // 无 SPI 操作！
        case IIO_CHAN_ACCEL_Y: *val = imu->ay; return IIO_VAL_INT;
        case IIO_CHAN_ACCEL_Z: *val = imu->az; return IIO_VAL_INT;

        // --- IMU Gyro: 读取缓存减去校准偏移 ---
        case IIO_CHAN_GYRO_X:
            *val = imu->gx;
            if (cal && cal->calibrated)
                *val -= cal->gyro_offset[0];           // ← 零偏校正
            return IIO_VAL_INT;
        // ... Y, Z 同理

        case IIO_CHAN_TEMP:
            *val = imu->temp;
            return IIO_VAL_INT;
        }

    case IIO_CHAN_INFO_SCALE:
        // AP3216C ALS: 0.01 lux/LSB → val=0, val2=10000 (IIO_VAL_INT_PLUS_MICRO)
        // Accel: 0.000488 g/LSB → val=0, val2=488 (校准后调整 val2)
        // Gyro:  0.060976 dps/LSB → val=0, val2=60976
        // Temp:  0.002941 °C/LSB → val=0, val2=2941

    case IIO_CHAN_INFO_OFFSET:
        // Temp 偏移: val=8500, val2=0 → 8500/1000=8.5°C
    }
}
```

**关键设计**：IMU 数据从缓存的 `imu->ax` 等字段读取，**不触发 SPI 传输**。这些缓存由 `icm20608_read_worker()` 以 100Hz 更新。

### 8.3 IIO 初始化/退出

```c
indaq_iio_init():                              [indaq_iio.c:342]
  indio_dev = iio_device_alloc(sizeof(struct indaq_iio))
  iio = iio_priv(indio_dev)
  indio_dev->name = "indaq"
  indio_dev->channels = indaq_iio_channels     // 10 通道数组
  indio_dev->num_channels = 10
  indio_dev->modes = INDIO_DIRECT_MODE
  iio_device_register(indio_dev)

indaq_iio_exit():
  iio_device_unregister(indio_dev)
  iio_device_free(indio_dev)
```

---

## 9. Input 子系统：加速度上报与 Tap 检测

### 9.1 数据流

```
内核定时器每 20ms:
                    │
indaq_input_worker()                             [indaq_input.c:86]
  │
  ├── imu = indev->imu ?: icm20608_get_imu()    ← 获取 IMU 实例
  │
  ├── if (imu) {
  │     input_report_abs(priv->input, ABS_X, imu->ax)  ← 上报加速度值
  │     input_report_abs(priv->input, ABS_Y, imu->ay)
  │     input_report_abs(priv->input, ABS_Z, imu->az)
  │     input_sync(priv->input)                        ← 同步事件
  │
  │     detect_tap(priv, imu->az)                      ← Tap 检测
  │   }
  │
  └── schedule_delayed_work(&priv->work, 20ms)         ← 重新调度
```

### 9.2 Tap 检测状态机

```c
detect_tap(priv, current_z)                        [indaq_input.c:51]
│
├── delta_z = abs(current_z - priv->prev_z)
│
├── if (delta_z > TAP_THRESHOLD)   // 阈值 500 ≈ 0.25g
│     │
│     ├── if (超时 > 200ms)        // 窗口过期，重新计数
│     │     priv->tap_count = 1
│     │
│     ├── else                     // 在窗口内，递增
│     │     priv->tap_count++
│     │
│     ├── priv->last_tap_jiffies = jiffies
│     │
│     └── if (tap_count >= 3)      // 三击触发
│           input_report_key(priv->input, BTN_TL, 1)
│           input_sync(priv->input)
│           input_report_key(priv->input, BTN_TL, 0)
│           input_sync(priv->input)
│           priv->tap_count = 0    // 重置计数
│
└── priv->prev_z = current_z
```

**状态机示意**：
```
state=0  ──(Z 突跳 >500)──→  state=1  (记录 last_tap_jiffies)
state=1  ──(200ms 内再跳)──→  state=2
state=2  ──(200ms 内再跳)──→  触发 BTN_TL → 回到 state=0
           (超时)          →  回到 state=0
```

### 9.3 Input 设备初始化

```c
indaq_input_init(indev):                          [indaq_input.c:117]
  ├── priv = devm_kzalloc(...)
  ├── input = input_allocate_device()
  ├── input->name = "INDAQ IMU Accel"
  ├── input->id.bustype = BUS_SPI
  │
  ├── set_bit(EV_ABS, input->evbit)                // ABS 事件
  ├── input_set_abs_params(input, ABS_X, -32768, 32767, 0, 0)
  ├── input_set_abs_params(input, ABS_Y, -32768, 32767, 0, 0)
  ├── input_set_abs_params(input, ABS_Z, -32768, 32767, 0, 0)
  │
  ├── set_bit(EV_KEY, input->evbit)                // 按键事件
  ├── set_bit(BTN_TL, input->keybit)               // 三击手势
  │
  ├── input_register_device(input)                 // → /dev/input/event*
  ├── INIT_DELAYED_WORK(&priv->work, indaq_input_worker)
  └── schedule_delayed_work(20ms)
```

用户态可用 `evtest /dev/input/event0` 查看加速度和 tap 事件。

---

## 10. GPIO 触发控制

### 10.1 硬件中断 → 采集启停

```
KEY0 按钮按下 (GPIO1_IO18 下降沿)
  │
  ├── [硬中断] indaq_gpio_irq_hard()              [indaq_gpioctrl.c:65]
  │     └── return IRQ_WAKE_THREAD               ← 唤醒线程
  │
  └── [线程] indaq_gpio_irq_thread()              [indaq_gpioctrl.c:24]
        │
        ├── msleep(50)                            ← 消抖
        ├── gpio_get_value() 确认仍为低电平         ← 防误触发
        │
        └── toggle capture state:
              was_active = atomic_read(&capture_active)
              if (was_active):
                  atomic_set(&capture_active, 0)   // STOP
              else:
                  indaq_ringbuf_reset(ringbuf)     // 清空缓冲区
                  atomic_set(&capture_active, 1)   // START
```

### 10.2 GPIO 初始化

```c
indaq_gpioctrl_init(indev):                       [indaq_gpioctrl.c:71]
  ├── of_get_named_gpio_flags("indaq,trigger-gpios")
  │     ← 从 DTS 获取: <&gpio1 18 GPIO_ACTIVE_LOW>
  ├── gpio_request(gpio, "indaq-trigger")          // 申请 GPIO
  ├── gpio_direction_input(gpio)                   // 配置为输入
  ├── gpio_to_irq(gpio) → irq                      // GPIO → IRQ 映射
  │
  └── request_threaded_irq(irq,
          indaq_gpio_irq_hard,                     // 硬中断 handler
          indaq_gpio_irq_thread,                   // 线程 handler
          IRQF_TRIGGER_FALLING | IRQF_ONESHOT,      // 下降沿触发, 中断屏蔽
          "indaq-gpio-trigger", ctrl)
```

### 10.3 设备树对应

```dts
indaq {
    compatible = "atomic,imx6ul-indaq";
    indaq,trigger-gpios = <&gpio1 18 GPIO_ACTIVE_LOW>;
    pinctrl-0 = <&pinctrl_key>;    /* KEY0: UART1_CTS_B → GPIO1_IO18 */
};
```

---

## 11. IMU 校准子系统

### 11.1 校准架构

```
用户态触发:
  echo 1 > /sys/kernel/debug/indaq/calib_gyro
  echo 1 > /sys/kernel/debug/indaq/calib_accel
  cat   /sys/kernel/debug/indaq/calib_params
```

### 11.2 陀螺仪零偏校准

```c
calib_measure_gyro_offset(indev):                 [indaq_calib.c:41]
  │
  ├── for (i = 0; i < 100; i++) {
  │     sum_x += imu->gx;          ← 从缓存的 IMU 数据加和
  │     sum_y += imu->gy;
  │     sum_z += imu->gz;
  │     msleep(imu->interval_ms);  ← 每 10ms 采样一次
  │   }                             ← 总共约 1 秒采集
  │
  ├── cal->gyro_offset[0] = sum_x / 100;    ← 取平均作为零偏
  ├── cal->gyro_offset[1] = sum_y / 100;
  ├── cal->gyro_offset[2] = sum_z / 100;
  ├── cal->gyro_samples = 100;
  └── cal->calibrated = true;
```

### 11.3 加速度计 Scale 校准

```c
calib_measure_accel_scale(indev):                 [indaq_calib.c:89]
  │
  ├── for (i = 0; i < 100; i++) {
  │     sum_z += imu->az;           // 静止平面时 Z 轴应 = 2048 LSB (1g)
  │     msleep(imu->interval_ms);
  │   }
  │
  ├── cal->accel_z_ref = sum_z / 100;
  │
  ├── if (accel_z_ref > 0)
  │     cal->accel_scale_num = 2048         // 期望值
  │     cal->accel_scale_den = accel_z_ref  // 测量值
  │   }
  └── cal->accel_calibrated = true;
```

**校正的应用**：在 IIO `read_raw` 的 `IIO_CHAN_INFO_SCALE` 中调整：

```c
case IIO_CHAN_ACCEL_X/Y/Z:
    *val2 = ICM20608_ACCEL_SCALE_16G_VAL2;     // 488 (0.000488 g/LSB)
    if (cal && cal->accel_calibrated) {
        *val2 = (*val2 * cal->accel_scale_num) / cal->accel_scale_den;
        // 例如: 488 * 2048 / 1950 = 512  (如果测得 Z=1950)
    }
```

### 11.4 DebugFS 接口

```
/sys/kernel/debug/indaq/
  ├── calib_params   (0444)  →  显示校准参数
  ├── calib_gyro     (0200)  →  写入触发陀螺仪校准
  └── calib_accel    (0200)  →  写入触发加速度计校准
```

---

## 12. Power Management 电源管理

### 12.1 系统 suspend/resume 通知

```c
indaq_pm_notifier(nb, event, data):                [indaq_pm.c:23]
  │
  ├── PM_SUSPEND_PREPARE / PM_HIBERNATION_PREPARE:
  │     pm->suspended = true                        // 设置挂起标志
  │     pr_info("INDAQ PM: suspending")
  │
  ├── PM_POST_SUSPEND / PM_POST_HIBERNATION:
  │     pm->suspended = false                       // 清除挂起标志
  │     pr_info("INDAQ PM: resumed")
  │
  └── return NOTIFY_OK / NOTIFY_DONE
```

### 12.2 全局查询函数

```c
bool indaq_pm_is_suspended(void)                   [indaq_pm.c:77]
{
    return g_pm ? g_pm->suspended : false;          // DebugFS stats 使用
}
```

PM 状态在 DebugFS `stats` 中显示为 `pm_suspended: 0` 或 `pm_suspended: 1`。

---

## 13. DebugFS 调试接口

### 13.1 文件结构

```
/sys/kernel/debug/indaq/              (由 indaq_debug_init 创建)
  │
  ├── capture_active   (0444)  →  采集状态 0/1
  ├── total_samples    (0444)  →  总样本数
  ├── sampling_rate    (0444)  →  采样率 Hz
  ├── ringbuf          (0444)  →  环形缓冲区摘要 (cap/count/head/tail/usage%)
  ├── ringbuf_capacity (0444)  →  容量
  ├── ringbuf_count    (0444)  →  当前样本数
  ├── head             (0444)  →  生产者指针
  ├── tail             (0444)  →  消费者指针
  ├── reg_peek         (0644)  →  IMU 寄存器读取
  ├── reg_poke         (0200)  →  IMU 寄存器写入
  ├── imu_raw          (0444)  →  最新 IMU 快照
  ├── stats            (0444)  →  驱动全局状态
  ├── calib_params     (0444)  →  (由 calib 创建) 校准参数
  ├── calib_gyro       (0200)  →  (由 calib 创建) 触发陀螺仪校准
  └── calib_accel      (0200)  →  (由 calib 创建) 触发加速度计校准
```

### 13.2 reg_peek 用法

```bash
echo 0x75 > /sys/kernel/debug/indaq/reg_peek    # 设置寄存器地址
cat /sys/kernel/debug/indaq/reg_peek             # 读取 WHO_AM_I
# 输出: REG[0x75] = 0xAF (175)

echo 0x1B > /sys/kernel/debug/indaq/reg_peek    # GYRO_CONFIG
cat /sys/kernel/debug/indaq/reg_peek
# 输出: REG[0x1B] = 0x18
```

**实现**：`reg_peek_write()` 用 `kstrtoul_from_user()` 解析十六进制地址 → `debug_reg_addr`；`reg_peek_show()` 调用 `icm20608_read_reg()` 发起 SPI 读取。

### 13.3 reg_poke 用法

```bash
echo 0x6B=0x01 > /sys/kernel/debug/indaq/reg_poke    # PWR_MGMT_1 = 0x01
```

**实现**：`reg_poke_write()` 解析 `reg=val` 格式，调用 `icm20608_write_reg()` 发起 SPI 写入。

### 13.4 stats 输出示例

```
version:        1.0.0
capture_active: 1
total_samples:  12345
sampling_rate:  100 Hz
ringbuf_capacity: 4096
ringbuf_count:    128
imu_ready:      yes
calibrated:     no
pm_suspended:   0
gpio_trigger:   1
tap_count:      2
```

---

## 14. libdaq 用户态 C API

### 14.1 函数总览

| 函数 | syscall 路径 | 说明 |
|------|-------------|------|
| `indaq_open()` | `open("/dev/indaq", O_RDWR|O_NONBLOCK)` | 打开设备 |
| `indaq_close()` | `close(fd)` | 关闭设备 |
| `indaq_start()` | `ioctl(fd, INDAQ_IOCTL_START_CAPTURE)` | 开始采集 |
| `indaq_stop()` | `ioctl(fd, INDAQ_IOCTL_STOP_CAPTURE)` | 停止采集 |
| `indaq_read()` | `read(fd, buf, size)` | 读取样本 |
| `indaq_wait()` | `poll(fd, POLLIN, timeout)` | 等待数据可用 |
| `indaq_get_info()` | `ioctl(fd, INDAQ_IOCTL_GET_INFO)` | 获取驱动信息 |
| `indaq_set_rate()` | `ioctl(fd, INDAQ_IOCTL_SET_SAMPLING_RATE)` | 设置采样率 |
| `indaq_print_sample()` | (纯计算) | 格式化打印样本 |

### 14.2 典型使用流程

```c
#include "libdaq.h"

int main() {
    struct indaq_sample buf[64];

    int fd = indaq_open("/dev/indaq");        // syscall: open()
    indaq_get_info(fd, &info);                // syscall: ioctl(GET_INFO)
    indaq_set_rate(fd, 100);                  // syscall: ioctl(SET_RATE)
    indaq_start(fd);                          // syscall: ioctl(START)

    while (1) {
        // 方式 1: 阻塞等待 + 读取
        if (indaq_wait(fd, 1000) > 0) {       // syscall: poll()
            int n = indaq_read(fd, buf, 64);  // syscall: read()
            for (int i = 0; i < n; i++)
                indaq_print_sample(&buf[i]);
        }

        // 方式 2: 直接阻塞读
        int n = indaq_read(fd, buf, 64);      // 内核中 wait_event_interruptible
        // ... 处理数据
    }

    indaq_stop(fd);                           // syscall: ioctl(STOP)
    indaq_close(fd);                          // syscall: close()
}
```

### 14.3 indaq_read() 返回值处理

```c
ssize_t indaq_read(int fd, struct indaq_sample *buf, size_t count)
{
    ssize_t n = read(fd, buf, count * sizeof(struct indaq_sample));

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -EAGAIN;          // 非阻塞模式下无数据可用
        return -1;                    // 其他错误
    }
    return n / sizeof(struct indaq_sample);  // 返回样本数
}
```

### 14.4 poll-based 等待

```c
int indaq_wait(int fd, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms);        // 内核调用 indaq_core 的 poll
    // 数据可用时 wake_up_interruptible 唤醒 poll
    return (ret > 0) ? 1 : 0;                   // 1=有数据, 0=超时
}
```

---

## 15. indaq_test CLI 测试程序

### 15.1 命令行参数

```bash
./indaq_test                    # 文本模式, 每秒读取
./indaq_test -b                 # 二进制模式, 持续读取
./indaq_test -b 50              # 二进制模式, 读 50 个样本后退出
./indaq_test -r 10              # 设置采样率为 10 Hz
./indaq_test -r 100 -b 200      # 100Hz, 读 200 个二进制样本
```

### 15.2 内部调用流程

```c
main()                                          [indaq_test.c:67]
  ├── signal(SIGINT, handle_sigint)
  │
  ├── open("/dev/indaq", O_RDONLY)               // syscall open
  │     └── 内核 → indaq_open()
  │
  ├── ioctl(fd, SET_SAMPLING_RATE, &rate)        // 可选
  │     └── 内核 → indaq_ioctl() case 5
  │
  ├── ioctl(fd, START_CAPTURE)                    // 必须
  │     └── 内核 → indaq_ioctl() case 3
  │           ├── indaq_ringbuf_reset()
  │           └── atomic_set(capture_active, 1)
  │
  ├── loop: read(fd, batch, sizeof(batch))        // 阻塞读取
  │     └── 内核 → indaq_read()
  │           ├── wait_event_interruptible()
  │           ├── indaq_ringbuf_read()
  │           └── copy_to_user()
  │
  ├── ioctl(fd, STOP_CAPTURE)
  │     └── 内核 → atomic_set(capture_active, 0)
  │
  └── close(fd)
```

---

## 16. Python 绑定 indaq_test.py

### 16.1 ctypes 结构体定义

```python
class IndaqSample(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("ts_ns", c_uint64),   #  0
        ("als",   c_uint16),   #  8
        ("ps",    c_uint16),   # 10
        ("ir",    c_uint16),   # 12
        ("ax",    c_int16),    # 14
        ("ay",    c_int16),    # 16
        ("az",    c_int16),    # 18
        ("temp",  c_int16),    # 20
        ("gx",    c_int16),    # 22
        ("gy",    c_int16),    # 24
        ("gz",    c_int16),    # 26
    ]  # 总大小 28 字节，与内核完全对齐
```

### 16.2 双路径设计

```python
class INDAQ:
    def __init__(self, lib_path="libdaq.so"):
        try:
            self._lib = CDLL(lib_path)     # 优先使用 libdaq.so
        except OSError:
            self._lib = None               # 回退到 os.open/ioctl/read

    # libdaq.so 路径:
    #   daq.open()  → lib.indaq_open()     → open("/dev/indaq")
    #   daq.start() → lib.indaq_start()    → ioctl(START)
    #   daq.read()  → lib.indaq_read()     → read(fd, buf, size)
    #
    # 无 libdaq 回退路径:
    #   daq.open()  → os.open()            → 直接 syscall
    #   daq.start() → fcntl.ioctl()        → 直接 syscall
    #   daq.read()  → os.read()            → 直接 syscall
```

### 16.3 使用示例

```python
from indaq_test import INDAQ

daq = INDAQ()
daq.open()
print(daq.get_info())          # 显示驱动信息
daq.set_rate(50)               # 50Hz
daq.start()
samples = daq.read(10)         # 读 10 个样本
for s in samples:
    print(s)                   # 格式化打印
daq.stop()
daq.close()
```

---

## 17. LVGL 图形界面 indaq_ui

### 17.1 main() 完整流程

```c
main()                                               [main.c:362]
  │
  ├── [1] indaq_open(INDAQ_DEVICE)                    // /dev/indaq
  │     └── libdaq → open("/dev/indaq", O_RDWR|O_NONBLOCK)
  │
  ├── [2] indaq_get_info(fd, &info)                   // 查询版本和状态
  │
  ├── [3] indaq_start(fd)                              // 开始采集
  │
  ├── [4] lv_init()                                    // LVGL 初始化
  │
  ├── [5] fbdev_init()                                 // /dev/fb0 初始化
  │     └── mmap framebuffer, 设置分辨率 1024×600
  │
  ├── [6] lv_disp_drv_register()                       // 显示驱动注册
  │     └── flush_cb = fbdev_flush
  │
  ├── [7] evdev_init()                                 // /dev/input/event1 触摸
  │
  ├── [8] lv_indev_drv_register()                      // 输入驱动注册
  │     └── read_cb = evdev_read
  │
  ├── [9] 创建 UI 布局:
  │     ├── create_header(scr)                          // 标题栏 "INDAQ MONITOR v1.0.0"
  │     ├── create_sensor_panel(left_panel)              // 左侧 60% 传感器数据
  │     │     ├── lbl_als_line   → "ALS: 123.4 lux  PS: 567  IR: 89  OBJ: YES"
  │     │     ├── lbl_accel_line → "X: +0.023g  Y: -0.001g  Z: +1.002g"
  │     │     ├── lbl_gyro_line  → "GX: +12.1  GY: -5.0  GZ: +3.2  Cal: YES"
  │     │     └── lbl_temp_line  → "25.3 C"
  │     │
  │     └── create_system_panel(right_panel)             // 右侧 40% 系统状态
  │           ├── lbl_core_line    → "Capture: RUNNING  Rate: 100 Hz"
  │           ├── lbl_ringbuf_line → "Count: 128 / 4096  Usage: 3%"
  │           ├── lbl_calib_line   → "Gyro: CALIBRATED  Off: 0/0/0"
  │           ├── lbl_pm_line      → "PM: active"
  │           ├── lbl_gpio_line    → "GPIO: active"
  │           ├── lbl_tap_line     → "Tap: 2"
  │           ├── lbl_iio_line     → "IIO: registered"
  │           ├── lbl_debugfs_line → "DebugFS: OK"
  │           └── lbl_health_line  → "Health: IMU OK"
  │
  ├── [10] lv_timer_create(update_sensor_data, 200ms)   // 创建刷新定时器
  │
  └── [11] 主循环:
        while (1) {
            lv_tick_inc(5);                              // LVGL 时钟推进
            lv_timer_handler();                          // 触发定时器回调
            usleep(5000);                                // 5ms 休眠
        }
```

### 17.2 update_sensor_data() 详细流程

```c
static void update_sensor_data(lv_timer_t *timer)        // 每 200ms 执行
{
    // ======== Step 1: 从 /dev/indaq 读取传感器数据 ========
    if (c->daq_ok) {
        struct indaq_sample batch[32];
        ssize_t n = indaq_read(c->daq_fd, batch, 32);   // 批量读取
        //            ↓ 内核: wait_event_interruptible → ringbuf_read → copy_to_user
        if (n > 0) {
            c->sample = batch[n - 1];                     // 取最新样本
        } else if (n == -EAGAIN) {
            goto do_system_status;                        // 无新数据，跳过传感器部分
        } else {
            // 设备错误，尝试重新打开
            indaq_close(c->daq_fd);
            c->daq_fd = indaq_open("/dev/indaq");
            if (c->daq_ok) indaq_start(c->daq_fd);
        }

        // 每 5 次刷新 (约 1 秒) 更新一次 info
        if (++icnt % 5 == 0)
            indaq_get_info(c->daq_fd, &c->info);          // ioctl(GET_INFO)
    }

    // ======== Step 2: 更新 AP3216C 数据显示 ========
    //  ALS: raw × 0.01 → lux
    //  PS > 100 → OBJ: YES (物体靠近检测)
    //  IR: 原始计数值

    // ======== Step 3: 更新加速度计显示 ========
    //  ax/2048 → g (使用 ±16g 量程: 2048 LSB/g)

    // ======== Step 4: 更新陀螺仪 + 温度显示 ========
    //  gx/16.4 → dps (使用 ±2000°/s 量程)
    //  temp/333.87 + 25 → °C

    // ======== Step 5: 通过 DebugFS 读取系统状态 ========
    //  ringbuf_capacity / ringbuf_count → 环形缓冲区使用率
    //  calib_params → 校准状态和偏移值
    //  stats → pm_suspended, gpio_trigger, tap_count, imu_ready
    //  /sys/bus/iio/devices/iio:device0/name → IIO 状态

do_system_status:
    // 从 /sys/kernel/debug/indaq/stats 解析所有系统状态
    // 更新: core_line, pm_line, gpio_line, tap_line,
    //       iio_line, debugfs_line, health_line
}
```

### 17.3 数据读取去重策略图解

```
时间线 (ms):
  0    50    100   150   200   250   300   350   400
  │     │     │     │     │     │     │     │     │
  I I I I I I I I I I I I I I I I I I I I I I I I I   IMU 采样 (100Hz, 每10ms)
  A                         A                         AP3216C 采样 (4Hz, 每250ms)
  │                                                     │
  └─── ringbuf 中堆积 ──────┤                          │
                            U───────U───────U───────U   UI 刷新 (200ms)
                                                      
  每次 UI 刷新:
    indaq_read(fd, batch, 32) → 取出 20 个样本
    (20 = 200ms / 10ms)
    只保留 batch[19] (最新)
```

### 17.4 UI 布局

```
┌────────────────────────────────────────────────────────┐
│  INDAQ MONITOR v1.0.0                    (header 40px) │
├──────────────────────────────┬─────────────────────────┤
│  -- AP3216C --               │  -- CORE --             │
│  ALS: 123.4(lux)[12345]      │  Capture: RUNNING       │
│  PS: 567(counts)   OBJ: YES  │  Rate: 100 Hz           │
│  IR: 89(counts)              │  Samples: 12345         │
│                              │                         │
│  -- ACCELEROMETER --         │  -- RING BUFFER --      │
│  X: +0.023(g)  Y: -0.001(g) │  Count: 128 / 4096      │
│  Z: +1.002(g)                │  Usage: 3%              │
│                              │                         │
│  -- GYROSCOPE --             │  -- CALIBRATION --      │
│  GX: +12.1(dps)  GY: -5.0   │  Gyro: CALIBRATED       │
│  GZ: +3.2(dps)  Cal: YES    │  Off: 0/0/0  Accel: cal │
│                              │                         │
│  -- TEMPERATURE --           │  -- SYSTEM --           │
│  25.3(C)                     │  PM: active             │
│                              │  GPIO: active           │
│                              │  Tap: 2                 │
│                              │  IIO: registered        │
│                              │  DebugFS: OK            │
│                              │  Health: IMU OK         │
│  (left 60%)                  │  (right 40%)            │
└──────────────────────────────┴─────────────────────────┘
```

---

## 18. 自测试模块 indaq_selftest

### 18.1 测试用例

| 测试函数 | 描述 | 验证点 |
|----------|------|--------|
| `ringbuf_create_destroy_test()` | 创建并销毁 1024 槽缓冲区 | 创建成功, capacity 正确 |
| `ringbuf_push_read_test()` | 推入 1 个样本 → 读取 | 读写数据匹配, 读后缓冲区为空 |
| `ringbuf_wraparound_test()` | 写入 32 个样本到 16 槽 | 循环覆盖正确, 仍有数据可读 |
| `ringbuf_reset_test()` | 写入 → reset → 读取 | reset 后缓冲区为空 |

### 18.2 测试宏

```c
#define RUN_TEST(name)                                 \
    do {                                               \
        test_count++;                                  \
        pr_info("SELFTEST: %s... ", name);             \
        if (name##_test() == 0) {                      \
            pass_count++;                              \
            pr_cont("PASS\n");                         \
        } else {                                       \
            pr_cont("FAIL\n");                         \
        }                                              \
    } while (0)

module_init(indaq_selftest_init)
  ├── RUN_TEST(ringbuf_create_destroy)
  ├── RUN_TEST(ringbuf_push_read)
  ├── RUN_TEST(ringbuf_wraparound)
  └── RUN_TEST(ringbuf_reset)
```

---

## 19. 完整调用链汇总

### 19.1 系统启动 → probe → 数据开始流动

```
U-Boot → Linux Kernel 启动
  │
  ├── [DTS 解析] 内核遍历设备树节点
  │     ├── indaq 节点 → platform_device 注册
  │     ├── ap3216c@1e → i2c_device 注册
  │     └── icm20608@0 → spi_device 注册
  │
  └── [模块加载] insmod indaq.ko
        │
        ├── indaq_module_init()
        │     ├── platform_driver_register() → indaq_probe()
        │     │     ├── indaq_ringbuf_create(4096)
        │     │     ├── alloc_chrdev_region / cdev_init / cdev_add
        │     │     ├── class_create / device_create → /dev/indaq
        │     │     ├── indaq_i2csens_init()      // 准备 I2C 子系统
        │     │     ├── indaq_imu_init()           // 关联 IMU → core
        │     │     ├── indaq_gpioctrl_init()      // 注册 GPIO 中断
        │     │     ├── indaq_iio_init()           // 注册 IIO 设备
        │     │     ├── indaq_debug_init()         // 创建 debugfs 文件
        │     │     ├── indaq_pm_init()            // 注册 PM notifier
        │     │     ├── indaq_calib_init()         // 校准子系统 + debugfs
        │     │     └── indaq_input_init()         // 注册 input + tap 检测
        │     │
        │     ├── indaq_register_i2c_driver() → ap3216c_probe()
        │     │     ├── devm_regmap_init_i2c()
        │     │     ├── regmap_write(RESET/ENABLE)
        │     │     └── schedule_delayed_work(250ms)  ← AP3216C 数据流启动
        │     │
        │     └── indaq_register_imu_driver() → icm20608_probe()
        │           ├── icm20608_init()
        │           │     ├── WHO_AM_I 校验 (0xAF)
        │           │     ├── 配置量程/DLPF/采样率
        │           │     └── 唤醒芯片
        │           └── schedule_delayed_work(10ms)    ← IMU 数据流启动
        │
        └── 传感器数据开始自动流入环形缓冲区
              ├── 每 10ms:  IMU SPI burst read → push_imu_sample → ringbuf
              └── 每 250ms: AP3216C regmap read ×6 → push_sample → ringbuf
```

### 19.2 用户态采集流程 (典型)

```
用户程序 (indaq_test / LVGL UI)
  │
  ├── open("/dev/indaq")           → indaq_open()
  │     └── filp->private_data = indev
  │
  ├── ioctl(START_CAPTURE)         → indaq_ioctl() case 3
  │     ├── indaq_ringbuf_reset()  → head=0, tail=0, count=0
  │     └── atomic_set(capture_active, 1)
  │
  ├── read(fd, buf, N*28)          → indaq_read()
  │     ├── kmalloc_array(N, 28)
  │     ├── wait_event_interruptible(read_wq, ringbuf->count > 0)
  │     │     └── 阻塞等待传感器 worker wake_up 唤醒
  │     ├── indaq_ringbuf_read()   → memcpy × n samples
  │     ├── copy_to_user(buf, n*28)
  │     └── kfree()
  │
  ├── ... 数据到达 ...
  │
  ├── ioctl(STOP_CAPTURE)          → indaq_ioctl() case 4
  │     └── atomic_set(capture_active, 0)
  │
  └── close(fd)                    → indaq_release()
```

### 19.3 所有 syscall 路径汇总

```
用户态 syscall              内核函数                    数据结构
────────────────────────────────────────────────────────────────
open()                    → indaq_open()             container_of → indev
read()                    → indaq_read()             ringbuf→buf[head/tail]
close()                   → indaq_release()
ioctl(GET_INFO)           → indaq_ioctl(): info
ioctl(START)              → indaq_ioctl(): ringbuf_reset + capture_active
ioctl(STOP)               → indaq_ioctl(): capture_active = 0
ioctl(SET_SAMPLING_RATE)  → indaq_ioctl(): → indaq_i2csens_set_interval()
poll()                    → 内核默认 poll + wake_up_interruptible
```

### 19.4 IIO sysfs 读数路径

```
用户态                         内核函数
───────────────────────────────────────────────
cat in_accel_x_raw   → iio_dev→info→read_raw()
                         → indaq_iio_read_raw()
                           → imu->ax (缓存, 无 SPI!)
                           → IIO_VAL_INT

cat in_illuminance_raw → indaq_iio_read_raw()
                           → indaq_i2csens_get_data()
                             → g_ap3216c_sens->als_value
                           → IIO_VAL_INT
```

### 19.5 Input 事件路径

```
内核定时器 (20ms)
  ├── indaq_input_worker()
  │     ├── input_report_abs(ABS_X, imu->ax)     // 上报加速度
  │     ├── input_report_abs(ABS_Y, imu->ay)
  │     ├── input_report_abs(ABS_Z, imu->az)
  │     └── detect_tap(priv, imu->az)            // 三击检测
  │
  └── 用户态 evtest → /dev/input/event*
```

### 19.6 GPIO 中断路径

```
KEY0 按钮按下 (下降沿)
  ├── [硬件 IRQ] indaq_gpio_irq_hard()
  │     └── IRQ_WAKE_THREAD
  │
  └── [线程 IRQ] indaq_gpio_irq_thread()
        ├── msleep(50)                    // 消抖
        ├── gpio_get_value() 确认低电平   // 防误触发
        └── toggle: capture_active 0↔1    // 启停切换
```

### 19.7 LVGL UI 刷新路径 (每 200ms)

```
lv_timer_handler()
  │
  └── update_sensor_data()
        │
        ├── [1] indaq_read(fd, batch, 32)
        │     └── 内核: 批量取出 ringbuf 32 样本
        │
        ├── [2] LVGL label_set_text_fmt() × 12 个标签
        │     ├── lbl_als_line:  ALS/PS/IR/OBJ
        │     ├── lbl_accel_line: X/Y/Z (g)
        │     ├── lbl_gyro_line:  GX/GY/GZ (dps)
        │     └── lbl_temp_line:  temperature (°C)
        │
        ├── [3] debugfs_read_u32() × 2    // ringbuf_capacity, count
        ├── [4] debugfs_read_line() × 1   // calib_params
        ├── [5] fopen/stats + fscanf × 5  // pm, gpio, tap, imu, iio
        │
        └── [6] LVGL label_set_text_fmt() × 8 个系统状态
```

### 19.8 模块卸载路径

```
rmmod indaq
  │
  ├── indaq_module_exit()
  │     ├── indaq_unregister_imu_driver()  → icm20608_remove()
  │     │     ├── cancel_delayed_work_sync()  // 停止 IMU worker
  │     │     └── icm20608_write_reg(SLEEP)   // 芯片休眠
  │     │
  │     ├── indaq_unregister_i2c_driver()  → ap3216c_remove()
  │     │     ├── cancel_delayed_work_sync()  // 停止 I2C worker
  │     │     └── regmap_write(DISABLE)       // 禁用传感器
  │     │
  │     └── platform_driver_unregister() → indaq_remove()
  │           ├── indaq_input_exit()         → input_unregister_device
  │           ├── indaq_calib_exit()         → debugfs_remove_recursive
  │           ├── indaq_pm_exit()            → unregister_pm_notifier
  │           ├── indaq_debug_exit()         → debugfs_remove_recursive
  │           ├── indaq_iio_exit()           → iio_device_unregister
  │           ├── indaq_gpioctrl_exit()      → free_irq + gpio_free
  │           ├── indaq_ringbuf_destroy()    → kfree(buf) + kfree(rb)
  │           ├── device_destroy / class_destroy
  │           ├── cdev_del / unregister_chrdev_region
  │           └── /dev/indaq 消失
```

---

## 附录 A: 关键数据结构大小与偏移

```c
struct indaq_sample {     // 28 bytes (__packed)
    u64 ts_ns;            //  0: 时间戳
    u16 als;              //  8: 环境光
    u16 ps;               // 10: 接近
    u16 ir;               // 12: 红外
    s16 ax, ay, az;       // 14: 加速度 (6B)
    s16 temp;             // 20: 温度
    s16 gx, gy, gz;       // 22: 角速度 (6B)
};

struct indaq_ringbuf {    // 28 bytes + 114688 bytes buffer
    struct indaq_sample *buf;  // 指向 4096 × 28 = 114688 bytes
    u32 capacity;              // = 4096
    u32 head, tail, count;     // 生产者、消费者、计数
    spinlock_t lock;
};

struct indaq_device {     // 核心结构体 (约 200 bytes)
    struct platform_device *pdev;
    struct device *dev;
    struct cdev cdev;
    struct class *cls;
    dev_t devno;
    atomic_t capture_active;
    u32 total_samples, sampling_rate;
    struct indaq_sample latest_sample;   // 融合样本
    struct indaq_ringbuf *ringbuf;
    wait_queue_head_t read_wq;
    struct mutex lock;
    void *i2csens, *imu, *gpioctrl, *iio, *calib, *input_dev;
    struct dentry *debug_dir;
};
```

## 附录 B: IOCTL 命令编码

```c
#define INDAQ_IOCTL_GET_INFO          _IOR('I', 0, struct indaq_info)
  // 编码: 0x80104900
  // 方向: 读, 大小: 16 bytes (4 × u32)

#define INDAQ_IOCTL_START_CAPTURE     _IO('I', 3)
  // 编码: 0x00004903
  // 方向: 无数据

#define INDAQ_IOCTL_STOP_CAPTURE      _IO('I', 4)
  // 编码: 0x00004904

#define INDAQ_IOCTL_SET_SAMPLING_RATE _IOW('I', 5, u32)
  // 编码: 0x40044905
  // 方向: 写, 大小: 4 bytes (u32)
```

## 附录 C: 转换公式

```
AP3216C:
  ALS (lux)     = raw * 0.01              // 1 LSB = 0.01 lux
  PS (counts)   = raw (10-bit)
  IR (counts)   = raw (10-bit)
  OBJ 检测      = PS > 100  (经验阈值)

ICM-20608 (±16g, ±2000°/s):
  Accel (g)     = raw / 2048
  Gyro  (°/s)   = raw / 16.4
  Temp  (°C)    = (raw - 521) / 340      // 或 raw / 333.87 + 25
```

## 附录 D: DebugFS 速查表

```bash
# 挂载
mount -t debugfs none /sys/kernel/debug

# 整体状态
cat /sys/kernel/debug/indaq/stats

# 环形缓冲区
cat /sys/kernel/debug/indaq/ringbuf

# IMU 寄存器读写
echo 0x75 > /sys/kernel/debug/indaq/reg_peek && cat /sys/kernel/debug/indaq/reg_peek
echo 0x6B=0x01 > /sys/kernel/debug/indaq/reg_poke

# IMU 最新数据快照
cat /sys/kernel/debug/indaq/imu_raw

# 校准
echo 1 > /sys/kernel/debug/indaq/calib_gyro    # 开始陀螺仪校准
cat /sys/kernel/debug/indaq/calib_params       # 查看校准结果
```
