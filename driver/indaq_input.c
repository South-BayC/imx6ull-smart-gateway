// SPDX-License-Identifier: GPL-2.0
/*
 * indaq_input.c - INDAQ Input subsystem driver
 *
 * Registers an input device that reports 3-axis accelerometer data
 * via EV_ABS events and detects triple-tap gestures (BTN_TL).
 *
 * A delayed workqueue periodically reads cached IMU data from the
 * core device structure (no extra SPI traffic) and reports it to
 * the input subsystem.
 *
 * This is completely independent of the IIO and ring buffer paths;
 * it provides an event-driven interface for motion/gesture detection.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include "indaq_core.h"
#include "indaq_imu.h"
#include "indaq_input.h"

/* Input device poll interval: 20 ms = 50 Hz (lightweight, cached reads) */
#define INDAQ_INPUT_INTERVAL_MS		20

/* Triple-tap detection parameters */
#define TAP_THRESHOLD			500	/* ~0.25g change in raw */
#define TAP_WINDOW_MS			200	/* max gap between taps */
#define TAP_COUNT_REQUIRED		3	/* taps to trigger event */

/*
 * Private data for input subsystem.
 * Embedded in the core device's void *input_dev.
 */
struct indaq_input_priv {
	struct input_dev	*input;		/* registered input device */
	struct delayed_work	work;		/* periodic report work */
	struct indaq_device	*indev;		/* back-pointer */

	/* Triple-tap detection state */
	s16			prev_z;
	int			tap_count;
	unsigned long		last_tap_jiffies;
};

/* ======== Triple-Tap Detection ======== */

static void detect_tap(struct indaq_input_priv *priv, s16 current_z)
{
	int delta_z = abs((int)current_z - (int)priv->prev_z);

	if (delta_z > TAP_THRESHOLD) {
		/* Check if this tap is within the time window */
		if (time_after(jiffies,
			       priv->last_tap_jiffies +
			       msecs_to_jiffies(TAP_WINDOW_MS))) {
			/* Window expired — start counting again */
			priv->tap_count = 1;
		} else {
			/* Within window — increment */
			priv->tap_count++;
		}
		priv->last_tap_jiffies = jiffies;

		/* Check for triple-tap */
		if (priv->tap_count >= TAP_COUNT_REQUIRED) {
			input_report_key(priv->input, BTN_TL, 1);
			input_sync(priv->input);
			input_report_key(priv->input, BTN_TL, 0);
			input_sync(priv->input);
			priv->tap_count = 0;

			dev_dbg(priv->indev->dev,
				"Triple-tap detected!\n");
		}
	}

	priv->prev_z = current_z;
}

/* ======== Periodic Work ======== */

static void indaq_input_worker(struct work_struct *work)
{
	struct indaq_input_priv *priv =
		container_of(work, struct indaq_input_priv, work.work);
	struct indaq_device *indev = priv->indev;
	struct indaq_imu *imu = indev->imu ? indev->imu : icm20608_get_imu();

	if (imu) {
		/* Report accelerometer position via ABS */
		input_report_abs(priv->input, ABS_X, imu->ax);
		input_report_abs(priv->input, ABS_Y, imu->ay);
		input_report_abs(priv->input, ABS_Z, imu->az);
		input_sync(priv->input);

		/* Detect tap gestures */
		detect_tap(priv, imu->az);
	}

	/* Re-arm */
	schedule_delayed_work(&priv->work,
			      msecs_to_jiffies(INDAQ_INPUT_INTERVAL_MS));
}

/* ======== Init / Exit ======== */

int indaq_input_get_tap_count(struct indaq_device *indev)
{
    struct indaq_input_priv *priv = indev->input_dev;
    return priv ? priv->tap_count : -1;
}

int indaq_input_init(struct indaq_device *indev)
{
	struct indaq_input_priv *priv;
	struct input_dev *input;
	int ret;

	priv = devm_kzalloc(indev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->indev = indev;

	/* Allocate input device */
	input = input_allocate_device();
	if (!input) {
		dev_err(indev->dev, "Failed to allocate input device\n");
		return -ENOMEM;
	}

	input->name = "INDAQ IMU Accel";
	input->id.bustype = BUS_SPI;
	input->id.vendor = 0x0001;
	input->id.product = 0x0001;
	input->id.version = 0x0100;
	input->dev.parent = indev->dev;

	/* Setup event types and codes */
	set_bit(EV_ABS, input->evbit);
	input_set_abs_params(input, ABS_X, -32768, 32767, 0, 0);
	input_set_abs_params(input, ABS_Y, -32768, 32767, 0, 0);
	input_set_abs_params(input, ABS_Z, -32768, 32767, 0, 0);

	set_bit(EV_KEY, input->evbit);
	set_bit(BTN_TL, input->keybit);   /* triple-tap gesture */

	ret = input_register_device(input);
	if (ret) {
		dev_err(indev->dev,
			"Failed to register input device: %d\n", ret);
		input_free_device(input);
		return ret;
	}

	priv->input = input;
	indev->input_dev = priv;

	/* Start periodic work */
	INIT_DELAYED_WORK(&priv->work, indaq_input_worker);
	schedule_delayed_work(&priv->work,
			      msecs_to_jiffies(INDAQ_INPUT_INTERVAL_MS));

	dev_info(indev->dev,
		 "Input subsystem registered: /dev/input/event*\n");
	return 0;
}

void indaq_input_exit(struct indaq_device *indev)
{
	struct indaq_input_priv *priv = indev->input_dev;

	if (!priv)
		return;

	/* Stop work */
	cancel_delayed_work_sync(&priv->work);

	/* Unregister and free input device */
	if (priv->input) {
		input_unregister_device(priv->input);
		/* input_unregister_device also frees */
	}

	indev->input_dev = NULL;
}

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("INDAQ Driver Team");
MODULE_DESCRIPTION("INDAQ Input subsystem driver (accel + tap detection)");
