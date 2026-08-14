// SPDX-License-Identifier: GPL-2.0
/*
 * indaq_ringbuf.c - INDAQ lock-free ring buffer for sensor samples
 *
 * SPSC design: I2C worker (producer) writes at head; read() (consumer)
 * reads from tail.  When full, oldest samples are overwritten.
 */

#include <linux/slab.h>
#include "indaq_ringbuf.h"

struct indaq_ringbuf *indaq_ringbuf_create(u32 capacity)
{
	struct indaq_ringbuf *rb;

	if (!capacity)
		capacity = INDAQ_RINGBUF_DEFAULT_SIZE;

	rb = kmalloc(sizeof(*rb), GFP_KERNEL);
	if (!rb)
		return NULL;

	rb->buf = kmalloc_array(capacity, sizeof(struct indaq_sample),
				GFP_KERNEL);
	if (!rb->buf) {
		kfree(rb);
		return NULL;
	}

	rb->capacity = capacity;
	rb->head = 0;
	rb->tail = 0;
	rb->count = 0;
	spin_lock_init(&rb->lock);

	return rb;
}

void indaq_ringbuf_destroy(struct indaq_ringbuf *rb)
{
	if (!rb)
		return;
	kfree(rb->buf);
	kfree(rb);
}

/*
 * Push one sample into the ring buffer.
 * Returns 0 on success, -ENOSPC if full (oldest overwritten implicitly).
 *
 * Note: overwrite-oldest policy — producer always advances head.
 */
int indaq_ringbuf_push(struct indaq_ringbuf *rb,
		       const struct indaq_sample *s)
{
	unsigned long flags;

	spin_lock_irqsave(&rb->lock, flags);

	memcpy(&rb->buf[rb->head], s, sizeof(*s));
	rb->head = (rb->head + 1) % rb->capacity;

	if (rb->count < rb->capacity) {
		rb->count++;
	} else {
		/* Buffer full — overwrite oldest, advance tail */
		rb->tail = (rb->tail + 1) % rb->capacity;
		spin_unlock_irqrestore(&rb->lock, flags);
		return -ENOSPC;
	}

	spin_unlock_irqrestore(&rb->lock, flags);
	return 0;
}

/*
 * Read up to @max samples from the ring buffer.
 * Returns the number of samples actually read.
 */
u32 indaq_ringbuf_read(struct indaq_ringbuf *rb,
		       struct indaq_sample *buf, u32 max)
{
	unsigned long flags;
	u32 i;

	spin_lock_irqsave(&rb->lock, flags);

	for (i = 0; i < max && rb->count > 0; i++) {
		memcpy(&buf[i], &rb->buf[rb->tail], sizeof(*buf));
		rb->tail = (rb->tail + 1) % rb->capacity;
		rb->count--;
	}

	spin_unlock_irqrestore(&rb->lock, flags);
	return i;
}

/* Reset ring buffer — discard all samples */
void indaq_ringbuf_reset(struct indaq_ringbuf *rb)
{
	unsigned long flags;

	spin_lock_irqsave(&rb->lock, flags);
	rb->head = 0;
	rb->tail = 0;
	rb->count = 0;
	spin_unlock_irqrestore(&rb->lock, flags);
}
