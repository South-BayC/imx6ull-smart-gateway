/**
 * cam_feed.c — 摄像头画面喂数实现（V4L2 → canvas）
 *
 * 数据流:
 *   /dev/video0 (OV5640, mx6s-csi)
 *     → DQBUF 取 YUYV 帧 (640×480，33ms 节流)
 *     → YUYV→RGB565 转换（查表，无浮点）+ 翻转/缩放 + 帧差粗判
 *     → 写入后台 ping-pong 缓冲并原子发布（不直写 canvas，消撕裂）
 *   UI 33ms 定时器: cam_feed_blit_if_ready(canvas_buf) 拷贝最新完成帧 → 局部 invalidate
 *
 * 线程安全:
 *   - cam 线程只写 s_frame[s_write_idx]（发布后切到另一块，UI 正在拷贝的缓冲不会被写）
 *   - s_cur_idx / s_frame_ready 为 int 原子性读写；UI 只在 blit 时拷贝 current 帧内容
 *   - 残余竞态：UI 长时间滞后（>2 帧周期）时可能拷到正在被写的缓冲 → 单帧撕裂自愈
 *   - 本线程绝不调用任何 lv_* API
 *
 * 诊断: cam 线程每 5s 打印 sensor 帧率(DQBUF) / conv 帧率(转换) / 单帧转换耗时，
 *       用于区分传感器侧瓶颈（sensor 低）与 CPU 侧瓶颈（conv_ms 高）。
 */
#include "cam_feed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define CAM_DEV     "/dev/video0"
#define CAM_I2C_DEV "/dev/i2c-1"
#define CAM_I2C_ADDR 0x3c             /* OV5640 7 位地址（=dts reg 值；i2ctransfer @0x3c 已实测通信成功） */
#ifndef I2C_SLAVE_FORCE
#define I2C_SLAVE_FORCE 0x0706        /* 地址被内核 ov5640 驱动占用，必须 FORCE */
#endif
#define CAM_FPS_MS  33         /* 30fps（与 LV_DEF_REFR_PERIOD 33ms 匹配） */
#define V4L2_BUF_CNT 4
#define CAM_FPS_TARGET 30      /* S_PARM 档位（驱动仅支持 15/30）：30fps 寄存器组时钟不同，
                                * 兼作捕获滚动诊断（若 30fps 下滚动消失/变化=时钟余量问题） */

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
        tbl_g_u[i] = (int16_t)((-400 * d) >> 10);   /* G -= 0.391(U-128) */
        tbl_g_v[i] = (int16_t)((-833 * e) >> 10);   /* G -= 0.813(V-128) */
        tbl_b_u[i] = (int16_t)((2066 * d) >> 10);
    }
}

static uint8_t *s_buf = NULL;          /* canvas 缓冲指针（cam_feed_start 传入，blit 目标） */
static volatile int s_frame_ready = 0; /* 帧就绪标志 */
static int s_started = 0;
static volatile int s_fps_val = 0;     /* 转换帧率（每秒结算，UI 显示用） */
static int s_fps_cnt = 0;              /* 当前秒转换帧计数 */

/* ---- 双缓冲 ping-pong（消撕裂）----
 * cam 线程转换写入 s_frame[s_write_idx]，完成后原子发布 s_cur_idx 并翻转写目标；
 * UI 只拷贝 s_frame[s_cur_idx]（发布后另一块才是写目标，互不碰撞） */
static uint8_t s_frame[2][CAM_DISP_W * CAM_DISP_H * 2];
static volatile int s_cur_idx = 0;     /* 最新完成帧索引（UI blit 源） */
static int s_write_idx = 0;            /* cam 线程当前写目标索引 */

/* ---- 非缓存源帧中转（帧率关键）----
 * V4L2 mmap 缓冲为 writecombine/非缓存映射（DMA 一致性），逐字节散读每次
 * 都付 DRAM 延迟（板测：缓存源 29ms/帧 vs 非缓存源 114ms/帧）。
 * 先整帧 memcpy（突发读）进缓存区，再查表转换，读全部命中 cache。 */
static uint8_t s_src_copy[CAM_SRC_W * CAM_SRC_H * 2];

/* ---- 翻转（OV5640 排线安装方向 180°，默认 V+H 全翻）---- */
static volatile int s_vflip = 1;
static volatile int s_hflip = 1;

/* ---- 诊断计数（区分传感器侧 / CPU 侧瓶颈）---- */
static volatile int s_cap_val = 0;     /* DQBUF 帧率（传感器实际交付） */
static int s_cap_cnt = 0;
static volatile int s_conv_ms_x10 = 0; /* 最近一秒平均单帧转换耗时 ×0.1ms */
static volatile int s_stream_ok = 0;   /* STREAMON 成功且未断流（占位符显隐用） */

/* ---- 运动检测粗判引擎（入侵判别第一级，帧差法） ----
 * 算法: RGB565 帧 → 降采样 80×50 灰度 → 与前帧逐像素差值
 *       → 差值 > 差值阈值 的像素占比 > RATIO_TH → 计一次命中（防抖 MOTION_DEBOUNCE 次）
 * 可配置: 对比间隔（每 N 个转换帧判定一次，08-29 需求）+ 灰度差阈值（过滤小偏差）
 * 内存: 降采样缓冲 2×8KB 静态；开销 ~0.5ms/次 */
#define MD_W           80
#define MD_H           50
#define MD_DIFF_TH     24     /* 像素灰度差阈值默认值（0-255，运行时可调） */
#define MD_RATIO_PCT   5      /* 变化像素占比阈值 %（全画面 5%） */
#define MD_DEBOUNCE    2      /* 连续 N 次超限 → 命中 */

static uint8_t md_prev[MD_W * MD_H];   /* 前帧降采样灰度 */
static uint8_t md_cur[MD_W * MD_H];    /* 当前帧降采样灰度 */
static int md_prev_valid = 0;
static int md_cnt = 0;
static volatile int s_motion_en = 1;   /* 运动检测使能（默认开） */
static volatile int s_motion_hits = 0; /* 命中计数（消费端读取清零） */
static volatile int s_motion_ratio = MD_RATIO_PCT; /* 变化占比阈值 %（可调） */
static volatile int s_motion_diff = MD_DIFF_TH;    /* 像素灰度差阈值（可调，过滤小偏差） */
static volatile int s_motion_interval = 1;         /* 帧差对比间隔 N（每 N 个转换帧判定一次） */
static int s_motion_tick = 0;                      /* 间隔计数器 */

/* ---- 公开接口（cam_feed.h 声明；非 static 供 UI/桥接层调用） ---- */
static void scale_map_init(void);   /* 前置声明：cam_feed_set_flip 需重建映射表 */

void cam_feed_set_motion_en(int en) { s_motion_en = en ? 1 : 0; }
int  cam_feed_get_motion_en(void)  { return s_motion_en; }
int  cam_feed_get_motion_hits(void)
{
    int v = s_motion_hits;
    s_motion_hits = 0;
    return v;
}
void cam_feed_set_motion_threshold(int pct)
{
    if (pct < 1) pct = 1;
    if (pct > 50) pct = 50;
    s_motion_ratio = pct;
}
int cam_feed_get_motion_threshold(void) { return s_motion_ratio; }

/* 帧差对比间隔：每 N 个转换帧判定一次（1=每帧；N 越大帧间差异越大、开销越小） */
void cam_feed_set_motion_interval(int n)
{
    if (n < 1) n = 1;
    if (n > 30) n = 30;
    s_motion_interval = n;
    s_motion_tick = 0;
}
int cam_feed_get_motion_interval(void) { return s_motion_interval; }

/* 像素灰度差阈值：过滤小幅亮度偏差（5~100，默认 24） */
void cam_feed_set_motion_diff(int d)
{
    if (d < 5) d = 5;
    if (d > 100) d = 100;
    s_motion_diff = d;
}
int cam_feed_get_motion_diff(void) { return s_motion_diff; }

/* 画面翻转（V=上下翻，H=左右翻；默认 V+H=180°，匹配排线安装方向）。
 * 需在 cam_feed_start 前调用（映射表在采集线程启动时构建）；启动后调用
 * 会重建映射表，存在一帧混用旧映射的瞬态，无害。 */
void cam_feed_set_flip(int v, int h)
{
    s_vflip = v ? 1 : 0;
    s_hflip = h ? 1 : 0;
    scale_map_init();
}

/* RGB565 → 降采样灰度（G 通道近似亮度） */
static void md_downsample(const uint8_t *frame, uint8_t *out)
{
    for (int y = 0; y < MD_H; y++) {
        const uint16_t *sline = (const uint16_t *)frame + (y * CAM_DISP_H / MD_H) * CAM_DISP_W;
        for (int x = 0; x < MD_W; x++) {
            uint16_t px = sline[x * CAM_DISP_W / MD_W];
            out[y * MD_W + x] = (uint8_t)((px >> 5) & 0x3F);   /* G 高 6 位 */
        }
    }
}

/* 帧差判定（每帧转换后调用，输入为刚完成的转换帧） */
static void motion_detect(const uint8_t *frame)
{
    if (!s_motion_en) { md_prev_valid = 0; md_cnt = 0; return; }

    md_downsample(frame, md_cur);

    if (md_prev_valid) {
        int diff_cnt = 0;
        int total = MD_W * MD_H;
        for (int i = 0; i < total; i++) {
            int d = md_cur[i] - md_prev[i];
            if (d < 0) d = -d;
            if (d > s_motion_diff) diff_cnt++;
        }
        if (diff_cnt * 100 > total * s_motion_ratio)
            md_cnt++;
        else
            md_cnt = 0;

        if (md_cnt >= MD_DEBOUNCE) {
            md_cnt = 0;
            s_motion_hits++;   /* 命中一次（消费端清零；持续运动周期性再命中） */
        }
    }

    memcpy(md_prev, md_cur, MD_W * MD_H);
    md_prev_valid = 1;
}

static inline int clamp8(int x)
{
    return x < 0 ? 0 : (x > 255 ? 255 : x);
}

static inline uint16_t pack_rgb565(int r, int g, int b)
{
    return (uint16_t)((clamp8(r) >> 3) << 11 | (clamp8(g) >> 2) << 5 | clamp8(b) >> 3);
}

/* ---- 缩放映射表（初始化一次，转换时无除法；含 V/H 翻转） ----
 * 列映射：显示 x → 源 x（最近邻，偶对齐 YUYV）；行映射：显示 y → 源 y */
static int map_col[CAM_DISP_W];
static int map_row[CAM_DISP_H];

static void scale_map_init(void)
{
    for (int x = 0; x < CAM_DISP_W; x++) {
        int sx = x * CAM_SRC_W / CAM_DISP_W;
        sx &= ~1;   /* YUYV 偶对齐（取共享 UV 对） */
        map_col[x] = s_hflip ? (CAM_SRC_W - 2 - sx) : sx;
    }
    for (int y = 0; y < CAM_DISP_H; y++) {
        int sy = y * CAM_SRC_H / CAM_DISP_H;
        map_row[y] = s_vflip ? (CAM_SRC_H - 1 - sy) : sy;
    }
}

/* ---- 单源行 YUYV→RGB565（NEON 8 像素/迭代；x86 模拟器回退标量） ----
 * 公式与标量路径一致：R=(Y-16)*1192>>10 + (V-128)*1634>>10 等（>>10 定点）。
 * NEON 用 vmull_n_s16 宽乘避免 s16 溢出，vqmovun 饱和窄化等价 clamp8。 */
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>

static void convert_row(const uint8_t *src, uint16_t *dst)
{
    const int16x8_t c16  = vdupq_n_s16(16);
    const int16x8_t c128 = vdupq_n_s16(128);
    const uint16x8_t mr  = vdupq_n_u16(0xF8);   /* R 高 5 位掩码（<<8 即 <<11） */
    const uint16x8_t mg  = vdupq_n_u16(0xFC);   /* G 高 6 位掩码（<<3 即 <<5） */

    /* vld4_u8 一次取 32 字节 = 16 像素（YUYV 每像素 2 字节）：
     * lane0=偶数像素 Y×8, lane1=U×8, lane2=奇数像素 Y×8, lane3=V×8 */
    for (int i = 0; i < CAM_SRC_W; i += 16, src += 32, dst += 16) {
        uint8x8x4_t q = vld4_u8(src);   /* lane0=Y偶 lane1=U lane2=Y奇 lane3=V */
        int16x8_t y0 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(q.val[0])), c16);
        int16x8_t y1 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(q.val[2])), c16);
        int16x8_t ud = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(q.val[1])), c128);
        int16x8_t vd = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(q.val[3])), c128);

        /* 宽乘避免 s16 溢出；(c*k)>>10 与标量 >>10 语义一致 */
        int16x8_t yc0 = vcombine_s16(vshrn_n_s32(vmull_n_s16(vget_low_s16(y0), 1192), 10),
                                     vshrn_n_s32(vmull_n_s16(vget_high_s16(y0), 1192), 10));
        int16x8_t yc1 = vcombine_s16(vshrn_n_s32(vmull_n_s16(vget_low_s16(y1), 1192), 10),
                                     vshrn_n_s32(vmull_n_s16(vget_high_s16(y1), 1192), 10));
        int16x8_t rv  = vcombine_s16(vshrn_n_s32(vmull_n_s16(vget_low_s16(vd), 1634), 10),
                                     vshrn_n_s32(vmull_n_s16(vget_high_s16(vd), 1634), 10));
        int16x8_t gu  = vcombine_s16(vshrn_n_s32(vmull_n_s16(vget_low_s16(ud), -400), 10),
                                     vshrn_n_s32(vmull_n_s16(vget_high_s16(ud), -400), 10));
        int16x8_t gv  = vcombine_s16(vshrn_n_s32(vmull_n_s16(vget_low_s16(vd), -833), 10),
                                     vshrn_n_s32(vmull_n_s16(vget_high_s16(vd), -833), 10));
        int16x8_t bu  = vcombine_s16(vshrn_n_s32(vmull_n_s16(vget_low_s16(ud), 2066), 10),
                                     vshrn_n_s32(vmull_n_s16(vget_high_s16(ud), 2066), 10));

        int16x8_t gadd = vaddq_s16(gu, gv);
        int16x8_t r0 = vaddq_s16(yc0, rv);
        int16x8_t g0 = vaddq_s16(yc0, gadd);
        int16x8_t b0 = vaddq_s16(yc0, bu);
        int16x8_t r1 = vaddq_s16(yc1, rv);
        int16x8_t g1 = vaddq_s16(yc1, gadd);
        int16x8_t b1 = vaddq_s16(yc1, bu);

        /* 饱和窄化 u8（等价 clamp8）→ RGB565 组包 → 偶/奇像素交错存储 */
        uint16x8_t r016 = vmovl_u8(vqmovun_s16(r0));
        uint16x8_t g016 = vmovl_u8(vqmovun_s16(g0));
        uint16x8_t b016 = vmovl_u8(vqmovun_s16(b0));
        uint16x8_t r116 = vmovl_u8(vqmovun_s16(r1));
        uint16x8_t g116 = vmovl_u8(vqmovun_s16(g1));
        uint16x8_t b116 = vmovl_u8(vqmovun_s16(b1));

        uint16x8_t px0 = vorrq_u16(vshlq_n_u16(vandq_u16(r016, mr), 8),
                                   vorrq_u16(vshlq_n_u16(vandq_u16(g016, mg), 3),
                                             vshrq_n_u16(b016, 3)));
        uint16x8_t px1 = vorrq_u16(vshlq_n_u16(vandq_u16(r116, mr), 8),
                                   vorrq_u16(vshlq_n_u16(vandq_u16(g116, mg), 3),
                                             vshrq_n_u16(b116, 3)));
        uint16x8x2_t out = {{ px0, px1 }};
        vst2q_u16(dst, out);
    }
}

#else  /* 模拟器（x86 host）标量回退 */

static void convert_row(const uint8_t *src, uint16_t *dst)
{
    for (int sx = 0; sx < CAM_SRC_W; sx += 2) {
        int y0 = src[sx], u = src[sx + 1], y1 = src[sx + 2], v = src[sx + 3];
        dst[sx]     = pack_rgb565(tbl_r_y[y0] + tbl_r_v[v],
                                  tbl_g_y[y0] + tbl_g_u[u] + tbl_g_v[v],
                                  tbl_b_y[y0] + tbl_b_u[u]);
        dst[sx + 1] = pack_rgb565(tbl_r_y[y1] + tbl_r_v[v],
                                  tbl_g_y[y1] + tbl_g_u[u] + tbl_g_v[v],
                                  tbl_b_y[y1] + tbl_b_u[u]);
    }
}

#endif

/* 源帧（YUYV）→ 翻转缩放写入 dst：两阶段——
 * ① convert_row 把需要的源行转成 RGB565 行缓存（NEON）
 * ② 按 map_row/map_col 取样到画布（行去重 + 列去重/翻转） */
static uint16_t s_row_tmp[CAM_SRC_W];

static void yuyv_scale_to_canvas(const uint8_t *src, uint8_t *dst_buf)
{
    uint16_t *dst = (uint16_t *)dst_buf;
    int prev_row = -1;
    for (int dy = 0; dy < CAM_DISP_H; dy++) {
        int srow = map_row[dy];
        if (srow != prev_row) {
            convert_row(src + srow * CAM_SRC_W * 2, s_row_tmp);
            prev_row = srow;
        }
        uint16_t *dline = dst + dy * CAM_DISP_W;
        for (int dx = 0; dx < CAM_DISP_W; dx++)
            dline[dx] = s_row_tmp[map_col[dx]];
    }
}

/* ---- 务实补丁：STREAMON 后重写 PLL 系统分频 ----
 * 根因链：驱动 STREAMON 的 ov5640_init_mode 用硬编码基线数组（0x3035=0x21 慢时钟）
 * 覆盖 S_PARM 设置的 30fps（30fps blob 本身 + 时序均正确，已验证）。
 * 板测证实：流媒体运行中热写 0x3035=0x11 后寄存器稳定保持、帧率 30fps（30s 无回写）。
 * 故在 STREAMON 后经 /dev/i2c-1 重写一次。init 流程根治后可移除本补丁。 */
static void ov5640_force_30fps_clk(void)
{
    unsigned char buf[3] = { 0x30, 0x35, 0x11 };   /* 寄存器 0x3035 = 0x11（sysdiv 2→1） */
    int f = open(CAM_I2C_DEV, O_RDWR);
    if (f < 0) {
        printf("[CAM] workaround open %s failed: %s\n", CAM_I2C_DEV, strerror(errno));
        return;
    }
    if (ioctl(f, I2C_SLAVE_FORCE, CAM_I2C_ADDR) < 0) {
        printf("[CAM] workaround ioctl failed: %s\n", strerror(errno));
        close(f);
        return;
    }
    int w = write(f, buf, sizeof(buf));
    if (w == 3)
        printf("[CAM] PLL 0x3035=0x11 applied (30fps workaround)\n");
    else
        printf("[CAM] workaround write failed (%d): %s\n", w, strerror(errno));
    close(f);
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

    /* 注：曾试过 SCHED_FIFO 提权，引发 RT throttling 且 UI 渲染被抢占更卡，已撤（08-29） */

    /* 启动基准：纯转换耗时（无 V4L2、无渲染干扰），区分低频/抢占与算法本身 */
    {
        uint8_t *bench_src = malloc(CAM_SRC_W * CAM_SRC_H * 2);
        if (bench_src) {
            struct timespec b0, b1;
            memset(bench_src, 0x80, CAM_SRC_W * CAM_SRC_H * 2);
            clock_gettime(CLOCK_MONOTONIC, &b0);
            for (int i = 0; i < 5; i++)
                yuyv_scale_to_canvas(bench_src, s_frame[0]);
            clock_gettime(CLOCK_MONOTONIC, &b1);
            printf("[CAM] conv_bench=%ld ms/frame (pure compute)\n",
                   ((b1.tv_sec - b0.tv_sec) * 1000L +
                    (b1.tv_nsec - b0.tv_nsec) / 1000000L) / 5);
            free(bench_src);
        }
    }

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

    /* 1.5 帧率/档位：显式 VGA 档 + 目标帧率，不依赖 G_PARM（其失败会跳过整段）。
     * 枚举实证：ov5640_mode_VGA_640_480 = 0（ov5640.c:51）。
     * 设置后回读——驱动是否真正接受档位一目了然 */
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = CAM_FPS_TARGET;
    parm.parm.capture.capturemode = 0;
    if (xioctl(fd, VIDIOC_S_PARM, &parm) < 0)
        printf("[CAM] S_PARM(%dfps,VGA) failed\n", CAM_FPS_TARGET);

    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_G_PARM, &parm) == 0)
        printf("[CAM] applied: %d/%d fps, capturemode=%d\n",
               parm.parm.capture.timeperframe.denominator,
               parm.parm.capture.timeperframe.numerator,
               parm.parm.capture.capturemode);

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
    printf("[CAM] streaming %dx%d YUYV from %s (bytesperline=%u sizeimage=%u)\n",
           fmt.fmt.pix.width, fmt.fmt.pix.height, CAM_DEV,
           (unsigned)fmt.fmt.pix.bytesperline, (unsigned)fmt.fmt.pix.sizeimage);
    s_stream_ok = 1;
    ov5640_force_30fps_clk();   /* STREAMON 后重写 PLL 分频（驱动 init 会覆盖，见函数注释） */

    /* 4. 采集循环：DQBUF → 转换（33ms 节流）→ 发布 → QBUF（30fps） */
    long long last_ms = 0, fps_last_ms = 0, first_frame_ms = 0;
    long conv_us_acc = 0;
    int  conv_n = 0, diag_printed = 0;
    while (1) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EINTR) continue;
            printf("[CAM] DQBUF failed\n");
            s_stream_ok = 0;
            break;
        }
        s_cap_cnt++;   /* 传感器实际交付帧计数 */

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long long now_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (first_frame_ms == 0) first_frame_ms = now_ms;

        if (now_ms - last_ms >= CAM_FPS_MS) {
            struct timespec t0, t1;
            long conv_us;
            last_ms = now_ms;

            clock_gettime(CLOCK_MONOTONIC, &t0);
            memcpy(s_src_copy, bufs[b.index].start, CAM_SRC_W * CAM_SRC_H * 2);
            yuyv_scale_to_canvas(s_src_copy, s_frame[s_write_idx]);
            /* 帧差粗判：按可配置间隔判定（间隔 N=隔 N 帧对比，帧间差异随 N 增大） */
            if (++s_motion_tick >= s_motion_interval) {
                s_motion_tick = 0;
                motion_detect(s_frame[s_write_idx]);   /* 命中计数供桥接层消费 */
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            conv_us = (t1.tv_sec - t0.tv_sec) * 1000000L
                    + (t1.tv_nsec - t0.tv_nsec) / 1000;
            conv_us_acc += conv_us;
            conv_n++;

            s_cur_idx = s_write_idx;   /* 原子发布完成帧，写目标切到另一块 */
            s_write_idx ^= 1;
            s_frame_ready = 1;
            s_fps_cnt++;
        }

        /* 每秒结算帧率/耗时（UI 读数用）；诊断行仅开播 5s 后打印一次，不再刷屏 */
        if (now_ms - fps_last_ms >= 1000) {
            s_fps_val = s_fps_cnt;
            s_cap_val = s_cap_cnt;
            s_conv_ms_x10 = conv_n ? (int)(conv_us_acc / conv_n / 100) : 0;
            s_fps_cnt = 0; s_cap_cnt = 0; conv_us_acc = 0; conv_n = 0;
            fps_last_ms = now_ms;
            if (!diag_printed && first_frame_ms && now_ms - first_frame_ms >= 5000) {
                diag_printed = 1;
                printf("[CAM] sensor=%d fps, conv=%d fps, conv_ms=%d.%d\n",
                       s_cap_val, s_fps_val, s_conv_ms_x10 / 10, s_conv_ms_x10 % 10);
            }
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
    (void)canvas;   /* 保留参数位；当前仅 blit 目标缓冲被使用 */
    if (s_started) return;
    s_started = 1;
    s_buf = buf;

    pthread_t tid;
    if (pthread_create(&tid, NULL, cam_thread, NULL) != 0) {
        printf("[CAM] thread create failed\n");
        return;
    }
    pthread_detach(tid);
    printf("[CAM] camera feed started r6-neon (canvas %dx%d, flip v=%d h=%d)\n",
           CAM_DISP_W, CAM_DISP_H, s_vflip, s_hflip);
}

/* UI 定时器调用：有新完成帧则拷入 canvas 缓冲并返回 1（随后调用方 invalidate） */
int cam_feed_blit_if_ready(uint8_t *dst)
{
    if (!s_frame_ready || !dst) return 0;
    s_frame_ready = 0;
    memcpy(dst, s_frame[s_cur_idx], CAM_DISP_W * CAM_DISP_H * 2);
    return 1;
}

int cam_feed_get_fps(void)
{
    return s_fps_val;
}

/* 采集流是否在工作（STREAMON 成功且未断流）：占位符/网格显隐用 */
int cam_feed_stream_ok(void)
{
    return s_stream_ok;
}

int cam_feed_copy_frame(uint8_t *dst)
{
    if (!s_buf) return -1;   /* 摄像头未启动，无有效帧 */
    memcpy(dst, s_frame[s_cur_idx], CAM_DISP_W * CAM_DISP_H * 2);
    return 0;
}
