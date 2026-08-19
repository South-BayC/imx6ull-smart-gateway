// SPDX-License-Identifier: GPL-2.0
/* beep_test：PWM 蜂鸣器测试程序（P4-B，手册 5.13.1）
 * 用法: beep_test --freq 500 --duty 50   （500Hz 50% 占空比响）
 *       beep_test --duty 0               （静音）
 *       beep_test --on / --off           （开关）
 * 注: stdout 无缓冲（setvbuf _IONBF），避免板端非交互 shell 丢行
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "beep_pwm.h"

#define DEV_PATH "/dev/beep_pwm0"

static void usage(const char *prog)
{
	fprintf(stderr,
		"用法: %s [选项]\n"
		"  --freq <Hz>     设置频率 (1~%d)\n"
		"  --duty <0-100>  设置占空比 (0=静音)\n"
		"  --on            启动 PWM 输出\n"
		"  --off           关闭 PWM 输出\n"
		"  -h, --help      显示帮助\n"
		"示例: %s --freq 500 --duty 50\n",
		prog, BEEP_PWM_MAX_FREQ, prog);
}

static int do_ioctl(int fd, unsigned long cmd, void *arg)
{
	if (ioctl(fd, cmd, arg) < 0) {
		fprintf(stderr, "ioctl(0x%lx) 失败: %s\n", cmd, strerror(errno));
		return -1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int fd;
	int freq = -1;
	int duty = -1;
	int on_off = 0;		/* 0=无操作，1=on，2=off */
	int i;

	/* stdout 无缓冲：避免 stdout 全缓冲在非交互 shell 丢行（P3 教训） */
	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--freq")) {
			if (++i >= argc) {
				fprintf(stderr, "--freq 缺少参数\n");
				return 1;
			}
			freq = atoi(argv[i]);
		} else if (!strcmp(argv[i], "--duty")) {
			if (++i >= argc) {
				fprintf(stderr, "--duty 缺少参数\n");
				return 1;
			}
			duty = atoi(argv[i]);
		} else if (!strcmp(argv[i], "--on")) {
			on_off = 1;
		} else if (!strcmp(argv[i], "--off")) {
			on_off = 2;
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	fd = open(DEV_PATH, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s 失败: %s\n", DEV_PATH, strerror(errno));
		return 1;
	}

	/* 按序执行：SET_FREQ / SET_DUTY（自动应用）→ ON/OFF */
	if (freq >= 0) {
		if (freq < 1 || freq > BEEP_PWM_MAX_FREQ) {
			fprintf(stderr, "频率越界: %d (1~%d)\n",
				freq, BEEP_PWM_MAX_FREQ);
			close(fd);
			return 1;
		}
		if (do_ioctl(fd, BEEP_IOC_SET_FREQ, &freq))
			goto fail;
		printf("频率已设置: %d Hz\n", freq);
	}
	if (duty >= 0) {
		if (duty < 0 || duty > 100) {
			fprintf(stderr, "占空比越界: %d (0~100)\n", duty);
			close(fd);
			return 1;
		}
		if (do_ioctl(fd, BEEP_IOC_SET_DUTY, &duty))
			goto fail;
		printf("占空比已设置: %d%%%s\n", duty, duty == 0 ? "（静音）" : "");
	}
	if (on_off == 1) {
		if (do_ioctl(fd, BEEP_IOC_ON, NULL))
			goto fail;
		printf("PWM 已启动\n");
	} else if (on_off == 2) {
		if (do_ioctl(fd, BEEP_IOC_OFF, NULL))
			goto fail;
		printf("PWM 已关闭\n");
	}

	close(fd);
	return 0;
fail:
	close(fd);
	return 1;
}