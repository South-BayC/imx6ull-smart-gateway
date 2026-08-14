/* SPDX-License-Identifier: GPL-2.0 */
/*
 * indaq_input.h - INDAQ Input subsystem driver header
 *
 * Reports IMU accelerometer data via Linux Input framework as EV_ABS
 * events, and detects triple-tap gestures (BTN_TL).
 */

#ifndef __INDAQ_INPUT_H__
#define __INDAQ_INPUT_H__

#include <linux/types.h>

struct indaq_device;

/* Sub-device init/exit */
int  indaq_input_init(struct indaq_device *indev);
void indaq_input_exit(struct indaq_device *indev);

/* Accessor: get current tap count (-1 if input not initialized) */
int  indaq_input_get_tap_count(struct indaq_device *indev);

#endif /* __INDAQ_INPUT_H__ */
