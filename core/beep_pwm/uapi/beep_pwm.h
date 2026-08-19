// SPDX-License-Identifier: GPL-2.0
/* beep_pwm UAPI（P4-B，手册 5.13.1）
 * ioctl 全集（4 个）：
 *   BEEP_IOC_SET_FREQ  _IOW  int    设置频率（Hz，1~20000）
 *   BEEP_IOC_SET_DUTY  _IOW  int    设置占空比（0-100，0=静音）
 *   BEEP_IOC_ON        _IO          按当前 freq/duty 启动
 *   BEEP_IOC_OFF       _IO          立即静音（enabled=false）
 */
#ifndef __BEEP_PWM_UAPI_H__
#define __BEEP_PWM_UAPI_H__

#include <linux/ioctl.h>
#include <linux/types.h>

#define BEEP_PWM_MAGIC		'B'
#define BEEP_PWM_MAX_FREQ	20000	/* 20kHz（人耳上限） */

#define BEEP_IOC_SET_FREQ	_IOW(BEEP_PWM_MAGIC, 1, int)
#define BEEP_IOC_SET_DUTY	_IOW(BEEP_PWM_MAGIC, 2, int)
#define BEEP_IOC_ON		_IO(BEEP_PWM_MAGIC, 3)
#define BEEP_IOC_OFF		_IO(BEEP_PWM_MAGIC, 4)

#endif /* __BEEP_PWM_UAPI_H__ */