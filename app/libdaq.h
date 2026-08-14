/* SPDX-License-Identifier: GPL-2.0 */
/*
 * libdaq.h - INDAQ userspace C API
 *
 * Provides a simple C library for communicating with the INDAQ
 * kernel driver (/dev/indaq). Supports both synchronous and
 * asynchronous (poll-based) data acquisition.
 *
 * Usage:
 *   int fd = indaq_open("/dev/indaq");
 *   indaq_start(fd);
 *   struct indaq_sample buf[64];
 *   int n = indaq_read(fd, buf, 64);
 *   indaq_stop(fd);
 *   indaq_close(fd);
 */

#ifndef LIBDAQ_H
#define LIBDAQ_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sample structure — must match kernel's struct indaq_sample.
 * Total size: 28 bytes.
 */
struct indaq_sample {
	uint64_t ts_ns;	/*  0: timestamp (nanoseconds) */
	uint16_t als;	/*  8: AP3216C ambient light */
	uint16_t ps;	/* 10: AP3216C proximity */
	uint16_t ir;	/* 12: AP3216C infrared */
	int16_t  ax;	/* 14: ICM-20608 accel X */
	int16_t  ay;	/* 16: ICM-20608 accel Y */
	int16_t  az;	/* 18: ICM-20608 accel Z */
	int16_t  temp;	/* 20: ICM-20608 temperature */
	int16_t  gx;	/* 22: ICM-20608 gyro X */
	int16_t  gy;	/* 24: ICM-20608 gyro Y */
	int16_t  gz;	/* 26: ICM-20608 gyro Z */
} __attribute__((packed));

/* Device info matching kernel's struct indaq_info */
struct indaq_info {
	uint32_t version;
	uint32_t sampling_rate;
	uint32_t total_samples;
	uint32_t errors;
};

/* IOCTL commands (must match kernel's INDAQ_IOCTL_* in indaq_core.h) */
#define INDAQ_IOC_MAGIC  'I'
#define INDAQ_IOCTL_GET_INFO          _IOR(INDAQ_IOC_MAGIC, 0, struct indaq_info)
#define INDAQ_IOCTL_START_CAPTURE     _IO(INDAQ_IOC_MAGIC,  3)
#define INDAQ_IOCTL_STOP_CAPTURE      _IO(INDAQ_IOC_MAGIC,  4)
#define INDAQ_IOCTL_SET_SAMPLING_RATE _IOW(INDAQ_IOC_MAGIC, 5, uint32_t)

/* ======== API Functions ======== */

/*
 * indaq_open - Open the INDAQ device.
 * @dev: Device path (e.g., "/dev/indaq").
 * Returns: file descriptor, or -1 on error.
 */
int indaq_open(const char *dev);

/*
 * indaq_close - Close the INDAQ device.
 * @fd: File descriptor from indaq_open().
 */
void indaq_close(int fd);

/*
 * indaq_start - Start data capture (IOCTL_START_CAPTURE).
 * Resets ring buffer and begins sensor data acquisition.
 * Returns: 0 on success, -1 on error.
 */
int indaq_start(int fd);

/*
 * indaq_stop - Stop data capture (IOCTL_STOP_CAPTURE).
 * Returns: 0 on success, -1 on error.
 */
int indaq_stop(int fd);

/*
 * indaq_read - Read samples from the device.
 * Blocks until at least one sample is available.
 *
 * @fd: File descriptor.
 * @buf: Output buffer for samples.
 * @count: Maximum number of samples to read.
 * Returns: number of samples actually read, or -1 on error.
 */
ssize_t indaq_read(int fd, struct indaq_sample *buf, size_t count);

/*
 * indaq_wait - Wait for data to become available (poll wrapper).
 * @fd: File descriptor.
 * @timeout_ms: Timeout in milliseconds (-1 = infinite).
 * Returns: 1 if data available, 0 if timeout, -1 on error.
 */
int indaq_wait(int fd, int timeout_ms);

/*
 * indaq_get_info - Get driver info via IOCTL.
 * @fd: File descriptor.
 * @info: Output structure.
 * Returns: 0 on success, -1 on error.
 */
int indaq_get_info(int fd, struct indaq_info *info);

/*
 * indaq_set_rate - Set sampling rate in Hz.
 * @fd: File descriptor.
 * @hz: Sampling rate (1-100 Hz).
 * Returns: 0 on success, -1 on error.
 */
int indaq_set_rate(int fd, unsigned int hz);

/*
 * indaq_print_sample - Format and print a sample to stdout.
 * Useful for debugging and quick monitoring.
 */
void indaq_print_sample(const struct indaq_sample *s);

#ifdef __cplusplus
}
#endif

#endif /* LIBDAQ_H */
