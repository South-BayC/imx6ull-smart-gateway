// SPDX-License-Identifier: GPL-2.0
/*
 * indaq_pm.c - INDAQ Power Management
 *
 * Registers a system PM notifier so the driver knows when
 * the system suspends/resumes. On suspend we set a flag so
 * indaq_core can skip pushing samples; on resume we clear it.
 *
 * The AP3216C I2C sensor's power-down is handled by the I2C
 * subsystem's own PM framework automatically.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/suspend.h>
#include <linux/notifier.h>
#include "indaq_core.h"
#include "indaq_pm.h"

static struct indaq_pm *g_pm;

static int indaq_pm_notifier(struct notifier_block *nb,
			     unsigned long event, void *unused)
{
	struct indaq_pm *pm = container_of(nb, struct indaq_pm, pm_nb);

	switch (event) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
		pm->suspended = true;
		pr_info("INDAQ PM: suspending\n");
		return NOTIFY_OK;

	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
		pm->suspended = false;
		pr_info("INDAQ PM: resumed\n");
		return NOTIFY_OK;
	}

	return NOTIFY_DONE;
}

int indaq_pm_init(struct indaq_device *indev)
{
	struct indaq_pm *pm;

	pm = devm_kzalloc(indev->dev, sizeof(*pm), GFP_KERNEL);
	if (!pm)
		return -ENOMEM;

	pm->suspended = false;
	pm->pm_nb.notifier_call = indaq_pm_notifier;

	register_pm_notifier(&pm->pm_nb);

	g_pm = pm;

	dev_info(indev->dev, "PM notifier registered\n");
	return 0;
}

void indaq_pm_exit(struct indaq_device *indev)
{
	struct indaq_pm *pm = g_pm;

	if (!pm)
		return;

	g_pm = NULL;
	unregister_pm_notifier(&pm->pm_nb);

	dev_info(indev->dev, "PM notifier unregistered\n");
}

bool indaq_pm_is_suspended(void)
{
	return g_pm ? g_pm->suspended : false;
}
