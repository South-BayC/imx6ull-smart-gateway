/**
 * @file    lvgl_port.c
 * @brief   LVGL 移植层实现 - fbdev 显示对接 + evdev 触摸对接
 * @author  显示驱动专家
 * @date    2026-08-22
 *
 * 使用 LVGL v9 官方 lv_linux_fbdev 驱动对接 /dev/fb0 (mxsfb)
 * 使用 LVGL v9 官方 lv_linux_evdev 驱动对接 /dev/input/event1 (GT911)
 * 硬件：1024x600 16bpp RGB565，分辨率由驱动 ioctl 自动读取，无需硬编码
 * 触摸：GT911 中断模式，/dev/input/event1，校准值由驱动 ioctl 自动读取 min/max
 * 配置依赖：core/lvgl/lv_conf.h (LV_USE_LINUX_FBDEV=1, LV_USE_LINUX_EVDEV=1, LV_COLOR_DEPTH=16, LV_DRAW_BUF_PARTIAL_MAX_ROWS=60)
 */

#include "lvgl_port.h"
#include "lvgl.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

/* ==================== 内部静态变量 ==================== */

static lv_display_t *s_disp = NULL;   /**< fbdev 显示设备句柄 */
static lv_indev_t *s_indev = NULL;    /**< evdev 触摸输入设备句柄 */

/* ==================== 内部函数声明 ==================== */

static uint32_t lvgl_port_tick_get_cb(void);
static int lvgl_port_evdev_init(void);

/* ==================== 公共函数实现 ==================== */

int lvgl_port_init(void)
{
    /* 1. LVGL 核心初始化 */
    lv_init();

    /* 2. 注册系统 tick 回调 (毫秒级)
     * 使用 clock_gettime(CLOCK_MONOTONIC) 获取单调时钟，避免系统时间调整影响 */
    lv_tick_set_cb(lvgl_port_tick_get_cb);

    /* 3. 创建 fbdev 显示设备
     * 官方驱动会自动通过 ioctl(FBIOGET_VSCREENINFO) 读取分辨率、色深等信息
     * 无需手动指定 1024x600，由 mxsfb 驱动上报 */
    s_disp = lv_linux_fbdev_create();
    if (s_disp == NULL) {
        fprintf(stderr, "[lvgl_port] ERROR: lv_linux_fbdev_create() 失败\n");
        return LVGL_PORT_ERR_FBDEV_CREATE;
    }

    /* 4. 绑定帧缓冲设备节点 /dev/fb0
     * 内部会 open("/dev/fb0")、mmap、设置 flush 回调等
     * 若打开失败 (权限/不存在)，返回非 0，此处仅记录日志不崩溃 */
    if (lv_linux_fbdev_set_file(s_disp, "/dev/fb0") != 0) {
        fprintf(stderr, "[lvgl_port] ERROR: lv_linux_fbdev_set_file(\"/dev/fb0\") 失败 - "
                        "请检查 /dev/fb0 是否存在、权限是否足够、mxsfb 驱动是否加载\n");
        lv_display_delete(s_disp);
        s_disp = NULL;
        return LVGL_PORT_ERR_FBDEV_OPEN;
    }

    /* 可选：设置部分刷新缓冲区行数 (需 lv_conf.h 中 LV_DRAW_BUF_PARTIAL_MAX_ROWS=60)
     * 这里不显式调用，由 lv_conf.h 宏控制 */

    printf("[lvgl_port] INFO: fbdev 显示对接成功 (/dev/fb0)\n");

    /* 5. 初始化 evdev 触摸输入设备 (GT911 /dev/input/event1)
     * 复用已创建的显示设备 s_disp，使用官方 API lv_evdev_create + lv_indev_set_display
     * 校准值由驱动 ioctl 自动读取 min/max，无需硬编码
     * 失败仅记录日志，不影响显示功能 */
    int evdev_ret = lvgl_port_evdev_init();
    if (evdev_ret != LVGL_PORT_OK) {
        fprintf(stderr, "[lvgl_port] WARN: evdev 触摸初始化失败 (错误码: %d)，仅显示功能可用\n", evdev_ret);
    }

    return LVGL_PORT_OK;
}

void lvgl_port_deinit(void)
{
    if (s_indev != NULL) {
        lv_indev_delete(s_indev);
        s_indev = NULL;
        printf("[lvgl_port] INFO: evdev 触摸输入设备已释放\n");
    }

    if (s_disp != NULL) {
        lv_display_delete(s_disp);
        s_disp = NULL;
        printf("[lvgl_port] INFO: fbdev 显示设备已释放\n");
    }
}

/* ==================== 内部函数实现 ==================== */

/**
 * @brief 系统 tick 回调 - 返回毫秒数
 * @return  当前单调时钟毫秒值
 * @note    供 lv_tick_set_cb() 使用，LVGL 内部用于动画、定时器等
 */
static uint32_t lvgl_port_tick_get_cb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/**
 * @brief 初始化 evdev 触摸输入设备 (GT911 /dev/input/event1)
 * @return  0 成功，负值失败
 * @note    使用官方 API lv_evdev_create + lv_indev_set_display 两行完成
 *          校准值由驱动 ioctl 自动读取 min/max，无需硬编码
 *          失败仅记录日志，不崩溃，支持重试 3 次
 */
static int lvgl_port_evdev_init(void)
{
    const char *evdev_path = "/dev/input/event1";
    const int max_retry = 3;
    int retry = 0;

    while (retry < max_retry) {
        /* 创建 evdev 输入设备：类型为指针(触摸屏)，路径为 /dev/input/event1
         * 内部会 open() 设备节点、通过 ioctl(EVIOCGABS) 自动读取 ABS_X/ABS_Y 的 min/max 校准值
         * 无需手写 read(input_event) 轮询，LVGL 内部线程自动处理 */
        s_indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, evdev_path);
        if (s_indev != NULL) {
            /* 绑定显示设备：将触摸坐标映射到显示坐标系
             * 复用已创建的 s_disp 变量，无需额外配置 */
            lv_indev_set_display(s_indev, s_disp);
            printf("[lvgl_port] INFO: evdev 触摸对接成功 (%s)\n", evdev_path);
            return LVGL_PORT_OK;
        }

        /* 打开失败：记录日志，等待 100ms 后重试 */
        fprintf(stderr, "[lvgl_port] WARN: lv_evdev_create(%s) 失败 (尝试 %d/%d): %s\n",
                evdev_path, retry + 1, max_retry, strerror(errno));
        retry++;
        if (retry < max_retry) {
            usleep(100000);  /* 100ms */
        }
    }

    /* 重试 3 次后仍失败：返回错误码，仅记录日志不崩溃 */
    fprintf(stderr, "[lvgl_port] ERROR: evdev 触摸初始化失败，已重试 %d 次 (%s)\n", max_retry, evdev_path);
    return LVGL_PORT_ERR_EVDEV_OPEN;
}