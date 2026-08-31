/**
 * @file    state_machine.h
 * @brief   智能安防网关 - 防区状态机管理
 * @author  LVGL UI 专家
 * @date    2026-08-22
 *
 * 状态机与 UI 分离设计：
 *   - 管理 4 个防区的在线/布防/异常/离线状态
 *   - 提供全局布防/撤防/消警操作
 *   - 通过回调函数通知 UI 层状态变化
 *   - 支持模拟告警事件（演示用）
 *
 * 设计令牌（对齐 Mockup 深色工业调色板）：
 *   - 状态机仅管理数据，不操作任何 LVGL 控件
 *   - UI 层通过回调获取状态变化并更新界面
 */

#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 常量定义 ==================== */

/** 防区数量 */
#define SM_ZONE_COUNT 4

/** 事件时间轴最大条数 */
#define SM_EVENT_MAX 20

/** 最大回调注册数 */
#define SM_MAX_CALLBACKS 4

/* ==================== 枚举类型 ==================== */

/**
 * @brief 防区状态枚举
 *
 * 状态转换规则：
 *   online  → armed   (布防)
 *   armed   → online  (撤防)
 *   armed   → alarm   (告警触发)
 *   alarm   → armed   (消警，若仍布防)
 *   alarm   → online  (消警，若已撤防)
 *   any     → offline (设备离线)
 *   offline → online  (设备恢复)
 */
typedef enum {
    SM_STATE_OFFLINE = 0,   /**< 设备离线 - 灰色 */
    SM_STATE_ONLINE,        /**< 在线正常 - 青色 */
    SM_STATE_ARMED,         /**< 布防中   - 绿色呼吸 */
    SM_STATE_ALARM          /**< 异常告警 - 红色闪烁 */
} sm_state_t;

/**
 * @brief 告警级别
 */
typedef enum {
    SM_LEVEL_NONE = 0,      /**< 无告警 */
    SM_LEVEL_LOW,           /**< 低级 - 信号弱/存储不足 */
    SM_LEVEL_MEDIUM,        /**< 中级 - 设备离线/CPU高温 */
    SM_LEVEL_HIGH           /**< 高级 - 入侵检测/异常闯入 */
} sm_level_t;

/**
 * @brief 事件类型（用于时间轴）
 */
typedef enum {
    SM_EVT_INFO = 0,        /**< 信息事件 - 青色 */
    SM_EVT_OK,              /**< 正常事件 - 绿色 */
    SM_EVT_WARN,            /**< 警告事件 - 琥珀色 */
    SM_EVT_ERROR            /**< 错误事件 - 红色 */
} sm_event_type_t;

/* ==================== 数据结构 ==================== */

/**
 * @brief 单个防区信息
 */
typedef struct {
    const char  *name;          /**< 防区名称（中文）：前门/后门/窗户/仓库 */
    const char  *id;            /**< 防区编号：Z-001 ~ Z-004 */
    const char  *sensor;        /**< 传感器类型：PIR+门磁/门窗磁/PIR+摄像 */
    const char  *ip;            /**< 设备 IP 地址 */
    const char  *fw;            /**< 固件版本 */
    const char  *rssi;          /**< 信号强度：-42dBm */
    sm_state_t   state;         /**< 当前状态 */
    sm_level_t   alarm_level;   /**< 当前告警级别 */
} sm_zone_t;

/**
 * @brief 事件时间轴条目
 */
typedef struct {
    sm_event_type_t type;       /**< 事件类型 */
    sm_level_t      level;      /**< 告警级别（仅 warn/error 有效） */
    const char     *title;      /**< 事件描述（中文） */
    const char     *location;   /**< 事件位置 */
    uint8_t         hour;       /**< 时 */
    uint8_t         minute;     /**< 分 */
} sm_event_t;

/**
 * @brief 状态变化回调函数类型
 *
 * @param zone_idx  变化的防区索引 (0~3)，-1 表示全局变化
 * @param new_state 新状态
 * @param user_data 用户数据指针
 */
typedef void (*sm_state_cb_t)(int zone_idx, sm_state_t new_state, void *user_data);

/**
 * @brief 事件变化回调函数类型
 *
 * @param event     新增事件指针
 * @param user_data 用户数据指针
 */
typedef void (*sm_event_cb_t)(const sm_event_t *event, void *user_data);

/* ==================== 公共 API ==================== */

/**
 * @brief 初始化状态机
 *
 * 执行流程：
 *   1. 初始化 4 个防区为在线状态
 *   2. 清空事件缓冲区
 *   3. 注册默认系统启动事件
 *
 * @note  调用 ui_home_init() 之前必须先调用此函数
 */
void sm_init(void);

/**
 * @brief 全局布防
 *
 * 将所有在线防区从 ONLINE 转为 ARMED 状态
 * 已离线的防区保持 OFFLINE
 * 添加"全部分区已布防"事件
 */
void sm_arm_all(void);

/**
 * @brief 全局撤防
 *
 * 将所有防区从 ARMED/ALARM 转为 ONLINE 状态
 * 添加"全部分区已撤防"事件
 */
void sm_disarm_all(void);

/**
 * @brief 消除告警
 *
 * 将所有 ALARM 状态的防区恢复：
 *   - 若全局已布防 → 恢复为 ARMED
 *   - 若全局已撤防 → 恢复为 ONLINE
 * 添加"告警已手动消除"事件
 *
 * @return  true 有告警被消除，false 当前无告警
 */
bool sm_ack_alarm(void);

/**
 * @brief 获取全局布防状态
 * @return  true 已布防，false 已撤防
 */
bool sm_is_armed(void);

/**
 * @brief 获取防区数量
 * @return  固定值 SM_ZONE_COUNT (4)
 */
int sm_get_zone_count(void);

/**
 * @brief 获取指定防区信息
 * @param idx  防区索引 (0~3)
 * @return     防区信息指针，越界返回 NULL
 */
const sm_zone_t *sm_get_zone(int idx);

/**
 * @brief 获取指定防区当前状态
 * @param idx  防区索引 (0~3)
 * @return     防区状态，越界返回 SM_STATE_OFFLINE
 */
sm_state_t sm_get_zone_state(int idx);

/**
 * @brief 获取事件数量
 * @return  当前事件条数 (0~SM_EVENT_MAX)
 */
int sm_get_event_count(void);

/**
 * @brief 获取指定索引的事件
 * @param idx  事件索引 (0 为最新)
 * @return     事件指针，越界返回 NULL
 */
const sm_event_t *sm_get_event(int idx);

/**
 * @brief 注册状态变化回调
 * @param cb         回调函数
 * @param user_data  用户数据
 * @return           0 成功，-1 回调表已满
 */
int sm_register_state_cb(sm_state_cb_t cb, void *user_data);

/**
 * @brief 注册事件变化回调
 * @param cb         回调函数
 * @param user_data  用户数据
 * @return           0 成功，-1 回调表已满
 */
int sm_register_event_cb(sm_event_cb_t cb, void *user_data);

/**
 * @brief 模拟随机告警（演示用）
 *
 * 随机选择一个 ARMED 状态的防区触发 ALARM
 * 仅在已布防状态下有效
 */
void sm_simulate_alarm(void);

#ifdef __cplusplus
}
#endif

#endif /* STATE_MACHINE_H */
