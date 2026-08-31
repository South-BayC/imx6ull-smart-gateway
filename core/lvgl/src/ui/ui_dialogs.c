/**
 * @file    ui_dialogs.c
 * @brief   Smart security gateway - dialog implementation
 * @date    2026-08-22
 */
#include "ui_dialogs.h"
#include "ui_home.h"
#include <stdio.h>
#include <string.h>

/* ===== Overlay and dialog box ===== */
static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_dlg_box = NULL;

/* ===== Helpers ===== */
static void close_dlg_cb(lv_event_t *e)
{
    lv_obj_t *dlg = (lv_obj_t *)lv_event_get_user_data(e);
    (void)e;
    if (dlg) lv_obj_add_flag(dlg, LV_OBJ_FLAG_HIDDEN);
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void on_overlay_click(lv_event_t *e)
{
    (void)e;
    if (s_dlg_box) lv_obj_add_flag(s_dlg_box, LV_OBJ_FLAG_HIDDEN);
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void ensure_overlay(void)
{
    if (s_overlay) return;
    lv_obj_t *scr = lv_screen_active();

    /* Full-screen overlay */
    s_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_overlay, 1024, 600);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_overlay, on_overlay_click, LV_EVENT_CLICKED, NULL);

    /* Dialog box */
    s_dlg_box = lv_obj_create(s_overlay);
    lv_obj_set_size(s_dlg_box, 420, 400);
    lv_obj_set_style_bg_color(s_dlg_box, lv_color_hex(0x1B2A4A), 0);
    lv_obj_set_style_bg_opa(s_dlg_box, LV_OPA_95, 0);
    lv_obj_set_style_border_color(s_dlg_box, lv_color_hex(0x253656), 0);
    lv_obj_set_style_border_width(s_dlg_box, 1, 0);
    lv_obj_set_style_border_opa(s_dlg_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dlg_box, 0, 0);
    lv_obj_set_style_pad_all(s_dlg_box, 0, 0);
    lv_obj_set_flex_flow(s_dlg_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(s_dlg_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_dlg_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_dlg_box, LV_OBJ_FLAG_CLICKABLE);
}

/* Clear all children from dialog body */
static void clear_body(void)
{
    if (!s_dlg_box) return;
    uint32_t cnt = lv_obj_get_child_count(s_dlg_box);
    /* Remove all but keep the box itself */
    for (uint32_t i = cnt; i > 0; i--) {
        lv_obj_t *child = lv_obj_get_child(s_dlg_box, i - 1);
        if (child) lv_obj_delete(child);
    }
}

/* Create header bar with title and close button */
static lv_obj_t *make_header(const char *title)
{
    lv_obj_t *hdr = lv_obj_create(s_dlg_box);
    lv_obj_set_size(hdr, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x1B2A4A), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, lv_color_hex(0x253656), 0);
    lv_obj_set_style_pad_hor(hdr, 16, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *tl = lv_label_create(hdr);
    lv_label_set_text(tl, title);
    lv_obj_set_style_text_font(tl, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(0xE6EDF7), 0);

    /* Close button */
    lv_obj_t *cb = lv_btn_create(hdr);
    lv_obj_set_size(cb, 28, 28);
    lv_obj_set_style_bg_opa(cb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(cb, lv_color_hex(0x253656), 0);
    lv_obj_set_style_border_width(cb, 1, 0);
    lv_obj_set_style_radius(cb, 0, 0);
    lv_obj_set_style_pad_all(cb, 0, 0);
    lv_obj_t *cx = lv_label_create(cb);
    lv_label_set_text(cx, "\xc3\x97");
    lv_obj_set_style_text_font(cx, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(cx, lv_color_hex(0x8A9AB5), 0);
    lv_obj_center(cx);
    lv_obj_add_event_cb(cb, close_dlg_cb, LV_EVENT_CLICKED, s_dlg_box);
    return hdr;
}

/* Create scrollable body area */
static lv_obj_t *make_body(void)
{
    lv_obj_t *body = lv_obj_create(s_dlg_box);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    return body;
}

/* Add a label row: label on left, value on right */
static void add_row(lv_obj_t *body, const char *label, const char *value, lv_color_t val_clr)
{
    lv_obj_t *row = lv_obj_create(body);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *ll = lv_label_create(row);
    lv_label_set_text(ll, label);
    lv_obj_set_style_text_font(ll, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(ll, lv_color_hex(0x5A6A85), 0);

    lv_obj_t *vl = lv_label_create(row);
    lv_label_set_text(vl, value);
    lv_obj_set_style_text_font(vl, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(vl, val_clr, 0);
}

/* Add section title */
static void add_section(lv_obj_t *body, const char *title)
{
    lv_obj_t *t = lv_label_create(body);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x5A6A85), 0);
    lv_obj_set_style_pad_hor(t, 16, 0);
    lv_obj_set_style_pad_top(t, 10, 0);
}

/* Bottom bar with close button */
static void add_close_btn(lv_obj_t *dlg)
{
    lv_obj_t *bar = lv_obj_create(dlg);
    lv_obj_set_size(bar, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1B2A4A), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x253656), 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 16, 0);

    lv_obj_t *btn = lv_btn_create(bar);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x2E4268), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 16, 0);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "\xe5\x85\xb3\xe9\x97\xad");
    lv_obj_set_style_text_font(bl, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0x8A9AB5), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, close_dlg_cb, LV_EVENT_CLICKED, dlg);
}


/* ==================== Zone Detail Dialog ==================== */

void ui_dialog_zone_detail_show(int zone_idx)
{
    if (zone_idx < 0 || zone_idx >= SM_ZONE_COUNT) return;
    const sm_zone_t *z = sm_get_zone(zone_idx);
    if (!z) return;

    ensure_overlay();
    clear_body();

    /* Header */
    char title_buf[48];
    snprintf(title_buf, sizeof(title_buf), "%s \xc2\xb7 \xe5\x88\x86\xe5\x8c\xba\xe8\xaf\xa6\xe6\x83\x85", z->name);
    make_header(title_buf);

    /* Body */
    lv_obj_t *body = make_body();

    /* State string and color */
    const char *state_str = "";
    lv_color_t state_clr = lv_color_hex(0x00D1FF);
    switch (z->state) {
        case SM_STATE_OFFLINE: state_str = "\xe7\xa6\xbb\xe7\xba\xbf"; state_clr = lv_color_hex(0x5A6A85); break;
        case SM_STATE_ONLINE:  state_str = "\xe5\x9c\xa8\xe7\xba\xbf"; state_clr = lv_color_hex(0x00D1FF); break;
        case SM_STATE_ARMED:   state_str = "\xe5\xb8\x83\xe9\x98\xb2\xe4\xb8\xad"; state_clr = lv_color_hex(0x20C997); break;
        case SM_STATE_ALARM:   state_str = "\xe5\x91\x8a\xe8\xad\xa6"; state_clr = lv_color_hex(0xFF4D4F); break;
    }

    add_row(body, "\xe5\x88\x86\xe5\x8c\xba\xe7\xbc\x96\xe5\x8f\xb7", z->id, lv_color_hex(0xE6EDF7));
    add_row(body, "\xe5\x88\x86\xe5\x8c\xba\xe5\x90\x8d\xe7\xa7\xb0", z->name, lv_color_hex(0xE6EDF7));
    add_row(body, "\xe5\xbd\x93\xe5\x89\x8d\xe7\x8a\xb6\xe6\x80\x81", state_str, state_clr);
    add_row(body, "\xe4\xbc\xa0\xe6\x84\x9f\xe5\x99\xa8\xe7\xb1\xbb\xe5\x9e\x8b", z->sensor, lv_color_hex(0xE6EDF7));
    add_row(body, "IP \xe5\x9c\xb0\xe5\x9d\x80", z->ip, lv_color_hex(0xE6EDF7));
    add_row(body, "\xe5\x9b\xba\xe4\xbb\xb6\xe7\x89\x88\xe6\x9c\xac", z->fw, lv_color_hex(0xE6EDF7));
    add_row(body, "\xe4\xbf\xa1\xe5\x8f\xb7\xe5\xbc\xba\xe5\xba\xa6", z->rssi, lv_color_hex(0xE6EDF7));

    /* Close button */
    add_close_btn(s_dlg_box);

    /* Show */
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_dlg_box, LV_OBJ_FLAG_HIDDEN);
}

/* ==================== Album Dialog ==================== */

void ui_dialog_album_show(const char *title)
{
    ensure_overlay();
    clear_body();

    /* Header */
    if (title) {
        char buf[64];
        snprintf(buf, sizeof(buf), "\xe6\x8a\x93\xe6\x8b\x8d\xe7\x9b\xb8\xe5\x86\x8c \xc2\xb7 %s", title);
        make_header(buf);
    } else {
        make_header("\xe6\x8a\x93\xe6\x8b\x8d\xe7\x9b\xb8\xe5\x86\x8c");
    }

    /* Body with 3x2 grid */
    lv_obj_t *body = make_body();
    lv_obj_set_style_pad_all(body, 12, 0);

    /* Simulated thumbnail data */
    static const char *times[] = {"14:32", "14:28", "14:15", "13:58", "13:42", "13:30"};
    static const char *levels[] = {"\xe9\xab\x98", "\xe4\xb8\xad", "\xe4\xbd\x8e", "\xe9\xab\x98", "\xe4\xbd\x8e", "\xe4\xb8\xad"};
    static const lv_color_t lvl_colors[] = {
        { .blue = 0x4F, .green = 0x4D, .red = 0xFF },  /* red */
        { .blue = 0x20, .green = 0xB0, .red = 0xFF },  /* amber */
        { .blue = 0xFF, .green = 0xD1, .red = 0x00 },  /* cyan */
        { .blue = 0x4F, .green = 0x4D, .red = 0xFF },
        { .blue = 0xFF, .green = 0xD1, .red = 0x00 },
        { .blue = 0x20, .green = 0xB0, .red = 0xFF },
    };

    lv_obj_t *grid = lv_obj_create(body);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_set_style_pad_row(grid, 8, 0);

    for (int i = 0; i < 6; i++) {
        lv_obj_t *thumb = lv_obj_create(grid);
        lv_obj_set_size(thumb, (420 - 40) / 3, 80);
        lv_obj_set_style_bg_color(thumb, lv_color_hex(0x162240), 0);
        lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(thumb, lv_color_hex(0x253656), 0);
        lv_obj_set_style_border_width(thumb, 1, 0);
        lv_obj_set_style_radius(thumb, 0, 0);
        lv_obj_set_style_pad_all(thumb, 0, 0);

        /* Thumbnail label */
        lv_obj_t *lbl = lv_label_create(thumb);
        char lbl_buf[24];
        snprintf(lbl_buf, sizeof(lbl_buf), "\xe6\x8a\x93\xe6\x8b\x8d #%d", i + 1);
        lv_label_set_text(lbl, lbl_buf);
        lv_obj_set_style_text_font(lbl, UI_FONT_CN16, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x5A6A85), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

        /* Level badge */
        lv_obj_t *lb = lv_label_create(thumb);
        lv_label_set_text(lb, levels[i]);
        lv_obj_set_style_text_font(lb, UI_FONT_CN16, 0);
        lv_obj_set_style_text_color(lb, lvl_colors[i], 0);
        lv_obj_align(lb, LV_ALIGN_TOP_LEFT, 4, 4);

        /* Time */
        lv_obj_t *tm = lv_label_create(thumb);
        lv_label_set_text(tm, times[i]);
        lv_obj_set_style_text_font(tm, UI_FONT_CN16, 0);
        lv_obj_set_style_text_color(tm, lv_color_hex(0x5A6A85), 0);
        lv_obj_align(tm, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
    }

    add_close_btn(s_dlg_box);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_dlg_box, LV_OBJ_FLAG_HIDDEN);
}

/* ==================== Settings Dialog ==================== */

static lv_obj_t *s_bright_val_lbl = NULL;
static lv_obj_t *s_volume_val_lbl = NULL;

static void on_brightness_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    if (s_bright_val_lbl) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", val);
        lv_label_set_text(s_bright_val_lbl, buf);
    }
    (void)e;
}

static void on_volume_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    if (s_volume_val_lbl) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", val);
        lv_label_set_text(s_volume_val_lbl, buf);
    }
    (void)e;
}

void ui_dialog_settings_show(void)
{
    ensure_overlay();
    clear_body();

    make_header("\xe7\xb3\xbb\xe7\xbb\x9f\xe8\xae\xbe\xe7\xbd\xae");

    lv_obj_t *body = make_body();

    /* --- Display Settings --- */
    add_section(body, "\xe6\x98\xbe\xe7\xa4\xba\xe8\xae\xbe\xe7\xbd\xae");

    lv_obj_t *br = lv_obj_create(body);
    lv_obj_set_size(br, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(br, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(br, 0, 0);
    lv_obj_set_style_pad_hor(br, 16, 0);
    lv_obj_set_style_pad_ver(br, 4, 0);
    lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(br, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *bl = lv_label_create(br);
    lv_label_set_text(bl, "\xe5\xb1\x8f\xe5\xb9\x95\xe4\xba\xae\xe5\xba\xa6");
    lv_obj_set_style_text_font(bl, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0x8A9AB5), 0);

    s_bright_val_lbl = lv_label_create(br);
    lv_label_set_text(s_bright_val_lbl, "80%");
    lv_obj_set_style_text_font(s_bright_val_lbl, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(s_bright_val_lbl, lv_color_hex(0x00D1FF), 0);

    lv_obj_t *bs = lv_slider_create(body);
    lv_obj_set_width(bs, LV_PCT(100));
    lv_obj_set_style_pad_hor(bs, 16, 0);
    lv_slider_set_range(bs, 0, 100);
    lv_slider_set_value(bs, 80, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bs, lv_color_hex(0x253656), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bs, lv_color_hex(0x00D1FF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bs, lv_color_hex(0x00D1FF), LV_PART_KNOB);
    lv_obj_add_event_cb(bs, on_brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* --- Audio Settings --- */
    add_section(body, "\xe9\x9f\xb3\xe9\xa2\x91\xe8\xae\xbe\xe7\xbd\xae");

    lv_obj_t *vr = lv_obj_create(body);
    lv_obj_set_size(vr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(vr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vr, 0, 0);
    lv_obj_set_style_pad_hor(vr, 16, 0);
    lv_obj_set_style_pad_ver(vr, 4, 0);
    lv_obj_set_flex_flow(vr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *vl = lv_label_create(vr);
    lv_label_set_text(vl, "\xe5\x91\x8a\xe8\xad\xa6\xe9\x9f\xb3\xe9\x87\x8f");
    lv_obj_set_style_text_font(vl, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(vl, lv_color_hex(0x8A9AB5), 0);

    s_volume_val_lbl = lv_label_create(vr);
    lv_label_set_text(s_volume_val_lbl, "60%");
    lv_obj_set_style_text_font(s_volume_val_lbl, UI_FONT_CN16, 0);
    lv_obj_set_style_text_color(s_volume_val_lbl, lv_color_hex(0x00D1FF), 0);

    lv_obj_t *vs = lv_slider_create(body);
    lv_obj_set_width(vs, LV_PCT(100));
    lv_obj_set_style_pad_hor(vs, 16, 0);
    lv_slider_set_range(vs, 0, 100);
    lv_slider_set_value(vs, 60, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(vs, lv_color_hex(0x253656), LV_PART_MAIN);
    lv_obj_set_style_bg_color(vs, lv_color_hex(0x00D1FF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vs, lv_color_hex(0x00D1FF), LV_PART_KNOB);
    lv_obj_add_event_cb(vs, on_volume_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* --- Network Status --- */
    add_section(body, "\xe7\xbd\x91\xe7\xbb\x9c\xe7\x8a\xb6\xe6\x80\x81");
    add_row(body, "\xe4\xbb\xa5\xe5\xa4\xaa\xe7\xbd\x91", "\xe5\x9c\xa8\xe7\xba\xbf", lv_color_hex(0x20C997));
    add_row(body, "IP \xe5\x9c\xb0\xe5\x9d\x80", "192.168.1.10", lv_color_hex(0x00D1FF));
    add_row(body, "MQTT \xe6\x9c\x8d\xe5\x8a\xa1\xe5\x99\xa8", "broker.local:1883", lv_color_hex(0x00D1FF));

    /* --- Alert Level Config --- */
    add_section(body, "\xe5\x91\x8a\xe8\xad\xa6\xe5\x88\x86\xe7\xba\xa7\xe9\x85\x8d\xe7\xbd\xae");
    add_row(body, "\xe9\xab\x98", "\xe5\x85\xa5\xe4\xbe\xb5\xe6\xa3\x80\xe6\xb5\x8b\xe3\x80\x81\xe5\xbc\x82\xe5\xb8\xb8\xe9\x97\xae\xe5\x85\xa5", lv_color_hex(0xFF4D4F));
    add_row(body, "\xe4\xb8\xad", "\xe4\xbf\xa1\xe5\x8f\xb7\xe5\xbc\xb1\xe3\x80\x81\xe8\xae\xbe\xe5\xa4\x87\xe7\xa6\xbb\xe7\xba\xbf", lv_color_hex(0xFFB020));
    add_row(body, "\xe4\xbd\x8e", "\xe6\xb8\xa9\xe5\xba\xa6\xe5\x81\x8f\xe9\xab\x98\xe3\x80\x81\xe5\xad\x98\xe5\x82\xa8\xe4\xb8\x8d\xe8\xb6\xb3", lv_color_hex(0x00D1FF));

    /* --- Scheduled Arming --- */
    add_section(body, "\xe5\xae\x9a\xe6\x97\xb6\xe5\xb8\x83\xe9\x98\xb2");
    add_row(body, "22:00-06:00 \xe8\x87\xaa\xe5\x8a\xa8\xe5\xb8\x83\xe9\x98\xb2", "\xe5\xb7\xb2\xe5\x90\xaf\xe7\x94\xa8", lv_color_hex(0x20C997));

    /* --- About --- */
    add_section(body, "\xe5\x85\xb3\xe4\xba\x8e");
    add_row(body, "\xe8\xae\xbe\xe5\xa4\x87\xe5\x9e\x8b\xe5\x8f\xb7", "i.MX6ULL \xe5\xae\x89\xe9\x98\xb2\xe7\xbd\x91\xe5\x85\xb3", lv_color_hex(0x00D1FF));
    add_row(body, "\xe5\x9b\xba\xe4\xbb\xb6\xe7\x89\x88\xe6\x9c\xac", "v2.3.1", lv_color_hex(0x00D1FF));
    add_row(body, "\xe5\x86\x85\xe6\xa0\xb8\xe7\x89\x88\xe6\x9c\xac", "Linux 5.4.0", lv_color_hex(0x00D1FF));
    add_row(body, "\xe5\xad\x98\xe5\x82\xa8\xe7\xa9\xba\xe9\x97\xb4", "14.2GB / 32GB", lv_color_hex(0x00D1FF));

    add_close_btn(s_dlg_box);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_dlg_box, LV_OBJ_FLAG_HIDDEN);
}

/* ==================== Close All ==================== */

void ui_dialog_close_all(void)
{
    if (s_dlg_box)  lv_obj_add_flag(s_dlg_box, LV_OBJ_FLAG_HIDDEN);
    if (s_overlay)  lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}
