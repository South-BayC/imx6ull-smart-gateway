/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * capture_source.h —— SUMP 工具链数据源抽象（P6-2）
 *
 * 统一边沿事件流接口。事件结构复用内核 UAPI（edt_capture.h），
 * 保证与 /dev/edt_capture0 的 16 字节事件记录零转换对接。
 *
 * 后端：
 *   - edt_capture 后端：真实板端数据源（gpio_event 多通道采集驱动）
 *   - sim 后端：确定性合成信号源（自测/演示，无板端可跑）
 */
#ifndef CAPTURE_SOURCE_H
#define CAPTURE_SOURCE_H

#include <stddef.h>
#include "edt_capture.h"

struct capture_source;

struct capture_source_ops {
	/* 打开数据源。dev 为设备路径（edt 后端）或 NULL（sim 后端） */
	int (*open)(struct capture_source *cs, const char *dev);
	/* 开始采集（edt: CLEAR_FIFO+RESET_STATS+START；sim: 生成事件流） */
	int (*start)(struct capture_source *cs);
	/* 读取最多 max 个事件到 evs，超时 timeout_ms。
	 * 返回：>0 事件数；0 超时无数据；<0 错误（-errno） */
	int (*read)(struct capture_source *cs, struct edt_capture_event *evs,
		    size_t max, int timeout_ms);
	/* 停止采集 */
	int (*stop)(struct capture_source *cs);
	/* 关闭并释放 */
	void (*close)(struct capture_source *cs);
};

struct capture_source {
	const struct capture_source_ops *ops;
	void *priv;
};

/* edt_capture 后端（/dev/edt_capture0） */
struct capture_source *capture_source_edt_new(void);

/* 合成信号后端（ch0=1kHz ch1=500Hz ch2=250Hz ch3=125Hz 方波，1.2 秒窗口） */
struct capture_source *capture_source_sim_new(void);

#endif /* CAPTURE_SOURCE_H */