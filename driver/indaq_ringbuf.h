/* SPDX-License-Identifier: GPL-2.0 */
/*
 * indaq_ringbuf.h - INDAQ lock-free ring buffer for sensor samples
 *
 * Single-producer single-consumer (SPSC) design with spinlock.
 * Producer: I2C workqueue (ap3216c_read_worker)
 * Consumer: userspace read() via indaq_core
 */

#ifndef __INDAQ_RINGBUF_H__
#define __INDAQ_RINGBUF_H__

#include <linux/types.h>
#include <linux/spinlock.h>

/* One sample: timestamp + ALS/PS/IR + IMU 6-axis + temp = 28 bytes (packed) */
struct indaq_sample {
	u64 ts_ns;	/*  8: timestamp */

	/* Light/proximity (AP3216C) */
	u16 als;	/* 10: ambient light */
	u16 ps;		/* 12: proximity */
	u16 ir;		/* 14: infrared */

	/* IMU (ICM-20608) */
	s16 ax, ay, az;	/* 20: accel X/Y/Z */
	s16 temp;	/* 22: temperature */
	s16 gx, gy, gz;	/* 28: gyro X/Y/Z */
} __packed;

#define INDAQ_RINGBUF_DEFAULT_SIZE  4096  /* number of sample slots */

struct indaq_ringbuf {
	struct indaq_sample *buf;
	u32 capacity;       /* total slots */
	u32 head;           /* next write position (producer) */
	u32 tail;           /* next read position (consumer) */
	u32 count;          /* samples in buffer */
	spinlock_t lock;
};

/* API */
struct indaq_ringbuf *indaq_ringbuf_create(u32 capacity);
void indaq_ringbuf_destroy(struct indaq_ringbuf *rb);
int  indaq_ringbuf_push(struct indaq_ringbuf *rb,
			const struct indaq_sample *s);
u32  indaq_ringbuf_read(struct indaq_ringbuf *rb,
			struct indaq_sample *buf, u32 max);
void indaq_ringbuf_reset(struct indaq_ringbuf *rb);

#endif /* __INDAQ_RINGBUF_H__ */
