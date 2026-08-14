// SPDX-License-Identifier: GPL-2.0
/*
 * indaq_imu.c - ICM-20608 6-Axis IMU SPI Driver
 *
 * ICM-20608 is a 6-axis MEMS IMU from InvenSense/TDK with:
 *   3-axis accelerometer (±2/±4/±8/±16g)
 *   3-axis gyroscope (±250/±500/±1000/±2000 °/s)
 *   Temperature sensor
 *
 * Connected on ECSPI3 (shared bus with no other SPI devices using SS0).
 * Polled via delayed workqueue; pushes samples into the INDAQ ring buffer.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/workqueue.h>
#include "indaq_imu.h"
#include "indaq_core.h"

/* Global pointer for indaq_core to link (kernel 4.1.15 compatible) */
static struct indaq_imu *icm20608_global_imu;

struct indaq_imu *icm20608_get_imu(void)
{
	return icm20608_global_imu;
}
EXPORT_SYMBOL_GPL(icm20608_get_imu);

/* ======== SPI Register Access ======== */

/*
 * icm20608_read_regs - burst read N bytes from ICM-20608 over SPI
 * @spi:   SPI device
 * @reg:   Starting register address (7-bit)
 * @val:   Output buffer (must be at least @len bytes)
 * @len:   Number of bytes to read
 *
 * SPI protocol: send address byte (R/W#=1, ADDR[6:0]), then read @len data
 * bytes.  CS is held active for the entire transaction.
 */
static int icm20608_read_regs(struct spi_device *spi, u8 reg,
			      u8 *val, u32 len)
{
	struct spi_transfer xfer[2] = {};
	struct spi_message msg;
	u8 addr;

	addr = reg | 0x80;	/* R/W# = 1 (read) */

	spi_message_init(&msg);

	xfer[0].tx_buf = &addr;
	xfer[0].len = 1;
	spi_message_add_tail(&xfer[0], &msg);

	xfer[1].rx_buf = val;
	xfer[1].len = len;
	spi_message_add_tail(&xfer[1], &msg);

	return spi_sync(spi, &msg);
}

/*
 * icm20608_read_reg - single register read
 */
int icm20608_read_reg(struct spi_device *spi, u8 reg, u8 *val)
{
	return icm20608_read_regs(spi, reg, val, 1);
}

/*
 * icm20608_write_reg - single register write
 * @spi: SPI device
 * @reg: Register address
 * @val: Value to write
 *
 * SPI protocol: send address byte (R/W#=0, ADDR[6:0]), then write data byte.
 */
int icm20608_write_reg(struct spi_device *spi, u8 reg, u8 val)
{
	u8 buf[2] = { reg & 0x7f, val };	/* R/W# = 0 (write) */

	return spi_write_then_read(spi, buf, 2, NULL, 0);
}

/* ======== Data Readout ======== */

/*
 * icm20608_read_all - burst-read all sensor data in one SPI transaction
 * @imu: ICM-20608 device handle
 *
 * Reads 14 bytes starting at ACCEL_XOUT_H (0x3B), covering:
 *   ACCEL_X/Y/Z (6 bytes), TEMP (2 bytes), GYRO_X/Y/Z (6 bytes)
 *
 * Returns 0 on success, negative errno on failure.
 */
static int icm20608_read_all(struct indaq_imu *imu)
{
	u8 buf[ICM20608_BURST_READ_LEN];
	int ret;

	ret = icm20608_read_regs(imu->spi, ICM20608_REG_ACCEL_XOUT_H,
				 buf, sizeof(buf));
	if (ret < 0) {
		dev_err_ratelimited(imu->dev,
				    "Burst read failed: %d\n", ret);
		return ret;
	}

	/* Accel — big-endian 16-bit signed */
	imu->ax = (s16)((buf[0]  << 8) | buf[1]);
	imu->ay = (s16)((buf[2]  << 8) | buf[3]);
	imu->az = (s16)((buf[4]  << 8) | buf[5]);

	/* Temperature */
	imu->temp = (s16)((buf[6]  << 8) | buf[7]);

	/* Gyro */
	imu->gx = (s16)((buf[8]  << 8) | buf[9]);
	imu->gy = (s16)((buf[10] << 8) | buf[11]);
	imu->gz = (s16)((buf[12] << 8) | buf[13]);

	return 0;
}

/* ======== Periodic Read Worker ======== */

static void icm20608_read_worker(struct work_struct *work)
{
	struct indaq_imu *imu = container_of(work, struct indaq_imu,
					     work.work);
	int ret;

	ret = icm20608_read_all(imu);
	if (ret == 0) {
		/* Push sample into INDAQ ring buffer */
		indaq_push_imu_sample(imu->ax, imu->ay, imu->az,
				      imu->gx, imu->gy, imu->gz,
				      imu->temp);
	}

	/* Re-arm */
	if (imu->interval_ms)
		schedule_delayed_work(&imu->work,
				      msecs_to_jiffies(imu->interval_ms));
}

/* ======== Chip Initialization ======== */

static int icm20608_init(struct indaq_imu *imu)
{
	struct spi_device *spi = imu->spi;
	u8 whoami;
	int ret;

	/* Verify chip identity */
	ret = icm20608_read_reg(spi, ICM20608_REG_WHO_AM_I, &whoami);
	if (ret < 0) {
		dev_err(imu->dev, "Failed to read WHO_AM_I: %d\n", ret);
		return ret;
	}

	/*
	 * ICM-20608 WHO_AM_I is 0xAF (or 0xAE depending on revision).
	 * Some clones / second-source parts return 0xAF masked with 0xFE.
	 */
	if ((whoami & ICM20608_WHO_AM_I_MASK) !=
	    (ICM20608_WHO_AM_I_VALUE & ICM20608_WHO_AM_I_MASK)) {
		dev_warn(imu->dev,
			 "Unexpected WHO_AM_I 0x%02x (expected 0x%02x)\n",
			 whoami, ICM20608_WHO_AM_I_VALUE);
		/* Non-fatal — continue anyway */
	}

	/* Reset device */
	ret = icm20608_write_reg(spi, ICM20608_REG_PWR_MGMT_1,
				 ICM20608_PWR1_DEVICE_RESET);
	if (ret < 0)
		return ret;
	msleep(50);

	/* Wake up: clear sleep, select PLL with best clock source */
	ret = icm20608_write_reg(spi, ICM20608_REG_PWR_MGMT_1,
				 ICM20608_PWR1_CLKSEL_PLL);
	if (ret < 0)
		return ret;
	msleep(10);

	/* Disable I2C interface (SPI-only mode, optional) */
	ret = icm20608_write_reg(spi, ICM20608_REG_INT_PIN_CFG, 0x02);
	if (ret < 0)
		return ret;

	/* Config: DLPF ~184 Hz, gyro bandwidth ~188 Hz */
	ret = icm20608_write_reg(spi, ICM20608_REG_CONFIG, 0x02);
	if (ret < 0)
		return ret;

	/* Gyro: ±2000 °/s full scale */
	ret = icm20608_write_reg(spi, ICM20608_REG_GYRO_CONFIG,
				 ICM20608_GYRO_FS_SEL_2000DPS);
	if (ret < 0)
		return ret;

	/* Accel: ±16g full scale */
	ret = icm20608_write_reg(spi, ICM20608_REG_ACCEL_CONFIG,
				 ICM20608_ACCEL_FS_SEL_16G);
	if (ret < 0)
		return ret;

	/* Accel DLPF ~218 Hz */
	ret = icm20608_write_reg(spi, ICM20608_REG_ACCEL_CONFIG2,
				 ICM20608_ACCEL_DLPF_CFG_BW_218HZ);
	if (ret < 0)
		return ret;

	/* Sample rate divider: 100 Hz (1 kHz internal / (1 + 9) = 100 Hz) */
	ret = icm20608_write_reg(spi, ICM20608_REG_SMPLRT_DIV, 9);
	if (ret < 0)
		return ret;

	dev_info(imu->dev,
		 "ICM-20608 initialized (WHO_AM_I=0x%02x)\n", whoami);
	return 0;
}

/* ======== SPI Driver ======== */

static int icm20608_probe(struct spi_device *spi)
{
	struct indaq_imu *imu;
	struct device *dev = &spi->dev;
	int ret;

	/* Validate DTS compatible: we expect "alientek,icm20608" */
	if (!of_match_node(spi->dev.driver->of_match_table,
			   dev->of_node)) {
		dev_warn(dev, "Unsupported device\n");
		return -ENODEV;
	}

	imu = devm_kzalloc(dev, sizeof(*imu), GFP_KERNEL);
	if (!imu)
		return -ENOMEM;

	imu->spi = spi;
	imu->dev = dev;
	imu->interval_ms = ICM20608_DEFAULT_INTERVAL_MS;

	/* Configure SPI bus parameters */
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	spi->max_speed_hz = min_t(u32, spi->max_speed_hz,
				  ICM20608_MAX_FREQ);
	ret = spi_setup(spi);
	if (ret < 0) {
		dev_err(dev, "SPI setup failed: %d\n", ret);
		return ret;
	}

	spi_set_drvdata(spi, imu);

	/* Initialize chip */
	ret = icm20608_init(imu);
	if (ret < 0) {
		dev_err(dev, "Chip initialization failed: %d\n", ret);
		return ret;
	}

	/* Start periodic read worker */
	INIT_DELAYED_WORK(&imu->work, icm20608_read_worker);
	schedule_delayed_work(&imu->work,
			      msecs_to_jiffies(imu->interval_ms));

	/* Store global pointer for indaq_imu_init */
	icm20608_global_imu = imu;

	dev_info(dev, "ICM-20608 IMU probed at %u Hz\n",
		 1000 / imu->interval_ms);
	return 0;
}

static int icm20608_remove(struct spi_device *spi)
{
	struct indaq_imu *imu = spi_get_drvdata(spi);

	/* Stop worker */
	cancel_delayed_work_sync(&imu->work);

	/* Clear global pointer */
	icm20608_global_imu = NULL;

	/* Put device to sleep */
	icm20608_write_reg(spi, ICM20608_REG_PWR_MGMT_1,
			   ICM20608_PWR1_SLEEP);

	dev_info(&spi->dev, "ICM-20608 removed\n");
	return 0;
}

static const struct spi_device_id icm20608_id[] = {
	{ "icm20608", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, icm20608_id);

static const struct of_device_id icm20608_of_match[] = {
	{ .compatible = "alientek,icm20608" },
	{ .compatible = "invensense,icm20608" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, icm20608_of_match);

static struct spi_driver icm20608_spi_driver = {
	.driver = {
		.name		= "icm20608",
		.of_match_table	= icm20608_of_match,
	},
	.probe		= icm20608_probe,
	.remove		= icm20608_remove,
	.id_table	= icm20608_id,
};

/* ======== INDAQ Sub-Device Integration ======== */

/*
 * These are called from indaq_core's module_init/exit.
 * They register/unregister the SPI driver so that device tree matching
 * triggers icm20608_probe automatically.
 */
int __init indaq_register_imu_driver(void)
{
	return spi_register_driver(&icm20608_spi_driver);
}

void indaq_unregister_imu_driver(void)
{
	spi_unregister_driver(&icm20608_spi_driver);
}

/*
 * indaq_imu_init - called from indaq_core probe.
 * Links the already-probed IMU instance into the core device.
 *
 * We use the global pointer set by icm20608_probe rather than
 * of_find_spi_device_by_node() which does not exist in kernel 4.1.15.
 *
 * If the SPI driver hasn't probed yet (global pointer is NULL), we
 * return 0 with a warning.  The worker will start when the SPI probe
 * completes and in the meantime no samples are pushed.
 */
int indaq_imu_init(struct indaq_device *indev)
{
	struct indaq_imu *imu = icm20608_global_imu;

	if (!imu) {
		dev_info(indev->dev,
			 "ICM-20608 SPI not probed yet\n");
		return 0;
	}

	imu->core_dev = indev;
	indev->imu = imu;

	dev_info(indev->dev, "ICM-20608 linked to INDAQ core\n");
	return 0;
}

void indaq_imu_exit(struct indaq_device *indev)
{
	if (indev->imu) {
		struct indaq_imu *imu = indev->imu;
		imu->core_dev = NULL;
		indev->imu = NULL;
	}
}

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("INDAQ ICM-20608 6-Axis IMU SPI Driver");
