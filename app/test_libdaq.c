/*
 * test_libdaq.c - libdaq C API 全面测试
 *
 * 编译（Linux 机器上）:
 *   arm-linux-gnueabihf-gcc -o test_libdaq test_libdaq.c -I. -Lbuild/app -ldaq
 *
 * 或通过 Makefile:
 *   make -C app test_libdaq
 *
 * 运行（板子上）:
 *   LD_LIBRARY_PATH=/lib/modules/4.1.15 ./test_libdaq
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "libdaq.h"

static int passed = 0;
static int failed = 0;

#define TEST(name, expr) do { \
	printf("  [TEST] %-45s ... ", name); \
	if (expr) { \
		printf("PASS\n"); \
		passed++; \
	} else { \
		printf("FAIL\n"); \
		failed++; \
	} \
} while (0)

int main(void)
{
	int fd;
	struct indaq_info info;
	struct indaq_sample samples[8];
	int n, ret;
	int i;

	printf("========================================\n");
	printf("  libdaq API 全面测试\n");
	printf("========================================\n\n");

	/* ---- 1. indaq_open ---- */
	printf("[1] indaq_open / indaq_close\n");
	fd = indaq_open("/dev/indaq");
	TEST("indaq_open returns valid fd", fd >= 0);

	if (fd < 0) {
		printf("\n✗ 无法打开 /dev/indaq，终止测试\n");
		return 1;
	}

	/* ---- 2. indaq_get_info ---- */
	printf("\n[2] indaq_get_info\n");
	memset(&info, 0, sizeof(info));
	ret = indaq_get_info(fd, &info);
	TEST("indaq_get_info succeeds", ret == 0);
	if (ret == 0) {
		printf("    version:       %u.%u\n",
		       info.version >> 8, info.version & 0xFF);
		printf("    sampling_rate: %u Hz\n", info.sampling_rate);
		printf("    total_samples: %u\n", info.total_samples);
		printf("    errors:        %u\n", info.errors);
		TEST("version == 1.0", info.version == 0x0100);
		TEST("sampling_rate > 0", info.sampling_rate > 0);
	}

	/* ---- 3. indaq_set_rate ---- */
	printf("\n[3] indaq_set_rate\n");
	ret = indaq_set_rate(fd, 50);
	TEST("indaq_set_rate(50) succeeds", ret == 0);

	/* 验证采样率已更新 */
	memset(&info, 0, sizeof(info));
	indaq_get_info(fd, &info);
	TEST("sampling_rate now 50 Hz", info.sampling_rate == 50);

	ret = indaq_set_rate(fd, 200);
	TEST("indaq_set_rate(200) rejects out-of-range", ret != 0);

	/* ---- 4. indaq_start ---- */
	printf("\n[4] indaq_start\n");
	ret = indaq_start(fd);
	TEST("indaq_start succeeds", ret == 0);

	/* ---- 5. indaq_wait + indaq_read ---- */
	printf("\n[5] indaq_wait / indaq_read\n");

	/* 等待数据就绪 */
	ret = indaq_wait(fd, 2000); /* 最多等 2 秒 */
	TEST("indaq_wait returns data available", ret > 0);

	/* 读取 8 个样本 */
	memset(samples, 0, sizeof(samples));
	n = indaq_read(fd, samples, 8);
	TEST("indaq_read returns > 0 samples", n > 0);
	if (n > 0) {
		TEST("samples have valid timestamps",
		     samples[0].ts_ns > 0);
		TEST("samples have valid accel/gyro/temp data",
		     samples[0].ax != 0 || samples[0].ay != 0 ||
		     samples[0].az != 0 || samples[0].temp != 0);

		printf("\n    ┌──────┬──────────────────┬───────┬───────┬───────┬───────┬───────┬───────┬───────┐\n");
		printf("    │  #   │ timestamp         │ ax    │ ay    │ az    │ gx    │ gy    │ gz    │ temp  │\n");
		printf("    ├──────┼──────────────────┼───────┼───────┼───────┼───────┼───────┼───────┼───────┤\n");
		for (i = 0; i < n && i < 5; i++) {
			printf("    │ %4d │ %16llu │ %5d │ %5d │ %5d │ %5d │ %5d │ %5d │ %5d │\n",
			       i, (unsigned long long)samples[i].ts_ns,
			       samples[i].ax, samples[i].ay, samples[i].az,
			       samples[i].gx, samples[i].gy, samples[i].gz,
			       samples[i].temp);
		}
		if (n > 5)
			printf("    │ ...  │ (%d more samples)                          │\n", n - 5);
		printf("    └──────┴──────────────────┴───────┴───────┴───────┴───────┴───────┴───────┴───────┘\n");

		/* 用 indaq_print_sample 打印第一个 */
		printf("\n    indaq_print_sample output:\n    ");
		indaq_print_sample(&samples[0]);
		printf("\n");
	}

	/* ---- 6. 重复等待+读取（验证多次调用） ---- */
	printf("[6] 多次 indaq_wait / indaq_read\n");
	for (i = 0; i < 3; i++) {
		ret = indaq_wait(fd, 1000);
		if (ret > 0) {
			n = indaq_read(fd, samples, 4);
			printf("    batch %d: read %d samples\n", i + 1, n);
		} else {
			printf("    batch %d: wait timeout (ret=%d)\n", i + 1, ret);
		}
	}

	/* ---- 7. indaq_stop ---- */
	printf("\n[7] indaq_stop\n");
	ret = indaq_stop(fd);
	TEST("indaq_stop succeeds", ret == 0);

	/* ---- 8. indaq_close ---- */
	printf("\n[8] indaq_close\n");
	indaq_close(fd);
	TEST("indaq_close (no crash)", 1);

	/* 重复 close 确保不崩溃 */
	indaq_close(fd);
	TEST("indaq_close double-call (no crash)", 1);

	/* ---- 汇总 ---- */
	printf("\n========================================\n");
	printf("  测试结果: %d 通过, %d 失败", passed, failed);
	if (failed > 0)
		printf(" ⚠️");
	else
		printf(" ✅");
	printf("\n========================================\n");

	return failed > 0 ? 1 : 0;
}
