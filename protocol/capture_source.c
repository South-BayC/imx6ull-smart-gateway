/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * capture_source.c —— SUMP 工具链数据源后端实现（P6-2）
 *
 * edt_capture 后端：真实板端数据源
 *   open  : O_RDWR 独占打开（驱动 CAP_EXCLUSIVE_OPEN）
 *   start : SET_CONFIG(全通道,双沿) → CLEAR_FIFO → RESET_STATS → START
 *   read  : poll 超时 + 阻塞 read（16 字节对齐事件）
 *
 * sim 后端：确定性合成信号源（自测/演示用）
 *   ch0=1kHz ch1=500Hz ch2=250Hz ch3=125Hz 方波（50% 占空比），
 *   覆盖 1.2 秒时间窗，事件带真实单调时钟时间戳，按时间排序。
 *   序列号与驱动语义一致（快照 sequence 从 1 开始）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include "capture_source.h"

#define EDT_DEV_DEFAULT "/dev/edt_capture0"

/* ================ edt_capture 后端 ================ */

struct edt_priv {
	int fd;
};

static int edt_open(struct capture_source *cs, const char *dev)
{
	struct edt_priv *p = cs->priv;
	struct edt_capture_caps caps;
	struct edt_capture_config cfg;

	if (!dev)
		dev = EDT_DEV_DEFAULT;

	p->fd = open(dev, O_RDWR);
	if (p->fd < 0) {
		fprintf(stderr, "capture_source: open %s: %s\n",
			dev, strerror(errno));
		return -errno;
	}

	/* 能力校验：ABI 版本 + 通道数 */
	if (ioctl(p->fd, EDT_IOC_GET_CAPS, &caps) < 0) {
		fprintf(stderr, "capture_source: GET_CAPS: %s\n", strerror(errno));
		close(p->fd);
		p->fd = -1;
		return -errno;
	}
	if (caps.abi_version != EDT_CAPTURE_ABI_VERSION ||
	    caps.max_channels > EDT_CAPTURE_MAX_CHANNELS) {
		fprintf(stderr, "capture_source: ABI 不匹配 (dev=%u expect=%u)\n",
			caps.abi_version, EDT_CAPTURE_ABI_VERSION);
		close(p->fd);
		p->fd = -1;
		return -EPROTONOSUPPORT;
	}
	if (!(caps.flags & EDT_CAPTURE_CAP_INITIAL_SNAPSHOT)) {
		fprintf(stderr, "capture_source: 驱动缺少 INITIAL_SNAPSHOT 能力\n");
		close(p->fd);
		p->fd = -1;
		return -EPROTONOSUPPORT;
	}

	/* 全通道双沿配置 */
	memset(&cfg, 0, sizeof(cfg));
	cfg.abi_version = EDT_CAPTURE_ABI_VERSION;
	cfg.struct_size = sizeof(cfg);
	cfg.channel_mask = (1u << caps.configured_channels) - 1u;
	cfg.edge_mask = EDT_CAPTURE_EDGE_BOTH;
	if (ioctl(p->fd, EDT_IOC_SET_CONFIG, &cfg) < 0) {
		fprintf(stderr, "capture_source: SET_CONFIG: %s\n", strerror(errno));
		close(p->fd);
		p->fd = -1;
		return -errno;
	}
	return 0;
}

static int edt_start(struct capture_source *cs)
{
	struct edt_priv *p = cs->priv;

	/* 清残留 FIFO + 清统计：保证快照干净、sequence 从 1 开始 */
	if (ioctl(p->fd, EDT_IOC_CLEAR_FIFO) < 0) {
		fprintf(stderr, "capture_source: CLEAR_FIFO: %s\n", strerror(errno));
		return -errno;
	}
	if (ioctl(p->fd, EDT_IOC_RESET_STATS) < 0) {
		fprintf(stderr, "capture_source: RESET_STATS: %s\n", strerror(errno));
		return -errno;
	}
	if (ioctl(p->fd, EDT_IOC_START) < 0) {
		fprintf(stderr, "capture_source: START: %s\n", strerror(errno));
		return -errno;
	}
	return 0;
}

static int edt_read(struct capture_source *cs, struct edt_capture_event *evs,
		    size_t max, int timeout_ms)
{
	struct edt_priv *p = cs->priv;
	struct pollfd pfd;
	ssize_t n;
	int rc;

	if (max == 0)
		return 0;

	pfd.fd = p->fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	rc = poll(&pfd, 1, timeout_ms);
	if (rc < 0)
		return -errno;
	if (rc == 0)
		return 0; /* 超时无数据 */

	n = read(p->fd, evs, (size_t)max * sizeof(*evs));
	if (n < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return 0;
		return -errno;
	}
	return (int)(n / (ssize_t)sizeof(*evs));
}

static int edt_stop(struct capture_source *cs)
{
	struct edt_priv *p = cs->priv;
	if (ioctl(p->fd, EDT_IOC_STOP) < 0) {
		fprintf(stderr, "capture_source: STOP: %s\n", strerror(errno));
		return -errno;
	}
	return 0;
}

static void edt_close(struct capture_source *cs)
{
	struct edt_priv *p = cs->priv;
	if (p->fd >= 0) {
		close(p->fd);
		p->fd = -1;
	}
	free(p);
	cs->priv = NULL;
}

/* 函数表：文件级 static const，确保落位 .rodata。
 * （修复：原栈上复合字面量在部署二进制中未被初始化，cs->ops 指向垃圾，
 *   间接调用 open/start 即跳非法地址 —— ②④ 崩溃根因） */
static const struct capture_source_ops edt_ops = {
	.open  = edt_open,
	.start = edt_start,
	.read  = edt_read,
	.stop  = edt_stop,
	.close = edt_close,
};

struct capture_source *capture_source_edt_new(void)
{
	struct capture_source *cs;
	struct edt_priv *p;

	cs = calloc(1, sizeof(*cs));
	p = calloc(1, sizeof(*p));
	if (!cs || !p) {
		free(cs);
		free(p);
		return NULL;
	}
	p->fd = -1;
	cs->priv = p;
	cs->ops = &edt_ops;
	return cs;
}

/* ================ sim 后端（确定性合成信号源） ================ */

#define SIM_DURATION_NS (1200000000ULL) /* 1.2 秒 */
#define SIM_CH0_HZ 1000u
#define SIM_CH1_HZ 500u
#define SIM_CH2_HZ 250u
#define SIM_CH3_HZ 125u

struct sim_priv {
	struct edt_capture_event *evs;
	size_t count;
	size_t pos;
	int started;
};

/* 生成一路方波的边沿事件（50% 占空比，初始低电平） */
static size_t sim_gen_wave(struct edt_capture_event *evs, size_t cap,
			   unsigned ch, unsigned hz, uint64_t t0,
			   uint32_t *seq)
{
	uint64_t period = 1000000000ULL / hz;
	uint64_t half = period / 2;
	uint64_t t;
	size_t n = 0;

	/* 快照：t0 时刻初始电平 0 */
	if (n < cap) {
		evs[n].timestamp_ns = t0;
		evs[n].sequence = (*seq)++;
		evs[n].channel = ch;
		evs[n].level = 0;
		evs[n].edge = 0;
		n++;
	}
	/* 边沿：k*period 上升，k*period+half 下降 */
	for (t = t0 + period; t < t0 + SIM_DURATION_NS; t += period) {
		if (n + 2 > cap)
			break;
		evs[n].timestamp_ns = t;
		evs[n].sequence = (*seq)++;
		evs[n].channel = ch;
		evs[n].level = 1;
		evs[n].edge = 1; /* 上升沿 */
		n++;
		if (t + half < t0 + SIM_DURATION_NS) {
			evs[n].timestamp_ns = t + half;
			evs[n].sequence = (*seq)++;
			evs[n].channel = ch;
			evs[n].level = 0;
			evs[n].edge = 2; /* 下降沿 */
			n++;
		}
	}
	return n;
}

static int sim_cmp(const void *a, const void *b)
{
	const struct edt_capture_event *ea = a, *eb = b;
	if (ea->timestamp_ns < eb->timestamp_ns)
		return -1;
	if (ea->timestamp_ns > eb->timestamp_ns)
		return 1;
	return (int)ea->sequence - (int)eb->sequence;
}

static int sim_open(struct capture_source *cs, const char *dev)
{
	(void)cs;
	(void)dev;
	return 0;
}

static int sim_start(struct capture_source *cs)
{
	struct sim_priv *p = cs->priv;
	struct edt_capture_event *evs;
	uint32_t seq = 1;
	uint64_t t0;
	size_t n = 0, cap;

	if (p->started)
		return 0;

	/* 容量估算：4 通道快照 + 各通道边沿（每周期上升+下降各 1），
	 * 向上取整到 1 秒整窗并留余量，避免截断 */
	{
		uint64_t sum_hz = SIM_CH0_HZ + SIM_CH1_HZ + SIM_CH2_HZ + SIM_CH3_HZ;
		uint64_t dur_s = SIM_DURATION_NS / 1000000000ULL + 1;
		cap = 4 + 2 * sum_hz * dur_s + 64;
	}
	evs = calloc(cap, sizeof(*evs));
	if (!evs)
		return -ENOMEM;

	t0 = (uint64_t)1000000000ULL; /* 确定性起始时间戳（自测断言用） */
	n += sim_gen_wave(evs + n, cap - n, 0, SIM_CH0_HZ, t0, &seq);
	n += sim_gen_wave(evs + n, cap - n, 1, SIM_CH1_HZ, t0, &seq);
	n += sim_gen_wave(evs + n, cap - n, 2, SIM_CH2_HZ, t0, &seq);
	n += sim_gen_wave(evs + n, cap - n, 3, SIM_CH3_HZ, t0, &seq);

	qsort(evs, n, sizeof(*evs), sim_cmp);
	/* 排序后重新编号：与真实驱动一致（sequence 按时间入队顺序递增） */
	for (size_t k = 0; k < n; k++)
		evs[k].sequence = (uint32_t)k + 1;
	p->evs = evs;
	p->count = n;
	p->pos = 0;
	p->started = 1;
	return 0;
}

static int sim_read(struct capture_source *cs, struct edt_capture_event *evs,
		    size_t max, int timeout_ms)
{
	struct sim_priv *p = cs->priv;
	size_t avail, take;

	(void)timeout_ms;
	if (!p->started)
		return 0;
	avail = p->count - p->pos;
	if (avail == 0)
		return 0;
	take = avail < max ? avail : max;
	memcpy(evs, p->evs + p->pos, take * sizeof(*evs));
	p->pos += take;
	return (int)take;
}

static int sim_stop(struct capture_source *cs)
{
	(void)cs;
	return 0;
}

static void sim_close(struct capture_source *cs)
{
	struct sim_priv *p = cs->priv;
	if (p) {
		free(p->evs);
		free(p);
		cs->priv = NULL;
	}
}

/* 同上：文件级 static const 函数表 */
static const struct capture_source_ops sim_ops = {
	.open  = sim_open,
	.start = sim_start,
	.read  = sim_read,
	.stop  = sim_stop,
	.close = sim_close,
};

struct capture_source *capture_source_sim_new(void)
{
	struct capture_source *cs;
	struct sim_priv *p;

	cs = calloc(1, sizeof(*cs));
	p = calloc(1, sizeof(*p));
	if (!cs || !p) {
		free(cs);
		free(p);
		return NULL;
	}
	cs->priv = p;
	cs->ops = &sim_ops;
	return cs;
}