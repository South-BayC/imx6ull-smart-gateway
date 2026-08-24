/**
 * dev_bridge.h — 外设信号桥接层
 *
 * 把板载驱动（LED/蜂鸣器/KEY0/AP3216C/ICM20608）接入 LVGL UI：
 *   采集线程：阻塞 poll /dev/key-event + 周期读传感器 → 共享状态（互斥保护）
 *   LVGL 定时器：主线程消费共享状态 → ui_events_* 更新 UI；
 *                LED 跟随布防状态、蜂鸣器跟随告警状态（变化时才 ioctl）
 *
 * 线程安全约定：
 *   - ui_events_* 只能在 LVGL 主线程调用（本层由 lv_timer 回调保证）
 *   - 采集线程只写共享状态，不触碰任何 LVGL 对象
 *   - 任一设备节点打开失败仅禁用该外设并打印警告，不影响其余功能
 */
#ifndef DEV_BRIDGE_H
#define DEV_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动外设桥接（在 ui_home_create 之后、主循环之前调用一次）
 * 内部创建采集线程 + LVGL 消费定时器；重复调用无副作用。
 */
void dev_bridge_start(void);

/**
 * 模块诊断信息快照（供 UI 检测面板显示）
 */
struct dev_diag {
    /* 设备在位（节点成功打开） */
    int led_ok, beep_ok, pwm_ok;
    int key_ok, als_ok, imu_ok;
    /* 输出当前状态：1=开/响 */
    int led_on, beep_on;
    /* PWM 输出状态与频率 Hz */
    int pwm_on, pwm_freq;
    /* KEY0 当前电平：1=按下 */
    int key_pressed;
    /* AP3216C 最新原始值（ir 10bit / als 16bit / ps 10bit 越大越近） */
    int als_ir, als_als, als_ps;
    /* ICM20608 最新原始值（±16G 量程）与温度（℃×10） */
    int imu_ax, imu_ay, imu_az;
    int imu_temp_x10;
    /* 采集使能（测试面板手动开关） */
    int als_en, imu_en;
};

/**
 * 获取模块诊断快照（线程安全，可在 LVGL 定时器中调用）
 * @param out 输出结构体，不允许 NULL
 */
void dev_bridge_get_diag(struct dev_diag *out);

/* ---- 预警触发源 / 数据通道枚举 ---- */
#define WARN_SRC_NONE    0   /* 不触发 */
#define WARN_SRC_AP3216C 1
#define WARN_SRC_ICM     2

/* AP3216C 数据通道（src=WARN_SRC_AP3216C 时使用） */
#define AP_DATA_PS       0   /* 距离（越大越近） */
#define AP_DATA_ALS      1   /* 光照 */
#define AP_DATA_IR       2   /* 红外 */

/* ICM20608 数据通道（src=WARN_SRC_ICM 时使用）
 * MAG = 合加速度模长 sqrt(ax²+ay²+az²) ×100（三轴矢量和，非平均值） */
#define IMU_DATA_MAG     0   /* 合加速度 ×100（单位 g×100） */
#define IMU_DATA_AX      1   /* X 轴加速度 ×100 */
#define IMU_DATA_AY      2   /* Y 轴加速度 ×100 */
#define IMU_DATA_AZ      3   /* Z 轴加速度 ×100 */
#define IMU_DATA_TEMP    4   /* 温度 ℃×10 */

/* 比较方式（阈值触发方向） */
#define WARN_CMP_GT      0   /* 数据 > 阈值 触发 */
#define WARN_CMP_EQ      1   /* 数据 == 阈值 触发 */
#define WARN_CMP_LT      2   /* 数据 < 阈值 触发 */

/* 单个安防位置的预警配置（0=前门 1=后门 2=窗户 3=仓库） */
struct zone_warn_cfg {
    int src;         /* 触发源：WARN_SRC_NONE / WARN_SRC_AP3216C / WARN_SRC_ICM */
    int channel;     /* 数据通道（AP3216C 用 AP_DATA_PS/ALS/IR；ICM 用 IMU_DATA_MAG/AX/AY/AZ） */
    int threshold;   /* 触发阈值（原始值或 ×100） */
    int cmp;         /* 比较方式：WARN_CMP_GT / EQ / LT */
    int enabled;     /* 1=启用该位置预警 */
};

/* 预警阈值配置（设置弹窗可调，采集线程实时生效） */
struct dev_config {
    struct zone_warn_cfg zones[4];  /* 四个安防位置 */
    int debounce;                   /* 连续 N 次超限才触发（防抖） */
    int cooldown_sec;               /* 消警后冷却秒数（期间不重复触发） */
};

/**
 * 读取预警阈值配置
 */
void dev_bridge_get_config(struct dev_config *out);

/**
 * 设置预警阈值配置（采集线程下一轮生效）
 */
void dev_bridge_set_config(const struct dev_config *cfg);

/* ---- 测试面板手动控制（与自动跟随逻辑共享缓存，互不打架） ---- */
void dev_bridge_set_beep(int on);         /* 手动开关板载蜂鸣器 */
void dev_bridge_set_pwm(int on, int freq);/* 手动开关 PWM 蜂鸣器并设频率 */
void dev_bridge_set_als_en(int en);       /* AP3216C 采集使能 */
void dev_bridge_set_imu_en(int en);       /* ICM20608 采集使能 */
int  dev_bridge_get_als_en(void);
int  dev_bridge_get_imu_en(void);

#ifdef __cplusplus
}
#endif

#endif /* DEV_BRIDGE_H */
