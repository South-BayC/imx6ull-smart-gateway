/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ICM20608_H
#define __ICM20608_H

/* ICM20608 六轴 IMU（P4-B，手册 5.13.3）
 * 硬件: 板载于 ECSPI3 总线（dts: icm20608@0，8MHz，SPI_MODE_0），无额外接线。
 *
 * 读取协议（read 一次性返回全量）:
 *   open("/dev/icm20608") → read(fd, buf, 28) 一次返回 7×s32（小端原始值）:
 *     buf[0..3]   = gyro_x   角速度原始值（×2000/32768 → dps）
 *     buf[4..7]   = gyro_y
 *     buf[8..11]  = gyro_z
 *     buf[12..15] = accel_x  加速度原始值（×16/32768 → g）
 *     buf[16..19] = accel_y
 *     buf[20..23] = accel_z
 *     buf[24..27] = temp     温度原始值（/326.8 + 25 → ℃）
 *
 * 量程配置（probe 时写入，正点原子板级验证值）:
 *   GYRO_CONFIG = 0x18  → ±2000dps
 *   ACCEL_CONFIG = 0x18 → ±16G
 *
 * 换算公式（内核只给原始值，物理量换算在用户态做，避免内核浮点）:
 *   角速度 dps = raw × 2000 / 32768
 *   加速度 g   = raw × 16 / 32768
 *   温度 ℃     = raw / 326.8 + 25
 *   静止验收: accel_z ≈ 1.0g，gyro 三轴 ≈ 0，temp ≈ 室温
 */
#define ICM20608_READ_LEN	28	/* 7×s32 */
#define ICM20608_WORD_CNT	7	/* gyro3 + accel3 + temp */

#endif /* __ICM20608_H */
