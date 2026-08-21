/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * event_reconstructor.c —— 边沿事件 → 固定采样率时间网格重建（P6-2）
 *
 * 算法（两遍扫描，O(n_events + n_samples)）：
 *   1. 快照遍：edge==0 的事件初始化各通道初始电平
 *   2. 网格遍：对每个网格点 t_i，推进事件指针应用 t <= t_i 的边沿事件，
 *      当前电平打包为样本字节（bit ch = level[ch]）
 *
 * 欠采样统计：同一网格单元（相邻网格点间）出现多个边沿事件时，
 * 取最后一个生效，collisions 计数（供文档"等效采样率限制"论证）。
 */
#include <stdlib.h>
#include <string.h>
#include "event_reconstructor.h"

int event_reconstruct(const struct edt_capture_event *evs, size_t n_events,
		      uint32_t sample_rate, size_t n_samples,
		      struct grid_result *out)
{
	uint8_t level[EDT_CAPTURE_MAX_CHANNELS] = { 0 };
	uint64_t dt_ns, t_last;
	uint64_t t0;
	size_t i, j = 0;
	uint8_t *grid;

	if (!evs || !out || n_events == 0 || sample_rate == 0 || n_samples == 0)
		return -1;

	memset(out, 0, sizeof(*out));

	/* 网格周期（纳秒），向上取整避免 R 大时 dt=0 */
	dt_ns = (1000000000ULL + sample_rate - 1) / sample_rate;

	/* t0 = 首个事件时间戳（快照），窗口覆盖 n_samples 个网格点。
	 * t_last 为最后一个网格点时刻，越窗统计以 t_last 为界，
	 * 避免 (t_last, window_end] 区间的事件成为统计盲区 */
	t0 = evs[0].timestamp_ns;
	t_last = t0 + dt_ns * (n_samples - 1);

	grid = malloc(n_samples);
	if (!grid)
		return -1;

	/* 遍 1：快照初始化电平 */
	for (i = 0; i < n_events; i++) {
		if (evs[i].edge == 0 && evs[i].channel < EDT_CAPTURE_MAX_CHANNELS)
			level[evs[i].channel] = evs[i].level;
	}

	/* 遍 2：网格填充 */
	for (i = 0; i < n_samples; i++) {
		uint64_t t = t0 + dt_ns * i;
		unsigned cell_edges[EDT_CAPTURE_MAX_CHANNELS] = { 0 };
		unsigned ch;
		uint8_t byte = 0;

		while (j < n_events && evs[j].timestamp_ns <= t) {
			if (evs[j].edge != 0 &&
			    evs[j].channel < EDT_CAPTURE_MAX_CHANNELS) {
				level[evs[j].channel] = evs[j].level;
				cell_edges[evs[j].channel]++;
			}
			j++;
		}
		/* 欠采样：仅同一通道在同一网格单元多次跳变才计碰撞
		 * （多通道同时跳变是正常信号，非欠采样） */
		for (ch = 0; ch < EDT_CAPTURE_MAX_CHANNELS; ch++)
			if (cell_edges[ch] > 1)
				out->collisions += cell_edges[ch] - 1;

		for (ch = 0; ch < EDT_CAPTURE_MAX_CHANNELS; ch++)
			if (level[ch])
				byte |= (uint8_t)(1u << ch);
		grid[i] = byte;
	}

	/* 统计 */
	out->samples = grid;
	out->count = n_samples;
	out->t0_ns = t0;
	out->sample_rate = sample_rate;
	out->events_used = j;
	for (; j < n_events; j++) {
		if (evs[j].edge != 0 && evs[j].timestamp_ns > t_last)
			out->events_beyond++;
	}
	return 0;
}

void grid_result_free(struct grid_result *g)
{
	if (g) {
		free(g->samples);
		g->samples = NULL;
		g->count = 0;
	}
}