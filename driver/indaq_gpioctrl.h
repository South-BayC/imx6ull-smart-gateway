/* SPDX-License-Identifier: GPL-2.0 */
/*
 * indaq_gpioctrl.h - GPIO-triggered capture control header
 *
 * Provides GPIO interrupt-based capture start/stop toggle for INDAQ.
 * Reads "indaq,trigger-gpios" from device tree.
 */

#ifndef __INDAQ_GPIOCTRL_H__
#define __INDAQ_GPIOCTRL_H__

#include <linux/types.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>

/* GPIO control private data structure */
struct indaq_gpioctrl {
	int gpio;              /* GPIO pin number */
	int irq;               /* IRQ number mapped from GPIO */
	struct device *dev;    /* Parent device for dev_* logging */
	struct indaq_device *indev; /* Back-reference to core device */
};

/* Sub-device init/exit interfaces (matching indaq_core.h) */
int indaq_gpioctrl_init(struct indaq_device *indev);
void indaq_gpioctrl_exit(struct indaq_device *indev);

#endif /* __INDAQ_GPIOCTRL_H__ */