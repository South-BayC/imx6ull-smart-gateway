/**
 * @file    main.c
 * @brief   智能安防网关 - LVGL 应用程序入口
 * @author  显示驱动专家
 * @date    2026-08-22
 *
 * 交叉编译目标：arm-linux-gnueabihf
 * 运行环境：i.MX6ULL + mxsfb fbdev (/dev/fb0) + GT911 触摸 (/dev/input/event1)
 * 分辨率：1024x600 16bpp RGB565
 *
 * 启动流程：
 *  1. lvgl_port_init() - 初始化 LVGL 核心 + fbdev 显示 + evdev 触摸
 *  2. sm_init()        - 初始化状态机 (4防区、事件缓冲、回调表)
 *  3. ui_home_init()   - 构建主界面 (顶栏/摄像头预览/防区卡片/事件轴/底栏)
 *  4. lv_timer_handler() 循环 - LVGL 任务处理器 (建议 5ms 周期)
 *
 * 退出：Ctrl+C 触发信号处理，调用 lvgl_port_deinit() 清理资源
 */

#include "lvgl_port.h"
#include "ui/ui_home.h"
#include "ui/state_machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

/* ==================== 全局状态 ==================== */

static volatile bool g_running = true;  /**< 主循环运行标志 */

/* ==================== 信号处理 ==================== */

/**
 * @brief 信号处理函数 - 优雅退出
 * @param sig  信号编号 (SIGINT/SIGTERM)
 */
static void signal_handler(int sig)
{
    (void)sig;
    printf("\n[lvgl_gateway] 接收到退出信号，正在清理资源...\n");
    g_running = false;
}

/* ==================== 状态机 → UI 桥接 ==================== */

/**
 * @brief 状态机事件回调 - 将状态机产生的事件桥接到主界面事件时间轴
 *
 * 通过 sm_register_event_cb() 注册，状态机每次 add_event() 时触发。
 * 将通用的 sm_event_t 转换为 UI 层使用的 ui_event_t 后显示到时间轴。
 *
 * @param event     状态机新增事件 (非空)
 * @param user_data 用户数据 (此处未使用)
 */
static void on_sm_event(const sm_event_t *event, void *user_data)
{
    (void)user_data;
    if (!event) return;

    ui_event_t ev;
    ev.type     = event->type;
    ev.level    = event->level;
    ev.title    = event->title;
    ev.location = event->location;
    ev.hour     = event->hour;
    ev.minute   = event->minute;
    ui_home_add_event(&ev);
}

/**
 * @brief 回放状态机初始化阶段已产生的事件到时间轴
 *
 * sm_init() 在 ui_home_init() 之前执行，期间 add_event() 的事件
 * 尚未注册回调，不会被实时桥接。此处统一回放，保证时间轴完整。
 * 按"从旧到新"追加 (最新事件由 ui_home_add_event 置顶)。
 */
static void replay_sm_events(void)
{
    for (int i = sm_get_event_count() - 1; i >= 0; i--) {
        const sm_event_t *e = sm_get_event(i);
        if (e) on_sm_event(e, NULL);
    }
}

/* ==================== 主函数 ==================== */

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("========================================\n");
    printf("  智能安防网关 - LVGL Gateway v1.0\n");
    printf("  i.MX6ULL + mxsfb fbdev + GT911\n");
    printf("  1024x600 16bpp RGB565\n");
    printf("========================================\n");

    /* 注册信号处理：支持 Ctrl+C 优雅退出 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 1. LVGL 移植层初始化：核心 + fbdev显示 + evdev触摸 */
    int ret = lvgl_port_init();
    if (ret != LVGL_PORT_OK) {
        fprintf(stderr, "[main] ERROR: lvgl_port_init() 失败 (错误码: %d)\n", ret);
        fprintf(stderr, "         请检查：\n");
        fprintf(stderr, "         1. /dev/fb0 是否存在 (mxsfb 驱动加载)\n");
        fprintf(stderr, "         2. /dev/input/event1 是否存在 (GT911 驱动加载)\n");
        fprintf(stderr, "         3. 当前用户是否有 video/input 组权限\n");
        return EXIT_FAILURE;
    }
    printf("[main] INFO: LVGL 移植层初始化成功\n");

    /* 2. 状态机初始化：4防区、事件缓冲、回调表 */
    sm_init();
    printf("[main] INFO: 状态机初始化完成\n");

    /* 3. UI 主界面初始化：构建完整交互界面 */
    lv_obj_t *scr = lv_screen_active();
    ui_home_init(scr);
    printf("[main] INFO: 主界面构建完成\n");

    /* 注册状态机事件回调，实时同步事件时间轴 (布防/撤防/告警/启动等) */
    sm_register_event_cb(on_sm_event, NULL);

    /* 回放初始化阶段 (sm_init) 已产生的事件 (系统启动/自检/联网等) */
    replay_sm_events();

    printf("[main] INFO: 进入主循环 (Ctrl+C 退出)\n");
    printf("[main] INFO: 目标帧率 >= 25 FPS (周期 <= 40ms)\n");

    /* 4. LVGL 主循环：周期调用 lv_timer_handler() */
    struct timespec ts_start, ts_end;
    const long target_period_ns = 5 * 1000 * 1000;  /* 5ms = 200 FPS 理论上限，实际受限于 fbdev 刷新 */

    while (g_running) {
        clock_gettime(CLOCK_MONOTONIC, &ts_start);

        /* LVGL 任务处理器：处理动画、定时器、输入事件、刷新等 */
        lv_timer_handler();

        clock_gettime(CLOCK_MONOTONIC, &ts_end);

        /* 计算耗时，动态休眠以维持 ~5ms 周期 */
        long elapsed_ns = (ts_end.tv_sec - ts_start.tv_sec) * 1000000000L +
                          (ts_end.tv_nsec - ts_start.tv_nsec);
        long sleep_ns = target_period_ns - elapsed_ns;

        if (sleep_ns > 0) {
            struct timespec ts_sleep = { .tv_sec = 0, .tv_nsec = sleep_ns };
            nanosleep(&ts_sleep, NULL);
        }
        /* 如果 elapsed_ns >= target_period_ns，说明处理超时，立即进入下一轮 */
    }

    /* 5. 清理资源 */
    printf("[main] INFO: 正在退出，释放资源...\n");
    lvgl_port_deinit();
    printf("[main] INFO: 退出完成\n");

    return EXIT_SUCCESS;
}