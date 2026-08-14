// SPDX-License-Identifier: GPL-2.0
/*
 * indaq_gpioctrl.c - GPIO-triggered capture control implementation
 *
 * Uses threaded IRQ to toggle capture state on GPIO falling edge.
 * Device tree property: "indaq,trigger-gpios" = <&gpio1 18 GPIO_ACTIVE_LOW>;
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include "indaq_core.h"
#include "indaq_i2csens.h"
#include "indaq_gpioctrl.h"

/* Threaded IRQ handler - runs in process context, minimal work */
static irqreturn_t indaq_gpio_irq_thread(int irq, void *dev_id)
{
	struct indaq_gpioctrl *ctrl = dev_id;
	struct indaq_device *indev = ctrl->indev;
	int was_active;

	if (!indev) {
		dev_err(ctrl->dev, "IRQ: no indaq_device\n");
		return IRQ_HANDLED;
	}

	/*
	 * Debounce: mechanical KEY button bounce lasts 5–20 ms.
	 * 50 ms sleep + IRQF_ONESHOT (IRQ masked during handler)
	 * prevents re-trigger from contact chatter.
	 */
	msleep(50);

	/* Re-check GPIO: confirm button is still pressed (active-low = 0).
	 * If it bounced back high we ignore this event.
	 */
	if (gpio_get_value(ctrl->gpio) != 0)
		return IRQ_HANDLED;

	/* Toggle capture state */
	was_active = atomic_read(&indev->capture_active);
	if (was_active) {
		atomic_set(&indev->capture_active, 0);
		dev_info(ctrl->dev, "GPIO trigger: capture STOPPED\n");
	} else {
		/* Reset ring buffer on new capture start */
		if (indev->ringbuf)
			indaq_ringbuf_reset(indev->ringbuf);
		atomic_set(&indev->capture_active, 1);
		dev_info(ctrl->dev, "GPIO trigger: capture STARTED\n");
	}

	return IRQ_HANDLED;
}

/* Hard IRQ handler - just acknowledge, defer to thread */
static irqreturn_t indaq_gpio_irq_hard(int irq, void *dev_id)
{
	/* Return IRQ_WAKE_THREAD to invoke the threaded handler */
	return IRQ_WAKE_THREAD;
}

int indaq_gpioctrl_init(struct indaq_device *indev)
{
	struct device *dev = indev->dev;
	struct indaq_gpioctrl *ctrl;
	int gpio, irq, ret;
	enum of_gpio_flags flags;

	dev_info(dev, "Initializing GPIO capture control...\n");

	/* Allocate private structure */
	ctrl = devm_kzalloc(dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	ctrl->dev = dev;
	ctrl->indev = indev;

	/* Parse "indaq,trigger-gpios" from device tree (optional) */
	gpio = of_get_named_gpio_flags(dev->of_node, "indaq,trigger-gpios", 0, &flags);
	if (gpio < 0) {
		dev_info(dev, "No 'indaq,trigger-gpios' in DT — GPIO trigger disabled (err=%d)\n", gpio);
		devm_kfree(dev, ctrl);
		return 0;  /* optional, not an error */
	}

	ctrl->gpio = gpio;

	/* Request GPIO */
	ret = gpio_request(gpio, "indaq-trigger");
	if (ret) {
		dev_err(dev, "Failed to request GPIO %d: %d\n", gpio, ret);
		devm_kfree(dev, ctrl);
		return ret;
	}

	/* Configure as input (interrupt source) */
	ret = gpio_direction_input(gpio);
	if (ret) {
		dev_err(dev, "Failed to set GPIO %d as input: %d\n", gpio, ret);
		gpio_free(gpio);
		devm_kfree(dev, ctrl);
		return ret;
	}

	/* Map GPIO to IRQ */
	irq = gpio_to_irq(gpio);
	if (irq < 0) {
		dev_err(dev, "Failed to map GPIO %d to IRQ: %d\n", gpio, irq);
		gpio_free(gpio);
		devm_kfree(dev, ctrl);
		return irq;
	}

	ctrl->irq = irq;

	/* Request threaded IRQ: hard handler + thread handler
	 * IRQF_TRIGGER_FALLING for active-low trigger (GPIO_ACTIVE_LOW in DT)
	 * IRQF_ONESHOT ensures hard IRQ is masked until thread completes
	 */
	ret = request_threaded_irq(irq,
				   indaq_gpio_irq_hard,
				   indaq_gpio_irq_thread,
				   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				   "indaq-gpio-trigger",
				   ctrl);
	if (ret) {
		dev_err(dev, "Failed to request threaded IRQ %d: %d\n", irq, ret);
		gpio_free(gpio);
		devm_kfree(dev, ctrl);
		return ret;
	}

	/* Store private data in core device */
	indev->gpioctrl = ctrl;

	dev_info(dev, "GPIO capture control initialized (GPIO=%d, IRQ=%d)\n", gpio, irq);
	return 0;
}

void indaq_gpioctrl_exit(struct indaq_device *indev)
{
	struct indaq_gpioctrl *ctrl = indev->gpioctrl;

	if (!ctrl)
		return;

	dev_info(ctrl->dev, "Cleaning up GPIO capture control...\n");

	/* Free IRQ */
	if (ctrl->irq > 0)
		free_irq(ctrl->irq, ctrl);

	/* Free GPIO */
	if (ctrl->gpio >= 0)
		gpio_free(ctrl->gpio);

	/* Clear reference */
	indev->gpioctrl = NULL;

	dev_info(ctrl->dev, "GPIO capture control cleaned up\n");
}

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("INDAQ GPIO-triggered capture control");