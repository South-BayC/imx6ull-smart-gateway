/**
 * cam_feed.c — 摄像头画面喂数实现（V4L2 → canvas）
 *
 * 数据流:
 *   /dev/video0 (OV5640, mxc_v4l2)
 *     → DQBUF 取 YUYV 帧 (320×240)
 *     → YUYV→RGB565 转换（整数近似，无浮点）
 *     → 直接写入 canvas 缓冲（ui_home.c 持有的静态数组）
 *     → g_frame_ready = 1
 *   UI 100ms 定时器: ready → lv_canvas_invalidate(canvas)（局部重绘）
 *
 * 线程安全:
 *   - canvas 缓冲单写（本线程）单读（LVGL 渲染），单帧撕裂无感知，不加锁
 *   - g_frame_ready int 原子性足够
 *   - 本线程绝不调用任何 lv_* API
 */
#include "cam_feed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define CAM_DEV     "/dev/video0"
#define CAM_FPS_MS  33         /* 30fps（与 LV_DEF_REFR_PERIOD 33ms 匹配） */
#define V4L2_BUF_CNT 4

/* ---- YUV→RGB 查表（初始化一次，转换时纯查表+加法，比逐像素乘除快 2-3 倍） ----
 * 公式: R = 1.164(Y-16) + 1.596(V-128)
 *       G = 1.164(Y-16) - 0.391(U-128) - 0.813(V-128)
 *       B = 1.164(Y-16) + 2.018(U-128)
 * 拆分为 Y 贡献表 + U/V 贡献表（YUYV 两像素共享 U/V，UV 只算一次） */
static int16_t tbl_r_y[256], tbl_g_y[256], tbl_b_y[256];
static int16_t tbl_r_v[256], tbl_g_u[256], tbl_g_v[256], tbl_b_u[256];

static void yuv_table_init(void)
{
    for (int i = 0; i < 256; i++) {
        int c = i - 16;
        tbl_r_y[i] = (int16_t)((1192 * c) >> 10);
        tbl_g_y[i] = (int16_t)((1192 * c) >> 10);
        tbl_b_y[i] = (int16_t)((1192 * c) >> 10);
    }
    for (int i = 0; i < 256; i++) {
        int d = i - 128, e = i - 128;
        tbl_r_v[i] = (int16_t)((1634 * e) >> 10);
        tbl_g_u[i] = (int16_t)((-833 * d) >> 10);
        tbl_g_v[i] = (int16_t)((-400 * e) >> 10);
        tbl_b_u[i] = (int16_t)((2066 * d) >> 10);
    }
}

static lv_obj_t *s_canvas = NULL;      /* 绑定的 canvas 对象（仅存指针，线程不触碰） */
static uint8_t *s_buf = NULL;          /* canvas 缓冲指针（cam_feed_start 传入） */
static volatile int s_frame_ready = 0; /* 帧就绪标志 */
static int s_started = 0;
static volatile int s_fps_val = 0;     /* 实测帧率（每秒结算） */
static int s_fps_cnt = 0;              /* 当前秒帧计数 */

static inline uint16_t pack_rgb565(int r, int g, int b)
{
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* ---- 缩放映射表（初始化一次，转换时无除法） ----
 * 列映射：显示 x → 源 x（最近邻，偶对齐 YUYV）；行映射：显示 y → 源 y */
static int map_col[CAM_DISP_W];
static int map_row[CAM_DISP_H];

static void scale_map_init(void)
{
    for (int x = 0; x < CAM_DISP_W; x++) {
        int sx = x * CAM_SRC_W / CAM_DISP_W;
        map_col[x] = sx & ~1;              /* YUYV 偶对齐（取共享 UV 对） */
    }
    for (int y = 0; y < CAM_DISP_H; y++) {
        map_row[y] = y * CAM_SRC_H / CAM_DISP_H;
    }
}

/* 源帧（YUYV）缩放 + 色彩转换 → 写 canvas（查表 + 映射表，无逐像素除法） */
static void yuyv_scale_to_canvas(const uint8_t *src)
{
    uint16_t *dst = (uint16_t *)s_buf;
    for (int dy = 0; dy < CAM_DISP_H; dy++) {
        const uint8_t *sline = src + map_row[dy] * CAM_SRC_W * 2;
        uint16_t *dline = dst + dy * CAM_DISP_W;
        for (int dx = 0; dx < CAM_DISP_W; dx++) {
            int sx = map_col[dx];
            int y  = sline[sx];
            int u  = sline[sx + 1];
            int v  = sline[sx + 3];        /* 与 sx 像素共享的 UV（sx 偶对齐） */
            int uv_r = tbl_r_v[v];
            int uv_g = tbl_g_u[u] + tbl_g_v[v];
            int uv_b = tbl_b_u[u];
            dline[dx] = pack_rgb565(tbl_r_y[y] + uv_r,
                                    tbl_g_y[y] + uv_g,
                                    tbl_b_y[y] + uv_b);
        }
    }
}

/* ---- V4L2 ioctl 包装 ---- */
static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

/* ================================================================
 * V4L2 采集线程
 * ================================================================ */
static void *cam_thread(void *arg)
{
    (void)arg;
    yuv_table_init();   /* 色彩查表初始化（一次） */
    scale_map_init();   /* 缩放映射表初始化（一次） */

    int fd = open(CAM_DEV, O_RDWR);
    if (fd < 0) {
        printf("[CAM] %s open failed: %s (camera preview disabled)\n",
               CAM_DEV, strerror(errno));
        return NULL;
    }

    /* 1. 格式：640×480 YUYV（OV5640 标准档，缩放到显示尺寸） */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = CAM_SRC_W;
    fmt.fmt.pix.height = CAM_SRC_H;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        printf("[CAM] S_FMT failed\n");
        close(fd);
        return NULL;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        printf("[CAM] YUYV not supported by driver\n");
        close(fd);
        return NULL;
    }

    /* 2. 申请缓冲（mmap） */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = V4L2_BUF_CNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        printf("[CAM] REQBUFS failed\n");
        close(fd);
        return NULL;
    }

    struct {
        void *start;
        size_t length;
    } bufs[V4L2_BUF_CNT];
    memset(bufs, 0, sizeof(bufs));

    for (int i = 0; i < V4L2_BUF_CNT; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
            printf("[CAM] QUERYBUF failed\n");
            close(fd);
            return NULL;
        }
        bufs[i].length = b.length;
        bufs[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, b.m.offset);
        if (bufs[i].start == MAP_FAILED) {
            printf("[CAM] mmap failed\n");
            close(fd);
            return NULL;
        }
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
            printf("[CAM] QBUF failed\n");
            close(fd);
            return NULL;
        }
    }

    /* 3. 开始采集 */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        printf("[CAM] STREAMON failed\n");
        close(fd);
        return NULL;
    }
    printf("[CAM] streaming %dx%d YUYV from %s\n",
           fmt.fmt.pix.width, fmt.fmt.pix.height, CAM_DEV);

    /* 4. 采集循环：DQBUF → 转换 → QBUF（30fps） */
    long long last_ms = 0, fps_last_ms = 0;
    while (1) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EINTR) continue;
            printf("[CAM] DQBUF failed\n");
            break;
        }

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long long now_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

        if (now_ms - last_ms >= CAM_FPS_MS) {
            last_ms = now_ms;
            yuyv_scale_to_canvas(bufs[b.index].start);
            s_frame_ready = 1;
            s_fps_cnt++;
        }

        /* 实测帧率：每秒结算一次（UI 读取显示） */
        if (now_ms - fps_last_ms >= 1000) {
            s_fps_val = s_fps_cnt;
            s_fps_cnt = 0;
            fps_last_ms = now_ms;
        }

        /* 归还缓冲：DQBUF 返回的 b（含 index）直接传回 QBUF（v4l2 标准用法） */
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
            printf("[CAM] re-QBUF failed\n");
            break;
        }
    }

    close(fd);
    return NULL;
}

/* ================================================================
 * 公共接口
 * ================================================================ */
void cam_feed_start(lv_obj_t *canvas, uint8_t *buf)
{
    if (s_started) return;
    s_started = 1;
    s_canvas = canvas;
    s_buf = buf;

    pthread_t tid;
    if (pthread_create(&tid, NULL, cam_thread, NULL) != 0) {
        printf("[CAM] thread create failed\n");
        return;
    }
    pthread_detach(tid);
    printf("[CAM] camera feed started (canvas %dx%d)\n", CAM_DISP_W, CAM_DISP_H);
}

int cam_feed_frame_ready(void)
{
    if (s_frame_ready) {
        s_frame_ready = 0;
        return 1;
    }
    return 0;
}

int cam_feed_get_fps(void)
{
    return s_fps_val;
}

int cam_feed_copy_frame(uint8_t *dst)
{
    if (!s_buf) return -1;   /* 摄像头未工作，无帧可拷 */
    memcpy(dst, s_buf, CAM_DISP_W * CAM_DISP_H * 2);
    return 0;
}
