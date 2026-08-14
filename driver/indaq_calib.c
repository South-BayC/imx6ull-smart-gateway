// SPDX-License-Identifier: GPL-2.0
/*
 * indaq_calib.c - INDAQ IMU calibration subsystem
 *
 * Gyro zero-offset calibration: averages N frames of gyro data while
 * the device is assumed stationary. The resulting offsets are stored
 * and can be retrieved by the IIO layer to correct raw readings.
 *
 * Accel scale correction is computed relative to the gravity reference
 * on the Z axis when stationary.
 *
 * All operations are triggered via DebugFS entries created by this
 * module under /sys/kernel/debug/indaq/.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/seq_file.h>
#include "indaq_core.h"
#include "indaq_imu.h"
#include "indaq_calib.h"

/* Number of samples to average for gyro zero-offset */
#define CALIB_GYRO_SAMPLES	100

/* ======== Calibration Operations ======== */

/*
 * calib_measure_gyro_offset - collect samples and compute gyro bias.
 *
 * Reads the IMU's cached gyro values @interval_ms apart for
 * CALIB_GYRO_SAMPLES iterations, averages them, and stores the
 * result in indev->calib.
 *
 * Caller must ensure the device is stationary during calibration.
 */
int calib_measure_gyro_offset(struct indaq_device *indev)
{
	struct indaq_calib_params *cal = indev->calib;
	struct indaq_imu *imu = indev->imu ? indev->imu : icm20608_get_imu();
	int i;
	s32 sum_x = 0, sum_y = 0, sum_z = 0;

	if (!imu)
		return -ENODEV;

	dev_info(indev->dev,
		 "Gyro offset calibration: collecting %d samples...\n",
		 CALIB_GYRO_SAMPLES);

	for (i = 0; i < CALIB_GYRO_SAMPLES; i++) {
		sum_x += imu->gx;
		sum_y += imu->gy;
		sum_z += imu->gz;
		msleep(imu->interval_ms);
	}

	cal->gyro_offset[0] = (s16)div_s64(sum_x, CALIB_GYRO_SAMPLES);
	cal->gyro_offset[1] = (s16)div_s64(sum_y, CALIB_GYRO_SAMPLES);
	cal->gyro_offset[2] = (s16)div_s64(sum_z, CALIB_GYRO_SAMPLES);
	cal->gyro_samples = CALIB_GYRO_SAMPLES;
	cal->calibrated = true;

	dev_info(indev->dev,
		 "Gyro offset calibrated: %d %d %d (from %d samples)\n",
		 cal->gyro_offset[0], cal->gyro_offset[1],
		 cal->gyro_offset[2], CALIB_GYRO_SAMPLES);
	return 0;
}

/* ======== Accel Scale Calibration ======== */

/*
 * calib_measure_accel_scale - measure Z-axis to compute scale correction.
 *
 * With device stationary and flat, the Z-axis should read ~2048 LSB
 * (for ±16g, 1g = 2048 LSB).  The correction factor is:
 *   corr = 2048 / measured_az
 *
 * Userspace multiplies the base scale (0.000488 g/LSB) by this factor,
 * or the IIO layer applies it via read_raw.
 *
 * Returns 0 on success.
 */
int calib_measure_accel_scale(struct indaq_device *indev)
{
	struct indaq_calib_params *cal = indev->calib;
	struct indaq_imu *imu = indev->imu ? indev->imu : icm20608_get_imu();
	int i;
	s32 sum_z = 0;

	if (!imu)
		return -ENODEV;

	dev_info(indev->dev,
		 "Accel scale calibration: collecting %d Z-axis samples...\n",
		 CALIB_GYRO_SAMPLES);

	for (i = 0; i < CALIB_GYRO_SAMPLES; i++) {
		sum_z += imu->az;
		msleep(imu->interval_ms);
	}

	cal->accel_z_ref = (s16)div_s64(sum_z, CALIB_GYRO_SAMPLES);

	/*
	 * Correction factor: expected 2048 / measured.
	 * Store as fraction (2048 / accel_z_ref) to avoid floating point.
	 * IIO will use num/den to adjust base scale.
	 */
	if (cal->accel_z_ref > 0) {
		cal->accel_scale_num = 2048;
		cal->accel_scale_den = (u16)cal->accel_z_ref;
	} else {
		cal->accel_scale_num = 1;
		cal->accel_scale_den = 1;
	}
	cal->accel_calibrated = true;

	dev_info(indev->dev,
		 "Accel scale calibrated: Z_ref=%d, correction=%u/%u\n",
		 cal->accel_z_ref,
		 cal->accel_scale_num, cal->accel_scale_den);
	return 0;
}

/* ======== DebugFS Interface ======== */

/*
 * calib_params_show - dump current calibration parameters.
 */
static int calib_params_show(struct seq_file *m, void *v)
{
	struct indaq_device *indev = m->private;
	struct indaq_calib_params *cal = indev->calib;

	if (!cal) {
		seq_puts(m, "Calibration: not initialized\n");
		return 0;
	}

	seq_printf(m, "calibrated:     %s\n",
		   cal->calibrated ? "yes" : "no");
	seq_printf(m, "gyro_samples:   %d\n", cal->gyro_samples);
	seq_printf(m, "gyro_offset.x:  %d\n", cal->gyro_offset[0]);
	seq_printf(m, "gyro_offset.y:  %d\n", cal->gyro_offset[1]);
	seq_printf(m, "gyro_offset.z:  %d\n", cal->gyro_offset[2]);
	seq_printf(m, "accel_calibrated: %s\n",
		   cal->accel_calibrated ? "yes" : "no");
	seq_printf(m, "accel_z_ref:    %d\n", cal->accel_z_ref);
	seq_printf(m, "accel_scale_correction: %u/%u\n",
		   cal->accel_scale_num, cal->accel_scale_den);
	return 0;
}
static int calib_params_open(struct inode *inode, struct file *file)
{
	return single_open(file, calib_params_show, inode->i_private);
}

static const struct file_operations calib_params_fops = {
	.owner   = THIS_MODULE,
	.open    = calib_params_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/*
 * calib_gyro_write - trigger gyro zero-offset calibration.
 * Any write (e.g. "echo 1 > calib_gyro") starts the process.
 */
static ssize_t calib_gyro_write(struct file *file,
				const char __user *user_buf,
				size_t len, loff_t *off)
{
	struct indaq_device *indev = file_inode(file)->i_private;
	int ret;

	ret = calib_measure_gyro_offset(indev);
	if (ret)
		dev_err(indev->dev, "Gyro calibration failed: %d\n", ret);

	return len;
}

static const struct file_operations calib_gyro_fops = {
	.write	= calib_gyro_write,
	.open	= simple_open,
	.llseek = noop_llseek,
};

/*
 * calib_accel_write - trigger accel scale calibration.
 * Any write (e.g. "echo 1 > calib_accel") starts the process.
 */
static ssize_t calib_accel_write(struct file *file,
				 const char __user *user_buf,
				 size_t len, loff_t *off)
{
	struct indaq_device *indev = file_inode(file)->i_private;
	int ret;

	ret = calib_measure_accel_scale(indev);
	if (ret)
		dev_err(indev->dev, "Accel calibration failed: %d\n", ret);

	return len;
}

static const struct file_operations calib_accel_fops = {
	.write	= calib_accel_write,
	.open	= simple_open,
	.llseek = noop_llseek,
};

/* ======== Init / Exit ======== */

int indaq_calib_init(struct indaq_device *indev)
{
	struct indaq_calib_params *cal;

	cal = devm_kzalloc(indev->dev, sizeof(*cal), GFP_KERNEL);
	if (!cal)
		return -ENOMEM;

	indev->calib = cal;

	/* Create DebugFS entries under the indaq debugfs directory */
	if (indev->debug_dir) {
		debugfs_create_file("calib_params", 0444,
				    indev->debug_dir, indev,
				    &calib_params_fops);
		debugfs_create_file("calib_gyro", 0200,
				    indev->debug_dir, indev,
				    &calib_gyro_fops);
		debugfs_create_file("calib_accel", 0200,
				    indev->debug_dir, indev,
				    &calib_accel_fops);
	}

	dev_info(indev->dev, "Calibration subsystem initialized\n");
	return 0;
}

void indaq_calib_exit(struct indaq_device *indev)
{
	/* DebugFS files are removed recursively by indaq_debug_exit */
	indev->calib = NULL;
}

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("INDAQ Driver Team");
MODULE_DESCRIPTION("INDAQ IMU calibration subsystem");
