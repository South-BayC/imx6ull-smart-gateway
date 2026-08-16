/* edt_capture_test.c —— gpio_event 采集驱动用户态测试（P3）
 * 两种模式：
 *   edt_capture_test --selftest  自动语义断言（kselftest 语义矩阵，无需按键）
 *   edt_capture_test [--nonblock|--poll]  交互读事件模式（按 KEY0）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include "edt_capture.h"

#define DEV "/dev/edt_capture0"

static int g_fail;

#define CHECK(cond, msg) do {						\
	if (cond)							\
		printf("[PASS] %s\n", msg);				\
	else {								\
		printf("[FAIL] %s\n", msg);				\
		g_fail++;						\
	}								\
} while (0)

static void print_ev(const struct edt_capture_event *ev)
{
	printf("t=%llu seq=%u ch=%u lv=%u edge=%u\n",
	       (unsigned long long)ev->timestamp_ns, ev->sequence,
	       ev->channel, ev->level, ev->edge);
}

/* ============ kselftest 语义矩阵（自动断言，不依赖按键） ============ */
static int selftest(void)
{
	struct edt_capture_caps caps;
	struct edt_capture_stats st;
	struct edt_capture_config cfg;
	struct edt_capture_event ev;
	int fd, fd2;
	ssize_t n;

	fd = open(DEV, O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	printf("== selftest on %s ==\n", DEV);

	/* ① GET_CAPS 版本/大小校验 */
	CHECK(ioctl(fd, EDT_IOC_GET_CAPS, &caps) == 0, "GET_CAPS 调用成功");
	CHECK(caps.abi_version == EDT_CAPTURE_ABI_VERSION,
	      "GET_CAPS abi_version == 2");
	CHECK(caps.struct_size == sizeof(caps), "GET_CAPS struct_size == 48");
	CHECK(caps.event_size == 16, "GET_CAPS event_size == 16");
	CHECK(caps.max_channels == 8, "GET_CAPS max_channels == 8");
	CHECK(caps.configured_channels >= 1, "GET_CAPS configured_channels >= 1");
	CHECK(caps.supported_edges == EDT_CAPTURE_EDGE_BOTH,
	      "GET_CAPS supported_edges == BOTH");
	CHECK(caps.fifo_depth_events == 1024, "GET_CAPS fifo_depth == 1024");
	CHECK(caps.flags & EDT_CAPTURE_CAP_NONBLOCK, "CAP_NONBLOCK 置位");
	CHECK(caps.flags & EDT_CAPTURE_CAP_POLL, "CAP_POLL 置位");
	CHECK(caps.flags & EDT_CAPTURE_CAP_EXCLUSIVE_OPEN, "CAP_EXCLUSIVE_OPEN 置位");
	CHECK(caps.flags & EDT_CAPTURE_CAP_INITIAL_SNAPSHOT, "CAP_INITIAL_SNAPSHOT 置位");

	/* ② 重复 open → EBUSY（独占打开） */
	fd2 = open(DEV, O_RDWR);
	CHECK(fd2 < 0 && errno == EBUSY, "重复 open → EBUSY");
	if (fd2 >= 0) close(fd2);

	/* ③ read 非 16 对齐 → EINVAL */
	n = read(fd, &ev, 5);
	CHECK(n < 0 && errno == EINVAL, "read(5) 非 16 对齐 → EINVAL");

	/* ④ 未 START：非阻塞空读 → EAGAIN */
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	n = read(fd, &ev, sizeof(ev));
	CHECK(n < 0 && errno == EAGAIN, "未 START 非阻塞空读 → EAGAIN");
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);

	/* ⑤ 未 START：阻塞 read 直接返回 0（不挂起） */
	n = read(fd, &ev, sizeof(ev));
	CHECK(n == 0, "未 START 阻塞 read → 0");

	/* ⑥ SET_CONFIG：运行中修改 → EBUSY；非法参数 → EINVAL */
	CHECK(ioctl(fd, EDT_IOC_START) == 0, "START 成功");
	memset(&cfg, 0, sizeof(cfg));
	cfg.abi_version = EDT_CAPTURE_ABI_VERSION;
	cfg.struct_size = sizeof(cfg);
	cfg.channel_mask = 0x1;
	cfg.edge_mask = EDT_CAPTURE_EDGE_BOTH;
	CHECK(ioctl(fd, EDT_IOC_SET_CONFIG, &cfg) == -1 && errno == EBUSY,
	      "运行中 SET_CONFIG → EBUSY");

	/* ⑦ START 后 FIFO 快照可读（单通道 1 个快照事件，edge==0） */
	n = read(fd, &ev, sizeof(ev));
	CHECK(n == (ssize_t)sizeof(ev), "START 后快照事件可读");
	if (n == (ssize_t)sizeof(ev)) {
		CHECK(ev.channel == 0, "快照 channel == 0");
		CHECK(ev.edge == 0, "快照 edge == 0（无边沿）");
		CHECK(ev.sequence == 1, "快照 sequence == 1");
		printf("      快照: "); print_ev(&ev);
	}

	/* ⑧ CLEAR_FIFO 后 stats.cleared 增加 */
	/* 再造一个快照（STOP 再 START）再清空 */
	CHECK(ioctl(fd, EDT_IOC_STOP) == 0, "STOP 成功");
	CHECK(ioctl(fd, EDT_IOC_START) == 0, "再 START 成功");
	CHECK(ioctl(fd, EDT_IOC_CLEAR_FIFO) == 0, "CLEAR_FIFO 成功");
	CHECK(ioctl(fd, EDT_IOC_GET_STATS, &st) == 0, "GET_STATS 成功");
	CHECK(st.cleared_events >= 1, "cleared_events >= 1（快照被清）");
	CHECK(st.snapshot_events >= 2, "snapshot_events >= 2");

	/* ⑨ SET_CONFIG：非法 mask → EINVAL；fifo 非 2 的幂 → EINVAL */
	CHECK(ioctl(fd, EDT_IOC_STOP) == 0, "STOP 成功（为 SET_CONFIG 准备）");
	memset(&cfg, 0, sizeof(cfg));
	cfg.abi_version = EDT_CAPTURE_ABI_VERSION;
	cfg.struct_size = sizeof(cfg);
	cfg.channel_mask = 0x0;      /* 空 mask */
	cfg.edge_mask = EDT_CAPTURE_EDGE_BOTH;
	CHECK(ioctl(fd, EDT_IOC_SET_CONFIG, &cfg) == -1 && errno == EINVAL,
	      "SET_CONFIG channel_mask=0 → EINVAL");
	cfg.channel_mask = 0x2;      /* 越界（configured=1） */
	CHECK(ioctl(fd, EDT_IOC_SET_CONFIG, &cfg) == -1 && errno == EINVAL,
	      "SET_CONFIG channel_mask=0x2 越界 → EINVAL");
	cfg.channel_mask = 0x1;
	cfg.fifo_depth_events = 1000;   /* 非 2 的幂 */
	CHECK(ioctl(fd, EDT_IOC_SET_CONFIG, &cfg) == -1 && errno == EINVAL,
	      "SET_CONFIG fifo=1000 非 2 的幂 → EINVAL");
	cfg.fifo_depth_events = 0;      /* 合法：保持当前 */
	cfg.edge_mask = EDT_CAPTURE_EDGE_RISING;
	CHECK(ioctl(fd, EDT_IOC_SET_CONFIG, &cfg) == 0,
	      "SET_CONFIG 合法配置成功");
	cfg.edge_mask = EDT_CAPTURE_EDGE_BOTH;   /* 恢复 */
	ioctl(fd, EDT_IOC_SET_CONFIG, &cfg);

	/* ⑩ STOP 后阻塞 read 返回 0 */
	CHECK(ioctl(fd, EDT_IOC_STOP) == 0, "STOP 成功");
	n = read(fd, &ev, sizeof(ev));
	CHECK(n == 0, "STOP 后阻塞 read → 0");

	/* ⑪ RESET_STATS 后主要计数清零 */
	CHECK(ioctl(fd, EDT_IOC_RESET_STATS) == 0, "RESET_STATS 成功");
	CHECK(ioctl(fd, EDT_IOC_GET_STATS, &st) == 0, "GET_STATS 成功");
	CHECK(st.irq_events == 0 && st.queued_events == 0,
	      "RESET 后 irq/queued == 0");
	CHECK(st.abi_version == EDT_CAPTURE_ABI_VERSION,
	      "RESET 保留 abi_version");

	/* ⑫ 非法 ioctl → ENOTTY */
	CHECK(ioctl(fd, 0xDEAD) == -1 && errno == ENOTTY, "非法 cmd → ENOTTY");

	close(fd);
	fd = open(DEV, O_RDWR);
	CHECK(fd >= 0, "close 后重新 open 成功（独占释放）");
	if (fd >= 0) close(fd);

	printf("== selftest done: %s ==\n", g_fail ? "FAIL" : "ALL PASS");
	return g_fail ? 1 : 0;
}

/* ============ 交互读事件模式 ============ */
static int interactive(int mode)
{
	int fd;
	struct edt_capture_caps caps;

	fd = open(DEV, O_RDWR);
	if (fd < 0) { perror("open"); return 1; }

	if (ioctl(fd, EDT_IOC_GET_CAPS, &caps) == 0)
		printf("caps: abi=%u channels=%u fifo=%u\n",
		       caps.abi_version, caps.configured_channels,
		       caps.fifo_depth_events);

	if (ioctl(fd, EDT_IOC_START) < 0) { perror("START"); return 1; }

	if (mode == 1) {
		/* 非阻塞模式 */
		int flags = fcntl(fd, F_GETFL);
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		printf("nonblock mode: press KEY0 (Ctrl-C to exit)\n");
		for (;;) {
			struct edt_capture_event ev;
			ssize_t n = read(fd, &ev, sizeof(ev));
			if (n == (ssize_t)sizeof(ev))
				print_ev(&ev);
			else if (n < 0 && errno == EAGAIN)
				printf("no event (EAGAIN)\n");
			usleep(100 * 1000);
		}
	} else if (mode == 2) {
		/* poll 模式 */
		printf("poll mode: press KEY0 (Ctrl-C to exit)\n");
		for (;;) {
			struct pollfd pfd = { .fd = fd, .events = POLLIN };
			int r = poll(&pfd, 1, 3000);
			if (r > 0 && (pfd.revents & POLLIN)) {
				struct edt_capture_event ev;
				if (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
					print_ev(&ev);
			} else if (r == 0) {
				printf("poll timeout 3s, no event\n");
			} else {
				perror("poll");
				break;
			}
		}
	} else {
		/* 阻塞模式 */
		printf("block mode: press KEY0 (Ctrl-C to exit)\n");
		for (;;) {
			struct edt_capture_event ev;
			if (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
				print_ev(&ev);
		}
	}
	close(fd);
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
		return selftest();
	if (argc > 1 && strcmp(argv[1], "--nonblock") == 0)
		return interactive(1);
	if (argc > 1 && strcmp(argv[1], "--poll") == 0)
		return interactive(2);
	return interactive(0);
}