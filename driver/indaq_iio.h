/* SPDX-License-Identifier: GPL-2.0 */
/*
 * indaq_iio.h - INDAQ IIO subsystem driver header
 *
 * Exposes AP3216C (ALS/PS/IR) and ICM-20608 (6-axis IMU + temp)
 * via the Linux IIO framework.
 */

#ifndef __INDAQ_IIO_H__
#define __INDAQ_IIO_H__

#include <linux/types.h>
#include <linux/iio/iio.h>

/* Forward declaration */
struct indaq_device;

/* Private data structure for IIO subsystem */
struct indaq_iio {
	struct iio_dev *indio_dev;
	struct indaq_device *indev;	/* back-pointer for read_raw */
};

/* Function declarations matching indaq_core.h */
int indaq_iio_init(struct indaq_device *indev);
void indaq_iio_exit(struct indaq_device *indev);

#endif /* __INDAQ_IIO_H__ */
