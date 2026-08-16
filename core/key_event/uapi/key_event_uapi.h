#ifndef _UAPI_KEY_EVENT_H
#define _UAPI_KEY_EVENT_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define KEY_EVENT_MAGIC 'K'
#define KEY_EVENT_CLEAR _IO(KEY_EVENT_MAGIC, 0x01)

/* 按键事件：时间戳(ns) + 键号 + 按下/松开 */
struct key_event {
    __u64 timestamp_ns;
    __u8  key;
    __u8  pressed;   /* 1=按下 0=松开 */
} __attribute__((packed));   /* 用户态/内核态共用，用 GCC 属性保证紧凑布局 */

#endif /* _UAPI_KEY_EVENT_H */