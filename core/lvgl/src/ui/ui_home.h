/**
 * @file    ui_home.h
 * @brief   智能安防网关 - 主界面布局与交互
 * @author  LVGL UI 专家
 * @date    2026-08-22
 *
 * 屏幕布局（1024x600）：
 *   ┌─────────────────────────────────────────────────────┐
 *   │ 顶栏 (36px): 系统名称 · 时钟 · 联网/布防状态        │
 *   ├───────────────────────────────┬─────────────────────┤
 *   │                               │ 事件时间轴           │
 *   │ 摄像头预览区 (630x310)        │ (360px 宽)          │
 *   │ 单摄4通道切换                 │ 20 条滚动事件        │
 *   ├───────────────────────────────┤                     │
 *   │ 2x2 分区卡片                  │                     │
 *   │ 前门 | 后门                   │                     │
 *   │ 窗户 | 仓库                   │                     │
 *   ├───────────────────────────────┴─────────────────────┤
 *   │ 底栏 (52px): 布防 | 撤防 | 消警 | 设置              │
 *   └─────────────────────────────────────────────────────┘
 *
 * 设计令牌对齐 Mockup：
 *   - 背景 #0B1220，卡片 #1B2A4A，边框 #253656
 *   - 文字 #E6EDF7(亮) / #8A9AB5(暗) / #5A6A85(更暗)
 *   - 强调色：青 #00D1FF / 绿 #20C997 / 琥珀 #FFB020 / 红 #FF4D4F
 *   - 中文字体：lv_font_SHSC_16 (正文) / lv_font_SHSC_20 (标题)
 */

#ifndef UI_HOME_H
#define UI_HOME_H

#include "lvgl.h"
#include "state_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 设计令牌（颜色） ==================== */

/** 背景色 */
#define UI_BG_PRIMARY       lv_color_hex(0x0B1220)
#define UI_BG_CARD          lv_color_hex(0x1B2A4A)
#define UI_BG_CARD_ALT      lv_color_hex(0x162240)

/** 边框色 */
#define UI_BORDER           lv_color_hex(0x253656)
#define UI_BORDER_LIGHT     lv_color_hex(0x2E4268)

/** 文字色 */
#define UI_TEXT_HI          lv_color_hex(0xE6EDF7)
#define UI_TEXT_LO          lv_color_hex(0x8A9AB5)
#define UI_TEXT_DIM         lv_color_hex(0x5A6A85)

/** 功能强调色 */
#define UI_CYAN             lv_color_hex(0x00D1FF)
#define UI_GREEN            lv_color_hex(0x20C997)
#define UI_AMBER            lv_color_hex(0xFFB020)
#define UI_RED              lv_color_hex(0xFF4D4F)

/** 状态指示色 */
#define UI_DOT_ONLINE       UI_GREEN
#define UI_DOT_ARMED        UI_GREEN
#define UI_DOT_ALARM        UI_RED
#define UI_DOT_OFFLINE      UI_TEXT_DIM

/* ==================== 设计令牌（字体） ==================== */

/** 中文字体 - 16px 正文 */
#define UI_FONT_CN16        (&lv_font_SHSC_16)
/** 中文字体 - 20px 标题 */
#define UI_FONT_CN20        (&lv_font_SHSC_20)

/* ==================== 设计令牌（布局） ==================== */

/** 屏幕尺寸 */
#define UI_SCREEN_W         1024
#define UI_SCREEN_H         600

/** 顶栏高度 */
#define UI_STATUS_BAR_H     36
/** 底栏高度 */
#define UI_BOTTOM_BAR_H     52
/** 摄像头预览区高度 */
#define UI_CAM_PREVIEW_H    310
/** 左侧列宽 */
#define UI_LEFT_COL_W       630
/** 右侧列宽 */
#define UI_RIGHT_COL_W      360
/** 卡片间距 */
#define UI_GAP              6
/** 内边距 */
#define UI_PADDING          8

/** 时间轴最大事件条数 */
#define UI_EVENT_MAX        20

/** 摄像头通道数 */
#define UI_CAM_COUNT        4

/* ==================== 数据结构 ==================== */

/**
 * @brief 事件时间轴条目（UI 层显示用）
 */
typedef struct {
    sm_event_type_t type;       /**< 事件类型 */
    sm_level_t      level;      /**< 告警级别 */
    const char     *title;      /**< 事件描述 */
    const char     *location;   /**< 事件位置 */
    uint8_t         hour;       /**< 时 */
    uint8_t         minute;     /**< 分 */
} ui_event_t;

/**
 * @brief 摄像头通道信息
 */
typedef struct {
    const char *name;           /**< 通道名称：前门/后门/仓库/周界 */
    const char *ch;             /**< 通道编号：CH01 ~ CH04 */
} ui_cam_info_t;

/* ==================== 公共 API ==================== */

/**
 * @brief 初始化主界面
 *
 * 执行流程：
 *   1. 创建屏幕根对象
 *   2. 构建顶栏、主区域（左+右）、底栏
 *   3. 注册状态机回调，同步初始状态
 *
 * @param parent  父对象（通常为 lv_screen_active()）
 * @note   调用前必须先调用 sm_init() 初始化状态机
 */
void ui_home_init(lv_obj_t *parent);

/**
 * @brief 刷新顶栏时钟显示
 *
 * 读取系统时间并更新顶栏中央时钟标签
 * 应由外部定时器每秒调用一次
 */
void ui_home_refresh_clock(void);

/**
 * @brief 更新指定防区卡片的显示状态
 *
 * 根据状态机状态更新卡片边框颜色、呼吸/闪烁动画、状态文字
 *
 * @param zone_idx  防区索引 (0~3)
 * @param state     新状态
 */
void ui_home_update_zone(int zone_idx, sm_state_t state);

/**
 * @brief 向时间轴添加新事件
 *
 * 在时间轴顶部插入新事件条目，自动移除超限的旧条目
 * 支持淡入动画效果
 *
 * @param event  事件数据
 */
void ui_home_add_event(const ui_event_t *event);

/**
 * @brief 切换摄像头预览通道
 *
 * 执行淡出 → 切换内容 → 淡入动画
 * 同时高亮对应的分区卡片
 *
 * @param cam_idx  摄像头索引 (0~3)
 */
void ui_home_switch_cam(int cam_idx);

/**
 * @brief 获取当前摄像头索引
 * @return  0~3
 */
int ui_home_get_current_cam(void);

/**
 * @brief 显示抓拍成功提示
 *
 * 在屏幕底部居中显示绿色提示条，1.5 秒后自动消失
 */
void ui_home_show_capture_toast(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_HOME_H */
