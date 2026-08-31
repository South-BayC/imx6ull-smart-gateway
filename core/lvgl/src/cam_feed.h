/**
 * cam_feed.h — 摄像头画面喂数（V4L2 → LVGL canvas）
 *
 * 架构（线程安全，双缓冲消撕裂）:
 *   V4L2 线程: /dev/video0 抓帧(640×480 YUYV) → 翻转+缩放+RGB565 转换
 *              → 写后台 ping-pong 缓冲 → 原子发布（不触碰任何 LVGL API）
 *   UI 定时器: cam_feed_blit_if_ready() 拷贝最新完成帧 → lv_obj_invalidate() 局部重绘
 *
 * 尺寸: 抓帧 640×480（OV5640 标准档）→ 最近邻缩放至显示 630×340（铺满预览区）
 * 内存: canvas 缓冲 630×340×2 ≈ 428KB（ui_home.c 静态持有）
 *      + 后台双缓冲 2×428KB（cam_feed.c 静态持有）
 *
 * 设备不存在（/dev/video0）时优雅禁用，不影响其余功能。
 */
#ifndef CAM_FEED_H
#define CAM_FEED_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAM_SRC_W   640    /* V4L2 抓帧宽（OV5640 标准档位） */
#define CAM_SRC_H   480    /* V4L2 抓帧高 */
#define CAM_DISP_W  630    /* 显示宽（铺满预览画面区） */
#define CAM_DISP_H  340    /* 显示高（CAM_PIC_Y 之下到摄像头区底部） */

/**
 * 启动摄像头喂数线程并绑定画布
 * @param canvas 已创建的 lv_canvas 对象（尺寸须为 CAM_DISP_W×CAM_DISP_H）
 * @param buf    canvas 缓冲指针（调用方持有，本模块直写）
 * 内部探测 /dev/video0；失败则打印警告并保持禁用。
 */
void cam_feed_start(lv_obj_t *canvas, uint8_t *buf);

/**
 * 查询是否有新完成帧并拷入 canvas 缓冲（UI 定时器轮询）
 * @param dst canvas 缓冲（须 ≥ CAM_DISP_W×CAM_DISP_H×2 字节，RGB565）
 * @return 1=已拷贝新帧（调用方应 lv_obj_invalidate）；0=无新帧
 */
int cam_feed_blit_if_ready(uint8_t *dst);

/**
 * 采集流状态（STREAMON 成功且未断流）
 * @return 1=工作正常（应隐藏占位文字/网格）；0=无画面（显示占位）
 */
int cam_feed_stream_ok(void);

/**
 * 画面翻转（OV5640 排线安装方向 180° 时默认 V+H 全翻）
 * @param v 1=上下翻转；h 1=左右翻转。需在 cam_feed_start 前调用（也可启动后调用，
 *          重建映射表存在一帧瞬态，无害）
 */
void cam_feed_set_flip(int v, int h);

/**
 * 查询实测帧率（每秒结算一次；摄像头未工作时为 0）
 */
int cam_feed_get_fps(void);

/**
 * 拷贝当前帧到目标缓冲（抓拍用）
 * @param dst 目标缓冲（须 ≥ CAM_DISP_W×CAM_DISP_H×2 字节，RGB565）
 * @return 0 成功；-1 摄像头未工作
 */
int cam_feed_copy_frame(uint8_t *dst);

/* ---- 运动检测粗判引擎（入侵判别第一级，帧差法） ---- */

/** 开关运动检测（默认开；关闭后不再产生命中） */
void cam_feed_set_motion_en(int en);
int  cam_feed_get_motion_en(void);

/**
 * 读取并清零运动命中标志
 * @return 自上次读取以来的命中次数（降采样帧差连续 MOTION_DEBOUNCE 次超限）
 */
int cam_feed_get_motion_hits(void);

/**
 * 运动检测阈值（变化像素占比 %，1-50，默认 5）
 * 占比超阈值的采样点灰度差 > 24 计为变化点
 */
void cam_feed_set_motion_threshold(int pct);
int  cam_feed_get_motion_threshold(void);

/**
 * 帧差对比间隔（08-29 需求：可隔几帧对比）
 * @param n 每 N 个转换帧做一次帧差判定（1=每帧，上限 30）
 * N 越大：帧间差异越大（更易触发）且粗判开销越小
 */
void cam_feed_set_motion_interval(int n);
int  cam_feed_get_motion_interval(void);

/**
 * 像素灰度差阈值（08-29 需求：过滤小偏差）
 * @param d 灰度差超过 d 的采样点才计为变化（5~100，默认 24）
 */
void cam_feed_set_motion_diff(int d);
int  cam_feed_get_motion_diff(void);

#ifdef __cplusplus
}
#endif

#endif /* CAM_FEED_H */
