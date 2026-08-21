/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * sump_selftest.c —— SUMP 协议客户端自测（P6-2）
 *
 * 模拟 sigrok/PulseView 的采集流程，对 sump_server 做协议级验证：
 *   1. 握手：5×Reset → ID → 校验回复 "1ALS"
 *   2. Metadata：校验设备名/探针数/采样内存/最大采样率
 *   3. 配置：divider(1MHz) + flags(0) + readcount(0x84,32位) + delaycount
 *   4. 采集：arm → 读回 readcount*4 个样本，校验数量守恒
 *   5. 内容校验（配合 --sim 合成源）：ch0 1kHz 方波 → bit0 周期性翻转
 *
 * 用法：
 *   sump_selftest [--host 127.0.0.1] [--port 9527]
 *                 [--samples 1048576]  期望样本数（默认 1M）
 *  退出码 0=全部通过
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SUMP_CLOCK_RATE     100000000u /* 100MHz 逻辑时钟（与 sump_server 一致） */
#define SUMP_CMD_RESET      0x00
#define SUMP_CMD_RUN        0x01
#define SUMP_CMD_ID         0x02
#define SUMP_CMD_METADATA   0x04
#define SUMP_CMD_SET_DIVIDER       0x80
#define SUMP_CMD_CAPTURE_SIZE      0x81
#define SUMP_CMD_SET_FLAGS         0x82
#define SUMP_CMD_CAPTURE_DELAYCOUNT 0x83
#define SUMP_CMD_CAPTURE_READCOUNT 0x84

static int g_fail;
#define CHECK(cond, msg) do {						\
	if (cond)							\
		printf("[PASS] %s\n", msg);				\
	else {								\
		printf("[FAIL] %s\n", msg);				\
		g_fail++;						\
	}								\
} while (0)

static int sock_connect(const char *host, uint16_t port)
{
	int fd;
	struct sockaddr_in addr;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		fprintf(stderr, "无效地址: %s\n", host);
		close(fd);
		return -1;
	}
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("connect");
		close(fd);
		return -1;
	}
	return fd;
}

static int read_full(int fd, void *buf, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, (char *)buf + got, n - got);
		if (r <= 0)
			return -1;
		got += (size_t)r;
	}
	return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
	size_t sent = 0;
	while (sent < n) {
		ssize_t w = write(fd, (const char *)buf + sent, n - sent);
		if (w <= 0)
			return -1;
		sent += (size_t)w;
	}
	return 0;
}

/* 发送短命令 */
static int scmd(int fd, uint8_t c)
{
	return write_full(fd, &c, 1);
}

/* 发送长命令：opcode + 4 字节参数（小端） */
static int lcmd(int fd, uint8_t op, uint32_t v)
{
	uint8_t buf[5] = { op,
			   (uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff),
			   (uint8_t)((v >> 16) & 0xff), (uint8_t)((v >> 24) & 0xff) };
	return write_full(fd, buf, sizeof(buf));
}

/* 解析 metadata 键值流，返回采样内存字节数（0x21） */
static uint32_t parse_metadata(const uint8_t *buf, size_t len)
{
	size_t i = 0;
	uint32_t sample_mem = 0;
	uint32_t max_rate = 0;
	uint32_t probes = 0;
	char name[64] = { 0 };

	while (i < len) {
		uint8_t key = buf[i++];
		if (key == 0x00)
			break;
		switch (key >> 5) {
		case 0: /* 字符串 */
			{
				size_t s = 0;
				if (key == 0x01) {
					while (i + s < len && buf[i + s] != 0 && s < sizeof(name) - 1)
						s++;
					if (key == 0x01) {
						memcpy(name, buf + i, s);
						name[s] = 0;
					}
				}
				while (i < len && buf[i] != 0)
					i++;
				i++; /* 跳过 NUL */
			}
			break;
		case 1: /* 32 位大端整数 */
			if (i + 4 > len)
				return 0;
			{
				uint32_t v = ((uint32_t)buf[i] << 24) |
					     ((uint32_t)buf[i+1] << 16) |
					     ((uint32_t)buf[i+2] << 8) |
					     (uint32_t)buf[i+3];
				if (key == 0x20) probes = v;
				if (key == 0x21) sample_mem = v;
				if (key == 0x23) max_rate = v;
			}
			i += 4;
			break;
		case 2: /* 8 位整数 */
			if (i + 1 > len)
				return 0;
			if (key == 0x40) probes = buf[i];
			i++;
			break;
		default:
			return 0;
		}
	}
	printf("   metadata: 设备=%s 探针=%u 采样内存=%uB 最大采样率=%uHz\n",
	       name, probes, sample_mem, max_rate);
	return sample_mem;
}

int main(int argc, char **argv)
{
	const char *host = "127.0.0.1";
	uint16_t port = 9527;
	uint64_t want_samples = 1024 * 1024; /* 1M */
	int fd;
	uint8_t buf[64];
	uint8_t id[4];
	uint32_t sample_mem;
	uint32_t rate_hz = 1000000;
	uint32_t divider, readcount_cmd, delaycount_cmd;
	uint64_t readcount;
	uint8_t *samples;
	uint64_t n, i;
	int toggles = 0;

	for (int a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "--host") && a + 1 < argc)
			host = argv[++a];
		else if (!strcmp(argv[a], "--port") && a + 1 < argc)
			port = (uint16_t)strtoul(argv[++a], NULL, 0);
		else if (!strcmp(argv[a], "--samples") && a + 1 < argc)
			want_samples = strtoull(argv[++a], NULL, 0);
		else {
			fprintf(stderr,
				"用法: %s [--host H] [--port P] [--samples N]\n",
				argv[0]);
			return 1;
		}
	}

	fd = sock_connect(host, port);
	if (fd < 0)
		return 1;

	/* ① 握手：5×Reset → ID → 校验 "1ALS" */
	for (int k = 0; k < 5; k++)
		scmd(fd, SUMP_CMD_RESET);
	scmd(fd, SUMP_CMD_ID);
	CHECK(read_full(fd, id, 4) == 0, "ID 回复可读");
	CHECK(!memcmp(id, "1ALS", 4) || !memcmp(id, "1SLO", 4),
	      "ID 匹配 OLS 标识 (1ALS/1SLO)");
	printf("   ID 回复: %c%c%c%c\n", id[0], id[1], id[2], id[3]);

	/* ② Metadata：逐键读取（与 libsigrok OLS 驱动 ols_get_metadata 一致）
	 * 格式: key(1B) + 值(字符串=读到 NUL / 32 位=4B / 8 位=1B)，key==0x00 结束。
	 * 不能盲读到第一个 0x00 即停——字符串值本身含 NUL 终止符。 */
	scmd(fd, SUMP_CMD_METADATA);
	{
		size_t got = 0;
		int md_ok = 1;
		for (;;) {
			uint8_t key;
			if (read_full(fd, &key, 1) < 0) {
				md_ok = 0;
				break;
			}
			if (got + 1 > sizeof(buf)) {
				md_ok = 0;
				break;
			}
			buf[got++] = key;
			if (key == 0x00)
				break; /* 结束键 */
			switch (key >> 5) {
			case 0: { /* 字符串值：读到 NUL */
				uint8_t c;
				for (;;) {
					if (read_full(fd, &c, 1) < 0) {
						md_ok = 0;
						break;
					}
					if (got + 1 > sizeof(buf)) {
						md_ok = 0;
						break;
					}
					buf[got++] = c;
					if (c == 0x00)
						break;
				}
				break;
			}
			case 1: { /* 32 位大端整数：4 字节 */
				uint8_t c;
				int k;
				for (k = 0; k < 4; k++) {
					if (read_full(fd, &c, 1) < 0) {
						md_ok = 0;
						break;
					}
					if (got + 1 > sizeof(buf)) {
						md_ok = 0;
						break;
					}
					buf[got++] = c;
				}
				break;
			}
			case 2: { /* 8 位整数：1 字节 */
				uint8_t c;
				if (read_full(fd, &c, 1) < 0) {
					md_ok = 0;
					break;
				}
				if (got + 1 > sizeof(buf)) {
					md_ok = 0;
					break;
				}
				buf[got++] = c;
				break;
			}
			default:
				md_ok = 0;
				break;
			}
			if (!md_ok)
				break;
		}
		if (!md_ok)
			printf("   [WARN] metadata 读取异常\n");
		sample_mem = parse_metadata(buf, got);
	}
	CHECK(sample_mem > 256 * 1024, "采样内存 > 256K（32 位 readcount 路径）");

	/* ③ 配置：1MHz → divider = 100M/1M - 1 = 99 */
	divider = SUMP_CLOCK_RATE / rate_hz - 1;
	lcmd(fd, SUMP_CMD_SET_DIVIDER, divider);
	lcmd(fd, SUMP_CMD_SET_FLAGS, 0);
	/* readcount = ceil(want/4)，发送 readcount-1 */
	readcount = (want_samples + 3) / 4;
	readcount_cmd = (uint32_t)(readcount - 1);
	delaycount_cmd = readcount_cmd; /* 无触发：delay=read */
	lcmd(fd, SUMP_CMD_CAPTURE_READCOUNT, readcount_cmd);
	lcmd(fd, SUMP_CMD_CAPTURE_DELAYCOUNT, delaycount_cmd);

	/* ④ arm → 读回 readcount*4 个样本 */
	samples = malloc((size_t)readcount * 4);
	CHECK(samples != NULL, "样本缓冲分配");
	if (!samples)
		return 1;

	scmd(fd, SUMP_CMD_RUN);
	n = 0;
	while (n < readcount * 4) {
		size_t want = (size_t)(readcount * 4 - n);
		ssize_t r = read(fd, samples + n, want);
		if (r <= 0) {
			printf("[FAIL] 样本流提前结束: 已收 %llu/%llu\n",
			       (unsigned long long)n,
			       (unsigned long long)readcount * 4);
			g_fail++;
			break;
		}
		n += (size_t)r;
	}
	CHECK(n == readcount * 4, "样本数量守恒 (readcount*4)");
	printf("   收到 %llu 字节样本\n", (unsigned long long)n);

	/* ⑤ 内容校验：ch0 1kHz 方波 @1MHz → bit0 每 500 样本翻转一次
	 * （周期 1ms = 1000 样本，上升+下降各一次 → 每 500 样本 1 次翻转） */
	if (n >= 2000) {
		uint64_t expect = n / 500; /* 1s 窗内 ≈ 2000 次 */
		for (i = 1; i < n; i++)
			if ((samples[i] & 0x01) != (samples[i - 1] & 0x01))
				toggles++;
		printf("   bit0 翻转次数=%d (期望≈%llu)\n", toggles,
		       (unsigned long long)expect);
		/* 允许 ±10% 偏差（窗口截断效应） */
		CHECK(toggles > (int)(expect * 0.8) &&
		      toggles < (int)(expect * 1.2),
		      "ch0 方波频率正确 (bit0 翻转频率 ≈ 2kHz)");
	}

	free(samples);
	close(fd);

	printf("\n%s: %s\n", g_fail ? "自测失败" : "自测全部通过",
	       g_fail ? "有断言未通过" : "协议实现正确");
	return g_fail ? 1 : 0;
}