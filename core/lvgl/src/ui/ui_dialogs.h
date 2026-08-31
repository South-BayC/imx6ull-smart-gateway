/**
 * @file    ui_dialogs.h
 * @brief   Smart security gateway - dialog system
 * @date    2026-08-22
 */
#ifndef UI_DIALOGS_H
#define UI_DIALOGS_H
#include "lvgl.h"
#include "state_machine.h"
#ifdef __cplusplus
extern "C" {
#endif
void ui_dialog_zone_detail_show(int zone_idx);
void ui_dialog_album_show(const char *title);
void ui_dialog_settings_show(void);
void ui_dialog_close_all(void);
#ifdef __cplusplus
}
#endif
#endif /* UI_DIALOGS_H */
