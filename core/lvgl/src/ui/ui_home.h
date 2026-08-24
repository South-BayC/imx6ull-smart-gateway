/**
 * ui_home.h — 1024x600 智能安防边缘网关主页
 *
 * 布局结构 (flex-col):
 *   info-bar   22px  (顶部说明条)
 *   status-bar 36px  (标题 · 时钟 · 联网/布防状态)
 *   main-area  flex:1  flex-row
 *   |  left-col   630px  (摄像头预览 + 2×2分区卡片)
 *   |  right-col  360px  (事件时间轴)
 *   bottom-bar 52px  (操作按钮)
 *
 * 交互:
 *   布防 → 分区卡片绿色呼吸动画
 *   撤防 → 卡片恢复正常
 *   消警 → 清除告警状态
 *   点击分区卡片 → 弹出详情
 *   设置 → 弹出设置面板
 *   摄像头切换 → 预览区切换画面
 *   抓拍 → Toast 提示
 */
#ifndef UI_HOME_H
#define UI_HOME_H

#include "lvgl/lvgl.h"

/* ========== 全局入口 ========== */
void ui_home_create(lv_obj_t *parent);

#endif /* UI_HOME_H */
