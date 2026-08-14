// SPDX-License-Identifier: GPL-2.0
/*
 * indaq_debug.c - INDAQ debugfs interface
 *
 * Exposes runtime status, register peek/poke, IMU raw data, and
 * calibration controls via debugfs at /sys/kernel/debug/indaq/.
 *
 * Enhanced with:
 *   reg_peek    — read IMU register:  echo 0x75 > reg_peek; cat reg_peek
 *   reg_poke    — write IMU register: echo 0x6B=0x01 > reg_poke
 *   imu_raw     — dump latest IMU 6-axis + temp snapshot
 *   stats       — driver statistics (total/imu/i2c samples, errors)
 *   calib_*     — calibration trigger and params (created by indaq_calib.c)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include "indaq_core.h"
#include "indaq_ringbuf.h"
#include "indaq_imu.h"
#include "indaq_calib.h"
#include "indaq_pm.h"
#include "indaq_input.h"
#include "indaq_debug.h"

/* ---------- Existing simple u32/bool debugfs files ---------- */

static int indaq_debug_capture_active_get(void *data, u64 *val)
{
	struct indaq_device *indev = data;
	*val = atomic_read(&indev->capture_active);
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(indaq_debug_capture_active_fops,
			indaq_debug_capture_active_get, NULL, "%llu\n");

static int indaq_debug_total_samples_get(void *data, u64 *val)
{
	struct indaq_device *indev = data;
	*val = indev->total_samples;
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(indaq_debug_total_samples_fops,
			indaq_debug_total_samples_get, NULL, "%llu\n");

static int indaq_debug_sampling_rate_get(void *data, u64 *val)
{
	struct indaq_device *indev = data;
	*val = indev->sampling_rate;
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(indaq_debug_sampling_rate_fops,
			indaq_debug_sampling_rate_get, NULL, "%llu\n");

/* ---------- Ring buffer debugfs file (spinlock protected) ---------- */

static int indaq_debug_ringbuf_show(struct seq_file *m, void *v)
{
	struct indaq_device *indev = m->private;
	struct indaq_ringbuf *rb = indev->ringbuf;
	unsigned long flags;

	if (!rb)
		return 0;

	spin_lock_irqsave(&rb->lock, flags);
	seq_printf(m, "capacity: %u\n", rb->capacity);
	seq_printf(m, "count:    %u\n", rb->count);
	seq_printf(m, "head:     %u\n", rb->head);
	seq_printf(m, "tail:     %u\n", rb->tail);
	seq_printf(m, "usage:    %u%%\n",
		   (rb->capacity > 0) ?
		   (rb->count * 100 / rb->capacity) : 0);
	spin_unlock_irqrestore(&rb->lock, flags);

	return 0;
}

static int indaq_debug_ringbuf_open(struct inode *inode, struct file *file)
{
	return single_open(file, indaq_debug_ringbuf_show, inode->i_private);
}

static const struct file_operations indaq_debug_ringbuf_fops = {
	.owner   = THIS_MODULE,
	.open    = indaq_debug_ringbuf_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ---------- Individual ringbuf field files ---------- */

static int indaq_debug_ringbuf_capacity_get(void *data, u64 *val)
{
	struct indaq_device *indev = data;
	struct indaq_ringbuf *rb = indev->ringbuf;
	unsigned long flags;

	if (!rb)
		return -ENODEV;

	spin_lock_irqsave(&rb->lock, flags);
	*val = rb->capacity;
	spin_unlock_irqrestore(&rb->lock, flags);
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(indaq_debug_ringbuf_capacity_fops,
			indaq_debug_ringbuf_capacity_get, NULL, "%llu\n");

static int indaq_debug_ringbuf_count_get(void *data, u64 *val)
{
	struct indaq_device *indev = data;
	struct indaq_ringbuf *rb = indev->ringbuf;
	unsigned long flags;

	if (!rb)
		return -ENODEV;

	spin_lock_irqsave(&rb->lock, flags);
	*val = rb->count;
	spin_unlock_irqrestore(&rb->lock, flags);
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(indaq_debug_ringbuf_count_fops,
			indaq_debug_ringbuf_count_get, NULL, "%llu\n");

static int indaq_debug_ringbuf_head_get(void *data, u64 *val)
{
	struct indaq_device *indev = data;
	struct indaq_ringbuf *rb = indev->ringbuf;
	unsigned long flags;

	if (!rb)
		return -ENODEV;

	spin_lock_irqsave(&rb->lock, flags);
	*val = rb->head;
	spin_unlock_irqrestore(&rb->lock, flags);
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(indaq_debug_ringbuf_head_fops,
			indaq_debug_ringbuf_head_get, NULL, "%llu\n");

static int indaq_debug_ringbuf_tail_get(void *data, u64 *val)
{
	struct indaq_device *indev = data;
	struct indaq_ringbuf *rb = indev->ringbuf;
	unsigned long flags;

	if (!rb)
		return -ENODEV;

	spin_lock_irqsave(&rb->lock, flags);
	*val = rb->tail;
	spin_unlock_irqrestore(&rb->lock, flags);
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(indaq_debug_ringbuf_tail_fops,
			indaq_debug_ringbuf_tail_get, NULL, "%llu\n");

/* ---------- Enhanced: reg_peek (read IMU register) ---------- */

/*
 * Usage:
 *   echo 0x75 > /sys/kernel/debug/indaq/reg_peek
 *   cat  /sys/kernel/debug/indaq/reg_peek
 *
 * K stores the register address (last written value); show reads it.
 */
static u8 debug_reg_addr;

static ssize_t reg_peek_write(struct file *file,
			      const char __user *user_buf,
			      size_t len, loff_t *off)
{
	struct indaq_device *indev = file_inode(file)->i_private;
	unsigned long addr;
	int ret;

	ret = kstrtoul_from_user(user_buf, len, 16, &addr);
	if (ret)
		return ret;
	if (addr > 0xFF)
		return -EINVAL;

	debug_reg_addr = (u8)addr;
	dev_dbg(indev->dev, "reg_peek: address set to 0x%02x\n",
		debug_reg_addr);
	return len;
}

static int reg_peek_show(struct seq_file *m, void *v)
{
	struct indaq_device *indev = m->private;
	struct indaq_imu *imu = indev->imu ? indev->imu : icm20608_get_imu();
	u8 val;

	if (!imu) {
		seq_puts(m, "IMU not initialized\n");
		return 0;
	}

	icm20608_read_reg(imu->spi, debug_reg_addr, &val);
	seq_printf(m, "REG[0x%02x] = 0x%02x (%d)\n",
		   debug_reg_addr, val, val);
	return 0;
}

static int reg_peek_open(struct inode *inode, struct file *file)
{
	return single_open(file, reg_peek_show, inode->i_private);
}

static const struct file_operations reg_peek_fops = {
	.owner   = THIS_MODULE,
	.open    = reg_peek_open,
	.read    = seq_read,
	.write   = reg_peek_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ---------- Enhanced: reg_poke (write IMU register) ---------- */

/*
 * Usage:
 *   echo 0x6B=0x01 > /sys/kernel/debug/indaq/reg_poke
 * Writes value 0x01 to register 0x6B.
 */
static ssize_t reg_poke_write(struct file *file,
			      const char __user *user_buf,
			      size_t len, loff_t *off)
{
	struct indaq_device *indev = file_inode(file)->i_private;
	struct indaq_imu *imu = indev->imu ? indev->imu : icm20608_get_imu();
	char buf[32];
	unsigned long addr, val;
	char *eq;
	int ret;

	if (!imu)
		return -ENODEV;

	if (len >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, user_buf, len))
		return -EFAULT;
	buf[len] = '\0';

	/* Trim newline */
	if (len > 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	eq = strchr(buf, '=');
	if (!eq)
		return -EINVAL;

	*eq = '\0';
	ret = kstrtoul(buf, 16, &addr);
	if (ret)
		return ret;
	ret = kstrtoul(eq + 1, 16, &val);
	if (ret)
		return ret;
	if (addr > 0xFF || val > 0xFF)
		return -EINVAL;

	ret = icm20608_write_reg(imu->spi, (u8)addr, (u8)val);
	if (ret < 0)
		return ret;

	dev_info(indev->dev, "reg_poke: REG[0x%02x] = 0x%02x\n",
		 (u8)addr, (u8)val);
	return len;
}

static const struct file_operations reg_poke_fops = {
	.owner   = THIS_MODULE,
	.write   = reg_poke_write,
	.open    = simple_open,
	.llseek  = noop_llseek,
};

/* ---------- Enhanced: imu_raw (show latest IMU snapshot) ---------- */

static int imu_raw_show(struct seq_file *m, void *v)
{
	struct indaq_device *indev = m->private;
	struct indaq_imu *imu = indev->imu;

	if (!imu)
		imu = icm20608_get_imu();
	if (!imu) {
		seq_puts(m, "IMU not initialized\n");
		return 0;
	}

	seq_printf(m, "accel:  %6d %6d %6d\n", imu->ax, imu->ay, imu->az);
	seq_printf(m, "gyro:   %6d %6d %6d\n", imu->gx, imu->gy, imu->gz);
	seq_printf(m, "temp:   %6d\n", imu->temp);
	seq_printf(m, "ready:  %s\n", imu->core_dev ? "yes" : "no");
	return 0;
}

static int imu_raw_open(struct inode *inode, struct file *file)
{
	return single_open(file, imu_raw_show, inode->i_private);
}

static const struct file_operations imu_raw_fops = {
	.owner   = THIS_MODULE,
	.open    = imu_raw_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ---------- Enhanced: stats (driver statistics) ---------- */

static int stats_show(struct seq_file *m, void *v)
{
	struct indaq_device *indev = m->private;
	struct indaq_ringbuf *rb = indev->ringbuf;

	seq_printf(m, "version:        %s\n", INDAQ_DRV_VERSION);
	seq_printf(m, "capture_active: %d\n",
		   atomic_read(&indev->capture_active));
	seq_printf(m, "total_samples:  %u\n", indev->total_samples);
	seq_printf(m, "sampling_rate:  %u Hz\n", indev->sampling_rate);

	if (rb) {
		unsigned long flags;
		spin_lock_irqsave(&rb->lock, flags);
		seq_printf(m, "ringbuf_capacity: %u\n", rb->capacity);
		seq_printf(m, "ringbuf_count:    %u\n", rb->count);
		spin_unlock_irqrestore(&rb->lock, flags);
	}

	seq_printf(m, "imu_ready:      %s\n",
		   (indev->imu || icm20608_get_imu()) ? "yes" : "no");
	seq_printf(m, "calibrated:     %s\n",
		   (indev->calib &&
		    ((struct indaq_calib_params *)indev->calib)->calibrated)
		   ? "yes" : "no");

	seq_printf(m, "pm_suspended:   %d\n",
		   indaq_pm_is_suspended() ? 1 : 0);
	seq_printf(m, "gpio_trigger:   %d\n",
		   indev->gpioctrl ? 1 : 0);
	{
		int tc = indev->input_dev
			 ? indaq_input_get_tap_count(indev) : -1;
		seq_printf(m, "tap_count:      %d\n", tc);
	}

	return 0;
}

static int stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, stats_show, inode->i_private);
}

static const struct file_operations stats_fops = {
	.owner   = THIS_MODULE,
	.open    = stats_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ---------- Init / Exit ---------- */

int indaq_debug_init(struct indaq_device *indev)
{
	struct dentry *dir;

	if (!indev)
		return -EINVAL;

	/* Create debugfs directory "indaq" under root debugfs */
	dir = debugfs_create_dir("indaq", NULL);
	if (IS_ERR_OR_NULL(dir)) {
		dev_err(indev->dev, "Failed to create debugfs directory\n");
		return -ENOMEM;
	}

	indev->debug_dir = dir;

	/* Simple u32/bool files */
	debugfs_create_file("capture_active", 0444, dir, indev,
			    &indaq_debug_capture_active_fops);
	debugfs_create_file("total_samples", 0444, dir, indev,
			    &indaq_debug_total_samples_fops);
	debugfs_create_file("sampling_rate", 0444, dir, indev,
			    &indaq_debug_sampling_rate_fops);

	/* Ring buffer composite file */
	debugfs_create_file("ringbuf", 0444, dir, indev,
			    &indaq_debug_ringbuf_fops);

	/* Individual ringbuf fields */
	debugfs_create_file("ringbuf_capacity", 0444, dir, indev,
			    &indaq_debug_ringbuf_capacity_fops);
	debugfs_create_file("ringbuf_count", 0444, dir, indev,
			    &indaq_debug_ringbuf_count_fops);
	debugfs_create_file("head", 0444, dir, indev,
			    &indaq_debug_ringbuf_head_fops);
	debugfs_create_file("tail", 0444, dir, indev,
			    &indaq_debug_ringbuf_tail_fops);

	/* === Enhanced DebugFS entries === */

	/* Register peek/poke — IMU accessed via global pointer if needed */
	debugfs_create_file("reg_peek", 0644, dir, indev, &reg_peek_fops);
	debugfs_create_file("reg_poke", 0200, dir, indev, &reg_poke_fops);

	/* IMU raw snapshot */
	debugfs_create_file("imu_raw", 0444, dir, indev, &imu_raw_fops);

	/* Driver statistics */
	debugfs_create_file("stats", 0444, dir, indev, &stats_fops);

	/*
	 * Calibration files (calib_params, calib_gyro) are created by
	 * indaq_calib_init(), which runs after indaq_debug_init().
	 */

	dev_info(indev->dev,
		 "Debugfs interface created at /sys/kernel/debug/indaq/\n");
	return 0;
}

void indaq_debug_exit(struct indaq_device *indev)
{
	if (!indev || !indev->debug_dir)
		return;

	debugfs_remove_recursive(indev->debug_dir);
	indev->debug_dir = NULL;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("INDAQ Driver Team");
MODULE_DESCRIPTION("INDAQ debugfs interface (enhanced)");
