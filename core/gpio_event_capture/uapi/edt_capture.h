/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * edt_capture.h —— gpio_event 多通道事件采集 UAPI（P3）
 * 设计定稿：手册 5.4.2（完整版），此处为项目实现
 *
 * 设备节点：/dev/edt_capture0（miscdevice，动态次设备号）
 * 事件记录固定 16 字节，时间戳单调时钟（CLOCK_MONOTONIC）
 */
#ifndef _UAPI_EDT_CAPTURE_H
#define _UAPI_EDT_CAPTURE_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* ABI 版本：用户态与内核态须一致（GET_CAPS 可查） */
#define EDT_CAPTURE_ABI_VERSION  2U
#define EDT_CAPTURE_MAX_CHANNELS 8U

/* 边沿类型位（edge_mask / ev.edge） */
#define EDT_CAPTURE_EDGE_RISING   (1U << 0)
#define EDT_CAPTURE_EDGE_FALLING  (1U << 1)
#define EDT_CAPTURE_EDGE_BOTH     (EDT_CAPTURE_EDGE_RISING | EDT_CAPTURE_EDGE_FALLING)

/* 能力位（caps.flags）：用户态据此选择可用特性 */
#define EDT_CAPTURE_CAP_NONBLOCK           (1U << 0)
#define EDT_CAPTURE_CAP_POLL               (1U << 1)
#define EDT_CAPTURE_CAP_EXCLUSIVE_OPEN     (1U << 2)
#define EDT_CAPTURE_CAP_MONOTONIC_TIMESTAMP (1U << 3)
#define EDT_CAPTURE_CAP_DROP_REASON_STATS  (1U << 4)
#define EDT_CAPTURE_CAP_INITIAL_SNAPSHOT   (1U << 5)
#define EDT_CAPTURE_CAP_RESET_STATS        (1U << 6)

/* 事件记录：固定 16 字节（通道 0-7，电平 0/1，边沿 0=unknown 1=rising 2=falling） */
struct edt_capture_event {
	__u64 timestamp_ns;   /* 单调时钟纳秒 */
	__u32 sequence;       /* 全局递增序列号（乱序证据） */
	__u16 channel;        /* 通道号 0-7 */
	__u8  level;          /* 当前电平 0/1 */
	__u8  edge;           /* 边沿类型 0=unknown 1=rising 2=falling */
};

/* 能力查询结果：48 字节 */
struct edt_capture_caps {
	__u32 abi_version;
	__u32 struct_size;
	__u32 event_size;
	__u32 max_channels;
	__u32 configured_channels;
	__u32 supported_edges;
	__u32 fifo_depth_events;
	__u32 flags;
	__u64 reserved[2];
};

/* 运行时配置：32 字节（SET_CONFIG 输入） */
struct edt_capture_config {
	__u32 abi_version;
	__u32 struct_size;
	__u32 channel_mask;
	__u32 edge_mask;
	__u32 fifo_depth_events;
	__u32 flags;
	__u32 reserved[2];
};

/* 统计：104 字节，可守恒统计（queued + dropped == irq 等可验证） */
struct edt_capture_stats {
	__u32 abi_version;
	__u32 struct_size;
	__u32 running;
	__u32 fifo_level_events;
	__u32 fifo_capacity_events;
	__u32 fifo_high_watermark;
	__u32 last_sequence;
	__u32 reserved0;
	__u64 irq_events;           /* 进入 IRQ 次数（running 且通道使能） */
	__u64 queued_events;        /* 成功入队（边沿 + 初始快照） */
	__u64 read_events;          /* 用户态读取 */
	__u64 dropped_events;       /* 丢弃总数（溢出 + 采样错误） */
	__u64 wake_signals;         /* 空→非空唤醒信号次数 */
	__u64 fifo_overflow_events; /* FIFO 满丢弃 */
	__u64 sample_error_events;  /* GPIO 电平读取失败 */
	__u64 snapshot_events;      /* START 初始快照 */
	__u64 cleared_events;       /* 显式清空 */
};

/* ioctl 命令集（魔数 'E'） */
#define EDT_IOC_MAGIC 'E'
#define EDT_IOC_GET_CAPS    _IOR(EDT_IOC_MAGIC, 0x00, struct edt_capture_caps)
#define EDT_IOC_START       _IO(EDT_IOC_MAGIC, 0x01)
#define EDT_IOC_STOP        _IO(EDT_IOC_MAGIC, 0x02)
#define EDT_IOC_SET_CONFIG  _IOW(EDT_IOC_MAGIC, 0x03, struct edt_capture_config)
#define EDT_IOC_CLEAR_FIFO  _IO(EDT_IOC_MAGIC, 0x04)
#define EDT_IOC_GET_STATS   _IOR(EDT_IOC_MAGIC, 0x05, struct edt_capture_stats)
#define EDT_IOC_RESET_STATS _IO(EDT_IOC_MAGIC, 0x06)

/* 编译期校验：用户态与内核态结构体大小必须一致（对齐由 u32/u64 布局保证） */
typedef char edt_capture_event_assert[
	(sizeof(struct edt_capture_event) == 16) ? 1 : -1];
typedef char edt_capture_caps_assert[
	(sizeof(struct edt_capture_caps) == 48) ? 1 : -1];
typedef char edt_capture_config_assert[
	(sizeof(struct edt_capture_config) == 32) ? 1 : -1];
typedef char edt_capture_stats_assert[
	(sizeof(struct edt_capture_stats) == 104) ? 1 : -1];

#endif /* _UAPI_EDT_CAPTURE_H */
