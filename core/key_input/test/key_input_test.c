// SPDX-License-Identifier: GPL-2.0
/* key_input_test：P6 input 子系统按键测试程序（手册 5.15.1）
 * 功能：
 *   1. 自动探测 /dev/input/eventX 中名字为 "imx6ull-gpio-keys" 的设备
 *      （也可手动指定节点：key_input_test /dev/input/event1）
 *   2. 读取 input_event 流，打印 EV_KEY 按下/释放事件
 * 用法：
 *   ./key_input_test            # 自动探测并监听
 *   ./key_input_test /dev/input/event1   # 指定节点
 * 验证：按下 KEY0 → 看到 code=227 值=1（按下）；松开 → 值=0（释放）
 * stdout 无缓冲（setvbuf _IONBF），避免板端非交互 shell 丢行（P3 教训）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/input.h>

/* 4.1.15 时代工具链头文件可能无 KEY_ARMED，兜底定义（与驱动一致） */
#ifndef KEY_ARMED
#define KEY_ARMED	227
#endif

#define DEV_NAME_MATCH	"imx6ull-gpio-keys"
#define DEV_INPUT_DIR	"/dev/input"

/* 自动探测匹配名字的 input 设备节点，成功返回 0 并写出路径 */
static int find_input_dev(char *out, size_t outlen)
{
	DIR *dir;
	struct dirent *ent;
	char path[64];
	char name[256];
	int fd;
	int ret = -1;

	dir = opendir(DEV_INPUT_DIR);
	if (!dir) {
		perror("opendir /dev/input");
		return -1;
	}

	while ((ent = readdir(dir)) != NULL) {
		if (strncmp(ent->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", DEV_INPUT_DIR, ent->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
		    strstr(name, DEV_NAME_MATCH)) {
			snprintf(out, outlen, "%s", path);
			ret = 0;
			close(fd);
			break;
		}
		close(fd);
	}
	closedir(dir);
	return ret;
}

/* 按键码 → 名称（常用几个，未知码显示数字） */
static const char *key_name(int code)
{
	switch (code) {
	case KEY_ARMED:	return "KEY_ARMED(227)";
	case KEY_A:	return "KEY_A";
	case KEY_POWER:	return "KEY_POWER";
	default:	return "UNKNOWN";
	}
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"用法: %s [节点]\n"
		"  无参数    自动探测 /dev/input/eventX 中名字含 "
		"\"%s\" 的设备\n"
		"  有参数    监听指定节点（如 /dev/input/event1）\n",
		prog, DEV_NAME_MATCH);
}

int main(int argc, char *argv[])
{
	char devpath[64];
	struct input_event ev;
	int fd;
	int count = 0;

	/* stdout 无缓冲：避免 stdout 全缓冲在非交互 shell 丢行（P3 教训） */
	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc > 2) {
		usage(argv[0]);
		return 1;
	}

	if (argc == 2) {
		snprintf(devpath, sizeof(devpath), "%s", argv[1]);
	} else {
		if (find_input_dev(devpath, sizeof(devpath)) != 0) {
			fprintf(stderr, "未找到 %s 设备，请确认驱动已加载 "
				"或手动指定节点\n", DEV_NAME_MATCH);
			return 1;
		}
	}

	fd = open(devpath, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	printf("监听 %s（Ctrl+C 退出）... 按下/松开 KEY0 观察事件\n", devpath);

	while (1) {
		ssize_t n = read(fd, &ev, sizeof(ev));
		if (n < (ssize_t)sizeof(ev)) {
			if (n < 0 && errno != EINTR)
				perror("read");
			continue;
		}
		if (ev.type != EV_KEY)
			continue;

		count++;
		printf("[%d] t=%ld.%06ld type=EV_KEY code=%d(%s) value=%d %s\n",
		       count,
		       (long)ev.time.tv_sec, (long)ev.time.tv_usec,
		       ev.code, key_name(ev.code), ev.value,
		       ev.value == 1 ? "按下" :
		       ev.value == 0 ? "释放" : "重复");
	}

	close(fd);
	return 0;
}