/**
 * @file    lv_conf.h
 * @brief   LVGL v9.2.2 配置文件 - 针对 i.MX6ULL + mxsfb fbdev 优化
 * @author  显示驱动专家
 * @date    2026-08-22
 *
 * 核心配置：
 *  - LV_USE_LINUX_FBDEV = 1        启用 Linux fbdev 显示驱动
 *  - LV_COLOR_DEPTH = 16           16bpp RGB565 (匹配 /dev/fb0 mxsfb)
 *  - LV_DRAW_BUF_PARTIAL_MAX_ROWS = 60  部分刷新缓冲区行数 (节省内存)
 *
 * 其余保持默认，仅覆盖本项目必需项
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
 * 核心配置
 *====================*/

/* LVGL 版本检查 */
#define LV_VERSION_MAJOR 9
#define LV_VERSION_MINOR 2
#define LV_VERSION_PATCH 2

/* 内存分配器：使用标准 malloc/free */
#define LV_MEM_CUSTOM      1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC   malloc
#define LV_MEM_CUSTOM_FREE    free
#define LV_MEM_CUSTOM_REALLOC realloc

/* 绘图缓冲区：部分刷新模式，每次刷新 60 行 (约 1024*60*2 = 120KB) */
#define LV_DRAW_BUF_PARTIAL_MAX_ROWS 60

/* 颜色深度：16 位 (RGB565) - 必须与 /dev/fb0 硬件一致 */
#define LV_COLOR_DEPTH 16

/* 坐标类型：int16_t 足够 1024x600 */
#define LV_COORD_T int16_t

/*====================
 * 显示驱动配置
 *====================*/

/* 启用 Linux fbdev 驱动 (官方驱动，自动 ioctl 读取分辨率/色深) */
#define LV_USE_LINUX_FBDEV 1

/* fbdev 设备路径 (可在运行时通过 lv_linux_fbdev_set_file 修改) */
#define LV_LINUX_FBDEV_BSD 0
#define LV_LINUX_FBDEV_RENDER_MODE LV_DISPLAY_RENDER_MODE_PARTIAL

/*====================
 * 输入设备配置
 *====================*/

/* 启用 Linux evdev 输入驱动 (触摸屏/按键) */
#define LV_USE_LINUX_EVDEV 1

/*====================
 * 字体配置
 *====================*/

/* 启用内置字体 */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1

/* 中文字体：由项目字体文件提供 (lv_font_SHSC_16/20.c) */
#define LV_FONT_CUSTOM_DECLARE \
    LV_FONT_DECLARE(lv_font_SHSC_16); \
    LV_FONT_DECLARE(lv_font_SHSC_20);

/*====================
 * 日志配置
 *====================*/

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_INFO
#define LV_LOG_PRINTF 1
#define LV_LOG_TRACE_MEM 0
#define LV_LOG_TRACE_TIMER 0
#define LV_LOG_TRACE_INDEV 0
#define LV_LOG_TRACE_DISP_REFR 0
#define LV_LOG_TRACE_EVENT 0
#define LV_LOG_TRACE_OBJ_CREATE 0
#define LV_LOG_TRACE_LAYOUT 0
#define LV_LOG_TRACE_ANIM 0

/*====================
 * 断言与调试
 *====================*/

#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0

/*====================
 * 性能优化
 *====================*/

/* 启用汇编优化 (ARM Cortex-A7 支持) */
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

/*====================
 * 示例与演示 (关闭以节省空间)
 *====================*/

#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_STRESS 0
#define LV_USE_DEMO_MUSIC 0

#endif /* LV_CONF_H */