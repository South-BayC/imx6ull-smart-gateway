// SPDX-License-Identifier: GPL-2.0
/* gt9147_test：触摸事件流验收工具（P7-1，配套教程 gt9147.ko）
 * 用法: ./gt9147_test [/dev/input/eventX]   缺省自动扫描 /proc/bus/input/devices
 * 验收标准: 手指划屏 → 打印 ABS_MT_POSITION_X/Y 坐标流 + BTN_TOUCH 按下/释放
 * 编译: arm-linux-gnueabihf-gcc -o gt9147_test gt9147_test.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/input.h>

#define NAME_MAX_LEN 64

/* 解析 /proc/bus/input/devices：按 Name 含触摸关键字精确定位 event 节点
 * （教程驱动 input 名称为 client->name，兼容 "gt9147"/"goodix,gt9147"） */
static int auto_find_device(char *out, size_t outlen)
{
	FILE *fp;
	char line[512];
	char cur_name[NAME_MAX_LEN] = "";
	char ev_node[32] = "";

	fp = fopen("/proc/bus/input/devices", "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		if (!strncmp(line, "N: Name=", 8)) {
			char *s = strchr(line, '"');
			char *e = s ? strrchr(s, '"') : NULL;
			size_t n;

			cur_name[0] = '\0';
			if (s && e && e > s) {
				n = (size_t)(e - s - 1);
				if (n >= sizeof(cur_name))
					n = sizeof(cur_name) - 1;
				memcpy(cur_name, s + 1, n);
				cur_name[n] = '\0';
			}
		} else if (!strncmp(line, "H: Handlers=", 12)) {
			char *tok;

			ev_node[0] = '\0';
			tok = strtok(line + 12, " \t\n");
			while (tok) {
				if (!strncmp(tok, "event", 5)) {
					strncpy(ev_node, tok,
						sizeof(ev_node) - 1);
					break;
				}
				tok = strtok(NULL, " \t\n");
			}
			if (ev_node[0] &&
			    (strstr(cur_name, "gt9147") ||
			     strstr(cur_name, "goodix-ts") ||
			     strstr(cur_name, "gt9xx"))) {
				snprintf(out, outlen, "/dev/input/%s", ev_node);
				fclose(fp);
				return 0;
			}
		}
	}
	fclose(fp);
	return -1;
}

static const char *abs_code_name(int code)
{
	switch (code) {
	case ABS_MT_POSITION_X:	return "ABS_MT_POSITION_X";
	case ABS_MT_POSITION_Y:	return "ABS_MT_POSITION_Y";
	case ABS_MT_TOUCH_MAJOR: return "ABS_MT_TOUCH_MAJOR";
	case ABS_MT_TRACKING_ID: return "ABS_MT_TRACKING_ID";
	case ABS_MT_SLOT:	return "ABS_MT_SLOT";
	default:		return "ABS_?";
	}
}

int main(int argc, char **argv)
{
	struct input_event ev;
	char dev[128];
	ssize_t n;
	int fd;

	if (argc > 1) {
		strncpy(dev, argv[1], sizeof(dev) - 1);
		dev[sizeof(dev) - 1] = '\0';
	} else if (auto_find_device(dev, sizeof(dev)) == 0) {
		printf("auto-detected: %s\n", dev);
	} else {
		fprintf(stderr, "no touch input device found\n");
		return 1;
	}

	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	printf("reading %s ... (Ctrl-C to quit)\n", dev);

	while ((n = read(fd, &ev, sizeof(ev))) == sizeof(ev)) {
		switch (ev.type) {
		case EV_ABS:
			printf("  ABS  %-22s %-6d value=%d\n",
			       abs_code_name(ev.code), ev.code, ev.value);
			break;
		case EV_KEY:
			printf("  KEY  code=%-4d %s\n",
			       ev.code, ev.value ? "PRESS" : "RELEASE");
			break;
		case EV_SYN:
			printf("  --- SYNC ---\n");
			break;
		default:
			printf("  type=%u code=%u value=%d\n",
			       ev.type, ev.code, ev.value);
			break;
		}
	}

	if (n < 0)
		perror("read");
	close(fd);
	return 0;
}
