// SPDX-License-Identifier: GPL-2.0
/*
 * libdaq.c - INDAQ userspace C API implementation
 *
 * Provides blocking read, poll-based wait, IOCTL control, and
 * formatted printing for the INDAQ kernel driver.
 *
 * Compile with: gcc -c libdaq.c -o libdaq.o
 * Link with:   gcc your_prog.c libdaq.o -o your_prog
 * Or build as shared library: gcc -shared -fPIC -o libdaq.so libdaq.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include "libdaq.h"

int indaq_open(const char *dev)
{
	int fd;

	fd = open(dev, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror("indaq_open");
		return -1;
	}
	return fd;
}

void indaq_close(int fd)
{
	if (fd >= 0)
		close(fd);
}

int indaq_start(int fd)
{
	int ret;

	ret = ioctl(fd, INDAQ_IOCTL_START_CAPTURE);
	if (ret < 0) {
		perror("indaq_start (IOCTL)");
		return -1;
	}
	return 0;
}

int indaq_stop(int fd)
{
	int ret;

	ret = ioctl(fd, INDAQ_IOCTL_STOP_CAPTURE);
	if (ret < 0) {
		perror("indaq_stop (IOCTL)");
		return -1;
	}
	return 0;
}

ssize_t indaq_read(int fd, struct indaq_sample *buf, size_t count)
{
    ssize_t n;
    size_t bytes = count * sizeof(struct indaq_sample);

    n = read(fd, buf, bytes);
    if (n < 0) {
        /* EAGAIN is not an error — just means no data right now */
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -EAGAIN;
        perror("indaq_read");
        return -1;
    }
    /* Return number of samples */
    return n / (ssize_t)sizeof(struct indaq_sample);
}

int indaq_wait(int fd, int timeout_ms)
{
	struct pollfd pfd;
	int ret;

	pfd.fd = fd;
	pfd.events = POLLIN;

	ret = poll(&pfd, 1, timeout_ms);
	if (ret < 0) {
		perror("indaq_wait (poll)");
		return -1;
	}
	return (ret > 0) ? 1 : 0;
}

int indaq_get_info(int fd, struct indaq_info *info)
{
	int ret;

	memset(info, 0, sizeof(*info));
	ret = ioctl(fd, INDAQ_IOCTL_GET_INFO, info);
	if (ret < 0) {
		perror("indaq_get_info (IOCTL)");
		return -1;
	}
	return 0;
}

int indaq_set_rate(int fd, unsigned int hz)
{
	int ret;

	if (hz < 1 || hz > 100) {
		fprintf(stderr, "indaq_set_rate: rate %u out of range (1-100 Hz)\n", hz);
		return -1;
	}

	ret = ioctl(fd, INDAQ_IOCTL_SET_SAMPLING_RATE, &hz);
	if (ret < 0) {
		perror("indaq_set_rate (IOCTL)");
		return -1;
	}
	return 0;
}

void indaq_print_sample(const struct indaq_sample *s)
{
	/* Detect if this is an IMU sample or AP3216C sample */
	if (s->ax == 0 && s->ay == 0 && s->az == 0 &&
	    s->gx == 0 && s->gy == 0 && s->gz == 0 && s->temp == 0) {
		/* Likely AP3216C-only sample */
		printf("[%5llu] ALS=%5u PS=%5u IR=%5u\n",
		       (unsigned long long)s->ts_ns,
		       s->als, s->ps, s->ir);
	} else {
		/* IMU sample */
		printf("[%5llu] ax=%6d ay=%6d az=%6d "
		       "gx=%6d gy=%6d gz=%6d temp=%6d\n",
		       (unsigned long long)s->ts_ns,
		       s->ax, s->ay, s->az,
		       s->gx, s->gy, s->gz, s->temp);
	}
}
