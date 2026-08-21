/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * sump_server.c —— SUMP 协议服务器（P6-2，手册 5.15.2）
 *
 * 将 gpio_event 边沿事件采集（edt_capture 驱动）暴露为 SUMP 逻辑分析仪，
 * 供 PulseView/sigrok 通过 TCP 连接（tcp://<ip>:9527）。
 *
 * 协议实现依据 libsigrok OLS 驱动源码（src/hardware/openbench-logic-sniffer）：
 *   - 握手：5×Reset(0x00) → ID(0x02) → 回复 4 字节 "1ALS"
 *   - metadata(0x04)：键值流以 0x00 结束；字符串(0x00-0x1F)、
 *     32 位大端整数(0x20-0x3F)、8 位整数(0x40-0x5F)
 *   - 长命令 5 字节，参数小端（WL32/WL16）；metadata 整数大端（RB32）——非对称！
 *   - 采样率：CLOCK_RATE=100MHz，R = 100MHz/(divider+1)，DEMUX(flags bit0) 时 ×2
 *   - 样本数：readcount=(samplecount+3)/4，设备发 readcount*4 样本；
 *     max_samples>256K 走 0x84(READCOUNT32)+0x83(DELAYCOUNT32)，
 *     否则走 0x81(READ16+DELAY16)
 *   - 设备倒序发送样本（最新先发），sigrok 反转后时间正序输出
 *   - 发送须连续，间隔 < 100ms（sigrok 超时判定）
 *
 * 等效采样说明（重要声明，须写入文档）：
 *   本工具是事件驱动等效采样，采样率上限受 GPIO 事件产生率限制，
 *   适用于低速协议调试（I2C ≤ 400kHz / UART），非硬件全速采样。
 *
 * 用法：
 *   sump_server [--device /dev/edt_capture0|--sim] [--port 9527]
 *               [--max-window 10000] [--verbose]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "capture_source.h"
#include "event_reconstructor.h"

/* ---------- SUMP 协议常量（与 libsigrok OLS 驱动一致） ---------- */
#define SUMP_CLOCK_RATE     100000000u /* 100MHz 逻辑时钟 */
#define SUMP_CMD_RESET      0x00
#define SUMP_CMD_RUN        0x01 /* arm */
#define SUMP_CMD_ID         0x02
#define SUMP_CMD_METADATA   0x04
#define SUMP_CMD_FINISH_NOW 0x05
#define SUMP_CMD_QUERY_INPUT 0x06
#define SUMP_CMD_ARM_ADV    0x0F
#define SUMP_CMD_SET_DIVIDER       0x80
#define SUMP_CMD_CAPTURE_SIZE      0x81 /* readcount-1(16) + delaycount-1(16) */
#define SUMP_CMD_SET_FLAGS         0x82
#define SUMP_CMD_CAPTURE_DELAYCOUNT 0x83 /* 32bit */
#define SUMP_CMD_CAPTURE_READCOUNT 0x84  /* 32bit */
#define SUMP_CMD_TRIGGER_MASK   0xC0 /* +4: stage */
#define SUMP_CMD_TRIGGER_VALUE  0xC1
#define SUMP_CMD_TRIGGER_CONFIG 0xC2

#define SUMP_ID_REPLY   "1ALS" /* 对应 SLA1（协议版本 1），sigrok 接受 1SLO/1ALS */

/* 长命令判定：0x80-0x9F（配置）与 0xC0-0xCF（触发） */
#define IS_LONG_CMD(c) (((c) >= 0x80 && (c) <= 0x9F) || ((c) >= 0xC0 && (c) <= 0xCF))

/* 网格内存上限 16MB（样本数上限），事件缓冲上限 4M 事件（64MB） */
#define MAX_GRID_SAMPLES (16u * 1024 * 1024)
#define MAX_EVENTS       (4u * 1024 * 1024)
#define DEFAULT_PORT     9527
#define DEFAULT_MAX_WINDOW_MS 10000u /* 10 秒 */
#define BATCH_EVENTS     4096

/* ---------- 服务器状态 ---------- */
struct sump_state {
	int listen_fd;
	int client_fd;

	/* 长命令参数（小端解析后） */
	uint32_t divider;
	uint32_t flags;
	uint64_t readcount;  /* 已 +1 恢复的 readcount */
	uint64_t delaycount;
	int have_divider;
	int have_readcount;

	/* 采集控制 */
	struct capture_source *cs;
	uint32_t max_window_ms;
	int verbose;

	/* 动态事件缓冲 */
	struct edt_capture_event *evs;
	size_t evs_count;
	size_t evs_cap;
};

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

/* ---------- 小端读取（长命令参数） ---------- */
static uint32_t rd_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

/* ---------- TCP 工具 ---------- */
static int tcp_listen(uint16_t port)
{
	int fd;
	int one = 1;
	struct sockaddr_in addr;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(fd);
		return -1;
	}
	if (listen(fd, 2) < 0) {
		perror("listen");
		close(fd);
		return -1;
	}
	return fd;
}

/* 阻塞读满 n 字节（返回 0 成功，-1 失败/断开） */
static int tcp_read_full(int fd, void *buf, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, (char *)buf + got, n - got);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			return -1; /* 对端关闭 */
		got += (size_t)r;
	}
	return 0;
}

/* 阻塞写满 n 字节（采集样本流须连续，避免 sigrok 100ms 超时） */
static int tcp_write_full(int fd, const void *buf, size_t n)
{
	size_t sent = 0;
	while (sent < n) {
		ssize_t w = write(fd, (const char *)buf + sent, n - sent);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		sent += (size_t)w;
	}
	return 0;
}

/* ---------- 命令响应 ---------- */

/* metadata 键值流（整数一律大端，sigrok RB32/RB8 读取） */
static void send_metadata(int fd)
{
	static const uint8_t meta[] = {
		/* type 0：字符串 */
		0x01, 'e', 'd', 't', '-', 's', 'u', 'm', 'p', 0x00, /* 设备名 */
		0x02, '1', '.', '0', 0x00,                            /* 固件版本 */
		/* type 1：32 位大端整数 */
		0x20, 0x00, 0x00, 0x00, 0x08, /* 探针数 8 */
		0x21, 0x00, 0x40, 0x00, 0x00, /* 采样内存 4MB（>256K 触发 32 位 readcount 路径） */
		0x23, 0x00, 0x1E, 0x84, 0x80, /* 最大采样率 2MHz */
		0x24, 0x00, 0x00, 0x00, 0x02, /* 协议版本 2 */
		/* type 2：8 位整数 */
		0x40, 0x08,                   /* 探针数 8（short） */
		0x41, 0x02,                   /* 协议版本 2（short） */
		0x00                          /* 结束 */
	};
	(void)tcp_write_full(fd, meta, sizeof(meta));
}

/* ---------- 事件采集（覆盖重建窗口） ---------- */
static int append_event(struct sump_state *st, const struct edt_capture_event *ev)
{
	if (st->evs_count >= MAX_EVENTS)
		return -1;
	if (st->evs_count == st->evs_cap) {
		size_t ncap = st->evs_cap ? st->evs_cap * 2 : 4096;
		struct edt_capture_event *ne;
		ne = realloc(st->evs, ncap * sizeof(*ne));
		if (!ne)
			return -1;
		st->evs = ne;
		st->evs_cap = ncap;
	}
	st->evs[st->evs_count++] = *ev;
	return 0;
}

/*
 * 采集事件直到：
 *   - 事件时间跨度 >= window_ns（覆盖完整网格窗口），或
 *   - 事件静止（poll 超时 200ms，信号已停止），或
 *   - 达到 max_window_ms 上限
 * 返回 0 成功（可能采集不足，网格后段电平保持）；<0 错误。
 */
static int acquire_events(struct sump_state *st, uint64_t window_ns)
{
	struct edt_capture_event batch[BATCH_EVENTS];
	uint64_t t0 = 0;
	int n;

	for (;;) {
		int timeout_ms;

		if (g_stop)
			return -1;
		if (st->evs_count == 0) {
			timeout_ms = (int)(window_ns / 1000000ULL) + 1;
			if (timeout_ms > 5000)
				timeout_ms = 5000;
		} else {
			uint64_t span =
				st->evs[st->evs_count - 1].timestamp_ns - t0;
			if (span >= window_ns)
				break; /* 事件跨度已覆盖窗口 */
			timeout_ms = (int)((window_ns - span) / 1000000ULL) + 1;
			if (timeout_ms > 200)
				timeout_ms = 200; /* 静止判定阈值 */
		}

		n = st->cs->ops->read(st->cs, batch, BATCH_EVENTS, timeout_ms);
		if (n < 0) {
			fprintf(stderr, "采集错误: %s\n", strerror(-n));
			return -1;
		}
		if (n == 0) {
			if (st->evs_count > 0)
				break; /* 事件静止，提前结束 */
			/* 窗口内无任何事件（理论上 START 快照必有，
			 * 此处防御驱动异常，避免死循环等待） */
			break;
		}
		for (int i = 0; i < n; i++) {
			if (st->evs_count == 0)
				t0 = batch[i].timestamp_ns;
			if (append_event(st, &batch[i]) < 0) {
				fprintf(stderr, "事件缓冲溢出（>%u 事件）\n",
					(unsigned)MAX_EVENTS);
				return -1;
			}
		}
		if (st->verbose) {
			printf("已采集 %zu 事件, 跨度 %.3f ms\n",
			       st->evs_count,
			       (double)(st->evs[st->evs_count-1].timestamp_ns - t0) / 1e6);
		}
	}
	return 0;
}

/* ---------- 一次采集会话：采集 → 重建 → 倒序发送 ---------- */
static int run_acquisition(struct sump_state *st)
{
	uint64_t n_samples;
	uint32_t rate;
	uint64_t window_ns;
	struct grid_result grid;
	int rc = -1;

	/* 采样率：R = CLOCK / (divider+1)；DEMUX(bit0) 时 ×2 */
	if (!st->have_divider)
		st->divider = 0;
	rate = SUMP_CLOCK_RATE / (st->divider + 1);
	if (st->flags & 0x01)
		rate *= 2;

	/* 样本数：readcount 恢复后 ×4（OLS 内存 32 位宽语义） */
	if (!st->have_readcount)
		st->readcount = 256;
	n_samples = st->readcount * 4;
	if (n_samples > MAX_GRID_SAMPLES) {
		fprintf(stderr, "样本数 %llu 超上限 %u，截断\n",
			(unsigned long long)n_samples, MAX_GRID_SAMPLES);
		n_samples = MAX_GRID_SAMPLES;
	}

	/* 采集窗口（纳秒）：与 event_reconstructor 网格 dt 一致（向上取整），
	 * 受 max_window 保护 */
	{
		uint64_t dt_ns = (1000000000ULL + rate - 1) / rate;
		uint64_t cap_ns = (uint64_t)st->max_window_ms * 1000000ULL;
		window_ns = dt_ns * n_samples;
		if (window_ns > cap_ns)
			window_ns = cap_ns;
	}

	if (st->verbose) {
		printf("采集: R=%u Hz, 样本=%llu, 窗口=%.3f ms\n",
		       rate, (unsigned long long)n_samples,
		       (double)window_ns / 1e6);
	}

	/* 清空上次事件缓冲 */
	st->evs_count = 0;

	rc = st->cs->ops->start(st->cs);
	if (rc < 0)
		return rc;
	rc = acquire_events(st, window_ns);
	if (rc < 0) {
		st->cs->ops->stop(st->cs);
		return rc;
	}
	st->cs->ops->stop(st->cs);

	/* 重建网格 */
	rc = event_reconstruct(st->evs, st->evs_count, rate,
			       (size_t)n_samples, &grid);
	if (rc < 0) {
		fprintf(stderr, "网格重建失败\n");
		return -1;
	}
	if (st->verbose) {
		printf("重建: %zu 样本, 事件 %llu (窗口内), 越窗 %llu, "
		       "同单元碰撞 %llu\n",
		       grid.count,
		       (unsigned long long)grid.events_used,
		       (unsigned long long)grid.events_beyond,
		       (unsigned long long)grid.collisions);
	}

	/* 倒序发送：最新样本在前（OLS 约定，sigrok 反转后时间正序） */
	{
		uint8_t *rev;

		if (st->verbose)
			fprintf(stderr, "[step] 开始发送 %llu 样本\n",
				(unsigned long long)n_samples);
		rev = malloc(n_samples);
		if (!rev) {
			grid_result_free(&grid);
			return -1;
		}
		for (uint64_t i = 0; i < n_samples; i++)
			rev[i] = grid.samples[n_samples - 1 - i];
		rc = tcp_write_full(st->client_fd, rev, n_samples);
		free(rev);
		if (st->verbose)
			fprintf(stderr, "[step] 发送完成 rc=%d\n", rc);
	}
	grid_result_free(&grid);
	return rc;
}

/* ---------- 命令循环（单客户端） ---------- */
static void client_loop(struct sump_state *st)
{
	uint8_t cmd;
	uint8_t arg[4];
	int running = 0;

	for (;;) {
		if (g_stop)
			return;
		if (tcp_read_full(st->client_fd, &cmd, 1) < 0)
			return; /* 对端关闭/断开 */

		if (IS_LONG_CMD(cmd)) {
			if (tcp_read_full(st->client_fd, arg, 4) < 0)
				return;
		}

		switch (cmd) {
		case SUMP_CMD_RESET:
			/* 重置采集参数（sigrok 发 5 次） */
			st->divider = 0;
			st->flags = 0;
			st->readcount = 0;
			st->delaycount = 0;
			st->have_divider = 0;
			st->have_readcount = 0;
			running = 0;
			break;

		case SUMP_CMD_ID:
			if (tcp_write_full(st->client_fd, SUMP_ID_REPLY, 4) < 0)
				return;
			break;

		case SUMP_CMD_METADATA:
			send_metadata(st->client_fd);
			break;

		case SUMP_CMD_RUN:
			/* 采集期间不再读命令；完成后回到 IDLE */
			if (!running) {
				running = 1;
				if (run_acquisition(st) < 0)
					return; /* 采集失败即断开 */
				running = 0;
			}
			break;

		case SUMP_CMD_FINISH_NOW:
		case SUMP_CMD_ARM_ADV:
		case SUMP_CMD_QUERY_INPUT:
			/* 未实现：sigrok 不使用；QUERY_INPUT 回复 4 字节 0 */
			if (cmd == SUMP_CMD_QUERY_INPUT) {
				uint32_t zero = 0;
				(void)tcp_write_full(st->client_fd, &zero, 4);
			}
			break;

		/* ---------- 长命令 ---------- */
		case SUMP_CMD_SET_DIVIDER:
			st->divider = rd_le32(arg) & 0xFFFFFFu;
			st->have_divider = 1;
			if (st->verbose)
				printf("divider=%u → R=%u Hz\n",
				       st->divider,
				       SUMP_CLOCK_RATE / (st->divider + 1));
			break;

		case SUMP_CMD_CAPTURE_SIZE: /* 0x81: 16+16 */
			st->readcount = (uint64_t)rd_le16(arg) + 1;
			st->delaycount = (uint64_t)rd_le16(arg + 2) + 1;
			st->have_readcount = 1;
			break;

		case SUMP_CMD_SET_FLAGS:
			st->flags = rd_le32(arg);
			if (st->verbose)
				printf("flags=0x%08x\n", st->flags);
			break;

		case SUMP_CMD_CAPTURE_DELAYCOUNT: /* 0x83: 32 */
			st->delaycount = (uint64_t)rd_le32(arg) + 1;
			break;

		case SUMP_CMD_CAPTURE_READCOUNT: /* 0x84: 32 */
			st->readcount = (uint64_t)rd_le32(arg) + 1;
			st->have_readcount = 1;
			if (st->verbose)
				printf("readcount=%llu → %llu 样本\n",
				       (unsigned long long)st->readcount,
				       (unsigned long long)st->readcount * 4);
			break;

		case SUMP_CMD_TRIGGER_MASK:
		case SUMP_CMD_TRIGGER_VALUE:
		case SUMP_CMD_TRIGGER_CONFIG:
			/* 触发：本实现始终全量采集（sigrok 无触发时强制
			 * stage0 mask=0/value=0 → 立即触发，全量输出）。
			 * 有触发配置时同样全量采集，由 sigrok 侧切分。 */
			break;

		default:
			/* 未知命令：忽略（保持协议健壮） */
			break;
		}
	}
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"用法: %s [选项]\n"
		"  --device <路径>  数据源设备（默认 /dev/edt_capture0）\n"
		"  --sim            使用内置合成信号源（自测/演示，无需板端）\n"
		"  --port <端口>    SUMP TCP 端口（默认 %d）\n"
		"  --max-window <ms> 最大采集窗口毫秒（默认 %u）\n"
		"  --verbose        打印采集/协议日志\n"
		"  -h               帮助\n",
		prog, DEFAULT_PORT, DEFAULT_MAX_WINDOW_MS);
}

int main(int argc, char **argv)
{
	const char *device = "/dev/edt_capture0";
	int use_sim = 0;
	uint16_t port = DEFAULT_PORT;
	struct sump_state st;
	struct sockaddr_in peer;
	socklen_t plen = sizeof(peer);

	/* 崩溃诊断保障：行缓冲/无缓冲确保 SIGSEGV/SIGILL 前 verbose 日志不丢 */
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	memset(&st, 0, sizeof(st));
	st.max_window_ms = DEFAULT_MAX_WINDOW_MS;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--sim")) {
			use_sim = 1;
		} else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			device = argv[++i];
		} else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
			port = (uint16_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--max-window") && i + 1 < argc) {
			st.max_window_ms = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--verbose")) {
			st.verbose = 1;
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	{
		/* 无 SA_RESTART：SIGINT/SIGTERM 打断阻塞中的 accept()（EINTR），
		 * 主循环顶部检查 g_stop 后干净退出。
		 * （修复：signal() 默认 SA_RESTART 会自动重启 accept()，
		 *   导致空闲服务器无法被 SIGTERM 终止、占死端口） */
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_sigint;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
		signal(SIGPIPE, SIG_IGN);
	}

	/* 数据源 */
	if (st.verbose)
		fprintf(stderr, "[step] 创建数据源 (%s)\n",
			use_sim ? "sim" : device);
	st.cs = use_sim ? capture_source_sim_new() : capture_source_edt_new();
	if (!st.cs) {
		fprintf(stderr, "数据源创建失败\n");
		return 1;
	}
	if (!use_sim && st.cs->ops->open(st.cs, device) < 0)
		return 1;
	if (st.verbose)
		fprintf(stderr, "[step] 设备打开成功\n");

	/* 监听 */
	st.listen_fd = tcp_listen(port);
	if (st.listen_fd < 0)
		return 1;
	printf("SUMP 服务器就绪: tcp://<本机IP>:%u (%s)\n",
	       port, use_sim ? "sim 合成源" : device);

	for (;;) {
		int cfd;
		if (g_stop)
			break;
		cfd = accept(st.listen_fd, (struct sockaddr *)&peer, &plen);
		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			perror("accept");
			break;
		}
		printf("客户端接入: %s:%u\n",
		       inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
		st.client_fd = cfd;
		client_loop(&st);
		close(cfd);
		st.client_fd = -1;
		printf("客户端断开\n");
	}

	if (st.cs->ops->stop)
		st.cs->ops->stop(st.cs);
	st.cs->ops->close(st.cs);
	close(st.listen_fd);
	free(st.evs);
	return 0;
}