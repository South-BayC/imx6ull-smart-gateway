/* ================================================================
 * ui_home.c — 1024x600 中文安防网关主页（完全重写）
 *
 * 布局: flex-col 1024x600
 *   status-bar 36px
 *   main-area  flex:1(512px) flex-row (left 630px | right 360px)
 *   bottom-bar 52px
 *
 * 内存预算:
 *   LV_MEM_SIZE = 256KB
 *   LVGL 对象 < 200
 *   定时器 = 2（时钟 + 呼吸/扫描线）
 *   事件列表 = 10 条（静态数组预分配）
 * ================================================================ */
#include "ui_home.h"
#include "ui_widgets.h"
#include "ui_events.h"
#include "../state_machine.h"
#include "../dev_bridge.h"
#include "../cam_feed.h"
#include "../storage_mgr.h"
#include "../mqtt_hub.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <sys/statvfs.h>

/* ---- 中文字库声明 ---- */
LV_FONT_DECLARE(lv_font_SHSC_16);
LV_FONT_DECLARE(lv_font_SHSC_20);

/* ================================================================
 * 色板 — 所有颜色以 HEX 宏定义，函数内调用 lv_color_hex()
 * ================================================================ */
#define CLR_BG         0x0B1220
#define CLR_CARD       0x1B2A4A
#define CLR_BORDER     0x253656
#define CLR_BORDER_LT  0x2E4268
#define CLR_TEXT_HI     0xE6EDF7
#define CLR_TEXT_LO     0x8A9AB5
#define CLR_TEXT_DIM    0x5A6A85
#define CLR_CYAN        0x00D1FF
#define CLR_GREEN       0x20C997
#define CLR_AMBER       0xFFB020
#define CLR_RED         0xFF4D4F
#define CLR_BLACK       0x000000

/* ================================================================
 * 布局尺寸常量
 * ================================================================ */
#define SCR_W        1024
#define SCR_H        600
#define SCR_VIS_H    572   /* 面板底部物理遮挡约28px，可视区高度（底栏完整显示在安全区内） */
#define STATUS_H     36
#define BOTTOM_H     52
/* 主区高度 = 可视572 - 状态栏36 - 底栏52 = 484（info-bar 已删除，空间补偿给主区） */
#define MAIN_H       (SCR_VIS_H - STATUS_H - BOTTOM_H)
#define LEFT_W       630
#define RIGHT_W      360
#define CAM_H        396   /* 分区卡片改一行后，摄像头预览区大幅加大 */
/* 摄像头头部两行：第一行徽标+通道信息，第二行切换 pill，各 28px */
#define CAM_HDR1_H   28
#define CAM_HDR2_H   28
#define CAM_PIC_Y    (CAM_HDR1_H + CAM_HDR2_H) /* 预览画面区起始 y */
#define GAP_SM       6
#define GAP_MD       8

/* ================================================================
 * 分区数据
 * ================================================================ */
#define ZONE_COUNT 4

typedef struct {
    const char *name;
    const char *id;
    const char *sensor;
    const char *ip;
    const char *fw;
    const char *rssi;
} zone_info_t;

static const zone_info_t zone_info[ZONE_COUNT] = {
    {"前门", "Z-001", "PIR+门",   "192.168.1.101", "v2.3.1", "-42dBm"},
    {"后门", "Z-002", "PIR+门",   "192.168.1.102", "v2.3.1", "-55dBm"},
    {"窗户", "Z-003", "门+窗",     "192.168.1.103", "v2.3.0", "-38dBm"},
    {"仓库", "Z-004", "PIR+监",   "192.168.1.104", "v2.3.1", "-61dBm"},
};

/* ================================================================
 * 摄像头数据
 * ================================================================ */
#define CAM_COUNT 4

typedef struct {
    const char *name;
    const char *ch;
    const char *label;
    uint32_t    bg_clr;   /* 预览背景色（HTML CAMS.bg 的深色变体） */
} cam_info_t;

static const cam_info_t cam_info[CAM_COUNT] = {
    {"前门", "CH01", "前门画面", 0x003C50},
    {"后门", "CH02", "后门画面", 0x1E3250},
    {"仓库", "CH03", "仓库画面", 0x32283C},
    {"窗户", "CH04", "窗户画面", 0x283C28},
};

static const int cam_to_zone[CAM_COUNT] = {0, 1, 3, 2};

/* ================================================================
 * 事件数据（25条，静态预分配；标题全部用 SHSC_16 字库内可显示的字改写，
 * 以免字库缺字渲染成乱码。与 HTML 事件模板语义一致）
 * ================================================================ */
#define EVT_MAX 25

typedef struct {
    const char *time;
    const char *title;
    const char *loc;
    uint32_t    dot_clr;
    const char *level;
} evt_t;

static evt_t evt_tbl[EVT_MAX] = {
    {"14:37", "已入设置界面",         "系统",       CLR_CYAN,   ""},        /* 设置面板已打开 */
    {"14:37", "当前正常",             "系统",       CLR_CYAN,   ""},        /* 当前无告警 */
    {"14:37", "全部分区已撤防",       "系统",       CLR_AMBER,  ""},
    {"14:37", "全部分区已布防",       "系统",       CLR_GREEN,  ""},
    {"14:36", "手动抓拍已保存",       "CH01",       CLR_GREEN,  ""},
    {"14:10", "系统 在位",           "主控",       CLR_CYAN,   ""},        /* 系统启动完成 */
    {"14:09", "检测正常",            "前门",       CLR_GREEN,  "low"},     /* 传感器自检通过 */
    {"14:07", "检测正常",            "后门",       CLR_GREEN,  "low"},     /* 传感器自检通过 */
    {"14:06", "网络在线",            "主控",       CLR_CYAN,   ""},        /* 网络连接已建立 */
    {"14:04", "云端在线",            "主控",       CLR_CYAN,   ""},        /* 云端同步就绪 */
    {"14:03", "画面 正常",           "前门 CH01",  CLR_GREEN,  "low"},     /* 摄像头信号正常 */
    {"14:01", "边缘 模式",           "主控",       CLR_CYAN,   ""},        /* 边缘粗筛引擎加载 */
    {"14:00", "仓库 中Z",            "仓库",       CLR_AMBER,  "medium"},  /* 仓库信号弱 < -60dBm */
    {"13:58", "事件 12 条/分",       "云端",       CLR_CYAN,   ""},        /* 事件上报 12 条/分 */
    {"13:57", "系统 v2.3.1",          "主控",       CLR_CYAN,   ""},        /* 固件校验通过 v2.3.1 */
    {"13:55", "NTP 时间",             "主控",       CLR_CYAN,   ""},        /* NTP 时间同步 */
    {"13:54", "PIR 检测",             "窗户",       CLR_GREEN,  "low"},     /* PIR 灵敏度校准 */
    {"13:52", "云端 系统",            "云端",       CLR_CYAN,   ""},        /* 日志上传完成 */
    {"13:51", "CPU 62C",              "主控",       CLR_AMBER,  "medium"},  /* CPU 温度 62°C */
    {"13:49", "MQTT 在线",            "云端",       CLR_CYAN,   ""},        /* MQTT 心跳正常 */
    {"13:48", "门状态 已关",          "前门",       CLR_GREEN,  "low"},     /* 门磁状态: 已关闭 */
    {"13:46", "存量 14.2GB",          "主控",       CLR_CYAN,   "low"},     /* 存储剩余 14.2GB */
    {"13:45", "画面 H.264",           "CH01",       CLR_CYAN,   ""},        /* 视频编码 H.264 720p */
    {"13:43", "门状态 已关",          "后门",       CLR_GREEN,  "low"},     /* 门磁状态: 已关闭 */
    {"13:42", "边缘模式",             "主控",       CLR_CYAN,   ""},        /* 边缘模式运行中 */
};
static int evt_count = EVT_MAX;

/* ================================================================
 * 全局状态
 * ================================================================ */
static int g_current_cam = 0;
static zone_state_t g_zone_states[ZONE_COUNT];

/* ================================================================
 * 需要动态更新的 LVGL 对象指针
 * ================================================================ */
static lv_obj_t *g_clock_lbl       = NULL;
static lv_obj_t *g_arm_lbl         = NULL;
static lv_obj_t *g_arm_dot         = NULL;
static lv_obj_t *g_cam_pills[CAM_COUNT];
static lv_obj_t *g_cam_title_lbl   = NULL;
static lv_obj_t *g_cam_text_lbl    = NULL;
static lv_obj_t *g_zone_cards[ZONE_COUNT];
static lv_obj_t *g_zone_status[ZONE_COUNT];
static lv_obj_t *g_zone_dots[ZONE_COUNT];
static lv_obj_t *g_event_list      = NULL;
static lv_obj_t *g_event_count_lbl = NULL;
static lv_obj_t *g_scanline        = NULL;
static lv_obj_t *g_cam_grid_lines[32];   /* 网格线句柄（有画面隐藏，无画面显示） */
static int       g_cam_grid_cnt    = 0;
static int       g_cam_overlay_hidden = 0;   /* 1=占位文字/网格已隐藏（有画面） */
static lv_obj_t *g_toast           = NULL;
static lv_obj_t *g_overlay_detail  = NULL;
static lv_obj_t *g_overlay_settings= NULL;
static lv_obj_t *g_detail_body     = NULL;
static lv_obj_t *g_detail_title_lbl= NULL;  /* 详情弹窗标题标签（动态更新分区名） */
static lv_obj_t *g_detail_state_lbl= NULL;  /* 详情弹窗"当前状态"值标签 */
static lv_obj_t *g_detail_arm_btn  = NULL;  /* 详情弹窗状态设置按钮（布防⇄撤防） */
static int g_detail_zone_idx       = -1;    /* 详情弹窗当前显示的分区索引 */
static lv_obj_t *g_album_overlay   = NULL;  /* 相册弹窗遮罩 */
static lv_obj_t *g_album_title_lbl = NULL;  /* 相册弹窗标题标签 */
static lv_obj_t *g_album_body      = NULL;  /* 相册弹窗内容区（重绘用） */
static lv_obj_t *g_cam_wrap        = NULL;  /* 摄像头容器（切换时改背景色） */

/* 测试模块弹窗 */
static lv_obj_t *g_test_overlay    = NULL;  /* 测试弹窗遮罩 */
static lv_obj_t *g_test_lbls[8];            /* 各模块状态行 label（1s 刷新） */
static int       g_idle_sec = 0;            /* 无触摸空闲秒数（超时休眠） */
#define IDLE_SLEEP_SEC  600                 /* 10 分钟无操作 → 屏幕熄灭 */

/* 告警确认弹窗（长按 1s 消警防误触 + 一键静音） */
static lv_obj_t *g_alarm_popup       = NULL;  /* 告警弹窗遮罩 */
static lv_obj_t *g_alarm_info_lbl    = NULL;  /* 告警信息行（区域/类型/时间） */
static lv_obj_t *g_confirm_bar       = NULL;  /* 长按进度条 */
static uint32_t  g_confirm_tick      = 0;     /* 长按起始 tick（0=未按） */
static int       g_alarm_zone        = -1;    /* 告警分区索引 */

/* 摄像头画布（630×340 铺满画面区，静态缓冲 ≈428KB 不占 LVGL 堆；UI 定时器从 cam_feed 双缓冲 blit） */
static uint8_t g_cam_canvas_buf[CAM_DISP_W * CAM_DISP_H * 2];
static lv_obj_t *g_cam_canvas = NULL;       /* canvas 对象 */

/* 入侵判别模式（设计：云端精判=粗判+上传 / 本地精判=粗判+NCNN） */
#define DETECT_MODE_CLOUD  0
#define DETECT_MODE_LOCAL  1
static int g_detect_mode = DETECT_MODE_CLOUD;   /* 默认云端精判 */
static lv_obj_t *g_mode_badge_lbl = NULL;       /* 摄像头区模式徽章（联动） */
static lv_obj_t *g_dm_btns[2] = {NULL, NULL};   /* 设置弹窗模式按钮组 */

/* ================================================================
 * 抓拍存储（真实画面）：全图（查看弹窗）+ 缩略图（相册 3×2）
 * 触发：抓拍按钮（手动）/ 预警触发（自动）；超出 6 张环形覆盖最旧
 * 内存：全图 6×428KB + 缩略图 6×65KB ≈ 2.96MB（静态分配，不占 LVGL 堆）
 * ================================================================ */
#define SNAP_MAX 6
#define SNAP_W   CAM_DISP_W          /* 630，与预览显示区一致 */
#define SNAP_H   CAM_DISP_H          /* 340 */
#define THUMB_W  240                 /* 相册缩略图宽（网格格尺寸） */
#define THUMB_H  135                 /* 相册缩略图高 */

typedef struct {
    uint16_t pix[SNAP_W * SNAP_H];   /* 全图 RGB565（查看弹窗显示） */
    uint16_t thumb[THUMB_W * THUMB_H]; /* 缩略图 RGB565（相册显示） */
    char     time[8];                /* "HH:MM" */
    char     zone[8];                /* 抓拍区域（前门/后门/...） */
    char     level[6];               /* "high"/"medium"/"low"/""（手动为空） */
} snapshot_t;

static snapshot_t g_snaps[SNAP_MAX];
static int        g_snap_count = 0;

/* 设置弹窗动态控件 */
static lv_obj_t *g_bright_lbl      = NULL;  /* 亮度数值标签 */
static lv_obj_t *g_vol_lbl         = NULL;  /* 音量数值标签 */
static int g_brightness = 80;               /* HTML 初始 80% */
static int g_volume      = 60;              /* HTML 初始 60% */
static int g_schedule_on = 0;               /* 定时布防开关状态 */
static lv_obj_t *g_sched_toggle   = NULL;  /* 定时布防开关 */
static lv_obj_t *g_sched_knob     = NULL;  /* 开关滑块 */
static lv_obj_t *g_sched_hint     = NULL;  /* 开关提示文字 */
static int g_sched_start_h = 22;           /* 定时布防开始小时（默认 22:00） */
static int g_sched_end_h   = 6;            /* 定时布防结束小时（默认 06:00） */

/* ================================================================
 * 定时器（仅2个）
 * ================================================================ */
static lv_timer_t *g_timer_clock = NULL;
static lv_timer_t *g_timer_anim  = NULL;
static int g_scan_y = 0;
static int g_breath_phase = 0;
static int g_sim_sec = 0;   /* 模拟时钟累计秒（14:31:00 起） */

/* 获取当前时间 HH:MM（供事件时间戳使用，与 HTML toTimeString().slice(0,5) 一致）
 * g_sim_sec 由时钟定时器每秒同步为系统时间的"当日秒数" */
static void _get_time_str(char *buf, int len)
{
    if (!buf || len <= 0) return;
    int total = g_sim_sec % 86400;
    snprintf(buf, len, "%02d:%02d", total / 3600, (total % 3600) / 60);
}

/* ================================================================
 * 前向声明
 * ================================================================ */

/* ===== SouthBay: 联网状态真实检测（每5秒 ping 网关） ===== */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static lv_obj_t *g_net_dot = NULL;
static lv_obj_t *g_net_lbl = NULL;

// 简单 TCP connect 探测网关 80 端口（无阻塞超时 200ms）
static int net_check(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = inet_addr("192.168.3.1");   // 板端网关 IP
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == 0) { close(fd); return 1; }
    if (errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = {0, 200000};
        ret = select(fd + 1, NULL, &wfds, NULL, &tv);
        close(fd);
        return (ret > 0) ? 1 : 0;
    }
    close(fd);
    return 0;
}

void ui_events_net_set(int online);   /* ui_events.h 实现于文件末尾 */

static void net_timer_cb(lv_timer_t *t) {
    (void)t;
    int online = net_check();
    ui_events_net_set(online);   /* 统一走公共接口（含状态变化事件） */
}

static void _build_status_bar(lv_obj_t *parent);
static void _build_cam_wrap(lv_obj_t *parent);
static void _build_zones_grid(lv_obj_t *parent);
static void _build_right_col(lv_obj_t *parent);
static void _build_bottom_bar(lv_obj_t *parent);
static void _build_detail_modal(void);
static void _build_settings_modal(void);
static void _build_album_modal(void);
static void _build_test_modal(void);
static void _build_viewer_modal(void);
static void _build_alarm_popup(void);
static void _open_alarm_popup(int zone_idx, const char *src);
static void _open_viewer(int idx);
static void snapshot_capture(const char *zone, const char *level);
static void _snapshots_restore(void);
static void _refresh_test_modal(void);
static void _screen_set_blank(int on);
static void _cam_refresh_cb(lv_timer_t *timer);
static void _on_event_view(lv_event_t *e);
static void _add_event(const char *title, const char *loc,
                       uint32_t dot_clr, const char *level);

/* ================================================================
 * Toast 工具（一次性定时器隐藏）
 * ================================================================ */
static void _toast_dismiss_cb(lv_timer_t *t)
{
    /* v9 中 lv_timer_t 为不透明结构体，不能直接访问 t->user_data，
     * 改用文件内全局指针 g_toast 获取 toast 对象 */
    (void)t;
    if (g_toast) lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_del(t);
}

static void _show_toast(const char *txt)
{
    if (!g_toast || !txt) return;
    lv_label_set_text(g_toast, txt);
    lv_obj_clear_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_t *t = lv_timer_create(_toast_dismiss_cb, 1500, g_toast);
    if (t) lv_timer_set_repeat_count(t, 1);
}

/* ================================================================
 * 状态栏布防指示（派生状态）：
 *   任一分区布防 → "布防中"（绿点）；全部分区未布防 → "已撤防"（琥珀点）
 * 在每次分区视觉更新后调用，覆盖全量/分区级/定时布防所有路径
 * ================================================================ */
static void _update_arm_indicator(void)
{
    int armed = ui_events_zone_has_armed();
    uint32_t clr = armed ? CLR_GREEN : CLR_AMBER;
    if (g_arm_lbl)
        lv_label_set_text(g_arm_lbl, armed ? "布防中" : "已撤防");
    if (g_arm_dot) {
        lv_obj_set_style_bg_color(g_arm_dot, lv_color_hex(clr), 0);
        lv_obj_set_style_shadow_color(g_arm_dot, lv_color_hex(clr), 0);
        lv_obj_set_style_shadow_width(g_arm_dot, 6, 0);
        lv_obj_set_style_shadow_opa(g_arm_dot, LV_OPA_COVER, 0);
    }
}

/* ================================================================
 * 更新分区卡片视觉状态
 * ================================================================ */
static void _update_zone_visual(int i)
{
    if (i < 0 || i >= ZONE_COUNT) return;
    zone_state_t st = g_zone_states[i];
    lv_obj_t *card = g_zone_cards[i];
    lv_obj_t *dot  = g_zone_dots[i];
    lv_obj_t *stat = g_zone_status[i];

    if (!card) return;

    switch (st) {
    case ZONE_ARMED:
        lv_obj_set_style_border_color(card, lv_color_hex(CLR_GREEN), 0);
        if (dot) lv_obj_set_style_bg_color(dot, lv_color_hex(CLR_GREEN), 0);
        if (stat) lv_label_set_text(stat, "布防中");
        break;
    case ZONE_ALARM:
        lv_obj_set_style_border_color(card, lv_color_hex(CLR_RED), 0);
        if (dot) lv_obj_set_style_bg_color(dot, lv_color_hex(CLR_RED), 0);
        if (stat) lv_label_set_text(stat, "异常触发");
        break;
    case ZONE_OFFLINE:
        lv_obj_set_style_border_color(card, lv_color_hex(CLR_TEXT_DIM), 0);
        if (dot) lv_obj_set_style_bg_color(dot, lv_color_hex(CLR_TEXT_DIM), 0);
        if (stat) lv_label_set_text(stat, "离线");
        break;
    default: /* ZONE_ONLINE */
        lv_obj_set_style_border_color(card, lv_color_hex(CLR_BORDER), 0);
        if (dot) lv_obj_set_style_bg_color(dot, lv_color_hex(CLR_GREEN), 0);
        if (stat) lv_label_set_text(stat, "已撤防");
        break;
    }

    _update_arm_indicator();   /* 状态栏布防指示联动（任一布防=布防中） */
}

/* ================================================================
 * 回调：布防按钮
 * ================================================================ */
static void _on_arm(lv_event_t *e)
{
    (void)e;
    arm_all();   /* 全量布防（幂等）：部分布防状态下可补齐其余分区 */

    for (int i = 0; i < ZONE_COUNT; i++) {
        g_zone_states[i] = ZONE_ARMED;
        _update_zone_visual(i);   /* 状态栏指示由 _update_arm_indicator 联动 */
    }

    _show_toast("全部分区已布防");
    _add_event("全部分区已布防",
               "系统", CLR_GREEN, "");   /* HTML addEvent('ok',...) */
}

/* ================================================================
 * 回调：撤防按钮
 * ================================================================ */
static void _on_disarm(lv_event_t *e)
{
    (void)e;
    if (!ui_events_zone_has_armed()) return;
    disarm_all();

    for (int i = 0; i < ZONE_COUNT; i++) {
        g_zone_states[i] = ZONE_ONLINE;
        _update_zone_visual(i);
    }

    _show_toast("全部分区已撤防");
    _add_event("全部分区已撤防",
               "系统", CLR_AMBER, "");   /* HTML addEvent('warn',...) */
}

/* ================================================================
 * 回调：消警按钮
 * ================================================================ */
static void _on_silence(lv_event_t *e)
{
    (void)e;
    int had_alarm = 0;
    for (int i = 0; i < ZONE_COUNT; i++) {
        if (g_zone_states[i] == ZONE_ALARM) {
            ack_alarm(i);
            /* 告警必产生于布防状态，消除后回到布防中 */
            g_zone_states[i] = ZONE_ARMED;
            _update_zone_visual(i);
            had_alarm = 1;
        }
    }
    if (had_alarm) {
        _show_toast("报警已消");
        _add_event("报警已消",
                   "系统", CLR_GREEN, "");   /* HTML addEvent('ok',...) */
    } else {
        _show_toast("当前状态正常");
        _add_event("当前正常",
                   "系统", CLR_CYAN, "");    /* HTML addEvent('info',...) */
    }}

/* ================================================================
 * 回调：设置按钮 — 显示设置弹窗
 * ================================================================ */
static void _on_settings(lv_event_t *e)
{
    (void)e;
    if (g_overlay_settings)
        lv_obj_clear_flag(g_overlay_settings, LV_OBJ_FLAG_HIDDEN);
    /* HTML doSettings：打开时记录事件 */
    _add_event("已入设置界面",
               "系统", CLR_CYAN, "");
}

/* ================================================================
 * 屏幕休眠/唤醒（fb0 blank）
 * 60s 无触摸 → blank(1)；任意触摸/KEY0 → 唤醒（KEY0 由 dev_bridge 处理）
 * ================================================================ */
static int g_screen_blanked = 0;   /* 屏幕是否处于 blank（休眠）状态 */

static void _screen_set_blank(int on)
{
    int fd = open("/sys/class/graphics/fb0/blank", O_WRONLY);
    if (fd < 0) return;
    if (lseek(fd, 0, SEEK_SET) >= 0) {
        char c = on ? '1' : '0';
        if (write(fd, &c, 1) < 0) perror("[UI] blank write");
    }
    close(fd);
    g_screen_blanked = on ? 1 : 0;
}

/* 任意触摸事件 → 用户活动（重置空闲计时 + 唤醒屏幕） */
static void _on_screen_touch(lv_event_t *e)
{
    (void)e;
    ui_events_user_activity();
}

/* ================================================================
 * 回调：测试模块按钮 — 显示模块状态弹窗
 * ================================================================ */
static void _on_test(lv_event_t *e)
{
    (void)e;
    if (g_test_overlay) {
        _refresh_test_modal();   /* 打开瞬间刷新一次 */
        lv_obj_clear_flag(g_test_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ================================================================
 * 详情弹窗：分区状态设置（布防⇄撤防，分区级独立控制）
 * ================================================================ */

/* 根据当前分区状态刷新弹窗内的状态行与按钮文字 */
static void _detail_refresh_state(void)
{
    int idx = g_detail_zone_idx;
    if (idx < 0 || idx >= ZONE_COUNT) return;
    zone_state_t st = g_zone_states[idx];
    const char *st_txt;
    uint32_t st_clr;

    switch (st) {
    case ZONE_ARMED: st_txt = "布防中";  st_clr = CLR_GREEN; break;
    case ZONE_ALARM: st_txt = "警";     st_clr = CLR_RED;   break;
    default:         st_txt = "已撤防"; st_clr = CLR_CYAN;  break;
    }

    if (g_detail_state_lbl) {
        lv_label_set_text(g_detail_state_lbl, st_txt);
        lv_obj_set_style_text_color(g_detail_state_lbl, lv_color_hex(st_clr), 0);
    }

    /* 按钮文字与配色：布防中 → 显示"撤防"；否则 → 显示"布防" */
    if (g_detail_arm_btn) {
        const char *txt = (st == ZONE_ARMED) ? "撤防" : "布防";
        uint32_t clr = (st == ZONE_ARMED) ? CLR_BORDER_LT : CLR_CYAN;
        lv_obj_set_style_border_color(g_detail_arm_btn, lv_color_hex(clr), 0);
        lv_obj_t *l = lv_obj_get_child(g_detail_arm_btn, 0);
        if (l) {
            lv_label_set_text(l, txt);
            lv_obj_set_style_text_color(l, lv_color_hex(clr), 0);
        }
    }
}

/* 状态设置按钮点击：切换该分区布防⇄撤防（告警中点击先消警） */
static void _on_zone_arm_toggle(lv_event_t *e)
{
    (void)e;
    int idx = g_detail_zone_idx;
    if (idx < 0 || idx >= ZONE_COUNT) return;

    switch (g_zone_states[idx]) {
    case ZONE_ALARM:
        ui_events_alarm_ack(idx);            /* 消警 → 回布防中 */
        break;
    case ZONE_ARMED:
        ui_events_zone_set_state(idx, ZONE_ONLINE);   /* 单独撤防 */
        _add_event("分区撤防", zone_info[idx].name, CLR_CYAN, "");
        break;
    default:
        ui_events_zone_set_state(idx, ZONE_ARMED);    /* 单独布防 */
        _add_event("分区布防", zone_info[idx].name, CLR_GREEN, "");
        break;
    }

    _detail_refresh_state();
}

/* ================================================================
 * 回调：分区卡片点击 — 显示详情弹窗
 * ================================================================ */
static void _on_zone_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= ZONE_COUNT || !g_detail_body || !g_overlay_detail)
        return;
    g_detail_zone_idx = idx;   /* 记录当前弹窗分区（状态按钮用） */

    const zone_info_t *z = &zone_info[idx];
    zone_state_t st = g_zone_states[idx];
    const char *st_txt;
    uint32_t st_clr;

    switch (st) {
    case ZONE_ARMED: st_txt = "布防中";  st_clr = CLR_GREEN; break;
    case ZONE_ALARM: st_txt = "警";    st_clr = CLR_RED;   break;
    default:         st_txt = "已撤防"; st_clr = CLR_CYAN;  break;
    }

    /* 清空旧内容并重建 */
    lv_obj_clean(g_detail_body);

    /* 动态更新弹窗标题："[分区名] · 分区详情"，与 HTML showDetail 一致 */
    if (g_detail_title_lbl) {
        char title_buf[32];
        snprintf(title_buf, sizeof(title_buf), "%s 分区",
                 z->name);
        lv_label_set_text(g_detail_title_lbl, title_buf);
    }

    /* 第一行：状态设置标签 + 布防/撤防按钮（分区级独立控制） */
    lv_obj_t *btn_row = uiw_obj(g_detail_body);
    if (btn_row) {
        lv_obj_set_size(btn_row, 368, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_top(btn_row, 8, 0);
        lv_obj_set_style_pad_bottom(btn_row, 8, 0);
        lv_obj_set_style_border_side(btn_row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(btn_row, lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_border_width(btn_row, 1, 0);
        uiw_label(btn_row, "状态设置", CLR_TEXT_DIM);

        g_detail_arm_btn = lv_button_create(btn_row);
        if (g_detail_arm_btn) {
            lv_obj_set_size(g_detail_arm_btn, 90, 32);
            lv_obj_set_style_bg_opa(g_detail_arm_btn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(g_detail_arm_btn, lv_color_hex(CLR_CYAN), 0);
            lv_obj_set_style_border_width(g_detail_arm_btn, 1, 0);
            lv_obj_set_style_radius(g_detail_arm_btn, 0, 0);
            lv_obj_set_style_pad_all(g_detail_arm_btn, 0, 0);
            lv_obj_clear_flag(g_detail_arm_btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *bl = uiw_label(g_detail_arm_btn, "布防", CLR_CYAN);
            if (bl) lv_obj_center(bl);   /* 字体居中 */
            lv_obj_add_event_cb(g_detail_arm_btn, _on_zone_arm_toggle,
                               LV_EVENT_CLICKED, NULL);
        }
    }

    /* 构造详情行 */
    struct { const char *label; const char *value; uint32_t val_clr; } rows[] = {
        {"分区 ID",   z->id,     CLR_CYAN},
        {"分区",   z->name,   CLR_TEXT_HI},
        {"当前状态",   st_txt,    st_clr},
        {"检测", z->sensor, CLR_TEXT_HI},
        {"IP",    z->ip,     CLR_CYAN},
        {"系统",   z->fw,     CLR_CYAN},
        {"dBm",   z->rssi,   CLR_CYAN},
    };

    for (int r = 0; r < 7; r++) {
        lv_obj_t *row = uiw_obj(g_detail_body);
        if (!row) continue;
        lv_obj_set_size(row, 368, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_top(row, 8, 0);
        lv_obj_set_style_pad_bottom(row, 8, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_border_width(row, 1, 0);

        uiw_label(row, rows[r].label, CLR_TEXT_DIM);
        lv_obj_t *val_lbl = uiw_label_font(row, rows[r].value, rows[r].val_clr,
                                           &lv_font_SHSC_16);
        /* 保存"当前状态"值标签（状态切换后实时更新） */
        if (r == 2) g_detail_state_lbl = val_lbl;
    }

    _detail_refresh_state();   /* 按状态设置按钮文字与配色 */

    lv_obj_clear_flag(g_overlay_detail, LV_OBJ_FLAG_HIDDEN);
}

/* ================================================================
 * 回调：摄像头切换（含淡入淡出 + 背景色变化 + 联动高亮分区卡片）
 * ================================================================ */
static void _opa_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void _on_cam_switch(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= CAM_COUNT || idx == g_current_cam) return;

    g_current_cam = idx;

    /* 更新 pill 高亮 */
    for (int i = 0; i < CAM_COUNT; i++) {
        if (!g_cam_pills[i]) continue;
        if (i == idx) {
            lv_obj_set_style_bg_opa(g_cam_pills[i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(g_cam_pills[i], lv_color_hex(CLR_CYAN), 0);
            lv_obj_set_style_border_color(g_cam_pills[i], lv_color_hex(CLR_CYAN), 0);
            /* pill 内文字变白 */
            lv_obj_t *child = lv_obj_get_child(g_cam_pills[i], 0);
            if (child) lv_obj_set_style_text_color(child, lv_color_hex(0xFFFFFF), 0);
        } else {
            lv_obj_set_style_bg_opa(g_cam_pills[i], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(g_cam_pills[i], lv_color_hex(CLR_BORDER), 0);
            lv_obj_t *child = lv_obj_get_child(g_cam_pills[i], 0);
            if (child) lv_obj_set_style_text_color(child, lv_color_hex(CLR_TEXT_LO), 0);
        }
    }

    /* 预览背景色变化（HTML CAMS.bg） */
    if (g_cam_wrap)
        lv_obj_set_style_bg_color(g_cam_wrap, lv_color_hex(cam_info[idx].bg_clr), 0);

    /* 更新预览文字（先淡出，再换文字淡入，近似 HTML fade 过渡） */
    if (g_cam_text_lbl) {
        lv_obj_set_style_opa(g_cam_text_lbl, LV_OPA_TRANSP, 0);
        lv_label_set_text(g_cam_text_lbl, cam_info[idx].label);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, g_cam_text_lbl);
        lv_anim_set_exec_cb(&a, _opa_anim_cb);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_time(&a, 300);
        lv_anim_set_repeat_count(&a, 1);
        lv_anim_start(&a);
    }

    /* 更新通道信息（fps 为 cam_feed 实测值，每秒由时钟定时器刷新） */
    if (g_cam_title_lbl) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s · %dfps",
                 cam_info[idx].ch, cam_feed_get_fps());
        lv_label_set_text(g_cam_title_lbl, buf);
    }

    /* 联动高亮对应分区卡片（HTML highlightZoneCard，映射 前门=0 后门=1 仓库=3 窗户=2） */
    int zidx = cam_to_zone[idx];
    for (int i = 0; i < ZONE_COUNT; i++) {
        if (!g_zone_cards[i]) continue;
        if (i == zidx) {
            lv_obj_set_style_border_color(g_zone_cards[i], lv_color_hex(CLR_CYAN), 0);
            lv_obj_set_style_shadow_color(g_zone_cards[i], lv_color_hex(CLR_CYAN), 0);
            lv_obj_set_style_shadow_width(g_zone_cards[i], 10, 0);
            lv_obj_set_style_shadow_opa(g_zone_cards[i], LV_OPA_40, 0);
        } else {
            lv_obj_set_style_shadow_width(g_zone_cards[i], 0, 0);
            _update_zone_visual(i);   /* 恢复为状态色边框 */
        }
    }
}

/* ================================================================
 * 回调：抓拍按钮
 * ================================================================ */
static void _on_capture(lv_event_t *e)
{
    (void)e;
    /* 抓拍当前预览画面（区域=当前通道，手动无级别）→ 存入相册 */
    snapshot_capture(cam_info[g_current_cam].name, "");
    _show_toast("抓拍已保存");
    _add_event("手动抓拍已保存",
               cam_info[g_current_cam].ch, CLR_GREEN, "");
}

/* ================================================================
 * 事件条目构建（HTML .event-item：时间+圆点+内容+级别徽章+查看按钮）
 * 初始列表与动态插入共用
 * ================================================================ */
static lv_obj_t *_append_event_item(lv_obj_t *list, const char *time,
                                    const char *title, const char *loc,
                                    uint32_t dot_clr, const char *level)
{
    if (!list) return NULL;

    lv_obj_t *item = uiw_obj(list);
    if (!item) return NULL;
    lv_obj_set_width(item, RIGHT_W - 2);
    lv_obj_set_height(item, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(item, 10, 0);
    lv_obj_set_style_pad_left(item, 12, 0);
    lv_obj_set_style_pad_right(item, 12, 0);
    lv_obj_set_style_pad_top(item, 8, 0);
    lv_obj_set_style_pad_bottom(item, 8, 0);
    lv_obj_set_style_border_side(item, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(item, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(item, 1, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

    /* 时间 */
    lv_obj_t *tl = uiw_label_font(item, time, CLR_TEXT_DIM, &lv_font_SHSC_16);
    if (tl) lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);

    /* 圆点 */
    uiw_dot(item, dot_clr, 6);

    /* 内容区 */
    lv_obj_t *body = uiw_obj(item);
    if (body) {
        lv_obj_set_flex_grow(body, 1);
        lv_obj_set_height(body, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_gap(body, 2, 0);

        lv_obj_t *title_lbl = uiw_label(body, title, CLR_TEXT_HI);
        if (title_lbl) lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
        lv_obj_t *loc_lbl = uiw_label(body, loc, CLR_TEXT_DIM);
        if (loc_lbl) lv_label_set_long_mode(loc_lbl, LV_LABEL_LONG_DOT);

        /* 级别徽章（high/medium/low 均显示，与 HTML getLevelBadge 一致） */
        if (level && level[0]) {
            uint32_t badge_bg, badge_tx, badge_bd;
            const char *badge_txt;
            if (strcmp(level, "high") == 0) {
                badge_bg = CLR_RED; badge_tx = CLR_RED;
                badge_bd = CLR_RED;
                badge_txt = "级别：高";
            } else if (strcmp(level, "medium") == 0) {
                badge_bg = CLR_AMBER; badge_tx = CLR_AMBER;
                badge_bd = CLR_AMBER;
                badge_txt = "级别：中";
            } else {
                badge_bg = CLR_CYAN; badge_tx = CLR_CYAN;
                badge_bd = CLR_CYAN;
                badge_txt = "级别：L";
            }
            lv_obj_t *badge = uiw_obj(body);
            if (badge) {
                lv_obj_set_height(badge, LV_SIZE_CONTENT);
                lv_obj_set_width(badge, LV_SIZE_CONTENT);
                lv_obj_set_style_bg_color(badge, lv_color_hex(badge_bg), 0);
                lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
                lv_obj_set_style_border_color(badge, lv_color_hex(badge_bd), 0);
                lv_obj_set_style_border_width(badge, 1, 0);
                lv_obj_set_style_pad_left(badge, 6, 0);
                lv_obj_set_style_pad_right(badge, 6, 0);
                lv_obj_set_style_pad_top(badge, 2, 0);
                lv_obj_set_style_pad_bottom(badge, 2, 0);
                lv_obj_t *badge_lbl = uiw_label(badge, badge_txt, badge_tx);
                if (badge_lbl) lv_label_set_long_mode(badge_lbl, LV_LABEL_LONG_DOT);
            }
        }
    }

    /* "查看"按钮（HTML .event-view-btn），点击弹出相册 */
    {
        lv_obj_t *vbtn = lv_button_create(item);
        if (vbtn) {
            /* 固定宽 46×22：SHSC_16 汉字"览"最小 16px，加大按钮保证完整显示不截断 */
            lv_obj_set_size(vbtn, 46, 22);
            lv_obj_set_style_bg_opa(vbtn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(vbtn, lv_color_hex(CLR_BORDER), 0);
            lv_obj_set_style_border_width(vbtn, 1, 0);
            lv_obj_set_style_radius(vbtn, 0, 0);
            lv_obj_set_style_pad_left(vbtn, 0, 0);
            lv_obj_set_style_pad_right(vbtn, 0, 0);
            lv_obj_set_style_pad_top(vbtn, 0, 0);
            lv_obj_set_style_pad_bottom(vbtn, 0, 0);
            lv_obj_clear_flag(vbtn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *vl = uiw_label(vbtn, "览", CLR_TEXT_LO);
            if (vl) lv_obj_center(vl);
            /* 传入事件标题作为相册标题 */
            lv_obj_add_event_cb(vbtn, _on_event_view, LV_EVENT_CLICKED,
                               (void *)title);
        }
    }
    return item;
}

/* ================================================================
 * 动态添加事件（HTML addEvent：插入列表顶部 + 计数更新 + 超限移除最旧）
 * ================================================================ */
static void _add_event(const char *title, const char *loc,
                       uint32_t dot_clr, const char *level)
{
    if (!g_event_list || !title) return;

    char tbuf[8];
    _get_time_str(tbuf, sizeof(tbuf));

    lv_obj_t *item = _append_event_item(g_event_list, tbuf, title, loc, dot_clr, level);
    if (!item) return;

    /* 插入到列表顶部（HTML insertBefore(list.firstChild)） */
    lv_obj_move_to_index(item, 0);

    /* 超限移除最旧（保留最多 EVT_MAX 条） */
    if (evt_count >= EVT_MAX) {
        uint32_t cnt = lv_obj_get_child_count(g_event_list);
        if (cnt > EVT_MAX) {
            lv_obj_t *last = lv_obj_get_child(g_event_list, cnt - 1);
            if (last) lv_obj_delete(last);
        }
    } else {
        evt_count++;
    }

    /* 更新计数徽章 */
    if (g_event_count_lbl) {
        char cnt_buf[8];
        snprintf(cnt_buf, sizeof(cnt_buf), "%d 条", evt_count);
        lv_label_set_text(g_event_count_lbl, cnt_buf);
    }

    /* 云端上报（MQTT fire-and-forget，不阻塞 UI；未配 broker 时模块内部禁用） */
    {
        char tbuf[8];
        _get_time_str(tbuf, sizeof(tbuf));
        mqtt_hub_publish_event(title, loc, level ? level : "", tbuf);
    }
}

/* ================================================================
 * 定时器回调：时钟更新（1秒）— 读取板子系统时间（真实时间）
 * ================================================================ */
static int _any_modal_visible(void);   /* 定义于 _timer_anim_cb 前 */
static void _backlight_set(int percent);   /* 定义于设置弹窗区 */
static void _refresh_test_modal(void);     /* 定义于测试弹窗区 */

static void _timer_clock_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!g_clock_lbl) return;

    /* 真实时间：读系统时钟（板上 date/NTP 同步后自动跟随） */
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    /* 事件时间戳来源同步为真实 HH:MM（g_sim_sec 换算为当日秒偏移） */
    g_sim_sec = tm_now.tm_hour * 3600 + tm_now.tm_min * 60 + tm_now.tm_sec;

    /* 摄像头标题：通道号 + 实测帧率（每秒刷新） */
    if (g_cam_title_lbl && g_cam_canvas) {
        char b[32];
        snprintf(b, sizeof(b), "%s · %dfps",
                 cam_info[g_current_cam].ch, cam_feed_get_fps());
        lv_label_set_text(g_cam_title_lbl, b);
    }

    /* 定时布防自动执行：开关开启且当前小时在 [start, end) 区间内 → 布防 */
    if (g_schedule_on) {
        int in_window;
        if (g_sched_start_h <= g_sched_end_h)
            in_window = (tm_now.tm_hour >= g_sched_start_h && tm_now.tm_hour < g_sched_end_h);
        else   /* 跨天：22:00-06:00 */
            in_window = (tm_now.tm_hour >= g_sched_start_h || tm_now.tm_hour < g_sched_end_h);
        if (in_window && !ui_events_is_armed()) {
            for (int i = 0; i < ZONE_COUNT; i++)
                ui_events_zone_set_state(i, ZONE_ARMED);
            ui_events_log("全部分区已布防", "系统", "");
            _show_toast("定时布防");
        } else if (!in_window && ui_events_is_armed()) {
            for (int i = 0; i < ZONE_COUNT; i++)
                ui_events_zone_set_state(i, ZONE_ONLINE);
            ui_events_log("全部分区已撤防", "系统", "");
        }
    }

    /* 测试模块弹窗打开时：每秒刷新实时数据 */
    if (g_test_overlay && !lv_obj_has_flag(g_test_overlay, LV_OBJ_FLAG_HIDDEN))
        _refresh_test_modal();

    /* 空闲休眠：60s 无触摸 → fb blank；触摸/KEY0 唤醒（blank 0） */
    g_idle_sec++;
    if (g_idle_sec >= IDLE_SLEEP_SEC) {
        _screen_set_blank(1);
        g_idle_sec = IDLE_SLEEP_SEC;   /* 饱和，避免反复写 */
    }

    /* 弹窗打开期间不刷新时钟文字（避免状态栏局部重绘穿帮），关闭后自动补上 */
    if (_any_modal_visible()) return;
    if (g_screen_blanked) return;   /* 休眠期间不绘制 */

    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    lv_label_set_text(g_clock_lbl, buf);
}

/* ================================================================
 * 定时器回调：呼吸动画 + 扫描线（50ms）
 * ================================================================ */

/* 任一弹窗是否可见（弹窗打开时冻结动画，避免板端 fbdev
 * 局部重绘把摄像头区条带合成到 top-layer 弹窗之上） */
static int _any_modal_visible(void)
{
    if (g_overlay_detail   && !lv_obj_has_flag(g_overlay_detail, LV_OBJ_FLAG_HIDDEN)) return 1;
    if (g_overlay_settings && !lv_obj_has_flag(g_overlay_settings, LV_OBJ_FLAG_HIDDEN)) return 1;
    if (g_album_overlay    && !lv_obj_has_flag(g_album_overlay, LV_OBJ_FLAG_HIDDEN)) return 1;
    if (g_test_overlay     && !lv_obj_has_flag(g_test_overlay, LV_OBJ_FLAG_HIDDEN)) return 1;
    if (g_alarm_popup      && !lv_obj_has_flag(g_alarm_popup, LV_OBJ_FLAG_HIDDEN)) return 1;
    return 0;
}

static void _timer_anim_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 屏幕休眠期间冻结动画（避免无效绘制 + 唤醒撕裂） */
    if (g_screen_blanked) return;

    /* 弹窗打开期间冻结动画（扫描线/呼吸），关闭后自动恢复 */
    if (_any_modal_visible()) return;

    g_breath_phase++;
    if (g_breath_phase > 100) g_breath_phase = 0;

    /* 呼吸动画：调整布防中卡片的 shadow_opa（任一分区布防即生效） */
    if (ui_events_zone_has_armed()) {
        /* sin-like: 0..40..0 */
        int opa;
        if (g_breath_phase <= 50)
            opa = g_breath_phase * 40 / 50;
        else
            opa = (100 - g_breath_phase) * 40 / 50;

        for (int i = 0; i < ZONE_COUNT; i++) {
            if (g_zone_states[i] == ZONE_ARMED && g_zone_cards[i]) {
                lv_obj_set_style_shadow_opa(g_zone_cards[i], opa, 0);
                lv_obj_set_style_shadow_width(g_zone_cards[i], 8 + opa / 4, 0);
            }
        }
    }

    /* 扫描线移动 */
    if (g_scanline) {
        g_scan_y += 2;
        if (g_scan_y > CAM_H) g_scan_y = 0;
        lv_obj_set_y(g_scanline, g_scan_y);
    }
}

/* ================================================================
 * status-bar  36px（HTML .status-bar）
 * ================================================================ */
static void _build_status_bar(lv_obj_t *p)
{
    lv_obj_t *bar = uiw_obj(p);
    if (!bar) return;
    lv_obj_set_size(bar, SCR_W, STATUS_H);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_CARD), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(bar, 16, 0);
    lv_obj_set_style_pad_right(bar, 16, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 左: 圆点装饰 + 标题 */
    /* 注: 原先此处用 LV_SYMBOL_HOME 图标，但 uiw_label 默认中文字库 SHSC_16
     * 无 FontAwesome 字形，会渲染成半个字符乱码，故改为纯色圆点装饰 */
    lv_obj_t *gl = uiw_obj(bar);
    if (gl) {
        /* 自适应内容宽度，避免被布局压缩裁字 */
        lv_obj_set_size(gl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(gl, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(gl, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(gl, 10, 0);
        lv_obj_clear_flag(gl, LV_OBJ_FLAG_SCROLLABLE);
        uiw_dot(gl, CLR_CYAN, 8);
        /* 标题自适应宽度，不用 LONG_DOT 裁剪 */
        uiw_label_font(gl, "智慧安防网关", CLR_TEXT_HI, &lv_font_SHSC_16);
        uiw_label_font(gl, " · ", CLR_TEXT_DIM, &lv_font_SHSC_16);
        uiw_label_font(gl, "边缘在线", CLR_TEXT_DIM, &lv_font_SHSC_16);
    }

    /* 中: 时钟 */
    g_clock_lbl = uiw_label(bar, "2026-08-22 14:32:00", CLR_TEXT_LO);
    if (g_clock_lbl) {
        lv_obj_set_style_text_font(g_clock_lbl, &lv_font_SHSC_16, 0);
    }

    /* 右侧: 联网 + 布防中 + i.MX6ULL —— 用 flex 容器从右往左排列（HTML .status-right） */
    lv_obj_t *sr = uiw_obj(bar);
    if (sr) {
        lv_obj_set_size(sr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(sr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sr, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(sr, 12, 0);
        lv_obj_clear_flag(sr, LV_OBJ_FLAG_SCROLLABLE);

        /* 组1: 联网 */
        lv_obj_t *g1 = uiw_obj(sr);
        if (g1) {
            lv_obj_set_size(g1, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(g1, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(g1, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(g1, 5, 0);
            lv_obj_clear_flag(g1, LV_OBJ_FLAG_SCROLLABLE);
            g_net_dot = uiw_dot(g1, CLR_GREEN, 8);
            g_net_lbl = uiw_label(g1, "联网", CLR_TEXT_LO);
            if (g_net_lbl) lv_obj_set_style_text_font(g_net_lbl, &lv_font_SHSC_16, 0);
        }

        /* 组2: 布防 */
        lv_obj_t *g2 = uiw_obj(sr);
        if (g2) {
            lv_obj_set_size(g2, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(g2, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(g2, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(g2, 5, 0);
            lv_obj_clear_flag(g2, LV_OBJ_FLAG_SCROLLABLE);
            g_arm_dot = uiw_dot(g2, CLR_GREEN, 8);
            if (g_arm_dot) {
                lv_obj_set_style_shadow_color(g_arm_dot, lv_color_hex(CLR_GREEN), 0);
                lv_obj_set_style_shadow_width(g_arm_dot, 6, 0);
                lv_obj_set_style_shadow_opa(g_arm_dot, LV_OPA_COVER, 0);
            }
            g_arm_lbl = uiw_label(g2, "已撤防", CLR_TEXT_LO);
            if (g_arm_lbl) lv_obj_set_style_text_font(g_arm_lbl, &lv_font_SHSC_16, 0);
        }

        /* 组3: i.MX6ULL */
        lv_obj_t *cpu_lbl = uiw_label(sr, "i.MX6ULL", CLR_TEXT_DIM);
        if (cpu_lbl) lv_obj_set_style_text_font(cpu_lbl, &lv_font_SHSC_16, 0);
    }
}


/* ================================================================
 * cam-wrap  630x294 摄像头预览
 * ================================================================ */
static void _build_cam_wrap(lv_obj_t *par)
{
    lv_obj_t *cam = uiw_obj(par);
    if (!cam) return;
    g_cam_wrap = cam;
    lv_obj_set_size(cam, LEFT_W, CAM_H);
    lv_obj_set_style_bg_opa(cam, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cam, lv_color_hex(cam_info[0].bg_clr), 0);
    lv_obj_set_style_border_color(cam, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(cam, 1, 0);
    lv_obj_add_flag(cam, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(cam, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 摄像头头部第一行 cam-header：徽标居左 + 通道信息居右 ---- */
    lv_obj_t *hdr = uiw_obj(cam);
    if (hdr) {
        lv_obj_set_size(hdr, LEFT_W, CAM_HDR1_H);
        lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(hdr, lv_color_hex(CLR_BLACK), 0);
        lv_obj_set_style_bg_opa(hdr, LV_OPA_40, 0);
        lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(hdr, lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_border_width(hdr, 1, 0);
        lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(hdr, 10, 0);
        lv_obj_set_style_pad_right(hdr, 10, 0);
        lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

        /* mode-badge：识别模式徽章（设置页切换 云端精判/本地精判 后联动） */
        lv_obj_t *badge = uiw_obj(hdr);
        if (badge) {
            lv_obj_set_size(badge, LV_SIZE_CONTENT, 26);
                        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(badge, lv_color_hex(CLR_CYAN), 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
            lv_obj_set_style_border_color(badge, lv_color_hex(CLR_CYAN), 0);
            lv_obj_set_style_border_width(badge, 1, 0);
            lv_obj_set_style_pad_left(badge, 8, 0);
            lv_obj_set_style_pad_right(badge, 8, 0);
            lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
            /* 自适应宽度完整显示；label 指针保存供模式切换联动 */
            g_mode_badge_lbl = uiw_label(badge, "云端精判模式",
                      CLR_CYAN);
        }

        /* cam-title：通道信息居右（fps 由 cam_feed 实测，每秒刷新） */
        g_cam_title_lbl = uiw_label_font(hdr, "CH01 · 30fps",
                                          CLR_TEXT_DIM, &lv_font_SHSC_16);
    }

    /* ---- 摄像头画布（630×340 铺满画面区，最先创建=最底层；
     *      pill/网格/准星/扫描线/抓拍按钮全部在其上层，不被遮挡） ---- */
    {
        g_cam_canvas = lv_canvas_create(cam);
        if (g_cam_canvas) {
            memset(g_cam_canvas_buf, 0, sizeof(g_cam_canvas_buf));   /* 初始黑 */
            lv_canvas_set_buffer(g_cam_canvas, g_cam_canvas_buf,
                                 CAM_DISP_W, CAM_DISP_H, LV_COLOR_FORMAT_RGB565);
            lv_obj_set_pos(g_cam_canvas, 0, CAM_PIC_Y);   /* 铺满画面区 */
            lv_obj_clear_flag(g_cam_canvas, LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    /* ---- 摄像头头部第二行：4 个通道切换 pill 水平居中 ---- */
    {
        lv_obj_t *sw = uiw_obj(cam);
        if (sw) {
            lv_obj_set_size(sw, LEFT_W, CAM_HDR2_H);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(sw, lv_color_hex(CLR_BLACK), 0);
            lv_obj_set_style_bg_opa(sw, LV_OPA_40, 0);
            lv_obj_set_flex_flow(sw, LV_FLEX_FLOW_ROW);
            /* 主轴居中排列 pill */
            lv_obj_set_flex_align(sw, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(sw, 8, 0);
            lv_obj_align(sw, LV_ALIGN_TOP_MID, 0, CAM_HDR1_H);
            lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);

            const char *pill_names[] = {"前门",
                                         "后门",
                                         "仓库",
                                         "窗户"};
            for (int i = 0; i < CAM_COUNT; i++) {
                lv_obj_t *pill = lv_button_create(sw);
                if (!pill) continue;
                lv_obj_set_height(pill, 22);
                lv_obj_set_width(pill, LV_SIZE_CONTENT);
                lv_obj_set_style_radius(pill, 0, 0);
                lv_obj_set_style_pad_left(pill, 14, 0);
                lv_obj_set_style_pad_right(pill, 14, 0);
                lv_obj_set_style_pad_top(pill, 2, 0);
                lv_obj_set_style_pad_bottom(pill, 2, 0);
                lv_obj_set_style_border_width(pill, 1, 0);
                lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

                if (i == 0) {
                    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
                    lv_obj_set_style_bg_color(pill, lv_color_hex(CLR_CYAN), 0);
                    lv_obj_set_style_border_color(pill, lv_color_hex(CLR_CYAN), 0);
                } else {
                    lv_obj_set_style_bg_opa(pill, LV_OPA_TRANSP, 0);
                    lv_obj_set_style_border_color(pill, lv_color_hex(CLR_BORDER), 0);
                }

                lv_obj_t *pl = uiw_label(pill, pill_names[i],
                                          i == 0 ? 0xFFFFFF : CLR_TEXT_LO);
                if (pl) {
                    lv_obj_center(pl);
                }

                g_cam_pills[i] = pill;
                lv_obj_add_event_cb(pill, _on_cam_switch, LV_EVENT_CLICKED,
                                   (void *)(intptr_t)i);
            }
        }
    }

    /* ---- 网格线（用多个细长对象模拟，从两行头部下方开始；有画面时隐藏） ---- */
    /* 横线 */
    for (int y = CAM_PIC_Y; y < CAM_H; y += 48) {
        lv_obj_t *line = uiw_obj(cam);
        if (!line) continue;
        lv_obj_set_size(line, LEFT_W, 1);
        lv_obj_set_style_bg_opa(line, LV_OPA_20, 0);
        lv_obj_set_style_bg_color(line, lv_color_hex(CLR_BORDER), 0);
        lv_obj_align(line, LV_ALIGN_TOP_LEFT, 0, y);
        if (g_cam_grid_cnt < (int)(sizeof(g_cam_grid_lines) / sizeof(g_cam_grid_lines[0])))
            g_cam_grid_lines[g_cam_grid_cnt++] = line;
    }
    /* 竖线 */
    for (int x = 48; x < LEFT_W; x += 48) {
        lv_obj_t *line = uiw_obj(cam);
        if (!line) continue;
        lv_obj_set_size(line, 1, CAM_H);
        lv_obj_set_style_bg_opa(line, LV_OPA_20, 0);
        lv_obj_set_style_bg_color(line, lv_color_hex(CLR_BORDER), 0);
        lv_obj_align(line, LV_ALIGN_TOP_LEFT, x, 0);
        if (g_cam_grid_cnt < (int)(sizeof(g_cam_grid_lines) / sizeof(g_cam_grid_lines[0])))
            g_cam_grid_lines[g_cam_grid_cnt++] = line;
    }

    /* ---- 十字准星（居中于两行头部下方的画面区） ---- */
    {
        int pic_cy = (CAM_PIC_Y + CAM_H) / 2 - CAM_H / 2; /* 画面区中心相对 cam 中心的偏移 */
        lv_obj_t *ch_h = uiw_obj(cam);
        if (ch_h) {
            lv_obj_set_size(ch_h, 80, 1);
            lv_obj_set_style_bg_opa(ch_h, LV_OPA_50, 0);
            lv_obj_set_style_bg_color(ch_h, lv_color_hex(CLR_CYAN), 0);
            lv_obj_align(ch_h, LV_ALIGN_CENTER, 0, pic_cy);
        }
        lv_obj_t *ch_v = uiw_obj(cam);
        if (ch_v) {
            lv_obj_set_size(ch_v, 1, 80);
            lv_obj_set_style_bg_opa(ch_v, LV_OPA_50, 0);
            lv_obj_set_style_bg_color(ch_v, lv_color_hex(CLR_CYAN), 0);
            lv_obj_align(ch_v, LV_ALIGN_CENTER, 0, pic_cy);
        }
    }

    /* ---- 扫描线（全局引用，动画移动） ---- */
    g_scanline = uiw_obj(cam);
    if (g_scanline) {
        lv_obj_set_size(g_scanline, LEFT_W, 1);
        lv_obj_set_style_bg_opa(g_scanline, LV_OPA_30, 0);
        lv_obj_set_style_bg_color(g_scanline, lv_color_hex(CLR_CYAN), 0);
        lv_obj_align(g_scanline, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_add_flag(g_scanline, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    /* ---- 预览文字（居中于画面区） ---- */
    g_cam_text_lbl = uiw_label(cam, "前门画面",
                                CLR_TEXT_DIM);
    if (g_cam_text_lbl) {
        lv_obj_set_style_text_font(g_cam_text_lbl, &lv_font_SHSC_16, 0);
        /* 下移至两行头部下方的画面区中心 */
        lv_obj_align(g_cam_text_lbl, LV_ALIGN_CENTER, 0,
                     (CAM_PIC_Y + CAM_H) / 2 - CAM_H / 2 + 10);
        lv_label_set_long_mode(g_cam_text_lbl, LV_LABEL_LONG_DOT);
    }

    /* ---- 抓拍按钮（右下角） ---- */
    {
        lv_obj_t *cap = lv_button_create(cam);
        if (cap) {
            lv_obj_set_size(cap, 80, 28);
            lv_obj_align(cap, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
            lv_obj_set_style_bg_opa(cap, LV_OPA_10, 0);
            lv_obj_set_style_bg_color(cap, lv_color_hex(CLR_AMBER), 0);
            lv_obj_set_style_border_color(cap, lv_color_hex(CLR_AMBER), 0);
            lv_obj_set_style_border_width(cap, 1, 0);
            lv_obj_set_style_radius(cap, 0, 0);
            lv_obj_set_style_pad_all(cap, 0, 0);
            lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *cl = uiw_label(cap, "抓拍",
                                      CLR_AMBER);
            if (cl) {
                lv_obj_center(cl);
                lv_label_set_long_mode(cl, LV_LABEL_LONG_DOT);
            }
            lv_obj_add_event_cb(cap, _on_capture, LV_EVENT_CLICKED, NULL);
        }
    }
}

/* ================================================================
 * zones-grid  1×4 分区卡片（一行四卡，为画面留出更大空间）
 * ================================================================ */
static void _build_zones_grid(lv_obj_t *par)
{
    lv_obj_t *grid = uiw_obj(par);
    if (!grid) return;
    /* 固定高度 = 左列472 - 396(cam) - 6(gap) = 70 */
    lv_obj_set_size(grid, LEFT_W, (MAIN_H - 12) - CAM_H - GAP_SM);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, GAP_SM, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    int card_w = 153;  /* (630 - 3*6) / 4 = 153 */
    int card_h = 70;   /* 一行矮卡片 */

    for (int i = 0; i < ZONE_COUNT; i++) {
        lv_obj_t *card = uiw_obj(grid);
        if (!card) continue;
        lv_obj_set_size(card, card_w, card_h);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(CLR_CARD), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        g_zone_cards[i] = card;
        g_zone_states[i] = ZONE_ONLINE;

        /* 两行布局（卡片 153×70，去掉 ID 行后垂直居中） */
        /* 行1：圆点 + 名称 */
        g_zone_dots[i] = uiw_dot(card, CLR_GREEN, 7);
        if (g_zone_dots[i]) lv_obj_align(g_zone_dots[i], LV_ALIGN_TOP_LEFT, 12, 17);

        lv_obj_t *name_lbl = uiw_label(card, zone_info[i].name, CLR_TEXT_HI);
        if (name_lbl) {
            lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
            lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 26, 14);
        }

        /* 行2：状态（左对齐） */
        g_zone_status[i] = uiw_label(card, "已撤防",
                                      CLR_TEXT_LO);
        if (g_zone_status[i]) {
            lv_label_set_long_mode(g_zone_status[i], LV_LABEL_LONG_DOT);
            lv_obj_align(g_zone_status[i], LV_ALIGN_TOP_LEFT, 12, 40);
        }

        /* 点击事件 */
        lv_obj_add_event_cb(card, _on_zone_click, LV_EVENT_CLICKED,
                           (void *)(intptr_t)i);
    }
}

/* ================================================================
 * right-col  右侧事件时间轴
 * ================================================================ */
static void _build_right_col(lv_obj_t *par)
{
    lv_obj_t *col = uiw_obj(par);
    if (!col) return;
    lv_obj_set_size(col, RIGHT_W, MAIN_H);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(col, 0, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    /* events-header */
    lv_obj_t *hdr = uiw_obj(col);
    if (hdr) {
        lv_obj_set_size(hdr, RIGHT_W, 32);
        lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(hdr, lv_color_hex(CLR_CARD), 0);
        lv_obj_set_style_border_color(hdr, lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_border_width(hdr, 1, 0);
        lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(hdr, 12, 0);
        lv_obj_set_style_pad_right(hdr, 12, 0);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *evt_hdr_lbl = uiw_label(hdr, "事件时间轴", CLR_TEXT_LO);
        if (evt_hdr_lbl) lv_label_set_long_mode(evt_hdr_lbl, LV_LABEL_LONG_DOT);

        /* 计数徽章 */
        g_event_count_lbl = lv_label_create(hdr);
        if (g_event_count_lbl) {
            char cnt_buf[8];
            snprintf(cnt_buf, sizeof(cnt_buf), "%d 条", evt_count);
            lv_label_set_text(g_event_count_lbl, cnt_buf);
            lv_label_set_long_mode(g_event_count_lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(g_event_count_lbl, &lv_font_SHSC_16, 0);
            lv_obj_set_style_text_color(g_event_count_lbl, lv_color_hex(CLR_CYAN), 0);
            lv_obj_set_style_bg_opa(g_event_count_lbl, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(g_event_count_lbl, lv_color_hex(CLR_CYAN), 0);
            lv_obj_set_style_bg_opa(g_event_count_lbl, LV_OPA_20, 0);
            lv_obj_set_style_pad_left(g_event_count_lbl, 6, 0);
            lv_obj_set_style_pad_right(g_event_count_lbl, 6, 0);
            lv_obj_set_style_pad_top(g_event_count_lbl, 2, 0);
            lv_obj_set_style_pad_bottom(g_event_count_lbl, 2, 0);
        }
    }

    /* events-list（可滚动，固定高度 = 主区512 - 32(header) = 480） */
    g_event_list = uiw_obj(col);
    if (!g_event_list) return;
    lv_obj_set_size(g_event_list, RIGHT_W, MAIN_H - 32);
    lv_obj_set_style_bg_opa(g_event_list, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g_event_list, lv_color_hex(CLR_CARD), 0);
    lv_obj_set_style_border_color(g_event_list, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(g_event_list, 1, 0);
    lv_obj_set_scrollbar_mode(g_event_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(g_event_list, LV_DIR_VER);
    /* 注：LVGL v9.2.2 无 lv_obj_set_style_scrollbar_width/color API，已移除 */
    lv_obj_set_flex_flow(g_event_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(g_event_list, 4, 0);
    lv_obj_set_style_pad_bottom(g_event_list, 4, 0);

    /* 创建事件条目 */
    for (int i = 0; i < evt_count; i++) {
        const evt_t *e = &evt_tbl[i];
        _append_event_item(g_event_list, e->time, e->title, e->loc,
                           e->dot_clr, e->level);
    }
}

/* ================================================================
 * bottom-bar  52px
 * ================================================================ */
static void _build_bottom_bar(lv_obj_t *p)
{
    lv_obj_t *bar = uiw_obj(p);
    if (!bar) return;
    lv_obj_set_size(bar, SCR_W, BOTTOM_H);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_CARD), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(bar, 12, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 布防 (青色) */
    lv_obj_t *b1 = uiw_op_btn(bar, "布防",
                               CLR_CYAN, CLR_CYAN, CLR_CYAN, LV_OPA_20, 130);
    if (b1) {
        lv_obj_clear_flag(b1, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b1, _on_arm, LV_EVENT_CLICKED, NULL);
    }

    /* 撤防 (灰色) */
    lv_obj_t *b2 = uiw_op_btn(bar, "撤防",
                               CLR_BORDER_LT, CLR_BORDER_LT, CLR_TEXT_LO, LV_OPA_10, 130);
    if (b2) {
        lv_obj_clear_flag(b2, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b2, _on_disarm, LV_EVENT_CLICKED, NULL);
    }

    /* 消警 (琥珀) */
    lv_obj_t *b3 = uiw_op_btn(bar, "消警",
                               CLR_AMBER, CLR_AMBER, CLR_AMBER, LV_OPA_10, 130);
    if (b3) {
        lv_obj_clear_flag(b3, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b3, _on_silence, LV_EVENT_CLICKED, NULL);
    }

    /* 设置 (深色) */
    lv_obj_t *b4 = uiw_op_btn(bar, "设置",
                               CLR_BORDER, CLR_BORDER, CLR_TEXT_LO, LV_OPA_30, 130);
    if (b4) {
        lv_obj_clear_flag(b4, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b4, _on_settings, LV_EVENT_CLICKED, NULL);
    }

    /* 测试 (青绿，模块状态诊断) */
    lv_obj_t *b5 = uiw_op_btn(bar, "检测",
                               CLR_GREEN, CLR_GREEN, CLR_GREEN, LV_OPA_10, 130);
    if (b5) {
        lv_obj_clear_flag(b5, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b5, _on_test, LV_EVENT_CLICKED, NULL);
    }
}

/* ================================================================
 * 分区详情弹窗
 * ================================================================ */
static void _build_detail_modal(void)
{
    g_overlay_detail = uiw_modal_create(lv_screen_active(),
                                        "分区", 440);
    if (!g_overlay_detail) return;

    /* 内容超出屏幕时滚动，row 定宽 368 需弹窗宽 ≥ 368+32(padding) */
    lv_obj_t *dbox = lv_obj_get_child(g_overlay_detail, 0);
    if (dbox) {
        lv_obj_t *dbody = lv_obj_get_child(dbox, 1);
        if (dbody) lv_obj_set_style_max_height(dbody, 440, 0);
    }

    /* 从 modal 中取出 box 的第二个 child（body 区域） */
    lv_obj_t *box = lv_obj_get_child(g_overlay_detail, 0);
    if (box) {
        /* 标题标签：box 第一个 child(head) 的第一个 child(label) */
        lv_obj_t *head = lv_obj_get_child(box, 0);
        if (head) g_detail_title_lbl = lv_obj_get_child(head, 0);
        /* body 是 box 的第二个 child (index 1) */
        g_detail_body = lv_obj_get_child(box, 1);
    }
}

/* ================================================================
 * 设置弹窗回调：亮度/音量滑条 + 定时布防开关
 * ================================================================ */
static void _on_brightness_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    g_brightness = (int)lv_slider_get_value(sl);
    if (g_bright_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%d%%", g_brightness);
        lv_label_set_text(g_bright_lbl, b);
    }
    _backlight_set(g_brightness);   /* 画面亮度 ↔ LCD 背光 */
}

static void _on_volume_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    g_volume = (int)lv_slider_get_value(sl);
    if (g_vol_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%d%%", g_volume);
        lv_label_set_text(g_vol_lbl, b);
    }
}

/* ---- 识别模式（云端精判=粗判+上传 / 本地精判=粗判+NCNN，设置页切换联动徽章） ---- */
static void _dm_refresh(void);   /* 前向声明：定义于下方 */

static void _on_dm_sel(lv_event_t *e)
{
    g_detect_mode = (int)(intptr_t)lv_event_get_user_data(e);
    _dm_refresh();
}

static void _dm_refresh(void)
{
    for (int i = 0; i < 2; i++) {
        if (!g_dm_btns[i]) continue;
        int active = (i == g_detect_mode);
        lv_obj_set_style_bg_opa(g_dm_btns[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_t *l = lv_obj_get_child(g_dm_btns[i], 0);
        if (l) lv_obj_set_style_text_color(l, active ? lv_color_hex(0xFFFFFF) : lv_color_hex(CLR_CYAN), 0);
    }
    /* 摄像头区 mode-badge 联动 */
    if (g_mode_badge_lbl)
        lv_label_set_text(g_mode_badge_lbl,
                          g_detect_mode == DETECT_MODE_LOCAL ? "本地精判模式" : "云端精判模式");
}

/* ---- 预警设置（四位置 × 触发源 × 数据通道 × 阈值，写入 dev_bridge 实时生效） ---- */
static int  g_warn_zone_sel = 0;            /* 当前编辑的位置 0-3 */
static lv_obj_t *g_warn_src_btns[3] = {NULL, NULL, NULL};   /* OFF/AP3216C/ICM */
static lv_obj_t *g_warn_ch_btns[5] = {NULL, NULL, NULL, NULL, NULL}; /* 数据通道 */
static lv_obj_t *g_warn_cmp_btns[3] = {NULL, NULL, NULL};  /* GT/EQ/LT 比较方式 */
static lv_obj_t *g_warn_thr_lbl = NULL;     /* 阈值数值 */
static lv_obj_t *g_warn_thr_sl  = NULL;     /* 阈值滑条 */

/* 位置名（字库内字） */
static const char *g_zone_names[4] = { "前门", "后门", "窗户", "仓库" };

/* 触发源/通道按钮组高亮刷新 */
static void _warn_refresh_ui(void);

/* 选择安防位置 */
static void _on_warn_zone_sel(lv_event_t *e)
{
    g_warn_zone_sel = (int)(intptr_t)lv_event_get_user_data(e);
    _warn_refresh_ui();
}

/* 选择触发源 */
static void _on_warn_src_sel(lv_event_t *e)
{
    struct dev_config cfg;
    dev_bridge_get_config(&cfg);
    int src = (int)(intptr_t)lv_event_get_user_data(e);
    cfg.zones[g_warn_zone_sel].src = src;
    cfg.zones[g_warn_zone_sel].enabled = (src != WARN_SRC_NONE);
    if (src == WARN_SRC_NONE) {
        /* 关闭时清掉旧通道 */
        cfg.zones[g_warn_zone_sel].channel = 0;
    } else if (cfg.zones[g_warn_zone_sel].channel >= 4) {
        cfg.zones[g_warn_zone_sel].channel = 0;
    }
    dev_bridge_set_config(&cfg);
    _warn_refresh_ui();
}

/* 选择数据通道 */
static void _on_warn_ch_sel(lv_event_t *e)
{
    struct dev_config cfg;
    dev_bridge_get_config(&cfg);
    int ch = (int)(intptr_t)lv_event_get_user_data(e);
    cfg.zones[g_warn_zone_sel].channel = ch;
    dev_bridge_set_config(&cfg);
    _warn_refresh_ui();
}

/* 选择比较方式（GT=大于 EQ=等于 LT=小于） */
static void _on_warn_cmp_sel(lv_event_t *e)
{
    struct dev_config cfg;
    dev_bridge_get_config(&cfg);
    int cmp = (int)(intptr_t)lv_event_get_user_data(e);
    cfg.zones[g_warn_zone_sel].cmp = cmp;
    dev_bridge_set_config(&cfg);
    _warn_refresh_ui();
}

/* 阈值滑条 */
static void _on_warn_thr_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    struct dev_config cfg;
    dev_bridge_get_config(&cfg);
    cfg.zones[g_warn_zone_sel].threshold = (int)lv_slider_get_value(sl);
    dev_bridge_set_config(&cfg);
    if (g_warn_thr_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%d", cfg.zones[g_warn_zone_sel].threshold);
        lv_label_set_text(g_warn_thr_lbl, b);
    }
}

/* ---- 消警冷却时间（默认 60s） ---- */
static lv_obj_t *g_cool_lbl = NULL;

static void _on_cool_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    struct dev_config cfg;
    dev_bridge_get_config(&cfg);
    cfg.cooldown_sec = (int)lv_slider_get_value(sl);
    dev_bridge_set_config(&cfg);
    if (g_cool_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%ds", cfg.cooldown_sec);
        lv_label_set_text(g_cool_lbl, b);
    }
}

/* 根据当前位置配置刷新按钮高亮/通道可见性/阈值 */
static void _warn_refresh_ui(void)
{
    struct dev_config cfg;
    dev_bridge_get_config(&cfg);
    struct zone_warn_cfg *zc = &cfg.zones[g_warn_zone_sel];

    /* 触发源按钮高亮 */
    for (int i = 0; i < 3; i++) {
        if (!g_warn_src_btns[i]) continue;
        int active = (i == zc->src);
        lv_obj_set_style_bg_opa(g_warn_src_btns[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(g_warn_src_btns[i], active ? lv_color_hex(CLR_CYAN) : lv_color_hex(CLR_CYAN), 0);
        lv_obj_t *ch = lv_obj_get_child(g_warn_src_btns[i], 0);
        if (ch) lv_obj_set_style_text_color(ch, active ? lv_color_hex(0xFFFFFF) : lv_color_hex(CLR_CYAN), 0);
    }

    /* 数据通道按钮：文字与显隐随触发源联动
     *  AP3216C → PS/ALS/IR（3 个）；ICM → MAG/AX/AY/AZ/TEMP（5 个）；OFF → 全隐藏 */
    static const char *ap_ch_names[5]  = { "PS", "ALS", "IR", "", "" };
    static const char *icm_ch_names[5] = { "MAG", "AX", "AY", "AZ", "TEMP" };
    const char *const *names = (zc->src == WARN_SRC_ICM) ? icm_ch_names :
                               (zc->src == WARN_SRC_AP3216C) ? ap_ch_names : NULL;
    for (int i = 0; i < 5; i++) {
        if (!g_warn_ch_btns[i]) continue;
        if (names && names[i][0]) {
            lv_obj_clear_flag(g_warn_ch_btns[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *l = lv_obj_get_child(g_warn_ch_btns[i], 0);
            if (l) lv_label_set_text(l, names[i]);
            int active = (i == zc->channel);
            lv_obj_set_style_bg_opa(g_warn_ch_btns[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            if (l) lv_obj_set_style_text_color(l, active ? lv_color_hex(0xFFFFFF) : lv_color_hex(CLR_CYAN), 0);
        } else {
            lv_obj_add_flag(g_warn_ch_btns[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 比较方式按钮（GT/EQ/LT）高亮 */
    for (int i = 0; i < 3; i++) {
        if (!g_warn_cmp_btns[i]) continue;
        int active = (i == zc->cmp);
        lv_obj_set_style_bg_opa(g_warn_cmp_btns[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_t *l = lv_obj_get_child(g_warn_cmp_btns[i], 0);
        if (l) lv_obj_set_style_text_color(l, active ? lv_color_hex(0xFFFFFF) : lv_color_hex(CLR_CYAN), 0);
    }

    /* 阈值滑条 + 数值（OFF 时禁用；AP 源 1-200；ICM 温度 200-600=20-60℃，其余 1-500） */
    if (g_warn_thr_sl) {
        if (zc->src == WARN_SRC_NONE) {
            lv_obj_add_state(g_warn_thr_sl, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(g_warn_thr_sl, LV_STATE_DISABLED);
            int max = 500;
            if (zc->src == WARN_SRC_AP3216C) max = 200;
            else if (zc->channel == IMU_DATA_TEMP) max = 600;
            lv_slider_set_range(g_warn_thr_sl, 1, max);
            lv_slider_set_value(g_warn_thr_sl, zc->threshold, LV_ANIM_OFF);
        }
    }
    if (g_warn_thr_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%d", zc->threshold);
        lv_label_set_text(g_warn_thr_lbl, b);
    }
}

/* ---- LCD 背光控制（pwm-backlight sysfs） ---- */
static int g_backlight_max = 100;

static int _backlight_init(void)
{
    /* 读 max_brightness */
    int fd = open("/sys/class/backlight/backlight/max_brightness", O_RDONLY);
    if (fd >= 0) {
        char buf[16] = {0};
        if (read(fd, buf, sizeof(buf) - 1) > 0)
            g_backlight_max = atoi(buf);
        close(fd);
    }
    return g_backlight_max > 0 ? g_backlight_max : 100;
}

/* 亮度百分比 → 背光 sysfs（0-100%） */
static void _backlight_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int fd = open("/sys/class/backlight/backlight/brightness", O_WRONLY);
    if (fd < 0) return;
    if (lseek(fd, 0, SEEK_SET) >= 0) {
        char buf[16];
        int val = percent * _backlight_init() / 100;
        snprintf(buf, sizeof(buf), "%d", val);
        if (write(fd, buf, strlen(buf)) < 0) perror("[UI] backlight write");
    }
    close(fd);
}

/* 读取当前背光百分比（初始化滑条用） */
static int _backlight_get(void)
{
    int fd = open("/sys/class/backlight/backlight/brightness", O_RDONLY);
    if (fd < 0) return 80;
    char buf[16] = {0};
    if (read(fd, buf, sizeof(buf) - 1) <= 0) { close(fd); return 80; }
    close(fd);
    int v = atoi(buf);
    int max = _backlight_init();
    return (max > 0) ? (v * 100 / max) : 80;
}

/* ---- 定时布防时间段设置 ---- */
static lv_obj_t *g_sched_start_lbl = NULL;   /* 开始时间显示 */
static lv_obj_t *g_sched_end_lbl   = NULL;   /* 结束时间显示 */

static void _on_sched_start_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    g_sched_start_h = (int)lv_slider_get_value(sl);
    if (g_sched_start_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%02d:00", g_sched_start_h);
        lv_label_set_text(g_sched_start_lbl, b);
    }
}

static void _on_sched_end_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    g_sched_end_h = (int)lv_slider_get_value(sl);
    if (g_sched_end_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%02d:00", g_sched_end_h);
        lv_label_set_text(g_sched_end_lbl, b);
    }
}

static void _on_sched_toggle(lv_event_t *e)
{
    (void)e;
    g_schedule_on = !g_schedule_on;
    if (g_schedule_on) {
        if (g_sched_toggle)
            lv_obj_set_style_bg_color(g_sched_toggle, lv_color_hex(CLR_CYAN), 0);
        if (g_sched_knob) lv_obj_align(g_sched_knob, LV_ALIGN_RIGHT_MID, -2, 0);
        if (g_sched_hint) {
            char b[32];
            snprintf(b, sizeof(b), "定时布防 %02d:00-%02d:00 已设",
                     g_sched_start_h, g_sched_end_h);
            lv_label_set_text(g_sched_hint, b);
            lv_obj_clear_flag(g_sched_hint, LV_OBJ_FLAG_HIDDEN);
        }
        /* 启用记录事件 */
        _add_event("定时布防已设",
                   "系统", CLR_CYAN, "");
    } else {
        if (g_sched_toggle)
            lv_obj_set_style_bg_color(g_sched_toggle, lv_color_hex(CLR_BORDER), 0);
        if (g_sched_knob) lv_obj_align(g_sched_knob, LV_ALIGN_LEFT_MID, 2, 0);
        if (g_sched_hint) lv_obj_add_flag(g_sched_hint, LV_OBJ_FLAG_HIDDEN);
        _add_event("定时布防已关",
                   "系统", CLR_CYAN, "");
    }
}

/* ================================================================
 * 设置弹窗
 * ================================================================ */
static void _build_settings_modal(void)
{
    g_overlay_settings = uiw_modal_create(lv_screen_active(),
                                           "系统设置", 440);
    if (!g_overlay_settings) return;

    lv_obj_t *box = lv_obj_get_child(g_overlay_settings, 0);
    if (!box) return;

    lv_obj_t *body = lv_obj_get_child(box, 1);
    if (!body) return;

    /* 固定弹窗大小：box 高度固定 500（不随内容变化），body 内部滚动 */
    lv_obj_set_height(box, 500);
    lv_obj_set_style_max_height(body, 452, 0);

    /* --- 显示设置 --- */
    uiw_label(body, "画面设置", CLR_TEXT_DIM);
    {
        lv_obj_t *row = uiw_obj(body);
        if (row) {
            lv_obj_set_size(row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(row, 6, 0);
            lv_obj_set_style_pad_bottom(row, 6, 0);
            uiw_label(row, "亮度", CLR_TEXT_LO);
            g_bright_lbl = uiw_label_font(row, "80%", CLR_CYAN, &lv_font_SHSC_16);
        }
        /* 亮度滑条（HTML input[type=range]）— 同步 LCD 背光 */
        g_brightness = _backlight_get();   /* 读当前背光百分比初始化 */
        if (g_bright_lbl) {
            char b[8];
            snprintf(b, sizeof(b), "%d%%", g_brightness);
            lv_label_set_text(g_bright_lbl, b);
        }
        lv_obj_t *sl = lv_slider_create(body);
        if (sl) {
            lv_obj_set_width(sl, 368);
            lv_obj_set_height(sl, 20);
            lv_slider_set_range(sl, 0, 100);
            lv_slider_set_value(sl, g_brightness, LV_ANIM_OFF);
            /* 轨道 */
            lv_obj_set_style_bg_color(sl, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
            /* 已填充部分 */
            lv_obj_set_style_bg_color(sl, lv_color_hex(CLR_CYAN), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
            /* 滑块圆钮 */
            lv_obj_set_style_bg_color(sl, lv_color_hex(CLR_CYAN), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_shadow_color(sl, lv_color_hex(CLR_CYAN), LV_PART_KNOB);
            lv_obj_set_style_shadow_width(sl, 6, LV_PART_KNOB);
            lv_obj_set_style_shadow_opa(sl, LV_OPA_40, LV_PART_KNOB);
            lv_obj_add_event_cb(sl, _on_brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }
    }

    /* --- 音频设置 --- */
    uiw_label(body, "音量设置", CLR_TEXT_DIM);
    {
        lv_obj_t *row = uiw_obj(body);
        if (row) {
            lv_obj_set_size(row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(row, 6, 0);
            lv_obj_set_style_pad_bottom(row, 6, 0);
            uiw_label(row, "音量", CLR_TEXT_LO);
            g_vol_lbl = uiw_label_font(row, "60%", CLR_CYAN, &lv_font_SHSC_16);
        }
        /* 音量滑条 */
        lv_obj_t *sl = lv_slider_create(body);
        if (sl) {
            lv_obj_set_width(sl, 368);
            lv_obj_set_height(sl, 20);
            lv_slider_set_range(sl, 0, 100);
            lv_slider_set_value(sl, g_volume, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(sl, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(sl, lv_color_hex(CLR_CYAN), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(sl, lv_color_hex(CLR_CYAN), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_shadow_color(sl, lv_color_hex(CLR_CYAN), LV_PART_KNOB);
            lv_obj_set_style_shadow_width(sl, 6, LV_PART_KNOB);
            lv_obj_set_style_shadow_opa(sl, LV_OPA_40, LV_PART_KNOB);
            lv_obj_add_event_cb(sl, _on_volume_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }
    }

    /* --- 网络状态 --- */
    uiw_label(body, "网络状态", CLR_TEXT_DIM);
    {
        const char *net_rows[][2] = {
            {"网络", "在线"},
            {"IP", "192.168.1.10"},
            {"MQTT 主机", "broker.local:1883"},
        };
        for (int r = 0; r < 3; r++) {
            lv_obj_t *row = uiw_obj(body);
            if (!row) continue;
            lv_obj_set_size(row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(row, 6, 0);
            lv_obj_set_style_pad_bottom(row, 6, 0);
            uiw_label(row, net_rows[r][0], CLR_TEXT_LO);
            uiw_label_font(row, net_rows[r][1], CLR_CYAN, &lv_font_SHSC_16);
        }
    }

    /* --- 告警分级 --- */
    uiw_label(body, "警级", CLR_TEXT_DIM);
    {
        /* HTML 原型完整描述：高=入侵检测、异常闯入 / 中=信号弱、设备离线 / 低=温度偏高、存储不足 */
        struct { const char *name; uint32_t clr; const char *desc; } levels[] = {
            {"高", CLR_RED,   "入侵检测"},
            {"中", CLR_AMBER, "部件离线"},
            {"L", CLR_CYAN,  "存量"},
        };
        for (int r = 0; r < 3; r++) {
            lv_obj_t *row = uiw_obj(body);
            if (!row) continue;
            lv_obj_set_size(row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(row, 8, 0);
            lv_obj_set_style_pad_top(row, 6, 0);
            lv_obj_set_style_pad_bottom(row, 6, 0);
            uiw_dot(row, levels[r].clr, 8);
            uiw_label(row, levels[r].name, CLR_TEXT_HI);
            uiw_label(row, levels[r].desc, CLR_TEXT_DIM);
        }
    }

    /* --- 识别设置（双模式选择：云端精判=粗判+上传 / 本地精判=粗判+NCNN） --- */
    uiw_label(body, "识别设置", CLR_TEXT_DIM);
    {
        lv_obj_t *dm_row = uiw_obj(body);
        if (dm_row) {
            lv_obj_set_size(dm_row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(dm_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(dm_row, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(dm_row, 8, 0);
            lv_obj_set_style_pad_bottom(dm_row, 4, 0);
            uiw_label(dm_row, "MODE", CLR_TEXT_DIM);
            const char *dm_names[2] = { "云端精判", "本地精判" };
            for (int i = 0; i < 2; i++) {
                lv_obj_t *b = lv_button_create(dm_row);
                if (!b) continue;
                lv_obj_set_size(b, LV_SIZE_CONTENT, 28);
                lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_color(b, lv_color_hex(CLR_BORDER), 0);
                lv_obj_set_style_border_width(b, 1, 0);
                lv_obj_set_style_radius(b, 0, 0);
                lv_obj_set_style_pad_left(b, 10, 0);
                lv_obj_set_style_pad_right(b, 10, 0);
                lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_t *l = uiw_label(b, dm_names[i], CLR_CYAN);
                if (l) lv_obj_center(l);
                lv_obj_add_event_cb(b, _on_dm_sel, LV_EVENT_CLICKED,
                                   (void *)(intptr_t)i);
                g_dm_btns[i] = b;
            }
        }
        _dm_refresh();   /* 初始高亮 + 徽章联动 */
    }

    /* --- 预警设置（四位置 × 触发源 × 数据通道 × 阈值） --- */
    uiw_label(body, "预警设置", CLR_TEXT_DIM);
    {
        /* 位置标签行 */
        lv_obj_t *zone_row = uiw_obj(body);
        if (zone_row) {
            lv_obj_set_size(zone_row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(zone_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(zone_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(zone_row, 4, 0);
            lv_obj_set_style_pad_bottom(zone_row, 8, 0);
            for (int i = 0; i < 4; i++) {
                lv_obj_t *b = lv_button_create(zone_row);
                if (!b) continue;
                lv_obj_set_size(b, 84, 28);
                lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_color(b, lv_color_hex(CLR_BORDER), 0);
                lv_obj_set_style_border_width(b, 1, 0);
                lv_obj_set_style_radius(b, 0, 0);
                lv_obj_set_style_pad_all(b, 0, 0);
                lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_t *l = uiw_label(b, g_zone_names[i], CLR_TEXT_LO);
                if (l) lv_obj_center(l);
                lv_obj_add_event_cb(b, _on_warn_zone_sel, LV_EVENT_CLICKED,
                                   (void *)(intptr_t)i);
            }
        }

        /* 触发源行：OFF / AP3216C / ICM */
        lv_obj_t *src_row = uiw_obj(body);
        if (src_row) {
            lv_obj_set_size(src_row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(src_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(src_row, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(src_row, 8, 0);
            lv_obj_set_style_pad_bottom(src_row, 4, 0);
            uiw_label(src_row, "SRC", CLR_TEXT_DIM);
            const char *src_names[3] = { "OFF", "AP3216C", "ICM" };
            for (int i = 0; i < 3; i++) {
                lv_obj_t *b = lv_button_create(src_row);
                if (!b) continue;
                lv_obj_set_size(b, LV_SIZE_CONTENT, 28);
                lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_color(b, lv_color_hex(CLR_BORDER), 0);
                lv_obj_set_style_border_width(b, 1, 0);
                lv_obj_set_style_radius(b, 0, 0);
                lv_obj_set_style_pad_left(b, 10, 0);
                lv_obj_set_style_pad_right(b, 10, 0);
                lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_t *l = uiw_label(b, src_names[i], CLR_CYAN);
                if (l) lv_obj_center(l);
                lv_obj_add_event_cb(b, _on_warn_src_sel, LV_EVENT_CLICKED,
                                   (void *)(intptr_t)i);
                g_warn_src_btns[i] = b;
            }
        }

        /* 数据通道行：PS/ALS/IR 或 MAG/AX/AY/AZ（随源变化） */
        lv_obj_t *ch_row = uiw_obj(body);
        if (ch_row) {
            lv_obj_set_size(ch_row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(ch_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(ch_row, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(ch_row, 8, 0);
            lv_obj_set_style_pad_bottom(ch_row, 4, 0);
            uiw_label(ch_row, "DATA", CLR_TEXT_DIM);
            /* 按钮名按源复用：AP3216C 用 PS/ALS/IR；ICM 用 MAG/AX/AY/AZ/TEMP */
            const char *ch_names[5] = { "PS", "ALS", "IR", "MAG", "TEMP" };
            for (int i = 0; i < 5; i++) {
                lv_obj_t *b = lv_button_create(ch_row);
                if (!b) continue;
                lv_obj_set_size(b, LV_SIZE_CONTENT, 28);
                lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_color(b, lv_color_hex(CLR_BORDER), 0);
                lv_obj_set_style_border_width(b, 1, 0);
                lv_obj_set_style_radius(b, 0, 0);
                lv_obj_set_style_pad_left(b, 10, 0);
                lv_obj_set_style_pad_right(b, 10, 0);
                lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_t *l = uiw_label(b, ch_names[i], CLR_CYAN);
                if (l) lv_obj_center(l);
                lv_obj_add_event_cb(b, _on_warn_ch_sel, LV_EVENT_CLICKED,
                                   (void *)(intptr_t)i);
                g_warn_ch_btns[i] = b;
            }
        }

        /* 比较方式行：GT(大于) / EQ(等于) / LT(小于) */
        lv_obj_t *cmp_row = uiw_obj(body);
        if (cmp_row) {
            lv_obj_set_size(cmp_row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(cmp_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(cmp_row, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(cmp_row, 8, 0);
            lv_obj_set_style_pad_bottom(cmp_row, 4, 0);
            uiw_label(cmp_row, "CMP", CLR_TEXT_DIM);
            const char *cmp_names[3] = { "GT", "EQ", "LT" };
            for (int i = 0; i < 3; i++) {
                lv_obj_t *b = lv_button_create(cmp_row);
                if (!b) continue;
                lv_obj_set_size(b, LV_SIZE_CONTENT, 28);
                lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_color(b, lv_color_hex(CLR_BORDER), 0);
                lv_obj_set_style_border_width(b, 1, 0);
                lv_obj_set_style_radius(b, 0, 0);
                lv_obj_set_style_pad_left(b, 10, 0);
                lv_obj_set_style_pad_right(b, 10, 0);
                lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_t *l = uiw_label(b, cmp_names[i], CLR_CYAN);
                if (l) lv_obj_center(l);
                lv_obj_add_event_cb(b, _on_warn_cmp_sel, LV_EVENT_CLICKED,
                                   (void *)(intptr_t)i);
                g_warn_cmp_btns[i] = b;
            }
        }

        /* 阈值行 + 滑条 */
        lv_obj_t *thr_row = uiw_obj(body);
        if (thr_row) {
            lv_obj_set_size(thr_row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(thr_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(thr_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(thr_row, 6, 0);
            lv_obj_set_style_pad_bottom(thr_row, 4, 0);
            uiw_label(thr_row, "LIMIT", CLR_TEXT_DIM);
            g_warn_thr_lbl = uiw_label_font(thr_row, "30", CLR_CYAN, &lv_font_SHSC_16);
        }
        g_warn_thr_sl = lv_slider_create(body);
        if (g_warn_thr_sl) {
            lv_obj_set_width(g_warn_thr_sl, 368);
            lv_obj_set_height(g_warn_thr_sl, 20);
            lv_slider_set_range(g_warn_thr_sl, 1, 200);
            lv_obj_set_style_bg_color(g_warn_thr_sl, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(g_warn_thr_sl, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(g_warn_thr_sl, lv_color_hex(CLR_CYAN), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(g_warn_thr_sl, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(g_warn_thr_sl, lv_color_hex(CLR_CYAN), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(g_warn_thr_sl, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(g_warn_thr_sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_shadow_color(g_warn_thr_sl, lv_color_hex(CLR_CYAN), LV_PART_KNOB);
            lv_obj_set_style_shadow_width(g_warn_thr_sl, 6, LV_PART_KNOB);
            lv_obj_set_style_shadow_opa(g_warn_thr_sl, LV_OPA_40, LV_PART_KNOB);
            lv_obj_add_event_cb(g_warn_thr_sl, _on_warn_thr_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }

        _warn_refresh_ui();   /* 初始高亮（位置0） */

        /* 消警冷却时间行 + 滑条 */
        struct dev_config cfg_cd;
        dev_bridge_get_config(&cfg_cd);
        lv_obj_t *cd_row = uiw_obj(body);
        if (cd_row) {
            lv_obj_set_size(cd_row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(cd_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(cd_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(cd_row, 6, 0);
            lv_obj_set_style_pad_bottom(cd_row, 4, 0);
            uiw_label(cd_row, "COOLDOWN", CLR_TEXT_DIM);
            g_cool_lbl = uiw_label_font(cd_row, "60s", CLR_CYAN, &lv_font_SHSC_16);
        }
        lv_obj_t *cd_sl = lv_slider_create(body);
        if (cd_sl) {
            lv_obj_set_width(cd_sl, 368);
            lv_obj_set_height(cd_sl, 20);
            lv_slider_set_range(cd_sl, 0, 300);   /* 0-300 秒 */
            lv_slider_set_value(cd_sl, cfg_cd.cooldown_sec, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(cd_sl, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(cd_sl, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(cd_sl, lv_color_hex(CLR_CYAN), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(cd_sl, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(cd_sl, lv_color_hex(CLR_CYAN), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(cd_sl, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(cd_sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_shadow_color(cd_sl, lv_color_hex(CLR_CYAN), LV_PART_KNOB);
            lv_obj_set_style_shadow_width(cd_sl, 6, LV_PART_KNOB);
            lv_obj_set_style_shadow_opa(cd_sl, LV_OPA_40, LV_PART_KNOB);
            lv_obj_add_event_cb(cd_sl, _on_cool_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }
    }

    /* --- 定时布防（时间段可设 + 开关） --- */
    uiw_label(body, "定时布防", CLR_TEXT_DIM);
    {
        lv_obj_t *row = uiw_obj(body);
        if (row) {
            lv_obj_set_size(row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(row, 6, 0);
            lv_obj_set_style_pad_bottom(row, 6, 0);
            uiw_label(row, "定时布防", CLR_TEXT_LO);

            /* 可点击开关（HTML .toggle-switch），初始 OFF：灰底 + 滑块在左 */
            g_sched_toggle = uiw_obj(row);
            if (g_sched_toggle) {
                lv_obj_set_size(g_sched_toggle, 40, 20);
                lv_obj_set_style_bg_color(g_sched_toggle, lv_color_hex(CLR_BORDER), 0);
                lv_obj_set_style_bg_opa(g_sched_toggle, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(g_sched_toggle, 10, 0);
                lv_obj_set_style_border_width(g_sched_toggle, 0, 0);
                lv_obj_add_flag(g_sched_toggle, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(g_sched_toggle, _on_sched_toggle, LV_EVENT_CLICKED, NULL);

                /* 滑块 */
                g_sched_knob = uiw_obj(g_sched_toggle);
                if (g_sched_knob) {
                    lv_obj_set_size(g_sched_knob, 16, 16);
                    lv_obj_set_style_bg_color(g_sched_knob, lv_color_hex(0xFFFFFF), 0);
                    lv_obj_set_style_bg_opa(g_sched_knob, LV_OPA_COVER, 0);
                    lv_obj_set_style_radius(g_sched_knob, LV_RADIUS_CIRCLE, 0);
                    lv_obj_set_style_border_width(g_sched_knob, 0, 0);
                    lv_obj_align(g_sched_knob, LV_ALIGN_LEFT_MID, 2, 0);
                }
            }
        }

        /* 开始时间行 */
        lv_obj_t *srow = uiw_obj(body);
        if (srow) {
            lv_obj_set_size(srow, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(srow, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(srow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(srow, 6, 0);
            lv_obj_set_style_pad_bottom(srow, 4, 0);
            uiw_label(srow, "START", CLR_TEXT_DIM);
            g_sched_start_lbl = uiw_label_font(srow, "22:00", CLR_AMBER, &lv_font_SHSC_16);
        }
        lv_obj_t *ssl = lv_slider_create(body);
        if (ssl) {
            lv_obj_set_width(ssl, 368);
            lv_obj_set_height(ssl, 20);
            lv_slider_set_range(ssl, 0, 23);
            lv_slider_set_value(ssl, g_sched_start_h, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(ssl, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(ssl, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(ssl, lv_color_hex(CLR_AMBER), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(ssl, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(ssl, lv_color_hex(CLR_AMBER), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(ssl, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(ssl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_shadow_color(ssl, lv_color_hex(CLR_AMBER), LV_PART_KNOB);
            lv_obj_set_style_shadow_width(ssl, 6, LV_PART_KNOB);
            lv_obj_set_style_shadow_opa(ssl, LV_OPA_40, LV_PART_KNOB);
            lv_obj_add_event_cb(ssl, _on_sched_start_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }

        /* 结束时间行 */
        lv_obj_t *erow = uiw_obj(body);
        if (erow) {
            lv_obj_set_size(erow, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(erow, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(erow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(erow, 6, 0);
            lv_obj_set_style_pad_bottom(erow, 4, 0);
            uiw_label(erow, "END", CLR_TEXT_DIM);
            g_sched_end_lbl = uiw_label_font(erow, "06:00", CLR_AMBER, &lv_font_SHSC_16);
        }
        lv_obj_t *esl = lv_slider_create(body);
        if (esl) {
            lv_obj_set_width(esl, 368);
            lv_obj_set_height(esl, 20);
            lv_slider_set_range(esl, 0, 23);
            lv_slider_set_value(esl, g_sched_end_h, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(esl, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(esl, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(esl, lv_color_hex(CLR_AMBER), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(esl, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(esl, lv_color_hex(CLR_AMBER), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(esl, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(esl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_shadow_color(esl, lv_color_hex(CLR_AMBER), LV_PART_KNOB);
            lv_obj_set_style_shadow_width(esl, 6, LV_PART_KNOB);
            lv_obj_set_style_shadow_opa(esl, LV_OPA_40, LV_PART_KNOB);
            lv_obj_add_event_cb(esl, _on_sched_end_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }
    }

    /* 提示文字（默认隐藏，开启时显示） */
    g_sched_hint = uiw_label(body, "定时布防 "
                              "22:00-06:00 "
                              "已设",
                              CLR_AMBER);
    if (g_sched_hint) lv_obj_add_flag(g_sched_hint, LV_OBJ_FLAG_HIDDEN);

    /* --- 关于 --- */
    uiw_label(body, "系统", CLR_TEXT_DIM);
    {
        const char *about[][2] = {
            {"主机", "i.MX6ULL 安防网关"},
            {"系统", "v2.3.1"},
            {"系统", "Linux 5.4.0"},
            {"存量", "14.2GB / 32GB"},
        };
        for (int r = 0; r < 4; r++) {
            lv_obj_t *row = uiw_obj(body);
            if (!row) continue;
            lv_obj_set_size(row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(row, 6, 0);
            lv_obj_set_style_pad_bottom(row, 6, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(CLR_BORDER), 0);
            lv_obj_set_style_border_width(row, 1, 0);
            uiw_label(row, about[r][0], CLR_TEXT_LO);
            uiw_label_font(row, about[r][1], CLR_CYAN, &lv_font_SHSC_16);
        }
    }
}

/* ================================================================
 * 回调：事件"查看"按钮 — 打开相册弹窗
 * ================================================================ */
static void _on_event_view(lv_event_t *e)
{
    const char *title = (const char *)lv_event_get_user_data(e);
    if (!g_album_overlay) return;

    /* 更新相册标题 */
    if (g_album_title_lbl) {
        char buf[64];
        snprintf(buf, sizeof(buf), "抓拍相册 · %s",
                 title ? title : "");
        lv_label_set_text(g_album_title_lbl, buf);
    }

    lv_obj_clear_flag(g_album_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* ================================================================
 * 相册弹窗（HTML #album-modal）：3×2 真实抓拍缩略图
 * 数据来自 g_snaps（抓拍按钮/预警触发经 snapshot_capture 写入）
 * TF 持久化/容量/恢复由 storage_mgr 组件提供
 * ================================================================ */

/* 抓拍：拷当前摄像头帧 → 全图 + 缩略图降采样 + 元数据 → TF 持久化 → 相册重绘
 * zone_idx 用于 TF 文件名编码（重启恢复时还原区域名） */
static void _rebuild_album_grid(void);
static void snapshot_capture_idx(int zone_idx, const char *zone, const char *level)
{
    /* 1. 环形插入到槽位 0（最新在前），超出 SNAP_MAX 覆盖最旧 */
    if (g_snap_count < SNAP_MAX) g_snap_count++;
    for (int i = g_snap_count - 1; i > 0; i--)
        g_snaps[i] = g_snaps[i - 1];

    /* 2. 拷贝当前摄像头帧（未工作则填黑，保留元数据可追溯） */
    if (cam_feed_copy_frame((uint8_t *)g_snaps[0].pix) != 0)
        memset(g_snaps[0].pix, 0, sizeof(g_snaps[0].pix));

    /* 3. 缩略图降采样（最近邻：630×340 → 240×135） */
    for (int ty = 0; ty < THUMB_H; ty++) {
        int sy = ty * SNAP_H / THUMB_H;
        const uint16_t *sline = g_snaps[0].pix + sy * SNAP_W;
        uint16_t *dline = g_snaps[0].thumb + ty * THUMB_W;
        for (int tx = 0; tx < THUMB_W; tx++) {
            dline[tx] = sline[tx * SNAP_W / THUMB_W];
        }
    }

    /* 4. 元数据 + TF 持久化（BMP 存档；无 TF 卡跳过） */
    _get_time_str(g_snaps[0].time, sizeof(g_snaps[0].time));
    snprintf(g_snaps[0].zone,  sizeof(g_snaps[0].zone),  "%s", zone ? zone : "手动");
    snprintf(g_snaps[0].level, sizeof(g_snaps[0].level), "%s", level ? level : "");
    {
        /* 级别串转 TF 文件名编码：high→hi medium→md low→lo */
        const char *cmp_lv = "none";
        if (strcmp(g_snaps[0].level, "high") == 0)   cmp_lv = "hi";
        else if (strcmp(g_snaps[0].level, "medium") == 0) cmp_lv = "md";
        else if (strcmp(g_snaps[0].level, "low") == 0)    cmp_lv = "lo";
        storage_snap_save(g_snaps[0].pix, SNAP_W, SNAP_H,
                          zone_idx, cmp_lv, g_snaps[0].time);
    }

    /* 5. 相册打开则重绘 */
    if (g_album_body)
        _rebuild_album_grid();
}

static void snapshot_capture(const char *zone, const char *level)
{
    /* 手动/无索引抓拍：区域名反查分区索引（供 TF 文件名编码） */
    int idx = 0;
    for (int i = 0; i < ZONE_COUNT; i++) {
        if (strcmp(zone_info[i].name, zone) == 0) { idx = i; break; }
    }
    snapshot_capture_idx(idx, zone, level);
}

/* 级别 → 颜色/文字（与事件徽章一致） */
static void _album_level_style(const char *level, uint32_t *clr, const char **txt)
{
    if (level && strcmp(level, "high") == 0) {
        *clr = CLR_RED;   *txt = "高";
    } else if (level && strcmp(level, "medium") == 0) {
        *clr = CLR_AMBER; *txt = "中";
    } else if (level && strcmp(level, "low") == 0) {
        *clr = CLR_CYAN;  *txt = "L";
    } else {
        *clr = CLR_CYAN;  *txt = "";
    }
}

/* 缩略图点击 → 打开查看弹窗 */
static void _on_thumb_click(lv_event_t *e)
{
    _open_viewer((int)(intptr_t)lv_event_get_user_data(e));
}

/* 重绘缩略图网格（数据变化后调用）：真实缩略图 + 左上角区域/级别 + 右下角时间 */
static void _rebuild_album_grid(void)
{
    if (!g_album_body) return;
    lv_obj_clean(g_album_body);

    const int cols = 3;
    int rows = (g_snap_count + cols - 1) / cols;
    if (rows < 1) rows = 1;

    for (int row = 0; row < rows; row++) {
        lv_obj_t *row_obj = uiw_obj(g_album_body);
        if (!row_obj) continue;
        lv_obj_set_size(row_obj, 744, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row_obj, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row_obj, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row_obj, 12, 0);
        lv_obj_set_style_pad_bottom(row_obj, 12, 0);

        for (int col = 0; col < cols; col++) {
            int idx = row * cols + col;
            if (idx >= g_snap_count) break;

            lv_obj_t *thumb = uiw_obj(row_obj);
            if (!thumb) continue;
            lv_obj_set_size(thumb, THUMB_W, THUMB_H);
            lv_obj_set_style_bg_color(thumb, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(thumb, lv_color_hex(CLR_BORDER), 0);
            lv_obj_set_style_border_width(thumb, 1, 0);
            lv_obj_clear_flag(thumb, LV_OBJ_FLAG_SCROLLABLE);

            /* 真实缩略图画布 */
            lv_obj_t *cv = lv_canvas_create(thumb);
            if (cv) {
                lv_canvas_set_buffer(cv, g_snaps[idx].thumb,
                                     THUMB_W, THUMB_H, LV_COLOR_FORMAT_RGB565);
                lv_obj_clear_flag(cv, LV_OBJ_FLAG_SCROLLABLE);
            }

            uint32_t lvl_clr;
            const char *lvl_txt;
            _album_level_style(g_snaps[idx].level, &lvl_clr, &lvl_txt);

            /* 左上角：抓拍区域 + 预警级别 */
            lv_obj_t *tag = uiw_obj(thumb);
            if (tag) {
                lv_obj_align(tag, LV_ALIGN_TOP_LEFT, 4, 4);
                lv_obj_set_size(tag, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                lv_obj_set_style_bg_color(tag, lv_color_hex(0x000000), 0);
                lv_obj_set_style_bg_opa(tag, LV_OPA_50, 0);
                lv_obj_set_style_pad_left(tag, 4, 0);
                lv_obj_set_style_pad_right(tag, 4, 0);
                lv_obj_set_style_pad_top(tag, 1, 0);
                lv_obj_set_style_pad_bottom(tag, 1, 0);
                char tag_buf[24];
                if (lvl_txt[0])
                    snprintf(tag_buf, sizeof(tag_buf), "%s %s",
                             g_snaps[idx].zone, lvl_txt);
                else
                    snprintf(tag_buf, sizeof(tag_buf), "%s", g_snaps[idx].zone);
                uiw_label(tag, tag_buf, CLR_TEXT_HI);
            }

            /* 右下角：抓拍时间（小字，黑底半透明） */
            lv_obj_t *tl = uiw_obj(thumb);
            if (tl) {
                lv_obj_align(tl, LV_ALIGN_BOTTOM_RIGHT, -4, -3);
                lv_obj_set_size(tl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                lv_obj_set_style_bg_color(tl, lv_color_hex(0x000000), 0);
                lv_obj_set_style_bg_opa(tl, LV_OPA_50, 0);
                lv_obj_set_style_pad_left(tl, 4, 0);
                lv_obj_set_style_pad_right(tl, 4, 0);
                uiw_label_font(tl, g_snaps[idx].time, CLR_TEXT_HI, &lv_font_SHSC_16);
            }

            /* 点击缩略图 → 查看大图 */
            lv_obj_add_flag(thumb, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(thumb, _on_thumb_click, LV_EVENT_CLICKED,
                               (void *)(intptr_t)idx);
        }
    }
}

/* ================================================================
 * 测试模块弹窗：显示各外设状态与实时数据（1s 刷新）
 * 数据来源 dev_bridge_get_diag()
 * ================================================================ */

/* ---- 测试面板控制回调 ---- */
static lv_obj_t *g_pwm_lbl = NULL;    /* PWM 频率数值 */
static int g_pwm_on = 0;
static int g_pwm_freq = 1000;

static void _on_test_beep_on(lv_event_t *e)
{
    (void)e;
    /* 板载蜂鸣器：每次点击翻转（BEEP 开关按钮） */
    int cur = 0;
    struct dev_diag d;
    dev_bridge_get_diag(&d);
    cur = d.beep_on;
    dev_bridge_set_beep(!cur);
}

static void _on_test_pwm_toggle(lv_event_t *e)
{
    (void)e;
    g_pwm_on = !g_pwm_on;
    dev_bridge_set_pwm(g_pwm_on, g_pwm_freq);
}

static void _on_test_pwm_freq(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    g_pwm_freq = (int)lv_slider_get_value(sl);
    if (g_pwm_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%dHz", g_pwm_freq);
        lv_label_set_text(g_pwm_lbl, b);
    }
}

static void _on_test_als_toggle(lv_event_t *e)
{
    (void)e;
    int en = dev_bridge_get_als_en();
    dev_bridge_set_als_en(!en);
}

static void _on_test_imu_toggle(lv_event_t *e)
{
    (void)e;
    int en = dev_bridge_get_imu_en();
    dev_bridge_set_imu_en(!en);
}

static void _on_test_motion_toggle(lv_event_t *e)
{
    (void)e;
    int en = cam_feed_get_motion_en();
    cam_feed_set_motion_en(!en);   /* 运动粗判开关（关闭后不产生命中） */
}

/* 小尺寸按钮助手 */
static lv_obj_t *_mini_btn(lv_obj_t *par, const char *txt, uint32_t clr,
                           lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(par);
    if (!b) return NULL;
    lv_obj_set_size(b, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(clr), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_pad_left(b, 10, 0);
    lv_obj_set_style_pad_right(b, 10, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = uiw_label(b, txt, clr);
    if (l) lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void _refresh_test_modal(void)
{
    struct dev_diag d;
    dev_bridge_get_diag(&d);

    char buf[64];
    /* 行 0: LED / KEY / MOTION（运动粗判使能状态） */
    snprintf(buf, sizeof(buf), "LED:%s KEY:%s MOT:%s",
             d.led_ok ? (d.led_on ? "ON " : "OFF") : "ERR",
             d.key_ok ? (d.key_pressed ? "DN " : "UP ") : "ERR",
             cam_feed_get_motion_en() ? "ON " : "OFF");
    if (g_test_lbls[0]) lv_label_set_text(g_test_lbls[0], buf);

    /* 行 1: BEEP / PWM（显示 ON/OFF + 频率，设备不在显示 ERR） */
    if (d.beep_ok || d.pwm_ok) {
        snprintf(buf, sizeof(buf), "BEEP:%s PWM:%s %dHz",
                 d.beep_ok ? (d.beep_on ? "ON " : "OFF") : "ERR",
                 d.pwm_ok ? (d.pwm_on ? "ON " : "OFF") : "ERR",
                 d.pwm_freq);
    } else {
        snprintf(buf, sizeof(buf), "BEEP:ERR PWM:ERR");
    }
    if (g_test_lbls[1]) lv_label_set_text(g_test_lbls[1], buf);

    /* 行 2: ICM20608 六轴（换算 g / ℃） */
    if (d.imu_ok) {
        snprintf(buf, sizeof(buf), "ICM20608%s AX:%+.2f AY:%+.2f AZ:%+.2f T:%.1fC",
                 d.imu_en ? "" : "(OFF)",
                 d.imu_ax / 2048.0f, d.imu_ay / 2048.0f, d.imu_az / 2048.0f,
                 d.imu_temp_x10 / 10.0f);
    } else {
        snprintf(buf, sizeof(buf), "ICM20608 ERR");
    }
    if (g_test_lbls[2]) lv_label_set_text(g_test_lbls[2], buf);

    /* 行 3: AP3216C 光感/距离 */
    if (d.als_ok) {
        snprintf(buf, sizeof(buf), "AP3216C%s ALS:%d PS:%d IR:%d",
                 d.als_en ? "" : "(OFF)",
                 d.als_als, d.als_ps, d.als_ir);
    } else {
        snprintf(buf, sizeof(buf), "AP3216C ERR");
    }
    if (g_test_lbls[3]) lv_label_set_text(g_test_lbls[3], buf);

    /* 行 4: TF 卡容量（持久化存储） */
    {
        int total_mb = -1, avail_mb = -1;
        storage_tf_capacity(&total_mb, &avail_mb);
        if (total_mb > 0)
            snprintf(buf, sizeof(buf), "TF:%d.%d/%dGB",
                     avail_mb / 1024, (avail_mb % 1024) * 10 / 1024, total_mb / 1024);
        else
            snprintf(buf, sizeof(buf), "TF:ERR");
        if (g_test_lbls[4]) lv_label_set_text(g_test_lbls[4], buf);
    }
}

static void _build_test_modal(void)
{
    g_test_overlay = uiw_modal_create(lv_screen_active(),
                                       "状态检测", 560);
    if (!g_test_overlay) return;

    lv_obj_t *box = lv_obj_get_child(g_test_overlay, 0);
    if (!box) return;

    lv_obj_t *body = lv_obj_get_child(box, 1);
    if (!body) return;
    lv_obj_set_style_max_height(body, 460, 0);

    /* ---- 控制区：各模块手动开关 ---- */
    lv_obj_t *ctl = uiw_obj(body);
    if (ctl) {
        lv_obj_set_size(ctl, 528, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ctl, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(ctl, 8, 0);
        lv_obj_set_style_pad_bottom(ctl, 10, 0);
        lv_obj_set_style_border_side(ctl, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(ctl, lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_border_width(ctl, 1, 0);

        _mini_btn(ctl, "BEEP",     CLR_GREEN, _on_test_beep_on);
        _mini_btn(ctl, "PWM",      CLR_AMBER, _on_test_pwm_toggle);
        _mini_btn(ctl, "AP3216C",  CLR_CYAN,  _on_test_als_toggle);
        _mini_btn(ctl, "ICM20608", CLR_CYAN,  _on_test_imu_toggle);
        _mini_btn(ctl, "MOTION",   CLR_GREEN, _on_test_motion_toggle);
    }

    /* PWM 频率滑条行 */
    {
        lv_obj_t *row = uiw_obj(body);
        if (row) {
            lv_obj_set_size(row, 528, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(row, 6, 0);
            lv_obj_set_style_pad_bottom(row, 4, 0);
            uiw_label(row, "PWM FREQ", CLR_TEXT_LO);
            g_pwm_lbl = uiw_label_font(row, "1000Hz", CLR_AMBER, &lv_font_SHSC_16);
        }
        lv_obj_t *sl = lv_slider_create(body);
        if (sl) {
            lv_obj_set_width(sl, 528);
            lv_obj_set_height(sl, 20);
            lv_slider_set_range(sl, 100, 5000);
            lv_slider_set_value(sl, g_pwm_freq, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(sl, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(sl, lv_color_hex(CLR_AMBER), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(sl, lv_color_hex(CLR_AMBER), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_shadow_color(sl, lv_color_hex(CLR_AMBER), LV_PART_KNOB);
            lv_obj_set_style_shadow_width(sl, 6, LV_PART_KNOB);
            lv_obj_set_style_shadow_opa(sl, LV_OPA_40, LV_PART_KNOB);
            lv_obj_add_event_cb(sl, _on_test_pwm_freq, LV_EVENT_VALUE_CHANGED, NULL);
        }
    }

    /* ---- 数据区：5 行模块状态（刷新函数按行序更新） ---- */
    const char *init[5] = { "LED:-- KEY:--", "BEEP:-- PWM:--",
                            "ICM --", "AP3216C --", "TF:--" };
    for (int r = 0; r < 5; r++) {
        lv_obj_t *row = uiw_obj(body);
        if (!row) continue;
        lv_obj_set_size(row, 528, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_top(row, 8, 0);
        lv_obj_set_style_pad_bottom(row, 8, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_border_width(row, 1, 0);

        uiw_label(row, "·", CLR_TEXT_DIM);
        g_test_lbls[r] = uiw_label_font(row, init[r], CLR_CYAN, &lv_font_SHSC_16);
    }

    _refresh_test_modal();
}

static void _build_album_modal(void)
{
    g_album_overlay = uiw_modal_create(lv_screen_active(),
                                        "抓拍相册", 780);
    if (!g_album_overlay) return;

    lv_obj_t *box = lv_obj_get_child(g_album_overlay, 0);
    if (!box) return;

    /* 获取标题标签（动态更新） */
    lv_obj_t *head = lv_obj_get_child(box, 0);
    if (head) g_album_title_lbl = lv_obj_get_child(head, 0);

    lv_obj_t *body = lv_obj_get_child(box, 1);
    if (!body) return;
    g_album_body = body;
    /* 内容超出屏幕时滚动，保证相册弹窗接近屏幕大小 */
    lv_obj_set_style_max_height(body, 480, 0);

    /* 相册初始为空：启动恢复函数（_snapshots_restore）从 TF 回填 */
    g_snap_count = 0;
    _rebuild_album_grid();
}

/* ================================================================
 * 启动恢复：扫描 TF 上 snap_*.bmp（mtime 最新 6 张）回填内存相册
 * 文件名协议: snap_z<zone>_<cmp>_<HHMMSS>.bmp → 还原区域/级别/时间
 * ================================================================ */
static void _snapshots_restore(void)
{
    char paths[SNAP_MAX][160];
    int n = storage_snap_list(paths, SNAP_MAX);
    if (n <= 0) return;

    for (int i = 0; i < n; i++) {
        /* 读 BMP 像素（失败跳过该文件） */
        if (storage_bmp_read(paths[i], g_snaps[i].pix, SNAP_W, SNAP_H) != 0)
            continue;

        /* 降采样缩略图 */
        for (int ty = 0; ty < THUMB_H; ty++) {
            int sy = ty * SNAP_H / THUMB_H;
            const uint16_t *sline = g_snaps[i].pix + sy * SNAP_W;
            uint16_t *dline = g_snaps[i].thumb + ty * THUMB_W;
            for (int tx = 0; tx < THUMB_W; tx++)
                dline[tx] = sline[tx * SNAP_W / THUMB_W];
        }

        /* 文件名解析元数据: snap_z<zone>_<cmp>_<HHMMSS>.bmp */
        char *fname = strrchr(paths[i], '/');
        fname = fname ? fname + 1 : paths[i];
        int z = 0;
        char cmp[8] = "none", hms[16] = "";
        if (sscanf(fname, "snap_z%d_%7[^_]_%15s", &z, cmp, hms) != 3) {
            z = 0;
            snprintf(cmp, sizeof(cmp), "none");
            snprintf(hms, sizeof(hms), "000000");
        }

        /* cmp → level */
        const char *lv = "";
        if (strcmp(cmp, "hi") == 0)      lv = "high";
        else if (strcmp(cmp, "md") == 0) lv = "medium";
        else if (strcmp(cmp, "lo") == 0) lv = "low";

        snprintf(g_snaps[i].zone, sizeof(g_snaps[i].zone), "%s",
                 (z >= 0 && z < ZONE_COUNT) ? zone_info[z].name : "手动");
        snprintf(g_snaps[i].level, sizeof(g_snaps[i].level), "%s", lv);
        if (strlen(hms) >= 4)
            snprintf(g_snaps[i].time, sizeof(g_snaps[i].time),
                     "%.2s:%.2s", hms, hms + 2);
        else
            snprintf(g_snaps[i].time, sizeof(g_snaps[i].time), "--:--");

        g_snap_count = i + 1;
    }

    _rebuild_album_grid();
}

/* ================================================================
 * 查看弹窗：显示缩略图对应的全尺寸照片（630×340，与主页显示区一致）
 * 关闭：标题栏 X / 点击遮罩（uiw_modal_create 内置）
 * ================================================================ */
static lv_obj_t *g_viewer_overlay    = NULL;  /* 查看弹窗遮罩 */
static lv_obj_t *g_viewer_canvas     = NULL;  /* 大图画布 */
static lv_obj_t *g_viewer_title_lbl  = NULL;  /* 标题（区域 · 时间） */

static void _open_viewer(int idx)
{
    if (!g_viewer_overlay || !g_viewer_canvas) return;
    if (idx < 0 || idx >= g_snap_count) return;

    /* 大图画布直接绑定该照片的存储缓冲（零拷贝） */
    lv_canvas_set_buffer(g_viewer_canvas, (void *)g_snaps[idx].pix,
                         SNAP_W, SNAP_H, LV_COLOR_FORMAT_RGB565);
    if (g_viewer_title_lbl) {
        char b[48];
        snprintf(b, sizeof(b), "%s · %s", g_snaps[idx].zone, g_snaps[idx].time);
        lv_label_set_text(g_viewer_title_lbl, b);
    }
    lv_obj_invalidate(g_viewer_canvas);
    lv_obj_clear_flag(g_viewer_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void _build_viewer_modal(void)
{
    g_viewer_overlay = uiw_modal_create(lv_screen_active(),
                                         "抓拍查看", 660);
    if (!g_viewer_overlay) return;

    lv_obj_t *box = lv_obj_get_child(g_viewer_overlay, 0);
    if (!box) return;

    lv_obj_t *head = lv_obj_get_child(box, 0);
    if (head) g_viewer_title_lbl = lv_obj_get_child(head, 0);

    lv_obj_t *body = lv_obj_get_child(box, 1);
    if (!body) return;

    g_viewer_canvas = lv_canvas_create(body);
    if (g_viewer_canvas) {
        /* 缓冲在 _open_viewer 时绑定到选中照片（零拷贝） */
        lv_obj_set_size(g_viewer_canvas, SNAP_W, SNAP_H);
        lv_obj_align(g_viewer_canvas, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_clear_flag(g_viewer_canvas, LV_OBJ_FLAG_SCROLLABLE);
    }
}

/* ================================================================
 * 告警确认弹窗（设计 2.2 P0：长按 1s 消警防误触 + 一键静音）
 * 预警触发时弹出，显示 区域/类型/级别/时间；
 * "1s 消警"按钮需按住 1 秒（进度条指示）确认；MUTE 仅静音不清告警。
 * ================================================================ */

/* 长按进度（LV_EVENT_PRESSING 持续触发；松开复位） */
static void _on_confirm_pressing(lv_event_t *e)
{
    (void)e;
    if (g_confirm_tick == 0) g_confirm_tick = lv_tick_get();
    uint32_t held = lv_tick_elaps(g_confirm_tick);

    if (g_confirm_bar)
        lv_bar_set_value(g_confirm_bar, held >= 1000 ? 100 : held / 10, LV_ANIM_OFF);

    if (held >= 1000) {
        g_confirm_tick = 0;
        if (g_confirm_bar) lv_bar_set_value(g_confirm_bar, 100, LV_ANIM_OFF);
        /* 确认消警：消除所有告警分区（回布防中）+ 关弹窗 */
        for (int i = 0; i < ZONE_COUNT; i++) {
            if (g_zone_states[i] == ZONE_ALARM) {
                g_zone_states[i] = ZONE_ARMED;
                _update_zone_visual(i);
            }
        }
        _add_event("告警确认", "系统", CLR_GREEN, "");
        _show_toast("告警已确认");
        if (g_alarm_popup)
            lv_obj_add_flag(g_alarm_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void _on_confirm_released(lv_event_t *e)
{
    (void)e;
    g_confirm_tick = 0;   /* 未满 1s 松开：复位进度 */
    if (g_confirm_bar)
        lv_bar_set_value(g_confirm_bar, 0, LV_ANIM_OFF);
}

/* 一键静音：蜂鸣器停（告警状态保留，消警后自动恢复跟随） */
static void _on_alarm_mute(lv_event_t *e)
{
    (void)e;
    dev_bridge_set_beep_mute(1);
    _add_event("告警静音", "系统", CLR_AMBER, "");
}

/* 预警触发时弹出告警确认弹窗（供 ui_events_alarm_trigger_src 调用） */
static void _open_alarm_popup(int zone_idx, const char *src)
{
    if (!g_alarm_popup) return;
    g_alarm_zone = zone_idx;

    if (g_alarm_info_lbl) {
        char b[64];
        snprintf(b, sizeof(b), "%s · %s 触发 · %s",
                 zone_info[zone_idx].name,
                 (src && src[0]) ? src : "SENSOR",
                 g_snaps[0].time);
        lv_label_set_text(g_alarm_info_lbl, b);
    }
    if (g_confirm_bar) lv_bar_set_value(g_confirm_bar, 0, LV_ANIM_OFF);
    g_confirm_tick = 0;

    lv_obj_clear_flag(g_alarm_popup, LV_OBJ_FLAG_HIDDEN);
}

static void _build_alarm_popup(void)
{
    g_alarm_popup = uiw_modal_create(lv_screen_active(), "告警", 440);
    if (!g_alarm_popup) return;

    lv_obj_t *box = lv_obj_get_child(g_alarm_popup, 0);
    if (!box) return;
    lv_obj_t *body = lv_obj_get_child(box, 1);
    if (!body) return;

    /* 告警信息行 */
    g_alarm_info_lbl = uiw_label_font(body, "--", CLR_RED, &lv_font_SHSC_16);
    if (g_alarm_info_lbl) {
        lv_obj_set_width(g_alarm_info_lbl, 368);
        lv_label_set_long_mode(g_alarm_info_lbl, LV_LABEL_LONG_DOT);
    }

    /* 长按消警按钮（带进度条指示按住时长） */
    lv_obj_t *btn = lv_button_create(body);
    if (btn) {
        lv_obj_set_size(btn, 368, 52);
        lv_obj_set_style_bg_color(btn, lv_color_hex(CLR_RED), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_30, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(CLR_RED), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *bl = uiw_label(btn, "1s 消警（按住）", CLR_RED);
        if (bl) lv_obj_center(bl);

        lv_obj_add_event_cb(btn, _on_confirm_pressing, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(btn, _on_confirm_released, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(btn, _on_confirm_released, LV_EVENT_PRESS_LOST, NULL);
    }

    /* 长按进度条 */
    g_confirm_bar = lv_bar_create(body);
    if (g_confirm_bar) {
        lv_obj_set_size(g_confirm_bar, 368, 8);
        lv_bar_set_range(g_confirm_bar, 0, 100);
        lv_bar_set_value(g_confirm_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(g_confirm_bar, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(g_confirm_bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(g_confirm_bar, lv_color_hex(CLR_RED), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(g_confirm_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    }

    /* 一键静音 */
    lv_obj_t *mute = lv_button_create(body);
    if (mute) {
        lv_obj_set_size(mute, 368, 40);
        lv_obj_set_style_bg_color(mute, lv_color_hex(CLR_AMBER), 0);
        lv_obj_set_style_bg_opa(mute, LV_OPA_20, 0);
        lv_obj_set_style_border_color(mute, lv_color_hex(CLR_AMBER), 0);
        lv_obj_set_style_border_width(mute, 1, 0);
        lv_obj_set_style_radius(mute, 0, 0);
        lv_obj_clear_flag(mute, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *ml = uiw_label(mute, "MUTE", CLR_AMBER);
        if (ml) lv_obj_center(ml);
        lv_obj_add_event_cb(mute, _on_alarm_mute, LV_EVENT_CLICKED, NULL);
    }
}

/* ================================================================
 * ui_home_create — 主入口
 * ================================================================ */
void ui_home_create(lv_obj_t *parent)
{
    if (!parent) return;

    (void)cam_to_zone; /* 暂未使用，保留供后续摄像头-分区联动 */

    /* 根容器：flex-col 不可滚动 */
    lv_obj_set_size(parent, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(parent, lv_color_hex(CLR_BG), 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* 初始化分区状态 */
    for (int i = 0; i < ZONE_COUNT; i++) g_zone_states[i] = ZONE_ONLINE;

    /* 1. status-bar 36px */
    _build_status_bar(parent);

    /* 2. main-area  固定 1024×512（不使用 flex_grow，避免高度塌陷） */
    /*    512 = 600 - 36(status) - 52(bottom)，info-bar 已删除，空间补偿给主区 */
    lv_obj_t *ma = uiw_obj(parent);
    if (ma) {
        lv_obj_set_size(ma, SCR_W, MAIN_H);
        lv_obj_set_flex_flow(ma, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ma, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_left(ma, 8, 0);
        lv_obj_set_style_pad_right(ma, 8, 0);
        lv_obj_set_style_pad_top(ma, GAP_SM, 0);
        lv_obj_set_style_pad_bottom(ma, GAP_SM, 0);
        lv_obj_set_style_pad_gap(ma, GAP_MD, 0);
        lv_obj_clear_flag(ma, LV_OBJ_FLAG_SCROLLABLE);

        /* 左列 630×500（固定高度 = MAIN_H - 上下padding 12，不依赖 flex_grow） */
        lv_obj_t *left = uiw_obj(ma);
        if (left) {
            lv_obj_set_size(left, LEFT_W, MAIN_H - 12);
            lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
            lv_obj_set_style_pad_gap(left, GAP_SM, 0);
            lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

            _build_cam_wrap(left);
            _build_zones_grid(left);
        }

        /* 右列 360×512 */
        _build_right_col(ma);
    }

    /* 3. bottom-bar 52px */
    _build_bottom_bar(parent);

    /* 4. Toast 提示（top layer 独立顶层，底部居中显示） */
    g_toast = uiw_label(lv_layer_top(), "", CLR_BLACK);
    if (g_toast) {
        lv_label_set_long_mode(g_toast, LV_LABEL_LONG_DOT);
        lv_obj_set_style_bg_opa(g_toast, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(g_toast, lv_color_hex(CLR_GREEN), 0);
        lv_obj_set_style_pad_left(g_toast, 18, 0);
        lv_obj_set_style_pad_right(g_toast, 18, 0);
        lv_obj_set_style_pad_top(g_toast, 6, 0);
        lv_obj_set_style_pad_bottom(g_toast, 6, 0);
        lv_obj_align(g_toast, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
    }

    /* 5. 弹窗（初始隐藏） */
    _build_detail_modal();
    _build_settings_modal();
    _build_album_modal();
    _build_test_modal();
    _build_viewer_modal();
    _build_alarm_popup();

    /* 6. 抓拍恢复：从 TF 回填上次开机的抓拍相册 */
    _snapshots_restore();

    /* 触摸唤醒：任意触摸重置空闲计时并解除 blank */
    lv_obj_add_event_cb(parent, _on_screen_touch, LV_EVENT_PRESSED, NULL);

    /* 6. 创建定时器 */
    g_timer_clock = lv_timer_create(_timer_clock_cb, 1000, NULL);
    g_timer_anim  = lv_timer_create(_timer_anim_cb, 50, NULL);
    lv_timer_create(net_timer_cb, 5000, NULL);   /* 联网检测 5s */

    /* 7. 摄像头喂数（V4L2 线程 + 33ms 帧刷新定时器，30fps） */
    if (g_cam_canvas) {
        cam_feed_start(g_cam_canvas, g_cam_canvas_buf);
        lv_timer_create(_cam_refresh_cb, 33, NULL);
    }
}

/* 摄像头帧刷新：拷贝最新完成帧（双缓冲发布）后局部 invalidate
 * （v9 canvas 是普通对象，用 lv_obj_invalidate）。
 * 附：有画面隐藏占位文字/网格线，断流自动恢复（需求 08-29） */
static void _cam_refresh_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_cam_canvas && cam_feed_blit_if_ready(g_cam_canvas_buf))
        lv_obj_invalidate(g_cam_canvas);

    if (g_cam_grid_cnt > 0 || g_cam_text_lbl) {
        int hide = cam_feed_stream_ok();
        if (hide != g_cam_overlay_hidden) {
            for (int i = 0; i < g_cam_grid_cnt; i++) {
                if (!g_cam_grid_lines[i]) continue;
                if (hide) lv_obj_add_flag(g_cam_grid_lines[i], LV_OBJ_FLAG_HIDDEN);
                else      lv_obj_clear_flag(g_cam_grid_lines[i], LV_OBJ_FLAG_HIDDEN);
            }
            if (g_cam_text_lbl) {
                if (hide) lv_obj_add_flag(g_cam_text_lbl, LV_OBJ_FLAG_HIDDEN);
                else      lv_obj_clear_flag(g_cam_text_lbl, LV_OBJ_FLAG_HIDDEN);
            }
            g_cam_overlay_hidden = hide;
        }
    }
}

/* ================================================================
 * ui_events 公共接口实现（真实状态信号接入层）
 *
 * 板端传感器/摄像头/网络/RTC 调用 ui_events.h 中声明的这些函数，
 * 即可驱动整个 UI 的状态显示、事件时间轴、抓拍相册。
 * ================================================================ */

/* 联网状态缓存（用于状态变化事件去重） */
static int g_net_online = 1;
static int g_net_online_init = 0;

void ui_events_zone_set_state(int id, zone_state_t st)
{
    if (id < 0 || id >= ZONE_COUNT) return;
    if (g_zone_states[id] == st) return;   /* 无变化不刷新 */

    g_zone_states[id] = st;
    /* 注：state_machine 只提供 arm_all/disarm_all 批量接口，
     * 单分区状态以 UI 侧 g_zone_states 为准（真实信号驱动 UI） */
    _update_zone_visual(id);
}

void ui_events_alarm_trigger_src(int id, const char *src)
{
    if (id < 0 || id >= ZONE_COUNT) return;
    if (g_zone_states[id] == ZONE_ALARM) return;

    g_zone_states[id] = ZONE_ALARM;
    _update_zone_visual(id);

    /* 预警自动抓拍：截当前画面存入相册（区域=告警分区，级别=high） */
    snapshot_capture_idx(id, zone_info[id].name, "high");

    /* 触发类型写入事件标题："SENSOR 触发" / "CAM 触发"（未来摄像头预警复用） */
    char title[32];
    snprintf(title, sizeof(title), "%s 触发", (src && src[0]) ? src : "SENSOR");
    _add_event(title, zone_info[id].name, CLR_RED, "high");
    _show_toast("异常 已触发");

    /* 告警确认弹窗（设计 P0：长按 1s 消警防误触 + MUTE 静音） */
    _open_alarm_popup(id, src);
}

void ui_events_alarm_trigger(int id)
{
    ui_events_alarm_trigger_src(id, "SENSOR");
}

void ui_events_alarm_ack(int id)
{
    if (id < 0 || id >= ZONE_COUNT) return;
    if (g_zone_states[id] != ZONE_ALARM) return;

    ack_alarm(id);
    /* 告警必产生于布防状态，消除后回到布防中 */
    g_zone_states[id] = ZONE_ARMED;
    _update_zone_visual(id);
    _add_event("报警已消", zone_info[id].name, CLR_GREEN, "");
}

void ui_events_log(const char *title, const char *loc, const char *level)
{
    _add_event(title ? title : "", loc ? loc : "系统",
               (level && strcmp(level, "high") == 0) ? CLR_RED :
               (level && strcmp(level, "medium") == 0) ? CLR_AMBER : CLR_CYAN,
               level ? level : "");
}

void ui_events_net_set(int online)
{
    lv_color_t clr = online ? lv_color_hex(CLR_GREEN) : lv_color_hex(CLR_TEXT_LO);
    const char *txt = online ? "联网" : "离线";
    if (g_net_dot) {
        lv_obj_set_style_bg_color(g_net_dot, clr, 0);
        lv_obj_set_style_shadow_color(g_net_dot, clr, 0);
    }
    if (g_net_lbl) {
        lv_label_set_text(g_net_lbl, txt);
        lv_obj_set_style_text_color(g_net_lbl, clr, 0);
    }

    /* 状态变化时写事件（首次调用不写，避免启动噪音） */
    if (g_net_online_init && g_net_online != online)
        _add_event(online ? "网络在线" : "网络离线", "主控",
                   online ? CLR_GREEN : CLR_AMBER, "");
    g_net_online = online;
    g_net_online_init = 1;
}

void ui_events_set_time(int h, int m, int s)
{
    /* 直接设置系统时间（settimeofday），时钟定时器下一秒自动跟随显示。
     * 外部真实时间源（RTC 芯片/NTP 同步程序）调用此接口即可驱动 UI。 */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_cur;
    time_t now = tv.tv_sec;
    localtime_r(&now, &tm_cur);
    tm_cur.tm_hour = h % 24;
    tm_cur.tm_min  = m % 60;
    tm_cur.tm_sec  = s % 60;
    tv.tv_sec = mktime(&tm_cur);
    if (settimeofday(&tv, NULL) != 0)
        perror("[UI] settimeofday failed");
}

int ui_events_is_armed(void)
{
    /* 任一分区布防即视为已布防（支持分区级独立布防/撤防） */
    return ui_events_zone_has_armed();
}

int ui_events_zone_has_armed(void)
{
    for (int i = 0; i < ZONE_COUNT; i++)
        if (g_zone_states[i] == ZONE_ARMED) return 1;
    return 0;
}

int ui_events_zone_is_armed(int id)
{
    if (id < 0 || id >= ZONE_COUNT) return 0;
    return (g_zone_states[id] == ZONE_ARMED) ? 1 : 0;
}

int ui_events_current_cam_zone(void)
{
    /* 单摄分时切换：当前预览通道映射分区（cam_to_zone: 前门=0 后门=1 仓库=3 窗户=2） */
    if (g_current_cam < 0 || g_current_cam >= CAM_COUNT) return -1;
    return cam_to_zone[g_current_cam];
}

int ui_events_zone_has_alarm(void)
{
    for (int i = 0; i < ZONE_COUNT; i++)
        if (g_zone_states[i] == ZONE_ALARM) return 1;
    return 0;
}

int ui_events_zone_alarm_level(void)
{
    /* 分区→级别映射（设计文档 2.2 告警分级）：门=低(1) 仓库=中(2) 窗/周界=高(3) */
    static const int zone_lvl[ZONE_COUNT] = { 1, 1, 3, 2 };
    int max_lvl = 0;
    for (int i = 0; i < ZONE_COUNT; i++)
        if (g_zone_states[i] == ZONE_ALARM && zone_lvl[i] > max_lvl)
            max_lvl = zone_lvl[i];
    return max_lvl;
}

void ui_events_user_activity(void)
{
    g_idle_sec = 0;             /* 重置空闲计时（重新 10 分钟计时） */
    _screen_set_blank(0);       /* 解除 blank 唤醒屏幕（fb 内容仍在，直接恢复） */

    /* 全屏 invalidate：下一帧（~5ms）自动重绘，消除 blank 恢复的撕裂。
     * 注意：不能在事件回调里用 lv_refr_now() 同步刷新——会阻塞 LVGL
     * 主线程导致触摸事件丢失（表现为界面卡死无响应）。 */
    lv_obj_invalidate(lv_screen_active());
}