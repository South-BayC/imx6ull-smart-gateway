/* SPDX-License-Identifier: GPL-2.0 */
/*
 * indaq_pm.h - INDAQ Power Management
 *
 * System suspend/resume tracking for the INDAQ driver.
 * Actual I2C sensor power-down is delegated to the I2C
 * subsystem's PM framework; this module provides a suspend
 * flag that the core can check before pushing samples.
 */

#ifndef __INDAQ_PM_H__
#define __INDAQ_PM_H__

#include <linux/types.h>
#include <linux/notifier.h>

struct indaq_device;

struct indaq_pm {
	struct notifier_block pm_nb;
	bool suspended;
};

int indaq_pm_init(struct indaq_device *indev);
void indaq_pm_exit(struct indaq_device *indev);

/* Helper: is the system currently suspended? */
bool indaq_pm_is_suspended(void);

#endif /* __INDAQ_PM_H__ */
