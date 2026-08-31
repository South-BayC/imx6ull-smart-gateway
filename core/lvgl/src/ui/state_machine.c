/**
 * @file    state_machine.c
 * @brief   智能安防网关 - 防区状态机实现
 * @author  LVGL UI 专家
 * @date    2026-08-22
 *
 * 纯 C 逻辑层，不依赖任何 LVGL 头文件
 * 通过回调机制通知 UI 层状态变化
 */

#include "state_machine.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ==================== 内部静态变量 ==================== */

/** 4 个防区信息（含名称、编号、传感器类型等固定参数） */
static sm_zone_t s_zones[SM_ZONE_COUNT] = {
    {
        .name      = "前门",
        .id        = "Z-001",
        .sensor    = "PIR+门磁",
        .ip        = "192.168.1.101",
        .fw        = "v2.3.1",
        .rssi      = "-42dBm",
        .state     = SM_STATE_ONLINE,
        .alarm_level = SM_LEVEL_NONE
    },
    {
        .name      = "后门",
        .id        = "Z-002",
        .sensor    = "PIR+门磁",
        .ip        = "192.168.1.102",
        .fw        = "v2.3.1",
        .rssi      = "-55dBm",
        .state     = SM_STATE_ONLINE,
        .alarm_level = SM_LEVEL_NONE
    },
    {
        .name      = "窗户",
        .id        = "Z-003",
        .sensor    = "门窗磁",
        .ip        = "192.168.1.103",
        .fw        = "v2.3.0",
        .rssi      = "-38dBm",
        .state     = SM_STATE_ONLINE,
        .alarm_level = SM_LEVEL_NONE
    },
    {
        .name      = "仓库",
        .id        = "Z-004",
        .sensor    = "PIR+摄像",
        .ip        = "192.168.1.104",
        .fw        = "v2.3.1",
        .rssi      = "-61dBm",
        .state     = SM_STATE_ONLINE,
        .alarm_level = SM_LEVEL_NONE
    }
};

/** 全局布防标志 */
static bool s_armed = false;

/** 事件缓冲区（环形，0 为最新） */
static sm_event_t s_events[SM_EVENT_MAX];
static int s_event_count = 0;

/** 状态变化回调表 */
static sm_state_cb_t s_state_cbs[SM_MAX_CALLBACKS];
static void *s_state_user_data[SM_MAX_CALLBACKS];
static int s_state_cb_count = 0;

/** 事件变化回调表 */
static sm_event_cb_t s_event_cbs[SM_MAX_CALLBACKS];
static void *s_event_user_data[SM_MAX_CALLBACKS];
static int s_event_cb_count = 0;

/* ==================== 内部辅助函数 ==================== */

/**
 * @brief 获取当前时间的时和分
 */
static void get_current_time(uint8_t *hour, uint8_t *minute)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    *hour   = (uint8_t)t->tm_hour;
    *minute = (uint8_t)t->tm_min;
}

/**
 * @brief 添加事件到缓冲区
 *
 * 新事件插入到数组头部（最新在前）
 * 超出 SM_EVENT_MAX 时丢弃最旧事件
 *
 * @param type     事件类型
 * @param level    告警级别
 * @param title    事件描述
 * @param location 事件位置
 */
static void add_event(sm_event_type_t type, sm_level_t level,
                      const char *title, const char *location)
{
    /* 缓冲区已满时，丢弃最旧事件（最后一个） */
    if (s_event_count >= SM_EVENT_MAX) {
        s_event_count = SM_EVENT_MAX - 1;
    }

    /* 新事件插入头部 */
    memmove(&s_events[1], &s_events[0], sizeof(sm_event_t) * s_event_count);

    /* 填充新事件 */
    s_events[0].type     = type;
    s_events[0].level    = level;
    s_events[0].title    = title;
    s_events[0].location = location;
    get_current_time(&s_events[0].hour, &s_events[0].minute);

    s_event_count++;

    /* 通知所有事件回调 */
    for (int i = 0; i < s_event_cb_count; i++) {
        if (s_event_cbs[i]) {
            s_event_cbs[i](&s_events[0], s_event_user_data[i]);
        }
    }
}

/**
 * @brief 通知所有状态变化回调
 */
static void notify_state_change(int zone_idx, sm_state_t new_state)
{
    for (int i = 0; i < s_state_cb_count; i++) {
        if (s_state_cbs[i]) {
            s_state_cbs[i](zone_idx, new_state, s_state_user_data[i]);
        }
    }
}

/* ==================== 公共 API 实现 ==================== */

void sm_init(void)
{
    /* 重置所有防区状态为在线 */
    for (int i = 0; i < SM_ZONE_COUNT; i++) {
        s_zones[i].state       = SM_STATE_ONLINE;
        s_zones[i].alarm_level = SM_LEVEL_NONE;
    }

    /* 重置全局状态 */
    s_armed = false;
    s_event_count = 0;

    /* 清空回调表 */
    s_state_cb_count = 0;
    s_event_cb_count = 0;
    memset(s_state_cbs, 0, sizeof(s_state_cbs));
    memset(s_event_cbs, 0, sizeof(s_event_cbs));

    /* 添加系统启动事件 */
    add_event(SM_EVT_INFO, SM_LEVEL_NONE, "系统启动完成", "主控");
    add_event(SM_EVT_OK,   SM_LEVEL_LOW,  "传感器自检通过", "前门");
    add_event(SM_EVT_OK,   SM_LEVEL_LOW,  "传感器自检通过", "后门");
    add_event(SM_EVT_INFO, SM_LEVEL_NONE, "网络连接已建立", "主控");
    add_event(SM_EVT_INFO, SM_LEVEL_NONE, "云端同步就绪", "主控");

    printf("[state_machine] INFO: 状态机初始化完成，%d 个防区就绪\n", SM_ZONE_COUNT);
}

void sm_arm_all(void)
{
    if (s_armed) {
        return;  /* 已布防，忽略重复操作 */
    }

    s_armed = true;

    /* 将所有在线防区切换为布防状态 */
    for (int i = 0; i < SM_ZONE_COUNT; i++) {
        if (s_zones[i].state == SM_STATE_ONLINE) {
            s_zones[i].state = SM_STATE_ARMED;
            notify_state_change(i, SM_STATE_ARMED);
        }
    }

    add_event(SM_EVT_OK, SM_LEVEL_NONE, "全部分区已布防", "系统");
    printf("[state_machine] INFO: 全局布防完成\n");
}

void sm_disarm_all(void)
{
    if (!s_armed) {
        return;  /* 已撤防，忽略重复操作 */
    }

    s_armed = false;

    /* 将所有布防/告警防区切换为在线状态 */
    for (int i = 0; i < SM_ZONE_COUNT; i++) {
        if (s_zones[i].state == SM_STATE_ARMED ||
            s_zones[i].state == SM_STATE_ALARM) {
            s_zones[i].state       = SM_STATE_ONLINE;
            s_zones[i].alarm_level = SM_LEVEL_NONE;
            notify_state_change(i, SM_STATE_ONLINE);
        }
    }

    add_event(SM_EVT_WARN, SM_LEVEL_NONE, "全部分区已撤防", "系统");
    printf("[state_machine] INFO: 全局撤防完成\n");
}

bool sm_ack_alarm(void)
{
    bool had_alarm = false;

    for (int i = 0; i < SM_ZONE_COUNT; i++) {
        if (s_zones[i].state == SM_STATE_ALARM) {
            had_alarm = true;
            /* 根据全局布防状态恢复 */
            s_zones[i].state       = s_armed ? SM_STATE_ARMED : SM_STATE_ONLINE;
            s_zones[i].alarm_level = SM_LEVEL_NONE;
            notify_state_change(i, s_zones[i].state);
        }
    }

    if (had_alarm) {
        add_event(SM_EVT_OK, SM_LEVEL_NONE, "告警已手动消除", "系统");
        printf("[state_machine] INFO: 告警已消除\n");
    } else {
        add_event(SM_EVT_INFO, SM_LEVEL_NONE, "当前无告警", "系统");
    }

    return had_alarm;
}

bool sm_is_armed(void)
{
    return s_armed;
}

int sm_get_zone_count(void)
{
    return SM_ZONE_COUNT;
}

const sm_zone_t *sm_get_zone(int idx)
{
    if (idx < 0 || idx >= SM_ZONE_COUNT) {
        return NULL;
    }
    return &s_zones[idx];
}

sm_state_t sm_get_zone_state(int idx)
{
    if (idx < 0 || idx >= SM_ZONE_COUNT) {
        return SM_STATE_OFFLINE;
    }
    return s_zones[idx].state;
}

int sm_get_event_count(void)
{
    return s_event_count;
}

const sm_event_t *sm_get_event(int idx)
{
    if (idx < 0 || idx >= s_event_count) {
        return NULL;
    }
    return &s_events[idx];
}

int sm_register_state_cb(sm_state_cb_t cb, void *user_data)
{
    if (s_state_cb_count >= SM_MAX_CALLBACKS) {
        return -1;
    }
    s_state_cbs[s_state_cb_count]       = cb;
    s_state_user_data[s_state_cb_count] = user_data;
    s_state_cb_count++;
    return 0;
}

int sm_register_event_cb(sm_event_cb_t cb, void *user_data)
{
    if (s_event_cb_count >= SM_MAX_CALLBACKS) {
        return -1;
    }
    s_event_cbs[s_event_cb_count]       = cb;
    s_event_user_data[s_event_cb_count] = user_data;
    s_event_cb_count++;
    return 0;
}

void sm_simulate_alarm(void)
{
    if (!s_armed) {
        return;
    }

    /* 收集所有 ARMED 状态的防区索引 */
    int armed_zones[SM_ZONE_COUNT];
    int armed_count = 0;

    for (int i = 0; i < SM_ZONE_COUNT; i++) {
        if (s_zones[i].state == SM_STATE_ARMED) {
            armed_zones[armed_count++] = i;
        }
    }

    if (armed_count == 0) {
        return;
    }

    /* 随机选择一个防区触发告警 */
    srand((unsigned int)time(NULL));
    int pick = armed_zones[rand() % armed_count];

    s_zones[pick].state       = SM_STATE_ALARM;
    s_zones[pick].alarm_level = SM_LEVEL_HIGH;
    notify_state_change(pick, SM_STATE_ALARM);

    add_event(SM_EVT_ERROR, SM_LEVEL_HIGH, "入侵检测触发", s_zones[pick].name);

    printf("[state_machine] WARN: 防区 %s 触发告警\n", s_zones[pick].name);
}
