/**
 * ui_widgets.c — 可复用 LVGL 小部件辅助函数
 */
#include "ui_widgets.h"
#include <string.h>

/* 声明中文字库 */
LV_FONT_DECLARE(lv_font_SHSC_16);
LV_FONT_DECLARE(lv_font_SHSC_20);

/* ---- 颜色宏（与 ui_home.c 保持一致） ---- */
#define W_CLR_CYAN   0x00D1FF
#define W_CLR_BORDER 0x253656

/* ================================================================
 * 弹窗关闭回调（隐藏传入的 overlay 对象）
 * ================================================================ */
static void _modal_close_event_cb(lv_event_t *e)
{
    lv_obj_t *overlay = (lv_obj_t *)lv_event_get_user_data(e);
    if (overlay) lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

/* ================================================================
 * 弹窗遮罩背景点击关闭（与 HTML overlay click-to-close 一致）
 * 仅当点击目标是遮罩本身（非弹窗框内子对象）时关闭
 * ================================================================ */
static void _modal_overlay_click_cb(lv_event_t *e)
{
    lv_obj_t *overlay = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_t *target  = lv_event_get_target(e);
    if (target == overlay && overlay) {
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ================================================================
 * uiw_obj — 透明零内边距容器
 * ================================================================ */
lv_obj_t *uiw_obj(lv_obj_t *par)
{
    if (!par) return NULL;
    lv_obj_t *o = lv_obj_create(par);
    if (!o) return NULL;
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

/* ================================================================
 * uiw_label — 带色标签（SHSC_16）
 * ================================================================ */
lv_obj_t *uiw_label(lv_obj_t *par, const char *txt, uint32_t hex)
{
    return uiw_label_font(par, txt, hex, &lv_font_SHSC_16);
}

lv_obj_t *uiw_label_font(lv_obj_t *par, const char *txt, uint32_t hex,
                          const lv_font_t *font)
{
    if (!par || !txt) return NULL;
    lv_obj_t *l = lv_label_create(par);
    if (!l) return NULL;
    lv_label_set_text(l, txt);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    if (hex) lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
    return l;
}

/* ================================================================
 * uiw_dot — 彩色圆点
 * ================================================================ */
lv_obj_t *uiw_dot(lv_obj_t *par, uint32_t hex, int size)
{
    if (!par) return NULL;
    lv_obj_t *d = lv_obj_create(par);
    if (!d) return NULL;
    lv_obj_set_size(d, size, size);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(hex), 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    return d;
}

/* ================================================================
 * uiw_op_btn — 底栏操作按钮
 * ================================================================ */
lv_obj_t *uiw_op_btn(lv_obj_t *par, const char *txt,
                      uint32_t bg, uint32_t bd, uint32_t tx,
                      lv_opa_t opa, int w)
{
    if (!par || !txt) return NULL;
    lv_obj_t *btn = lv_button_create(par);
    if (!btn) return NULL;
    lv_obj_set_size(btn, w, 34);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, opa, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(bd), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);

    lv_obj_t *bl = uiw_label(btn, txt, tx);
    if (bl) lv_obj_center(bl);
    return btn;
}

/* ================================================================
 * uiw_modal_create — 模态弹窗
 *
 * 创建全屏半透明遮罩 + 居中弹窗框，返回内容区对象
 * ================================================================ */
lv_obj_t *uiw_modal_create(lv_obj_t *parent, const char *title, int w)
{
    if (!parent || !title) return NULL;

    /* 全屏遮罩：创建在 lv_layer_top()（LVGL 独立顶层），
     * 避免成为主界面 flex 布局的子项而被排到屏幕下方（弹窗不弹出问题） */
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    if (!overlay) return NULL;
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x060A14), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_80, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(overlay, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    /* 点击遮罩背景关闭弹窗（HTML .modal-overlay click 行为） */
    lv_obj_add_event_cb(overlay, _modal_overlay_click_cb, LV_EVENT_CLICKED, overlay);

    /* 弹窗框 */
    lv_obj_t *box = lv_obj_create(overlay);
    if (!box) { lv_obj_delete(overlay); return NULL; }
    lv_obj_set_width(box, w);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1B2A4A), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(W_CLR_BORDER), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);

    /* 标题栏 */
    lv_obj_t *head = uiw_obj(box);
    if (head) {
        lv_obj_set_size(head, w, 40);
        lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_side(head, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(head, lv_color_hex(W_CLR_BORDER), 0);
        lv_obj_set_style_border_width(head, 1, 0);
        lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(head, 16, 0);
        lv_obj_set_style_pad_right(head, 16, 0);

        uiw_label_font(head, title, 0xE6EDF7, &lv_font_SHSC_20);

        /* 关闭按钮 */
        lv_obj_t *close_btn = lv_button_create(head);
        if (close_btn) {
            lv_obj_set_size(close_btn, 28, 28);
            lv_obj_set_style_bg_opa(close_btn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(close_btn, lv_color_hex(W_CLR_BORDER), 0);
            lv_obj_set_style_border_width(close_btn, 1, 0);
            lv_obj_set_style_radius(close_btn, 0, 0);
            lv_obj_set_style_pad_all(close_btn, 0, 0);
            /* 关闭按钮：字库无 LV_SYMBOL_CLOSE 字形，用 ASCII "X" 替代避免乱码 */
            lv_obj_t *cl = uiw_label(close_btn, "X", 0x8A9AB5);
            if (cl) lv_obj_center(cl);
            /* 点击关闭：隐藏整个弹窗 */
            lv_obj_add_event_cb(close_btn, _modal_close_event_cb,
                               LV_EVENT_CLICKED, overlay);
            lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    /* 内容区 */
    lv_obj_t *body = uiw_obj(box);
    if (body) {
        lv_obj_set_width(body, w);
        lv_obj_set_height(body, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(body, 16, 0);
        lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    }

    return overlay;
}
