/* SPDX-License-Identifier: GPL-2.0 */
/*
 * indaq_imu.h - ICM-20608 6-Axis IMU Driver Header
 *
 * ICM-20608 is a 6-axis MEMS IMU (3-axis accel + 3-axis gyro + temp)
 * from InvenSense/TDK. Connected on ECSPI3.
 *
 * Register map is MPU-6050-family compatible with vendor-specific
 * extensions.
 */

#ifndef __INDAQ_IMU_H__
#define __INDAQ_IMU_H__

#include <linux/types.h>

/* ======== ICM-20608 Register Map ======== */

#define ICM20608_REG_ACCEL_XOUT_H		0x3B
#define ICM20608_REG_ACCEL_XOUT_L		0x3C
#define ICM20608_REG_ACCEL_YOUT_H		0x3D
#define ICM20608_REG_ACCEL_YOUT_L		0x3E
#define ICM20608_REG_ACCEL_ZOUT_H		0x3F
#define ICM20608_REG_ACCEL_ZOUT_L		0x40
#define ICM20608_REG_TEMP_OUT_H			0x41
#define ICM20608_REG_TEMP_OUT_L			0x42
#define ICM20608_REG_GYRO_XOUT_H		0x43
#define ICM20608_REG_GYRO_XOUT_L		0x44
#define ICM20608_REG_GYRO_YOUT_H		0x45
#define ICM20608_REG_GYRO_YOUT_L		0x46
#define ICM20608_REG_GYRO_ZOUT_H		0x47
#define ICM20608_REG_GYRO_ZOUT_L		0x48

/* Burst read length: 14 bytes (accel3 + temp + gyro3) */
#define ICM20608_BURST_READ_LEN			14

#define ICM20608_REG_SMPLRT_DIV		0x19
#define ICM20608_REG_CONFIG			0x1A
#define ICM20608_REG_GYRO_CONFIG		0x1B
#define ICM20608_REG_ACCEL_CONFIG		0x1C
#define ICM20608_REG_ACCEL_CONFIG2		0x1D
#define ICM20608_REG_INT_PIN_CFG		0x37
#define ICM20608_REG_INT_ENABLE			0x38
#define ICM20608_REG_INT_STATUS		0x3A
#define ICM20608_REG_PWR_MGMT_1		0x6B
#define ICM20608_REG_PWR_MGMT_2		0x6C
#define ICM20608_REG_WHO_AM_I			0x75

/* PWR_MGMT_1 bits */
#define ICM20608_PWR1_DEVICE_RESET		BIT(7)
#define ICM20608_PWR1_SLEEP			BIT(6)
#define ICM20608_PWR1_CLKSEL_PLL		(0x3 << 0)

/* GYRO_CONFIG bits */
#define ICM20608_GYRO_FS_SEL_250DPS		(0x0 << 3)
#define ICM20608_GYRO_FS_SEL_500DPS		(0x1 << 3)
#define ICM20608_GYRO_FS_SEL_1000DPS		(0x2 << 3)
#define ICM20608_GYRO_FS_SEL_2000DPS		(0x3 << 3)

/* ACCEL_CONFIG bits */
#define ICM20608_ACCEL_FS_SEL_2G		(0x0 << 3)
#define ICM20608_ACCEL_FS_SEL_4G		(0x1 << 3)
#define ICM20608_ACCEL_FS_SEL_8G		(0x2 << 3)
#define ICM20608_ACCEL_FS_SEL_16G		(0x3 << 3)

/* ACCEL_CONFIG2 bits */
#define ICM20608_ACCEL_DLPF_CFG_BW_218HZ	0x01

/* WHO_AM_I expected values */
#define ICM20608_WHO_AM_I_VALUE		0xAF
#define ICM20608_WHO_AM_I_MASK		0xFE  /* bit0 is reserved/DOF */

/* ======== Driver Constants ======== */

#define ICM20608_MAX_FREQ			10000000 /* 10 MHz SPI */
#define ICM20608_DEFAULT_INTERVAL_MS		10	   /* 100 Hz */

/* Sensitivity scales */
#define ICM20608_ACCEL_SENSITIVITY_2G		16384
#define ICM20608_ACCEL_SENSITIVITY_4G		8192
#define ICM20608_ACCEL_SENSITIVITY_8G		4096
#define ICM20608_ACCEL_SENSITIVITY_16G		2048

#define ICM20608_GYRO_SENSITIVITY_250DPS	131
#define ICM20608_GYRO_SENSITIVITY_500DPS	65.5
#define ICM20608_GYRO_SENSITIVITY_1000DPS	32.8
#define ICM20608_GYRO_SENSITIVITY_2000DPS	16.4

#define ICM20608_TEMP_SENSITIVITY		340
#define ICM20608_TEMP_OFFSET			521

/* ======== ICM-20608 Driver Structure ======== */

struct indaq_imu {
	struct spi_device	*spi;
	struct device		*dev;

	/* Configuration */
	u32			interval_ms;	/* readout interval */

	/* Workqueue for periodic reads */
	struct delayed_work	work;

	/* Last raw values */
	s16			ax, ay, az;
	s16			temp;
	s16			gx, gy, gz;

	/* Pointer back to core device */
	struct indaq_device	*core_dev;
};

/* ======== API ======== */

/* SPI register access — used by DebugFS reg_peek/reg_poke */
int icm20608_read_reg(struct spi_device *spi, u8 reg, u8 *val);
int icm20608_write_reg(struct spi_device *spi, u8 reg, u8 val);

/* Global IMU instance accessor — for subsystems that probe after IMU */
struct indaq_imu *icm20608_get_imu(void);

/* SPI driver registration — called from indaq_core module init/exit */
int  indaq_register_imu_driver(void);
void indaq_unregister_imu_driver(void);

/* Sub-device init/exit — called from indaq_core probe/remove */
int  indaq_imu_init(struct indaq_device *indev);
void indaq_imu_exit(struct indaq_device *indev);

#endif /* __INDAQ_IMU_H__ */
