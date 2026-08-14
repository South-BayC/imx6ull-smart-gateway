/* SPDX-License-Identifier: GPL-2.0 */
/*
 * indaq_debug.h - INDAQ debugfs interface header
 */

#ifndef __INDAQ_DEBUG_H__
#define __INDAQ_DEBUG_H__

#include <linux/types.h>
#include <linux/debugfs.h>

struct indaq_device;

/* Debugfs private data structure */
struct indaq_debug {
	struct dentry *debug_dir;
};

/* Function declarations matching indaq_core.h */
int indaq_debug_init(struct indaq_device *indev);
void indaq_debug_exit(struct indaq_device *indev);

#endif /* __INDAQ_DEBUG_H__ */