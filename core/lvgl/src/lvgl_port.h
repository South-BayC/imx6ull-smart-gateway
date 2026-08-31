/**
 * @file    lvgl_port.h
 * @brief   LVGL 移植层接口声明 - fbdev 显示对接
 * @author  显示驱动专家
 * @date    2026-08-22
 *
 * 该模块负责 LVGL v9 与 Linux fbdev (/dev/fb0) 的对接初始化。
 * 使用官方 lv_linux_fbdev 驱动，无需手写 mmap/flush 回调。
 * 硬件：mxsfb 驱动的 /dev/fb0，1024x600 16bpp RGB565
 */

#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LVGL 移植层初始化
 *
 * 执行流程：
 *  1. lv_init() - LVGL 核心初始化
 *  2. lv_tick_set_cb() - 注册系统 tick 回调 (毫秒)
 *  3. lv_linux_fbdev_create() - 创建 fbdev 显示设备
 *  4. lv_linux_fbdev_set_file(disp, "/dev/fb0") - 绑定帧缓冲设备节点
 *
 * @return  0 成功，负值失败 (错误码见下方)
 * @note    失败时仅打印日志，不崩溃，调用者可决定是否重试或降级
 */
int lvgl_port_init(void);

/**
 * @brief LVGL 移植层去初始化
 *
 * 释放 fbdev 显示设备，清理 LVGL 资源
 */
void lvgl_port_deinit(void);

/* 错误码定义 */
#define LVGL_PORT_OK              (0)   /**< 成功 */
#define LVGL_PORT_ERR_INIT        (-1)  /**< lv_init 失败 */
#define LVGL_PORT_ERR_TICK        (-2)  /**< tick 回调注册失败 */
#define LVGL_PORT_ERR_FBDEV_CREATE (-3) /**< fbdev 创建失败 */
#define LVGL_PORT_ERR_FBDEV_OPEN  (-4)  /**< /dev/fb0 打开失败 */
#define LVGL_PORT_ERR_EVDEV_OPEN  (-5)  /**< /dev/input/event1 打开失败 */

#ifdef __cplusplus
}
#endif

#endif /* LVGL_PORT_H */