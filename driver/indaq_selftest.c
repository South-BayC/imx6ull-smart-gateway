// SPDX-License-Identifier: GPL-2.0
/*
 * indaq_selftest.c - INDAQ kernel self-test module
 *
 * Lightweight sanity checks for internal INDAQ components.
 * Compiles as a separate module; requires indaq.ko to be loaded
 * for full integration tests, but ring buffer tests are standalone.
 *
 * Build:
 *   make -C <KERNELDIR> M=$PWD CONFIG_INDAQ_SELFTEST=m modules
 *
 * Load:
 *   insmod indaq_selftest.ko
 *   dmesg | tail -40
 *
 * Unload:
 *   rmmod indaq_selftest
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include "indaq_ringbuf.h"
#include "indaq_calib.h"

#define TEST_PASS	0
#define TEST_FAIL	-1

static int test_count;
static int pass_count;

#define RUN_TEST(name)				\
	do {					\
		test_count++;			\
		pr_info("SELFTEST: %s... ", name);	\
		if (name##_test() == 0) {	\
			pass_count++;		\
			pr_cont("PASS\n");	\
		} else {			\
			pr_cont("FAIL\n");	\
		}				\
	} while (0)

/* ======== Ring Buffer Tests ======== */

static int ringbuf_create_destroy_test(void)
{
	struct indaq_ringbuf *rb;

	rb = indaq_ringbuf_create(1024);
	if (!rb)
		return TEST_FAIL;
	if (rb->capacity != 1024)
		return TEST_FAIL;

	indaq_ringbuf_destroy(rb);
	return TEST_PASS;
}

static int ringbuf_push_read_test(void)
{
	struct indaq_ringbuf *rb;
	struct indaq_sample s_in = { .ts_ns = 12345, .als = 100, .ps = 200 };
	struct indaq_sample s_out[4];
	u32 n;

	rb = indaq_ringbuf_create(64);
	if (!rb)
		return TEST_FAIL;

	/* Push one sample */
	if (indaq_ringbuf_push(rb, &s_in) != 0)
		return TEST_FAIL;

	/* Read it back */
	n = indaq_ringbuf_read(rb, s_out, 4);
	if (n != 1)
		return TEST_FAIL;
	if (s_out[0].ts_ns != 12345 || s_out[0].als != 100)
		return TEST_FAIL;

	/* Verify empty after read */
	n = indaq_ringbuf_read(rb, s_out, 4);
	if (n != 0)
		return TEST_FAIL;

	indaq_ringbuf_destroy(rb);
	return TEST_PASS;
}

static int ringbuf_wraparound_test(void)
{
	struct indaq_ringbuf *rb;
	struct indaq_sample s;
	u32 n, i;

	rb = indaq_ringbuf_create(16);
	if (!rb)
		return TEST_FAIL;

	/* Fill buffer past capacity to trigger wraparound */
	for (i = 0; i < 32; i++) {
		s.ts_ns = i;
		if (indaq_ringbuf_push(rb, &s) != 0) {
			/* Buffer full — expected at capacity */
			break;
		}
	}

	/* Read all available samples */
	n = indaq_ringbuf_read(rb, &s, 1);
	if (n == 0)
		return TEST_FAIL;	/* should have at least 1 */

	indaq_ringbuf_destroy(rb);
	return TEST_PASS;
}

static int ringbuf_reset_test(void)
{
	struct indaq_ringbuf *rb;
	struct indaq_sample s = { .ts_ns = 1 };
	struct indaq_sample out[4];
	u32 n;

	rb = indaq_ringbuf_create(16);
	if (!rb)
		return TEST_FAIL;

	indaq_ringbuf_push(rb, &s);
	indaq_ringbuf_reset(rb);

	/* After reset, buffer should be empty */
	n = indaq_ringbuf_read(rb, out, 4);
	if (n != 0)
		return TEST_FAIL;

	indaq_ringbuf_destroy(rb);
	return TEST_PASS;
}

/* ======== Module Init ======== */

static int __init indaq_selftest_init(void)
{
	pr_info("INDAQ self-test module loaded\n");
	pr_info("================================\n");

	RUN_TEST(ringbuf_create_destroy);
	RUN_TEST(ringbuf_push_read);
	RUN_TEST(ringbuf_wraparound);
	RUN_TEST(ringbuf_reset);

	pr_info("================================\n");
	pr_info("Results: %d/%d passed\n", pass_count, test_count);

	if (pass_count == test_count)
		pr_info("SELFTEST: ALL TESTS PASSED\n");
	else
		pr_warn("SELFTEST: %d TESTS FAILED\n",
			test_count - pass_count);

	return 0;
}

static void __exit indaq_selftest_exit(void)
{
	pr_info("INDAQ self-test module unloaded\n");
}

module_init(indaq_selftest_init);
module_exit(indaq_selftest_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("INDAQ Driver Team");
MODULE_DESCRIPTION("INDAQ kernel self-test module");
MODULE_VERSION("1.0");
