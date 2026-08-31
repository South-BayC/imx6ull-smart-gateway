/**
 * @file    ui_home.c
 * @brief   智能安防网关 - 主界面实现 (1024x600)
 * @date    2026-08-22
 */

#include "ui_home.h"
#include "ui_dialogs.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* ===== 内部静态变量 ===== */
static lv_obj_t *s_screen        = NULL;
static lv_obj_t *s_status_bar    = NULL;
static lv_obj_t *s_lbl_clock     = NULL;
static lv_obj_t *s_lbl_arm_state = NULL;
static lv_obj_t *s_cam_area      = NULL;
static lv_obj_t *s_lbl_cam_title = NULL;
static lv_obj_t *s_cam_pills[UI_CAM_COUNT];
static lv_obj_t *s_zone_cards[SM_ZONE_COUNT];
static lv_obj_t *s_zone_dots[SM_ZONE_COUNT];
static lv_obj_t *s_zone_stats[SM_ZONE_COUNT];
static lv_obj_t *s_event_list    = NULL;
static lv_obj_t *s_lbl_evt_count = NULL;
static lv_obj_t *s_toast         = NULL;
static lv_timer_t *s_anim_timer  = NULL;
static lv_timer_t *s_clock_timer = NULL;
static int s_anim_tick = 0;
static int s_current_cam = 0;
static int s_event_item_count = 0;

/* ===== 摄像头通道 ===== */
static const ui_cam_info_t s_cams[UI_CAM_COUNT] = {
    { "前门", "CH01" },
    { "后门", "CH02" },
    { "仓库", "CH03" },
    { "周界", "CH04" }
};
static const int s_cam_to_zone[UI_CAM_COUNT] = { 0, 1, 3, 2 };

/* ===== 前向声明 ===== */
static void build_status_bar(lv_obj_t *p);
static void build_main_area(lv_obj_t *p);
static void build_left_col(lv_obj_t *p);
static void build_cam_preview(lv_obj_t *p);
static void build_zone_grid(lv_obj_t *p);
static void build_right_col(lv_obj_t *p);
static void build_bottom_bar(lv_obj_t *p);
static void anim_timer_cb(lv_timer_t *t);
static void clock_timer_cb(lv_timer_t *t);
static void apply_zone_style(int idx, sm_state_t st);
static void on_btn_arm(lv_event_t *e);
static void on_btn_disarm(lv_event_t *e);
static void on_btn_silence(lv_event_t *e);
static void on_btn_settings(lv_event_t *e);
static void on_zone_card(lv_event_t *e);
static void on_cam_pill(lv_event_t *e);
static lv_obj_t *make_btn(lv_obj_t *p, const char *t, lv_color_t bc, lv_color_t tc);

/* ==================== 公共 API ==================== */

void ui_home_init(lv_obj_t *parent)
{
    s_screen = parent;
    lv_obj_set_style_bg_color(s_screen, UI_BG_PRIMARY, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_flex_flow(s_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_screen, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_STRETCH, LV_FLEX_ALIGN_STRETCH);

    build_status_bar(s_screen);
    build_main_area(s_screen);
    build_bottom_bar(s_screen);

    /* 抓拍提示（浮动在底栏上方） */
    s_toast = lv_obj_create(s_screen);
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_bg_color(s_toast, UI_GREEN, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_toast, 0, 0);
    lv_obj_set_style_border_width(s_toast, 0, 0);
    lv_obj_set_style_pad_hor(s_toast, 18, 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *toast_lbl = lv_label_create(s_toast);
    lv_label_set_text(toast_lbl, "\xe2\x9c\x93 " "\xe6\x8a\x93\xe6\x8b\x8d\xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98");
    lv_obj_set_style_text_font(toast_lbl, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(toast_lbl, lv_color_black(), 0);
    lv_obj_center(toast_lbl);

    s_anim_timer  = lv_timer_create(anim_timer_cb, 50, NULL);
    s_clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
    clock_timer_cb(NULL);
    for (int i = 0; i < SM_ZONE_COUNT; i++)
        apply_zone_style(i, sm_get_zone_state(i));
}

void ui_home_refresh_clock(void) { clock_timer_cb(NULL); }

void ui_home_update_zone(int idx, sm_state_t st)
{
    if (idx >= 0 && idx < SM_ZONE_COUNT) apply_zone_style(idx, st);
}

int ui_home_get_current_cam(void) { return s_current_cam; }

void ui_home_show_capture_toast(void)
{
    if (!s_toast) return;
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_toast, UI_GREEN, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_label_set_text(s_toast, "✓ 抓拍已保存");
}

void ui_home_add_event(const ui_event_t *ev)
{
    if (!s_event_list || !ev) return;

    /* 达到上限时移除最旧条目 (最后一个 child)，保证始终显示最近 UI_EVENT_MAX 条事件 */
    uint32_t child_cnt = lv_obj_get_child_count(s_event_list);
    while (child_cnt >= UI_EVENT_MAX) {
        lv_obj_t *oldest = lv_obj_get_child(s_event_list, child_cnt - 1);
        if (oldest) lv_obj_delete(oldest);
        child_cnt = lv_obj_get_child_count(s_event_list);
        if (s_event_item_count > 0) s_event_item_count--;
    }

    lv_obj_t *item = lv_obj_create(s_event_list);
    /* 新事件插入到列表顶部 (索引 0)，保证最新事件始终在最上方可见 */
    lv_obj_move_to_index(item, 0);
    lv_obj_set_size(item, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_set_style_pad_all(item, 6, 0);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(item, 8, 0);

    /* 时间 */
    lv_obj_t *tl = lv_label_create(item);
    lv_label_set_text_fmt(tl, "%02d:%02d", ev->hour, ev->minute);
    lv_obj_set_style_text_font(tl, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(tl, UI_TEXT_DIM, 0);
    lv_obj_set_width(tl, 50);

    /* 颜色点 */
    lv_obj_t *dot = lv_obj_create(item);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, 3, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    switch (ev->type) {
        case SM_EVT_INFO:  lv_obj_set_style_bg_color(dot, UI_CYAN, 0);  break;
        case SM_EVT_OK:    lv_obj_set_style_bg_color(dot, UI_GREEN, 0); break;
        case SM_EVT_WARN:  lv_obj_set_style_bg_color(dot, UI_AMBER, 0); break;
        case SM_EVT_ERROR: lv_obj_set_style_bg_color(dot, UI_RED, 0);   break;
    }

    /* 内容区 */
    lv_obj_t *body = lv_obj_create(item);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *tl2 = lv_label_create(body);
    lv_label_set_text(tl2, ev->title);
    lv_obj_set_style_text_font(tl2, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(tl2, UI_TEXT_HI, 0);

    lv_obj_t *ll = lv_label_create(body);
    lv_label_set_text(ll, ev->location);
    lv_obj_set_style_text_font(ll, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(ll, UI_TEXT_DIM, 0);

    if (ev->level != SM_LEVEL_NONE) {
        lv_obj_t *lb = lv_label_create(body);
        const char *lt = "";
        lv_color_t lc = UI_CYAN;
        switch (ev->level) {
            case SM_LEVEL_HIGH:   lt = "级别：高";   lc = UI_RED;    break;
            case SM_LEVEL_MEDIUM: lt = "级别：中"; lc = UI_AMBER;  break;
            case SM_LEVEL_LOW:    lt = "级别：低";    lc = UI_CYAN;   break;
            default: break;
        }
        lv_label_set_text(lb, lt);
        lv_obj_set_style_text_font(lb, UI_FONT_CN16, 0);
        lv_obj_set_style_text_color(lb, lc, 0);
    }

    s_event_item_count++;
    if (s_lbl_evt_count) {
        char cb[16];
        snprintf(cb, sizeof(cb), "%d 条", s_event_item_count);
        lv_label_set_text(s_lbl_evt_count, cb);
    }
}

void ui_home_switch_cam(int cam_idx)
{
    if (cam_idx < 0 || cam_idx >= UI_CAM_COUNT || cam_idx == s_current_cam) return;
    s_current_cam = cam_idx;

    if (s_lbl_cam_title) {
        char tb[32];
        snprintf(tb, sizeof(tb), "%s \xc2\xb7 %s", s_cams[cam_idx].ch, s_cams[cam_idx].name);
        lv_label_set_text(s_lbl_cam_title, tb);
    }

    for (int i = 0; i < UI_CAM_COUNT; i++) {
        if (!s_cam_pills[i]) continue;
        if (i == cam_idx) {
            lv_obj_set_style_bg_color(s_cam_pills[i], UI_CYAN, 0);
            lv_obj_set_style_bg_opa(s_cam_pills[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(s_cam_pills[i], LV_OPA_TRANSP, 0);
        }
    }
}

/* ==================== 构建函数 ==================== */

static void build_status_bar(lv_obj_t *parent)
{
    s_status_bar = lv_obj_create(parent);
    lv_obj_set_size(s_status_bar, UI_SCREEN_W, UI_STATUS_BAR_H);
    lv_obj_set_style_bg_color(s_status_bar, UI_BG_CARD, 0);
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_status_bar, 0, 0);
    lv_obj_set_style_border_side(s_status_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_status_bar, UI_BORDER, 0);
    lv_obj_set_style_border_opa(s_status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_status_bar, 16, 0);
    lv_obj_set_flex_flow(s_status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 左侧 */
    lv_obj_t *left = lv_obj_create(s_status_bar);
    lv_obj_set_height(left, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 8, 0);

    lv_obj_t *d = lv_label_create(left);
    lv_label_set_text(d, "\xe2\x97\x86");
    lv_obj_set_style_text_color(d, UI_CYAN, 0);
    lv_obj_set_style_text_font(d, UI_FONT_CN16, 0);

    lv_obj_t *sn = lv_label_create(left);
    lv_label_set_text(sn, "智慧安防网关 · 边缘在线");
    lv_obj_set_style_text_font(sn, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(sn, UI_TEXT_HI, 0);

    /* 中央时钟 */
    s_lbl_clock = lv_label_create(s_status_bar);
    lv_label_set_text(s_lbl_clock, "2026-08-22 00:00:00");
    lv_obj_set_style_text_font(s_lbl_clock, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(s_lbl_clock, UI_TEXT_LO, 0);

    /* 右侧 */
    lv_obj_t *right = lv_obj_create(s_status_bar);
    lv_obj_set_height(right, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 14, 0);

    lv_obj_t *nd = lv_obj_create(right);
    lv_obj_set_size(nd, 8, 8);
    lv_obj_set_style_radius(nd, 4, 0);
    lv_obj_set_style_bg_color(nd, UI_GREEN, 0);
    lv_obj_set_style_bg_opa(nd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(nd, 0, 0);
    lv_obj_set_style_pad_all(nd, 0, 0);

    lv_obj_t *nl = lv_label_create(right);
    lv_label_set_text(nl, "联网");
    lv_obj_set_style_text_font(nl, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(nl, UI_TEXT_LO, 0);

    lv_obj_t *ad = lv_obj_create(right);
    lv_obj_set_size(ad, 8, 8);
    lv_obj_set_style_radius(ad, 4, 0);
    lv_obj_set_style_bg_color(ad, UI_GREEN, 0);
    lv_obj_set_style_bg_opa(ad, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ad, 0, 0);
    lv_obj_set_style_pad_all(ad, 0, 0);

    s_lbl_arm_state = lv_label_create(right);
    lv_label_set_text(s_lbl_arm_state, "已撤防");
    lv_obj_set_style_text_font(s_lbl_arm_state, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(s_lbl_arm_state, UI_TEXT_LO, 0);

    lv_obj_t *dl = lv_label_create(right);
    lv_label_set_text(dl, "i.MX6ULL");
    lv_obj_set_style_text_font(dl, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(dl, UI_TEXT_DIM, 0);
}

static void build_main_area(lv_obj_t *parent)
{
    lv_obj_t *ma = lv_obj_create(parent);
    lv_obj_set_flex_grow(ma, 1);
    lv_obj_set_width(ma, UI_SCREEN_W);
    lv_obj_set_style_bg_opa(ma, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ma, 0, 0);
    lv_obj_set_style_pad_all(ma, UI_PADDING, 0);
    lv_obj_set_flex_flow(ma, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ma, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_STRETCH, LV_FLEX_ALIGN_STRETCH);
    lv_obj_set_style_pad_column(ma, UI_GAP, 0);
    build_left_col(ma);
    build_right_col(ma);
}

static void build_left_col(lv_obj_t *parent)
{
    lv_obj_t *lc = lv_obj_create(parent);
    lv_obj_set_width(lc, UI_LEFT_COL_W);
    lv_obj_set_flex_grow(lc, 0);
    lv_obj_set_style_bg_opa(lc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lc, 0, 0);
    lv_obj_set_style_pad_all(lc, 0, 0);
    lv_obj_set_flex_flow(lc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(lc, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_STRETCH, LV_FLEX_ALIGN_STRETCH);
    lv_obj_set_style_pad_row(lc, UI_GAP, 0);
    build_cam_preview(lc);
    build_zone_grid(lc);
}

static void build_cam_preview(lv_obj_t *parent)
{
    s_cam_area = lv_obj_create(parent);
    lv_obj_set_size(s_cam_area, UI_LEFT_COL_W, UI_CAM_PREVIEW_H);
    lv_obj_set_style_bg_color(s_cam_area, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_cam_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_cam_area, UI_BORDER, 0);
    lv_obj_set_style_border_width(s_cam_area, 1, 0);
    lv_obj_set_style_border_opa(s_cam_area, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_cam_area, 0, 0);

    /* 顶部栏 */
    lv_obj_t *ch = lv_obj_create(s_cam_area);
    lv_obj_set_size(ch, UI_LEFT_COL_W, 36);
    lv_obj_set_style_bg_color(ch, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ch, LV_OPA_50, 0);
    lv_obj_set_style_border_width(ch, 0, 0);
    lv_obj_set_style_pad_hor(ch, 10, 0);
    lv_obj_set_flex_flow(ch, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ch, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 模式徽标 */
    lv_obj_t *mb = lv_label_create(ch);
    lv_label_set_text(mb, "云端精判模式 · 实时预览");
    lv_obj_set_style_text_font(mb, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(mb, UI_CYAN, 0);
    lv_obj_set_style_bg_color(mb, UI_CYAN, 0);
    lv_obj_set_style_bg_opa(mb, LV_OPA_10, 0);
    lv_obj_set_style_border_color(mb, UI_CYAN, 0);
    lv_obj_set_style_border_width(mb, 1, 0);
    lv_obj_set_style_border_opa(mb, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(mb, 8, 0);
    lv_obj_set_style_pad_ver(mb, 2, 0);

    /* 摄像头标题 */
    s_lbl_cam_title = lv_label_create(ch);
    lv_label_set_text(s_lbl_cam_title, "CH01 \xb7 前门");
    lv_obj_set_style_text_font(s_lbl_cam_title, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(s_lbl_cam_title, UI_TEXT_DIM, 0);

    /* 切换按钮组 */
    lv_obj_t *pw = lv_obj_create(ch);
    lv_obj_set_height(pw, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(pw, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pw, 0, 0);
    lv_obj_set_style_pad_all(pw, 0, 0);
    lv_obj_set_flex_flow(pw, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(pw, 8, 0);

    static const char *pn[] = {"前门", "后门", "仓库", "周界"};
    for (int i = 0; i < UI_CAM_COUNT; i++) {
        s_cam_pills[i] = lv_btn_create(pw);
        lv_obj_set_size(s_cam_pills[i], LV_SIZE_CONTENT, 28);
        lv_obj_set_style_bg_opa(s_cam_pills[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_cam_pills[i], UI_BORDER, 0);
        lv_obj_set_style_border_width(s_cam_pills[i], 1, 0);
        lv_obj_set_style_radius(s_cam_pills[i], 0, 0);
        lv_obj_set_style_pad_hor(s_cam_pills[i], 12, 0);
        lv_obj_add_event_cb(s_cam_pills[i], on_cam_pill, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *pl = lv_label_create(s_cam_pills[i]);
        lv_label_set_text(pl, pn[i]);
        lv_obj_set_style_text_font(pl, UI_FONT_CN16, 0);
        lv_obj_set_style_text_color(pl, UI_TEXT_LO, 0);
        lv_obj_center(pl);
        if (i == 0) {
            lv_obj_set_style_bg_color(s_cam_pills[i], UI_CYAN, 0);
            lv_obj_set_style_bg_opa(s_cam_pills[i], LV_OPA_COVER, 0);
        }
    }

    /* 十字线 */
    lv_obj_t *ch2 = lv_obj_create(s_cam_area);
    lv_obj_set_size(ch2, 80, 1);
    lv_obj_set_style_bg_color(ch2, UI_CYAN, 0);
    lv_obj_set_style_bg_opa(ch2, LV_OPA_50, 0);
    lv_obj_set_style_border_width(ch2, 0, 0);
    lv_obj_set_style_pad_all(ch2, 0, 0);
    lv_obj_align(ch2, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *cv = lv_obj_create(s_cam_area);
    lv_obj_set_size(cv, 1, 80);
    lv_obj_set_style_bg_color(cv, UI_CYAN, 0);
    lv_obj_set_style_bg_opa(cv, LV_OPA_50, 0);
    lv_obj_set_style_border_width(cv, 0, 0);
    lv_obj_set_style_pad_all(cv, 0, 0);
    lv_obj_align(cv, LV_ALIGN_CENTER, 0, 0);

    /* 预览提示 */
    lv_obj_t *ph = lv_label_create(s_cam_area);
    lv_label_set_text(ph, "前门画面");
    lv_obj_set_style_text_font(ph, UI_FONT_CN20, 0);
    lv_obj_set_style_text_color(ph, UI_TEXT_DIM, 0);
    lv_obj_align(ph, LV_ALIGN_CENTER, 0, 20);
}

static void build_zone_grid(lv_obj_t *parent)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_set_flex_grow(g, 1);
    lv_obj_set_width(g, UI_LEFT_COL_W);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(g, UI_GAP, 0);
    lv_obj_set_style_pad_row(g, UI_GAP, 0);

    static const char *zn[] = {"前门", "后门", "窗户", "仓库"};
    for (int i = 0; i < SM_ZONE_COUNT; i++) {
        s_zone_cards[i] = lv_btn_create(g);
        lv_obj_set_size(s_zone_cards[i], (UI_LEFT_COL_W - UI_GAP) / 2, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(s_zone_cards[i], UI_BG_CARD, 0);
        lv_obj_set_style_bg_opa(s_zone_cards[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_zone_cards[i], UI_BORDER, 0);
        lv_obj_set_style_border_width(s_zone_cards[i], 1, 0);
        lv_obj_set_style_border_opa(s_zone_cards[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_zone_cards[i], 0, 0);
        lv_obj_set_style_pad_all(s_zone_cards[i], 10, 0);
        lv_obj_set_flex_flow(s_zone_cards[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_zone_cards[i], LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_add_event_cb(s_zone_cards[i], on_zone_card, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* 名称行 */
        lv_obj_t *nr = lv_obj_create(s_zone_cards[i]);
        lv_obj_set_size(nr, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(nr, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(nr, 0, 0);
        lv_obj_set_style_pad_all(nr, 0, 0);
        lv_obj_set_flex_flow(nr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(nr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(nr, 6, 0);

        s_zone_dots[i] = lv_obj_create(nr);
        lv_obj_set_size(s_zone_dots[i], 7, 7);
        lv_obj_set_style_radius(s_zone_dots[i], 4, 0);
        lv_obj_set_style_bg_color(s_zone_dots[i], UI_DOT_ONLINE, 0);
        lv_obj_set_style_bg_opa(s_zone_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_zone_dots[i], 0, 0);
        lv_obj_set_style_pad_all(s_zone_dots[i], 0, 0);

        lv_obj_t *nl = lv_label_create(nr);
        lv_label_set_text(nl, zn[i]);
        lv_obj_set_style_text_font(nl, UI_FONT_CN16, 0);
        lv_obj_set_style_text_color(nl, UI_TEXT_HI, 0);

        s_zone_stats[i] = lv_label_create(s_zone_cards[i]);
        lv_label_set_text(s_zone_stats[i], "在线 · 正常");
        lv_obj_set_style_text_font(s_zone_stats[i], UI_FONT_CN16, 0);
        lv_obj_set_style_text_color(s_zone_stats[i], UI_TEXT_LO, 0);

        lv_obj_t *il = lv_label_create(s_zone_cards[i]);
        char ib[16];
        snprintf(ib, sizeof(ib), "Z-%03d", i + 1);
        lv_label_set_text(il, ib);
        lv_obj_set_style_text_font(il, UI_FONT_CN16, 0);
        lv_obj_set_style_text_color(il, UI_TEXT_DIM, 0);
    }
}

static void build_right_col(lv_obj_t *parent)
{
    lv_obj_t *rc = lv_obj_create(parent);
    lv_obj_set_width(rc, UI_RIGHT_COL_W);
    lv_obj_set_flex_grow(rc, 0);
    lv_obj_set_style_bg_opa(rc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rc, 0, 0);
    lv_obj_set_style_pad_all(rc, 0, 0);
    lv_obj_set_flex_flow(rc, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rc, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_STRETCH, LV_FLEX_ALIGN_STRETCH);

    /* 头部 */
    lv_obj_t *eh = lv_obj_create(rc);
    lv_obj_set_size(eh, UI_RIGHT_COL_W, 32);
    lv_obj_set_style_bg_color(eh, UI_BG_CARD, 0);
    lv_obj_set_style_bg_opa(eh, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(eh, UI_BORDER, 0);
    lv_obj_set_style_border_width(eh, 1, 0);
    lv_obj_set_style_pad_hor(eh, 12, 0);
    lv_obj_set_flex_flow(eh, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(eh, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *et = lv_label_create(eh);
    lv_label_set_text(et, "事件时间轴");
    lv_obj_set_style_text_font(et, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(et, UI_TEXT_LO, 0);

    s_lbl_evt_count = lv_label_create(eh);
    lv_label_set_text(s_lbl_evt_count, "0 条");
    lv_obj_set_style_text_font(s_lbl_evt_count, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(s_lbl_evt_count, UI_CYAN, 0);
    lv_obj_set_style_bg_color(s_lbl_evt_count, UI_CYAN, 0);
    lv_obj_set_style_bg_opa(s_lbl_evt_count, LV_OPA_10, 0);
    lv_obj_set_style_pad_hor(s_lbl_evt_count, 8, 0);
    lv_obj_set_style_pad_ver(s_lbl_evt_count, 1, 0);

    /* 列表 */
    s_event_list = lv_obj_create(rc);
    lv_obj_set_flex_grow(s_event_list, 1);
    lv_obj_set_width(s_event_list, UI_RIGHT_COL_W);
    lv_obj_set_style_bg_color(s_event_list, UI_BG_CARD, 0);
    lv_obj_set_style_bg_opa(s_event_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_event_list, UI_BORDER, 0);
    lv_obj_set_style_border_width(s_event_list, 1, 0);
    lv_obj_set_style_pad_all(s_event_list, 0, 0);
    lv_obj_set_flex_flow(s_event_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_event_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_event_list, LV_SCROLLBAR_MODE_AUTO);
}

static void build_bottom_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, UI_SCREEN_W, UI_BOTTOM_BAR_H);
    lv_obj_set_style_bg_color(bar, UI_BG_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, UI_BORDER, 0);
    lv_obj_set_style_pad_hor(bar, 20, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 12, 0);

    lv_obj_t *b1 = make_btn(bar, "布防", UI_CYAN, UI_CYAN);
    lv_obj_add_event_cb(b1, on_btn_arm, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b2 = make_btn(bar, "撤防", UI_BORDER_LIGHT, UI_TEXT_LO);
    lv_obj_add_event_cb(b2, on_btn_disarm, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b3 = make_btn(bar, "消警", UI_AMBER, UI_AMBER);
    lv_obj_add_event_cb(b3, on_btn_silence, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b4 = make_btn(bar, "设置", UI_BORDER, UI_TEXT_LO);
    lv_obj_add_event_cb(b4, on_btn_settings, LV_EVENT_CLICKED, NULL);
}

static lv_obj_t *make_btn(lv_obj_t *p, const char *t, lv_color_t bc, lv_color_t tc)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 38);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_bg_color(b, bc, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_10, 0);
    lv_obj_set_style_border_color(b, bc, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(b, 28, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(l, tc, 0);
    lv_obj_center(l);
    return b;
}

static void apply_zone_style(int idx, sm_state_t st)
{
    if (idx < 0 || idx >= SM_ZONE_COUNT || !s_zone_cards[idx]) return;

    lv_obj_set_style_border_color(s_zone_cards[idx], UI_BORDER, 0);

    static const char *stxt[] = {"离线", "在线 · 正常", "布防中 · 监控中", "异常 · 已触发"};
    lv_color_t sc[] = { UI_DOT_OFFLINE, UI_DOT_ONLINE, UI_DOT_ARMED, UI_DOT_ALARM };

    if (s_zone_dots[idx])  lv_obj_set_style_bg_color(s_zone_dots[idx], sc[st], 0);
    if (s_zone_stats[idx]) lv_label_set_text(s_zone_stats[idx], stxt[st]);

    switch (st) {
        case SM_STATE_ARMED:
            lv_obj_set_style_border_color(s_zone_cards[idx], UI_GREEN, 0);
            break;
        case SM_STATE_ALARM:
            lv_obj_set_style_border_color(s_zone_cards[idx], UI_RED, 0);
            break;
        default:
            break;
    }
}

/* ==================== 事件回调 ==================== */

static void on_btn_arm(lv_event_t *e)
{
    (void)e;
    sm_arm_all();
    if (s_lbl_arm_state) {
        lv_label_set_text(s_lbl_arm_state, "已布防");
    }
    for (int i = 0; i < SM_ZONE_COUNT; i++)
        apply_zone_style(i, sm_get_zone_state(i));
}

static void on_btn_disarm(lv_event_t *e)
{
    (void)e;
    sm_disarm_all();
    if (s_lbl_arm_state) {
        lv_label_set_text(s_lbl_arm_state, "已撤防");
    }
    for (int i = 0; i < SM_ZONE_COUNT; i++)
        apply_zone_style(i, sm_get_zone_state(i));
}

static void on_btn_silence(lv_event_t *e)
{
    (void)e;
    sm_ack_alarm();
    for (int i = 0; i < SM_ZONE_COUNT; i++)
        apply_zone_style(i, sm_get_zone_state(i));
}

static void on_btn_settings(lv_event_t *e)
{
    (void)e;
    ui_dialog_settings_show();
}

static void on_zone_card(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ui_dialog_zone_detail_show(idx);
}

static void on_cam_pill(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ui_home_switch_cam(idx);
}

/* ==================== 定时器回调 ==================== */

static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_lbl_clock) return;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char buf[24];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    lv_label_set_text(s_lbl_clock, buf);
}

static void anim_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_anim_tick++;

    for (int i = 0; i < SM_ZONE_COUNT; i++) {
        if (!s_zone_cards[i]) continue;
        sm_state_t st = sm_get_zone_state(i);
        if (st == SM_STATE_ARMED) {
            /* 绿光呼吸: opa 在 OPA_10 ~ OPA_40 间循环 */
            int cycle = s_anim_tick % 50;
            lv_opa_t opa = (cycle < 25) ?
                (lv_opa_t)(LV_OPA_10 + (LV_OPA_40 - LV_OPA_10) * cycle / 25) :
                (lv_opa_t)(LV_OPA_40 - (LV_OPA_40 - LV_OPA_10) * (cycle - 25) / 25);
            lv_obj_set_style_bg_opa(s_zone_cards[i], opa, 0);
        } else if (st == SM_STATE_ALARM) {
            /* 红色闪烁: 快速明灭 */
            int blink = (s_anim_tick / 3) % 2;
            lv_obj_set_style_bg_opa(s_zone_cards[i],
                blink ? LV_OPA_30 : LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_zone_cards[i], UI_RED, 0);
        } else {
            lv_obj_set_style_bg_opa(s_zone_cards[i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_zone_cards[i], UI_BG_CARD, 0);
        }
    }
}
