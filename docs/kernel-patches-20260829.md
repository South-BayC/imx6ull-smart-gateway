# 内核补丁存档（2026-08-29，摄像头链路验收版）

> 内核树：`/home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay`（VM 本地，不入项目 git）
> 本文档记录项目期间对该内核树的三处源码修改，**内核树若重建/丢失，按此文档重放**。
> 背景：详见《项目进度.md》3.8 节（摄像头链路排障全链）。

## 补丁 1：mx6s_capture.c —— CSICR1 FCC 位修正（滚动根治）

**症状**：画面从左向右持续滚动、右侧滚出左侧回绕（捕获侧行同步滑移）。
**定位**：板端 CSICR1 位翻转实验——翻转 bit8（FCC）滚动停止；翻转 HSYNC_POL/GCLK 出现绿花屏（原配置正确）。

两处修改，`drivers/media/platform/mxc/subdev/mx6s_capture.c`：

① `csi_init_interface()`：删除 FCC 置位行（FCC=1 时 FIFO 写时钟与传感器像素流失步）

```c
 	val |= BIT_SOF_POL;
 	val |= BIT_REDGE;
 	val |= BIT_GCLK_MODE;
 	val |= BIT_HSYNC_POL;
-	val |= BIT_FCC;
 	val |= 0 << SHIFT_MCLKDIV;
 	val |= BIT_MCLKEN;
```

② 流使能路径（约 440 行）：FCC 保持 0

```c
 	cr1 = csi_read(csi_dev, CSI_CSICR1);
-	csi_write(csi_dev, cr1 | BIT_FCC, CSI_CSICR1);
+	csi_write(csi_dev, cr1 & ~BIT_FCC, CSI_CSICR1);  /* 08-29: FCC=0 消除水平滚动回绕 */
```

注：驱动另有 ~1080/1085 行两处 `& ~BIT_FCC`（原生即清除语义），现全路径统一 FCC=0。

## 补丁 2：ov5640.c —— 30fps VGA 寄存器组系统分频修正（16→30fps）

**症状**：30fps 档实测只有 16fps；板端 i2c 读回证实传感器时序（VTS 984/HTS 1896）正确但 PCLK 仅 ~30MHz（应 56MHz）。
**定位**：PLL 系统分频寄存器 0x3035=0x21（÷3）在 24MHz MCLK 组合下产出 PCLK 减半；同族其他档位寄存器组（SVGA/720P 等）原生即 0x11（÷2）——NXP VGA blob 的不一致。另：修改 0x3035 的 [7:4] 位会被传感器硬件钳回（写 0x11 读回 0x21 中的低 4 位保留现象），因此**改 0x3037 预分频 + 0x3035 时须与实测一致**。

`drivers/media/platform/mxc/capture/ov5640.c` 三处：

① `ov5640_setting_30fps_VGA_640_480[]`（约 225 行）：0x3037 预分频 3→1（热写实测 26fps）

```c
-	{0x3036, 0x46, 0, 0}, {0x3037, 0x13, 0, 0}, {0x3503, 0x00, 0, 0},
+	{0x3036, 0x46, 0, 0}, {0x3037, 0x11, 0, 0}, {0x3503, 0x00, 0, 0},
```

② 同数组（约 225 行）：0x3035 系统分频 2→1（热写实测 30fps，最终采用）

```c
-	{0x5001, 0xa3, 0, 0}, {0x3034, 0x1a, 0, 0}, {0x3035, 0x21, 0, 0},
+	{0x5001, 0xa3, 0, 0}, {0x3034, 0x1a, 0, 0}, {0x3035, 0x11, 0, 0},
```

③ `ov5640_init_setting_30fps_VGA[]`（125 行，STREAMON 基线数组——**关键**：init_mode 在每次
STREAMON 时软复位并重下载数组，会覆盖 S_PARM 的设置；不同步此处的 0x3035 会导致
30fps 开机即回退 16fps）

```c
-	{0x3034, 0x1a, 0, 0}, {0x3035, 0x21, 0, 0}, {0x3036, 0x46, 0, 0},
+	{0x3034, 0x1a, 0, 0}, {0x3035, 0x11, 0, 0}, {0x3036, 0x46, 0, 0},
```

注意：`ov5640_setting_15fps_VGA_640_480[]`（243 行附近）保持 0x21 不变——15fps 档语义不变。
15fps 档的其他寄存器组（125/207/279/315 行）保持 0x21 不变。

## 补丁 3：dts —— CSI 引脚电气参数提速（信号余量，预防性）

`imx6ull-southbay-emmc.dts` 的 `pinctrl_csi1`：12 个 CSI 引脚 pad 值 `0x1b088 → 0x1b0f9`
（SRE=1 快斜率、DSE=110 强驱动、SPEED=11 高速档）。水平滚动问题期间应用；后续 PCLK 提升后
信号余量更好。已随项目 dts 提交。

## 内核配置（历次重编定案清单，重编必查）

`CONFIG_VIDEO_MXC_CAPTURE=y`、`CONFIG_VIDEO_MXC_CSI_CAMERA=y`（6ULL CSI 主机 mx6s_capture）、
`CONFIG_MXC_CAMERA_OV5640=y`、`CONFIG_SPI_GPIO=y`、`CONFIG_GPIO_74X164=y`（74HC595，
ov5640 pwn/rst 提供者）、`CONFIG_IIO=y`、`CONFIG_VIDEO_MXC_PXP_V4L2` 保持关闭（让位 /dev/video0）。
