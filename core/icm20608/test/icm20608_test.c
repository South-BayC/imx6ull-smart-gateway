// SPDX-License-Identifier: GPL-2.0
/* icm20608_test：ICM20608 六轴 IMU 读取测试（P4-B，手册 5.13.3）
 * 用法:
 *   ./icm20608_test [--num N]    默认无限循环，每秒打印一组六轴+温度
 * 验收（手册 5.13.4）:
 *   - 静止时 accel_z ≈ 1.0g，accel_x/y ≈ 0，gyro 三轴 ≈ 0
 *   - temp ≈ 室温（25℃ 附近）
 * 量程: ±16G / ±2000dps（驱动 probe 配置，见 uapi/icm20608.h）
 * 交叉编译: arm-linux-gnueabihf-gcc icm20608_test.c -o icm20608_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

#define DEV_PATH	"/dev/icm20608"
#define READ_LEN	28	/* 7×s32，与内核 uapi/icm20608.h 一致 */
#define WORD_CNT	7

/* 换算（内核只出原始值，物理量在此换算） */
#define GYRO_FS	2000.0f	/* ±2000dps */
#define ACCEL_FS	16.0f	/* ±16G */
#define TEMP_SLOPE	326.8f	/* ℃ = raw/326.8 + 25 */
#define TEMP_OFFSET	25.0f

static void print_usage(const char *prog)
{
	fprintf(stderr, "用法: %s [--num N]   每秒打印一组，默认无限循环\n", prog);
}

int main(int argc, char *argv[])
{
	int fd, count = -1, i = 0;
	int32_t raw[WORD_CNT];
	float gx, gy, gz, ax, ay, az, tc;
	ssize_t n;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--num") && i + 1 < argc) {
			count = atoi(argv[++i]);
		} else {
			print_usage(argv[0]);
			return 1;
		}
	}

	fd = open(DEV_PATH, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s 失败: %s\n", DEV_PATH, strerror(errno));
		return 1;
	}
	setvbuf(stdout, NULL, _IONBF, 0);	/* 立即输出，避免 NFS/管道丢行 */

	i = 0;
	for (;;) {
		n = read(fd, raw, sizeof(raw));
		if (n != (ssize_t)sizeof(raw)) {
			fprintf(stderr, "read 失败(n=%zd): %s\n",
				n, strerror(errno));
			break;
		}

		gx = raw[0] * GYRO_FS / 32768.0f;
		gy = raw[1] * GYRO_FS / 32768.0f;
		gz = raw[2] * GYRO_FS / 32768.0f;
		ax = raw[3] * ACCEL_FS / 32768.0f;
		ay = raw[4] * ACCEL_FS / 32768.0f;
		az = raw[5] * ACCEL_FS / 32768.0f;
		tc = raw[6] / TEMP_SLOPE + TEMP_OFFSET;

		printf("gyro(%+7.2f %+7.2f %+7.2f)dps | "
		       "accel(%+6.3f %+6.3f %+6.3f)g | temp %6.2fC\n",
		       gx, gy, gz, ax, ay, az, tc);

		if (count >= 0 && ++i >= count)
			break;
		sleep(1);
	}

	close(fd);
	return 0;
}
