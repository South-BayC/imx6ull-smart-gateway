/*
 * indaq_test.c - INDAQ sensor read test program
 *
 * Usage:
 *   ./indaq_test              - text mode, read from ring buffer
 *   ./indaq_test -b [count]   - binary mode, read raw samples (default: infinite)
 *   ./indaq_test -r <hz>      - set sampling rate (1-1000 Hz)
 *   ./indaq_test -r 10 -b 50  - 10 Hz, capture 50 samples
 *
 * Examples:
 *   ./indaq_test -r 500        # 500 Hz text mode
 *   ./indaq_test -r 10 -b 100  # 10 Hz, 100 binary samples
 *
 * Build: arm-linux-gnueabihf-gcc -o indaq_test indaq_test.c -Wall
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>

/* Must match kernel struct indaq_sample (28 bytes) */
struct indaq_sample {
	unsigned long long ts_ns;	/*  8 */
	unsigned short als;		/*  2 */
	unsigned short ps;		/*  2 */
	unsigned short ir;		/*  2 */
	short ax, ay, az;		/*  6 */
	short temp;			/*  2 */
	short gx, gy, gz;		/*  6 */
} __attribute__((packed));

/* Must match kernel IOCTL defs in indaq_core.h */
#define INDAQ_IOC_MAGIC              'I'
#define INDAQ_IOCTL_START_CAPTURE     _IO(INDAQ_IOC_MAGIC,  3)
#define INDAQ_IOCTL_STOP_CAPTURE      _IO(INDAQ_IOC_MAGIC,  4)
#define INDAQ_IOCTL_SET_SAMPLING_RATE _IOW(INDAQ_IOC_MAGIC, 5, unsigned int)

static volatile int keep_running = 1;

static void handle_sigint(int sig)
{
	keep_running = 0;
}

static void print_sample(const struct indaq_sample *s)
{
	/* Detect IMU-only sample (AP3216C fields all zero) */
	int is_imu = (s->als == 0 && s->ps == 0 && s->ir == 0);

	if (is_imu) {
		printf("[%llu] IMU ax=%-6d ay=%-6d az=%-6d "
		       "gx=%-6d gy=%-6d gz=%-6d temp=%-5d\n",
		       s->ts_ns, s->ax, s->ay, s->az,
		       s->gx, s->gy, s->gz, s->temp);
	} else {
		printf("[%llu] ALS=%-5u  PS=%-5u  IR=%-5u\n",
		       s->ts_ns, s->als, s->ps, s->ir);
	}
}

int main(int argc, char *argv[])
{
	int fd, ret;
	int binary_mode = 0;
	int count = 0;       /* 0 = infinite */
	int sample_rate = 0; /* 0 = no change */

	signal(SIGINT, handle_sigint);

	/* Parse args */
	int i;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-b") == 0)
			binary_mode = 1;
		else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc)
			sample_rate = atoi(argv[++i]);
		else if (!binary_mode && count == 0)
			count = atoi(argv[i]);
	}

	fd = open("/dev/indaq", O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Failed to open /dev/indaq: %s\n",
			strerror(errno));
		return 1;
	}

	/* Apply sampling rate if requested */
	if (sample_rate > 0) {
		if (ioctl(fd, INDAQ_IOCTL_SET_SAMPLING_RATE,
			  &sample_rate) < 0) {
			fprintf(stderr, "Failed to set rate %d Hz: %s\n",
				sample_rate, strerror(errno));
			close(fd);
			return 1;
		}
		printf("Sampling rate set to %d Hz\n", sample_rate);
	}

	/* Start capture — without this, indaq_push_sample discards data */
	if (ioctl(fd, INDAQ_IOCTL_START_CAPTURE) < 0) {
		fprintf(stderr, "Failed to start capture: %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}

	if (binary_mode) {
		/* Binary mode: read raw samples from ring buffer */
		struct indaq_sample batch[64];
		int total = 0;

		printf("INDAQ Binary Read (Ctrl+C to stop)\n");
		printf("----------------------------------------\n");

		while (keep_running) {
			ret = read(fd, batch, sizeof(batch));
			if (ret < 0) {
				if (errno == EINTR)
					continue;
				fprintf(stderr, "read error: %s\n",
					strerror(errno));
				break;
			}

			int n = ret / sizeof(struct indaq_sample);
			int i;
			for (i = 0; i < n; i++) {
				print_sample(&batch[i]);
				total++;
			}
			fflush(stdout);

			if (count > 0 && total >= count)
				break;
		}
		printf("\nRead %d samples total.\n", total);
	} else {
		/* Text mode: read samples at 1-second intervals */
		struct indaq_sample batch[64];
		int interval_ms = 1000;

		printf("INDAQ Test — reading every %d ms (Ctrl+C to stop)\n",
		       interval_ms);
		printf("----------------------------------------\n");

		while (keep_running) {
			ret = read(fd, batch, sizeof(batch));
			if (ret < 0) {
				if (errno == EINTR)
					continue;
				fprintf(stderr, "read error: %s\n",
					strerror(errno));
				break;
			}

			int n = ret / sizeof(struct indaq_sample);
			int i;
			for (i = 0; i < n; i++)
				print_sample(&batch[i]);
			fflush(stdout);

			usleep(interval_ms * 1000);
		}
	}

	/* Stop capture */
	ioctl(fd, INDAQ_IOCTL_STOP_CAPTURE);

	close(fd);
	return 0;
}
