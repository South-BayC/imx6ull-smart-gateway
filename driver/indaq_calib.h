/* SPDX-License-Identifier: GPL-2.0 */
/*
 * indaq_calib.h - INDAQ IMU calibration subsystem
 *
 * Provides gyro zero-offset calibration and accel scale correction
 * for ICM-20608. Triggered via DebugFS.
 */

#ifndef __INDAQ_CALIB_H__
#define __INDAQ_CALIB_H__

#include <linux/types.h>

/* IMU calibration parameters */
struct indaq_calib_params {
	s16 gyro_offset[3];	/* X/Y/Z gyro zero-bias */
	bool calibrated;	/* gyro calibration completed flag */
	int gyro_samples;	/* number of samples used for offset */

	/* Accel scale correction */
	s16 accel_z_ref;	/* measured Z-axis raw (stationary, flat) */
	u16 accel_scale_num;	/* scale correction numerator */
	u16 accel_scale_den;	/* scale correction denominator */
	bool accel_calibrated;	/* accel calibration completed flag */
};

/* Sub-device init/exit */
int  indaq_calib_init(struct indaq_device *indev);
void indaq_calib_exit(struct indaq_device *indev);

/* Calibration operations */
int calib_measure_gyro_offset(struct indaq_device *indev);
int calib_measure_accel_scale(struct indaq_device *indev);

#endif /* __INDAQ_CALIB_H__ */
