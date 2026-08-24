/**
 * ui_events.h — 真实状态信号接入接口层
 *
 * 供板端真实信号源（传感器/摄像头/网络/RTC）调用，
 * 驱动整个安防网关 UI 的状态显示、事件时间轴、抓拍相册。
 *
 * 接入示例（真实传感器中断）：
 *   ui_events_zone_set_state(0, ZONE_ALARM);   // 前门告警
 *   ui_events_alarm_trigger(0);                // 或直接触发告警（含事件记录）
 *
 * 所有函数内部均做 NULL/越界检查，线程安全由调用方保证
 * （建议在 LVGL 主循环线程内调用，或在回调中通过 lv_async_call 投递）。
 */
#ifndef UI_EVENTS_H
#define UI_EVENTS_H

#include "../state_machine.h"   /* zone_state_t: ZONE_ONLINE / ZONE_ARMED / ZONE_ALARM / ZONE_OFFLINE */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 设置分区状态（真实传感器信号入口）
 * @param id  分区索引 0-3（0=前门 1=后门 2=窗户 3=仓库）
 * @param st  新状态（ZONE_ONLINE / ZONE_ARMED / ZONE_ALARM / ZONE_OFFLINE）
 * 自动刷新分区卡片 UI（边框/圆点/状态文字）。
 */
void ui_events_zone_set_state(int id, zone_state_t st);

/**
 * 触发告警（真实告警信号入口）
 * @param id 分区索引 0-3
 * 将该分区置为 ZONE_ALARM 并写入事件时间轴（红色，级别 high）。
 */
void ui_events_alarm_trigger(int id);

/**
 * 触发告警（带触发源类型）
 * @param id  分区索引 0-3
 * @param src 触发类型标识："SENSOR"（传感器触发）/ "CAM"（摄像头预警，预留）
 * 事件标题显示为 "SENSOR 触发" / "CAM 触发"。
 */
void ui_events_alarm_trigger_src(int id, const char *src);

/**
 * 消除告警（真实消警信号入口）
 * @param id 分区索引 0-3
 * 恢复为布防中（若已布防）或在线，并写入事件时间轴。
 */
void ui_events_alarm_ack(int id);

/**
 * 事件时间轴追加（任何系统事件入口）
 * @param title 事件标题（须为 SHSC_16 字库内字符，否则显示方框）
 * @param loc   位置（"前门"/"主控"/"云端"/"CH01" 等）
 * @param level 级别："" / "high" / "medium" / "low"
 * 自动插入时间轴顶部、更新计数（最多保留 25 条）。
 */
void ui_events_log(const char *title, const char *loc, const char *level);

/**
 * 设置联网状态（真实网络信号入口）
 * @param online 1=在线 0=离线
 * 更新状态栏联网圆点颜色与文字，并在状态变化时写入事件时间轴。
 */
void ui_events_net_set(int online);

/**
 * 设置系统时间（真实 RTC 信号入口，每秒调用）
 * @param h 时 0-23
 * @param m 分 0-59
 * @param s 秒 0-59
 * 更新状态栏时钟显示（内部以 14:31:00 为基准偏移，可被真实时间覆盖）。
 */
void ui_events_set_time(int h, int m, int s);

/**
 * 查询布防状态（任一分区布防即为已布防）
 * @return 1=任一分区处于 ZONE_ARMED 0=全部分区未布防
 */
int ui_events_is_armed(void);

/**
 * 查询是否有任一分区处于布防（LED 联动/呼吸动画/定时布防用）
 * @return 1=任一分区处于 ZONE_ARMED
 */
int ui_events_zone_has_armed(void);

/**
 * 查询单个分区是否布防（分区级预警触发门槛）
 * @param id 分区索引 0-3
 * @return 1=该分区 ZONE_ARMED
 */
int ui_events_zone_is_armed(int id);

/**
 * 查询当前预览通道对应的分区索引（单摄分时切换语义）
 * @return 分区索引 0-3；异常返回 -1
 * 运动粗判命中时，告警归属当前预览通道对应的分区
 */
int ui_events_current_cam_zone(void);

/**
 * 查询是否有任一分区处于告警（外设联动用，如蜂鸣器）
 * @return 1=有告警 0=无告警
 */
int ui_events_zone_has_alarm(void);

/**
 * 查询当前告警的最高级别（分级声光策略用）
 * @return 0=无告警 1=低（门）2=中（仓库）3=高（窗/周界）
 * 映射按设计文档 2.2：门=低、仓库=中、窗/周界=高；多分区同时告警取最高
 */
int ui_events_zone_alarm_level(void);

/**
 * 用户活动通知（触摸/按键唤醒统一入口）
 * 重置屏幕空闲计时并解除 blank（屏幕休眠期间按 KEY0/触摸即唤醒）
 */
void ui_events_user_activity(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_EVENTS_H */
