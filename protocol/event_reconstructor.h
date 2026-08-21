/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * event_reconstructor.h —— 边沿事件 → 固定采样率时间网格重建（P6-2）
 *
 * 核心思想（手册 5.15.2 重要声明）：
 *   本工具链是"事件驱动等效采样"：以边沿事件（时间戳 + 电平）重建
 *   固定采样率 R 的时间网格，等效采样率受事件产生率限制。
 *   适用于低速协议调试（I2C ≤ 400kHz / UART），非硬件全速采样。
 *
 * 网格语义：
 *   - t0 = 第一个事件时间戳（START 快照）
 *   - 网格点 t_i = t0 + i * (1e9 / R) 纳秒
 *   - 样本字节 bit0..bit7 对应通道 0..7，值为该时刻电平
 *   - 初始电平来自快照事件（edge==0）；边沿事件按时间推进电平
 */
#ifndef EVENT_RECONSTRUCTOR_H
#define EVENT_RECONSTRUCTOR_H

#include <stddef.h>
#include <stdint.h>
#include "edt_capture.h"

struct grid_result {
	uint8_t *samples;       /* 重建网格：n_samples 字节，调用方负责释放 */
	size_t count;           /* 实际网格样本数 */
	uint64_t t0_ns;         /* 网格起点（首个事件时间戳） */
	uint32_t sample_rate;   /* 实际使用的采样率 Hz */
	uint64_t events_used;   /* 用于重建的边沿事件数 */
	uint64_t events_beyond; /* 超出网格窗口被忽略的事件数 */
	uint64_t collisions;    /* 同一网格单元内多个边沿事件（欠采样信号） */
};

/*
 * 重建网格。evs 必须按 timestamp_ns 升序（驱动/模拟源保证）。
 * 返回 0 成功（调用 grid_result_free 释放）；<0 错误。
 */
int event_reconstruct(const struct edt_capture_event *evs, size_t n_events,
		      uint32_t sample_rate, size_t n_samples,
		      struct grid_result *out);

void grid_result_free(struct grid_result *g);

#endif /* EVENT_RECONSTRUCTOR_H */