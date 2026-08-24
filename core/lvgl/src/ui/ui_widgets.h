/**
 * ui_widgets.h — 可复用 LVGL 小部件辅助函数
 *
 * 提供透明容器、彩色标签、彩色圆点、操作按钮等基础构建块。
 * 所有函数内部做 NULL 检查，安全可靠。
 */
#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

#include "lvgl/lvgl.h"

/**
 * 创建透明零内边距容器
 * @param par  父对象
 * @return 新对象或 NULL
 */
lv_obj_t *uiw_obj(lv_obj_t *par);

/**
 * 创建带色标签（使用 SHSC_16 字体）
 * @param par  父对象
 * @param txt  文本（不允许 NULL）
 * @param hex  颜色 HEX 值
 * @return 标签对象或 NULL
 */
lv_obj_t *uiw_label(lv_obj_t *par, const char *txt, uint32_t hex);

/**
 * 创建带色标签（指定字体）
 */
lv_obj_t *uiw_label_font(lv_obj_t *par, const char *txt, uint32_t hex,
                          const lv_font_t *font);

/**
 * 创建彩色圆点
 * @param par  父对象
 * @param hex  颜色 HEX 值
 * @param size 圆点直径
 * @return 圆点对象或 NULL
 */
lv_obj_t *uiw_dot(lv_obj_t *par, uint32_t hex, int size);

/**
 * 创建底栏操作按钮
 * @param par   父对象
 * @param txt   按钮文字
 * @param bg    背景色 HEX
 * @param bd    边框色 HEX
 * @param tx    文字色 HEX
 * @param opa   背景透明度
 * @param w     按钮宽度
 * @return 按钮对象或 NULL
 */
lv_obj_t *uiw_op_btn(lv_obj_t *par, const char *txt,
                      uint32_t bg, uint32_t bd, uint32_t tx,
                      lv_opa_t opa, int w);

/**
 * 创建弹窗（模态对话框）
 * @param parent  全屏遮罩父对象
 * @param title   弹窗标题
 * @param w       弹窗宽度
 * @return 弹窗遮罩对象（隐藏状态，清除 HIDDEN 标志即可显示）或 NULL
 */
lv_obj_t *uiw_modal_create(lv_obj_t *parent, const char *title, int w);

#endif /* UI_WIDGETS_H */
