// SPDX-License-Identifier: GPL-2.0
/*
 * indaq_iio.c - INDAQ IIO subsystem driver
 *
 * Exposes both AP3216C light/proximity/IR sensors and ICM-20608
 * 6-axis IMU (accel + gyro + temperature) via the Linux IIO framework.
 *
 * Kernel 4.1 compatible API: uses iio_device_alloc / iio_device_register
 * (no devm_ helpers for IIO on 4.1).
 *
 * IMU channels read cached values from indaq_imu (updated at 100 Hz
 * by the delayed workqueue) — no SPI transaction per read_raw call.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include "indaq_core.h"
#include "indaq_imu.h"
#include "indaq_i2csens.h"
#include "indaq_calib.h"
#include "indaq_iio.h"

/*
 * Combined IIO channels: AP3216C (3) + ICM-20608 (7) = 10 channels.
 *
 * Channel layout:
 *   0: IIO_LIGHT        - AP3216C ALS
 *   1: IIO_PROXIMITY    - AP3216C PS
 *   2: IIO_INTENSITY    - AP3216C IR
 *   3: IIO_ACCEL X      - ICM-20608 accel X
 *   4: IIO_ACCEL Y      - ICM-20608 accel Y
 *   5: IIO_ACCEL Z      - ICM-20608 accel Z
 *   6: IIO_ANGL_VEL X   - ICM-20608 gyro X
 *   7: IIO_ANGL_VEL Y   - ICM-20608 gyro Y
 *   8: IIO_ANGL_VEL Z   - ICM-20608 gyro Z
 *   9: IIO_TEMP         - ICM-20608 temperature
 */
enum indaq_iio_chan_idx {
	IIO_CHAN_ALS = 0,
	IIO_CHAN_PS,
	IIO_CHAN_IR,
	IIO_CHAN_ACCEL_X,
	IIO_CHAN_ACCEL_Y,
	IIO_CHAN_ACCEL_Z,
	IIO_CHAN_GYRO_X,
	IIO_CHAN_GYRO_Y,
	IIO_CHAN_GYRO_Z,
	IIO_CHAN_TEMP,
	IIO_CHAN_NUM,
};

static const struct iio_chan_spec indaq_iio_channels[] = {
	/* ---------- AP3216C (3 channels — scan_index=-1 excludes from buffer) ---------- */
	{
		.type = IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.channel = 0,
		.address = IIO_CHAN_ALS,
		.scan_index = -1,
	},
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.channel = 1,
		.address = IIO_CHAN_PS,
		.scan_index = -1,
	},
	{
		.type = IIO_INTENSITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.channel = 2,
		.address = IIO_CHAN_IR,
		.scan_index = -1,
	},

	/* ---------- ICM-20608 Accel (3 channels) ---------- */
	{
		.type = IIO_ACCEL,
		.modified = 1,
		.channel2 = IIO_MOD_X,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
		.channel = 3,
		.address = IIO_CHAN_ACCEL_X,
		.scan_index = 0,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_LE,
		},
	},
	{
		.type = IIO_ACCEL,
		.modified = 1,
		.channel2 = IIO_MOD_Y,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
		.channel = 4,
		.address = IIO_CHAN_ACCEL_Y,
		.scan_index = 1,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_LE,
		},
	},
	{
		.type = IIO_ACCEL,
		.modified = 1,
		.channel2 = IIO_MOD_Z,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
		.channel = 5,
		.address = IIO_CHAN_ACCEL_Z,
		.scan_index = 2,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_LE,
		},
	},

	/* ---------- ICM-20608 Gyro (3 channels) ---------- */
	{
		.type = IIO_ANGL_VEL,
		.modified = 1,
		.channel2 = IIO_MOD_X,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
		.channel = 6,
		.address = IIO_CHAN_GYRO_X,
		.scan_index = 3,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_LE,
		},
	},
	{
		.type = IIO_ANGL_VEL,
		.modified = 1,
		.channel2 = IIO_MOD_Y,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
		.channel = 7,
		.address = IIO_CHAN_GYRO_Y,
		.scan_index = 4,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_LE,
		},
	},
	{
		.type = IIO_ANGL_VEL,
		.modified = 1,
		.channel2 = IIO_MOD_Z,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
		.channel = 8,
		.address = IIO_CHAN_GYRO_Z,
		.scan_index = 5,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_LE,
		},
	},

	/* ---------- ICM-20608 Temperature ---------- */
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_OFFSET),
		.channel = 9,
		.address = IIO_CHAN_TEMP,
		.scan_index = 6,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_LE,
		},
	},
};

/* Scale factors for ICM-20608 at current full-scale settings */
/*
 * Accel: ±16g → 2048 LSB/g
 *   scale = 1/2048 g/LSB ≈ 0.000488281 g/LSB
 *   IIO_VAL_INT_PLUS_MICRO: val=0, val2=488000 → 0.488 mg/LSB
 *
 * Gyro: ±2000dps → 16.4 LSB/dps
 *   scale = 1/16.4 dps/LSB ≈ 0.0609756 dps/LSB
 *   IIO_VAL_INT_PLUS_MICRO: val=0, val2=60976 → 0.061 °/s/LSB
 *
 * Temperature: 340 LSB/°C, offset 521 LSB (at 0°C)
 *   T(°C) = (raw - temp_offset) / 340
 *   We report raw and provide scale + offset for userspace calculation.
 */
#define ICM20608_ACCEL_SCALE_16G_VAL	0
#define ICM20608_ACCEL_SCALE_16G_VAL2	488	   /* 1/2048 ≈ 0.000488 g/LSB */

#define ICM20608_GYRO_SCALE_2000DPS_VAL	0
#define ICM20608_GYRO_SCALE_2000DPS_VAL2	60976  /* 1/16.4 ≈ 0.060976 °/s/LSB */

#define ICM20608_TEMP_SCALE_VAL		0
#define ICM20608_TEMP_SCALE_VAL2	2941   /* 1/340 ≈ 0.002941 °C/LSB */
#define ICM20608_TEMP_OFFSET_VAL	8500
#define ICM20608_TEMP_OFFSET_VAL2	0

/* ======== IIO Callbacks ======== */

static int indaq_iio_read_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int *val, int *val2, long mask)
{
	struct indaq_iio *iio = iio_priv(indio_dev);
	struct indaq_device *indev = iio->indev;
	struct indaq_imu *imu = indev->imu ? indev->imu : icm20608_get_imu();
	struct indaq_calib_params *cal = indev->calib;
	u16 als, ps, ir;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->address) {
		/* --- AP3216C channels --- */
		case IIO_CHAN_ALS:
		case IIO_CHAN_PS:
		case IIO_CHAN_IR:
			ret = indaq_i2csens_get_data(&als, &ps, &ir);
			if (ret)
				return ret;
			switch (chan->address) {
			case IIO_CHAN_ALS: *val = als; break;
			case IIO_CHAN_PS:  *val = ps;  break;
			case IIO_CHAN_IR:  *val = ir;  break;
			}
			return IIO_VAL_INT;

		/* --- IMU accel channels --- */
		case IIO_CHAN_ACCEL_X: *val = imu->ax; return IIO_VAL_INT;
		case IIO_CHAN_ACCEL_Y: *val = imu->ay; return IIO_VAL_INT;
		case IIO_CHAN_ACCEL_Z: *val = imu->az; return IIO_VAL_INT;

		/* --- IMU gyro channels (apply calibrated offset) --- */
		case IIO_CHAN_GYRO_X:
			*val = imu->gx;
			if (cal && cal->calibrated)
				*val -= cal->gyro_offset[0];
			return IIO_VAL_INT;
		case IIO_CHAN_GYRO_Y:
			*val = imu->gy;
			if (cal && cal->calibrated)
				*val -= cal->gyro_offset[1];
			return IIO_VAL_INT;
		case IIO_CHAN_GYRO_Z:
			*val = imu->gz;
			if (cal && cal->calibrated)
				*val -= cal->gyro_offset[2];
			return IIO_VAL_INT;

		/* --- IMU temperature --- */
		case IIO_CHAN_TEMP:
			*val = imu->temp;
			return IIO_VAL_INT;

		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_SCALE:
		switch (chan->address) {
		case IIO_CHAN_ALS:
			/* AP3216C ALS: 0.01 lux per LSB */
			*val = 0;
			*val2 = 10000;
			return IIO_VAL_INT_PLUS_MICRO;

		case IIO_CHAN_ACCEL_X:
		case IIO_CHAN_ACCEL_Y:
		case IIO_CHAN_ACCEL_Z:
			*val = ICM20608_ACCEL_SCALE_16G_VAL;
			*val2 = ICM20608_ACCEL_SCALE_16G_VAL2;
			/* Apply accel scale correction if calibrated */
			if (cal && cal->accel_calibrated &&
			    cal->accel_scale_den > 0) {
				*val2 = (*val2 * cal->accel_scale_num)
					/ cal->accel_scale_den;
			}
			return IIO_VAL_INT_PLUS_MICRO;

		case IIO_CHAN_GYRO_X:
		case IIO_CHAN_GYRO_Y:
		case IIO_CHAN_GYRO_Z:
			*val = ICM20608_GYRO_SCALE_2000DPS_VAL;
			*val2 = ICM20608_GYRO_SCALE_2000DPS_VAL2;
			return IIO_VAL_INT_PLUS_MICRO;

		case IIO_CHAN_TEMP:
			*val = ICM20608_TEMP_SCALE_VAL;
			*val2 = ICM20608_TEMP_SCALE_VAL2;
			return IIO_VAL_INT_PLUS_MICRO;

		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_OFFSET:
		if (chan->address != IIO_CHAN_TEMP)
			return -EINVAL;
		*val = ICM20608_TEMP_OFFSET_VAL;
		*val2 = ICM20608_TEMP_OFFSET_VAL2;
		return IIO_VAL_INT_PLUS_MICRO;

	default:
		return -EINVAL;
	}
}

/* IIO info structure */
static const struct iio_info indaq_iio_info = {
	.read_raw	= indaq_iio_read_raw,
	.driver_module	= THIS_MODULE,
};

/* ======== IIO Init / Exit ======== */

int indaq_iio_init(struct indaq_device *indev)
{
	struct indaq_iio *iio;
	struct iio_dev *indio_dev;
	int ret;

	dev_info(indev->dev, "Initializing IIO subsystem\n");

	/* Allocate IIO device (kernel 4.1 API — no devm_) */
	indio_dev = iio_device_alloc(sizeof(*iio));
	if (!indio_dev) {
		dev_err(indev->dev, "Failed to allocate IIO device\n");
		return -ENOMEM;
	}

	iio = iio_priv(indio_dev);
	iio->indio_dev = indio_dev;
	iio->indev = indev;

	/* Configure IIO device */
	indio_dev->name = "indaq";
	indio_dev->dev.parent = indev->dev;
	indio_dev->info = &indaq_iio_info;
	indio_dev->channels = indaq_iio_channels;
	indio_dev->num_channels = ARRAY_SIZE(indaq_iio_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	/* Register IIO device */
	ret = iio_device_register(indio_dev);
	if (ret) {
		dev_err(indev->dev, "Failed to register IIO device: %d\n",
			ret);
		iio_device_free(indio_dev);
		return ret;
	}

	/* Store private data in core device */
	indev->iio = iio;

	dev_info(indev->dev,
		 "IIO initialized: %d channels (AP3216C+ICM20608)\n",
		 indio_dev->num_channels);
	return 0;
}

void indaq_iio_exit(struct indaq_device *indev)
{
	struct indaq_iio *iio = indev->iio;

	if (!iio || !iio->indio_dev)
		return;

	dev_info(indev->dev, "Exiting IIO subsystem\n");

	iio_device_unregister(iio->indio_dev);
	iio_device_free(iio->indio_dev);

	indev->iio = NULL;
}

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("INDAQ IIO driver (AP3216C + ICM-20608)");
