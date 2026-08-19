/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __AP3216C_H
#define __AP3216C_H

/* AP3216C 环境光/接近/红外三合一传感器（P4-B，手册 5.13.2）
 * 硬件: 板载于 I2C1 总线，器件地址 0x1E；无额外接线。
 *
 * 读取协议（read 一次性返回全量）:
 *   open("/dev/ap3216c0") → read(fd, buf, 6) 一次返回 3×u16（小端）:
 *     buf[0..1] = ir   红外原始值（10 位）
 *     buf[2..3] = als  环境光原始值（16 位）
 *     buf[4..5] = ps   距离原始值（10 位，越大越近）
 *
 * 寄存器（正点原子 21_iic/ap3216creg.h + datasheet，板级验证）:
 *   0x00 SYS_CONF   0x04=软复位 → 延时 → 0x03=开 ALS+PS+IR
 *   0x0A/0x0B IR    0x0A.bit7=溢出标志; 有效 10 位: 高8<<2 | 低2
 *   0x0C/0x0D ALS   低/高字节直接拼 16 位（bit15:12 通常为 0）
 *   0x0E/0x0F PS    0x0E.bit6=溢出标志; 有效 10 位: 高6<<4 | 低4
 *
 * ★手册 5.13.2 骨架中 PS 寄存器地址写 0x10 系笔误：
 *   实际为 0x0E/0x0F（与正点原子 ap3216creg.h / datasheet 一致）。
 *   ALS 手册骨架用 (raw>>4)&0x7FF（取 11 位），正点原子直接拼 16 位；
 *   两者在 bit15:12=0 时等价，本驱动采用正点原子拼法（板级验证）。
 */
#define AP3216C_READ_LEN	6	/* ir+als+ps = 3×u16 */

#endif /* __AP3216C_H */
