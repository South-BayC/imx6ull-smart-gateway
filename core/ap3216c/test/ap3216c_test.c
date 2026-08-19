// SPDX-License-Identifier: GPL-2.0
/* ap3216c_test：AP3216C 传感器读取测试（P4-B，手册 5.13.2）
 * 用法:
 *   ./ap3216c_test [次数]    默认无限循环，每秒打印 ir/als/ps
 * 验收:
 *   - 正常打印三列读数（als 随光照变化）
 *   - 手遮挡传感器（板上小窗口）时 ps 明显变大，移开后回落
 * 交叉编译: arm-linux-gnueabihf-gcc ap3216c_test.c -o ap3216c_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

#define DEV_PATH	"/dev/ap3216c0"
#define READ_LEN	6	/* ir+als+ps = 3×u16，与内核 uapi/ap3216c.h 一致 */

int main(int argc, char *argv[])
{
	int fd, count = -1, i = 0;
	uint16_t data[3];
	ssize_t n;

	if (argc > 1)
		count = atoi(argv[1]);

	fd = open(DEV_PATH, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s 失败: %s\n", DEV_PATH, strerror(errno));
		return 1;
	}
	setvbuf(stdout, NULL, _IONBF, 0);	/* 立即输出，避免 NFS/管道丢行 */

	for (;;) {
		n = read(fd, data, sizeof(data));
		if (n != (ssize_t)sizeof(data)) {
			fprintf(stderr, "read 失败(n=%zd): %s\n",
				n, strerror(errno));
			break;
		}
		printf("ir=%4u als=%4u ps=%4u\n", data[0], data[1], data[2]);

		if (count >= 0 && ++i >= count)
			break;
		sleep(1);
	}

	close(fd);
	return 0;
}
