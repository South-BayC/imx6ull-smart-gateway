/* SPDX-License-Identifier: GPL-2.0 */
/*
 * trace/edt_capture.h —— gpio_event 采集 tracepoint 定义（P3）
 * 模块内 tracepoint：本文件由 .c 以 CREATE_TRACE_POINTS 方式实例化，
 * 供 ftrace 观测完整事件路径：irq → enqueue → drop → read
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM edt_capture

#if !defined(_TRACE_EDT_CAPTURE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_EDT_CAPTURE_H

#include <linux/tracepoint.h>

/* IRQ 入口：通道 / 电平 / 时间戳 */
TRACE_EVENT(edt_capture_irq,
	TP_PROTO(int channel, int level, u64 timestamp_ns),
	TP_ARGS(channel, level, timestamp_ns),
	TP_STRUCT__entry(
		__field(int, channel)
		__field(int, level)
		__field(u64, timestamp_ns)
	),
	TP_fast_assign(
		__entry->channel = channel;
		__entry->level = level;
		__entry->timestamp_ns = timestamp_ns;
	),
	TP_printk("channel=%d level=%d ts=%llu",
		  __entry->channel, __entry->level, __entry->timestamp_ns)
);

/* 入队成功：序列号 / 通道 / 当前 FIFO 深度 */
TRACE_EVENT(edt_capture_enqueue,
	TP_PROTO(u32 sequence, int channel, unsigned int fifo_len),
	TP_ARGS(sequence, channel, fifo_len),
	TP_STRUCT__entry(
		__field(u32, sequence)
		__field(int, channel)
		__field(unsigned int, fifo_len)
	),
	TP_fast_assign(
		__entry->sequence = sequence;
		__entry->channel = channel;
		__entry->fifo_len = fifo_len;
	),
	TP_printk("seq=%u channel=%d fifo_len=%u",
		  __entry->sequence, __entry->channel, __entry->fifo_len)
);

/* 丢弃（FIFO 满）：序列号已消耗但未入队 */
TRACE_EVENT(edt_capture_drop,
	TP_PROTO(u32 sequence, int channel, unsigned int fifo_len),
	TP_ARGS(sequence, channel, fifo_len),
	TP_STRUCT__entry(
		__field(u32, sequence)
		__field(int, channel)
		__field(unsigned int, fifo_len)
	),
	TP_fast_assign(
		__entry->sequence = sequence;
		__entry->channel = channel;
		__entry->fifo_len = fifo_len;
	),
	TP_printk("seq=%u channel=%d fifo_len=%u",
		  __entry->sequence, __entry->channel, __entry->fifo_len)
);

/* 用户态读取：事件数 / 剩余 FIFO 深度 */
TRACE_EVENT(edt_capture_read,
	TP_PROTO(int events, unsigned int fifo_len),
	TP_ARGS(events, fifo_len),
	TP_STRUCT__entry(
		__field(int, events)
		__field(unsigned int, fifo_len)
	),
	TP_fast_assign(
		__entry->events = events;
		__entry->fifo_len = fifo_len;
	),
	TP_printk("events=%d fifo_len=%u",
		  __entry->events, __entry->fifo_len)
);

#endif /* _TRACE_EDT_CAPTURE_H */

/* 以下部分必须在保护块之外：生成器需要反复包含
 * TRACE_INCLUDE_PATH = trace：配合 Makefile 的 ccflags-y += -I$(src)，
 * 使 #include trace/edt_capture.h 在模块目录下命中 */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH trace
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE edt_capture
#include <trace/define_trace.h>