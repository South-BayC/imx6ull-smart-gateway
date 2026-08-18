/* edt_capture_bench.c —— P3-5 真机基准工具（手册 5.7.3）
 * 指标：交付事件率（ev/s）、丢失率（dropped/irq）、交付年龄（timestamp→read 延迟）、CPU 占用
 * 激励：函数发生器 / 按键连续按压（--duration 秒后自动 STOP 出汇总）
 * 用法：./edt_capture_bench [--duration N] [--report I]   默认 5s、每秒报告
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include "edt_capture.h"

#define DEV "/dev/edt_capture0"

static unsigned long long now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int main(int argc, char *argv[])
{
	int duration = 5, report = 1;
	int i, fd;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--duration") && i + 1 < argc)
			duration = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--report") && i + 1 < argc)
			report = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--help")) {
			printf("usage: %s [--duration N] [--report I]\n", argv[0]);
			return 0;
		}
	}
	if (duration < 1) duration = 1;
	if (report < 1) report = 1;

	struct edt_capture_caps caps;
	struct edt_capture_stats st0, st1;
	struct timespec cts;
	unsigned long long t_start, t_last, t_now;
	unsigned long long read_total = 0, last_read = 0;
	unsigned long long age_sum = 0, age_min = ~0ULL, age_max = 0;
	unsigned long long cpu_last;
	unsigned long long irq_base, queued_base, dropped_base, snapshot_base;

	fd = open(DEV, O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);   /* 排空循环需要非阻塞 */

	if (ioctl(fd, EDT_IOC_GET_CAPS, &caps) < 0) { perror("GET_CAPS"); return 1; }
	printf("caps: abi=%u channels=%u fifo=%u events edges=%u\n",
	       caps.abi_version, caps.configured_channels,
	       caps.fifo_depth_events, caps.supported_edges);

	if (ioctl(fd, EDT_IOC_GET_STATS, &st0) < 0) { perror("GET_STATS"); return 1; }
	irq_base = st0.irq_events;
	queued_base = st0.queued_events;
	dropped_base = st0.dropped_events;
	snapshot_base = st0.snapshot_events;

	if (ioctl(fd, EDT_IOC_START) < 0) { perror("START"); return 1; }

	t_start = t_last = t_now = now_ns();
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cts);
	cpu_last = (unsigned long long)cts.tv_sec * 1000000000ULL + cts.tv_nsec;

	printf("bench %ds, report every %ds: press signal source now...\n",
	       duration, report);

	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr = poll(&pfd, 1, 200);   /* 200ms 心跳，无事件也推进报告/退出 */

		t_now = now_ns();

		if (pr > 0 && (pfd.revents & POLLIN)) {
			/* 非阻塞排空 FIFO：避免 poll 间隔内积压误判丢失 */
			for (;;) {
				struct edt_capture_event ev;
				ssize_t n = read(fd, &ev, sizeof(ev));
				if (n == (ssize_t)sizeof(ev)) {
					read_total++;
					{
						unsigned long long age = t_now - ev.timestamp_ns;
						age_sum += age;
						if (age < age_min) age_min = age;
						if (age > age_max) age_max = age;
					}
				} else {
					if (n < 0 && errno != EAGAIN)
						perror("read");
					break;
				}
			}
		}

		if (t_now - t_last >= (unsigned long long)report * 1000000000ULL) {
			double dt = (double)(t_now - t_last) / 1e9;
			unsigned long long cnow, cdt;
			unsigned long long n_ev = read_total - last_read;
			double rate = dt > 0 ? (double)n_ev / dt : 0.0;
			double age_avg = n_ev ? (double)age_sum / (double)n_ev : 0.0;

			clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cts);
			cnow = (unsigned long long)cts.tv_sec * 1000000000ULL + cts.tv_nsec;
			cdt = cnow - cpu_last;

			/* 无事件间隔：年龄统计显示 0（age_min 初值 ~0ULL 不可直接打印） */
			printf("[%5.1fs] rate=%10.1f ev/s  age(min/avg/max)=%7.2f/%7.2f/%7.2f ms  cpu=%5.1f%%\n",
			       (double)(t_now - t_start) / 1e9, rate,
			       n_ev ? (double)age_min / 1e6 : 0.0, age_avg / 1e6,
			       n_ev ? (double)age_max / 1e6 : 0.0,
			       dt > 0 ? (double)cdt / (double)(t_now - t_last) * 100.0 : 0.0);

			last_read = read_total;
			t_last = t_now;
			cpu_last = cnow;
			age_sum = 0;
			age_min = ~0ULL;
			age_max = 0;

			if (t_now - t_start >= (unsigned long long)duration * 1000000000ULL)
				break;
		}
	}

	ioctl(fd, EDT_IOC_STOP);
	ioctl(fd, EDT_IOC_GET_STATS, &st1);

	{
		unsigned long long irq = st1.irq_events - irq_base;
		unsigned long long queued = st1.queued_events - queued_base;
		unsigned long long dropped = st1.dropped_events - dropped_base;
		unsigned long long snapshot = st1.snapshot_events - snapshot_base;

		printf("\n== 汇总 ==\n");
		printf("用户态读到: %llu 事件\n", read_total);
		printf("IRQ 事件:   %llu    入队: %llu    丢失: %llu    快照: %llu\n",
		       irq, queued, dropped, snapshot);
		printf("守恒(irq+snapshot == queued+dropped): %s\n",
		       (irq + snapshot == queued + dropped) ? "OK" : "MISMATCH");
		printf("丢失率:     %.4f%%  (dropped/irq)\n",
		       irq ? (double)dropped / (double)irq * 100.0 : 0.0);
		printf("FIFO 残留(STOP 清空): %llu 事件\n",
		       queued > read_total ? queued - read_total : 0);
	}

	close(fd);
	return 0;
}
