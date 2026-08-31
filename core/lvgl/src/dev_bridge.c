/**
 * dev_bridge.c — 外设信号桥接层实现
 *
 * 信号映射（板载驱动 → UI）：
 *   /dev/input/eventX                 KEY0(gpio-keys, KEY_ARMED) 按下 → 屏幕唤醒
 *   IIO ap3216c（sysfs）              距离靠近（布防中，防抖）→ 前门告警
 *   IIO icm20608（sysfs）             震动（布防中，防抖）→ 仓库告警
 *   /dev/led                          LED 跟随布防状态（告警闪烁/布防常亮）
 *   /sys/class/leds/beep/brightness   板载蜂鸣器跟随告警状态（出厂 dts gpio-leds）
 */
#include "dev_bridge.h"
#include "cam_feed.h"
#include "detector.h"
#include "ui/ui_events.h"
#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <dirent.h>
#include <time.h>
#include <pthread.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/input.h>

/* ---- 驱动 UAPI（与 core/ 各驱动 uapi 保持一致，内联声明避免跨目录依赖） ---- */

/* led_uapi.h */
#define LED_IOC_MAGIC 'L'
#define LED_IOC_ON    _IO(LED_IOC_MAGIC, 0x01)
#define LED_IOC_OFF   _IO(LED_IOC_MAGIC, 0x02)

/* beep_pwm.h（PWM 蜂鸣器手动控制） */
#define BEEP_IOC_SET_FREQ _IOW('B', 1, int)
#define BEEP_IOC_SET_DUTY _IOW('B', 2, int)
#define BEEP_IOC_ON       _IO('B', 3)
#define BEEP_IOC_OFF      _IO('B', 4)

/* ---- KEY0：gpio-keys input 子系统（dts key-input 节点，code=227 KEY_ARMED）---- */
#define KEY_DEV_NAME  "imx6ull-gpio-keys"
#ifndef KEY_ARMED
#define KEY_ARMED     227   /* 4.1.15 工具链头文件可能缺失，兜底（与驱动一致） */
#endif

/* ---- 设备节点 ---- */
/* AP3216C/ICM20608 已迁移 IIO 框架（P7-4）：用户态走
 * /sys/bus/iio/devices/iio:deviceX/ 下的通道属性文件（见 find_iio_path） */
#define IIO_SYSFS   "/sys/bus/iio/devices"
#define DEV_LED     "/dev/led"
#define DEV_BEEP    "/sys/class/leds/beep/brightness"
/*                     ↑ 出厂 dts gpio-leds 节点（label="beep", GPIO5_IO01 低有效）
 *                       内核已处理极性：写 '1' 响 / '0' 停 */
#define DEV_PWM     "/dev/beep_pwm0"         /* 外接无源蜂鸣器（PWM3） */
#define PWM_DEFAULT_FREQ 1000                /* beep_pwm 默认 period=1ms → 1kHz */

/* 探测 IIO 设备：遍历 iio:device*，读 name 属性匹配（如 "ap3216c"）。
 * 找到则输出设备目录路径（如 /sys/bus/iio/devices/iio:device0） */
static int find_iio_path(const char *dev_name, char *out, size_t len)
{
    DIR *dir = opendir(IIO_SYSFS);
    struct dirent *ent;
    char npath[128];
    char name[64];
    int ret = -1;

    if (!dir) return -1;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "iio:device", 10) != 0) continue;
        snprintf(npath, sizeof(npath), IIO_SYSFS "/%s/name", ent->d_name);
        int fd = open(npath, O_RDONLY);
        if (fd < 0) continue;
        memset(name, 0, sizeof(name));
        if (read(fd, name, sizeof(name) - 1) > 0) {
            char *nl = strchr(name, '\n');
            if (nl) *nl = '\0';
            if (strcmp(name, dev_name) == 0) {
                snprintf(out, len, IIO_SYSFS "/%s", ent->d_name);
                ret = 0;
            }
        }
        close(fd);
        if (ret == 0) break;
    }
    closedir(dir);
    return ret;
}

/* 打开 IIO 通道属性文件（保持 fd，读取时 lseek 回开头） */
static int open_iio_attr(const char *dev_path, const char *attr)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", dev_path, attr);
    return open(path, O_RDONLY);
}

/* 读 IIO 属性整数值（sysfs ASCII → int） */
static int read_iio_int(int fd, int *val)
{
    char buf[32] = {0};
    int n;

    if (fd < 0) return -1;
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return -1;
    *val = atoi(buf);
    return 0;
}

#define DEV_PWM     "/dev/beep_pwm0"   /* 外接无源蜂鸣器（PWM3，仅探测在位） */

/* ---- 阈值/周期 ---- */
#define POLL_MS          100     /* KEY0 poll 超时 = 采集循环节拍 */
#define ALS_PERIOD       5       /* 每 N 个节拍读一次 ap3216c（500ms） */
#define IMU_PERIOD       2       /* 每 N 个节拍读一次 icm20608（200ms） */

/* ---- 预警阈值配置（设置弹窗可调，采集线程读取） ---- */
static struct dev_config g_cfg = {
    .zones = {
        /* 前门:   AP3216C 距离 > 30 触发 */
        { .src = WARN_SRC_AP3216C, .channel = AP_DATA_PS, .threshold = 30, .cmp = WARN_CMP_GT, .enabled = 1 },
        /* 后门:   默认不触发 */
        { .src = WARN_SRC_NONE, .channel = 0, .threshold = 0, .cmp = WARN_CMP_GT, .enabled = 0 },
        /* 窗户:   默认不触发 */
        { .src = WARN_SRC_NONE, .channel = 0, .threshold = 0, .cmp = WARN_CMP_GT, .enabled = 0 },
        /* 仓库:   ICM 合加速度 > 150 触发 */
        { .src = WARN_SRC_ICM, .channel = IMU_DATA_MAG, .threshold = 150, .cmp = WARN_CMP_GT, .enabled = 1 },
    },
    .debounce = 2,
    .cooldown_sec = 60,   /* 消警后 60s 冷却，期间不重复触发 */
};

/* ---- 共享状态（采集线程写 / LVGL 定时器读） ---- */
struct dev_shm {
    pthread_mutex_t lock;
    int key_toggle;        /* KEY0 按下事件计数（消费后清零） */
    int warn_hit[4];       /* 各位置预警触发标志（消费后清零） */
    /* ---- 诊断快照（采集线程持续刷新） ---- */
    int key_ok, als_ok, imu_ok;   /* 设备在位 */
    int key_level;                /* KEY0 当前电平 1=按下 */
    int als_ir, als_als, als_ps;  /* AP3216C 最新原始值 */
    int imu_ax, imu_ay, imu_az;   /* ICM20608 最新原始值 */
    int imu_temp_x10;             /* 温度 ℃×10 */
    /* ---- 测试面板使能（手动开关） ---- */
    int als_en, imu_en;           /* 采集使能 1=开 */
};

static struct dev_shm g_shm = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};
static int g_started = 0;

/* ---- 控制输出句柄（LVGL 定时器线程专用，无需加锁） ---- */
static int g_led_fd  = -1;
static int g_beep_fd = -1;
static int g_pwm_fd  = -1;
static int g_led_on  = -1;   /* 上次 LED 状态（-1=未知） */
static int g_beep_on = -1;   /* 上次蜂鸣器状态（-1=未知） */
static int g_pwm_on_cur = 0; /* 当前 PWM 开关状态（测试面板可调） */
static int g_pwm_freq_cur = PWM_DEFAULT_FREQ;  /* 当前 PWM 频率（测试面板可调） */
static time_t g_zone_last_trigger[4] = {0};    /* 各位置上次触发时间（冷却用） */
static int g_beep_muted = 0;  /* 静音挂起（告警弹窗静音按钮；消警自动解除） */
static int g_snd_phase = 0;   /* 分级声光节奏相位（200ms/拍） */
static int g_pre_result = DET_RESULT_NONE;  /* 两级管线：当前事件初步结论（FINAL 定案参考） */

/* 云端入侵类型键 → 时间轴用词（server.py 归一：person/animal/object） */
static const char *det_type_word(const char *key)
{
    if (!key || !key[0]) return "";
    if (!strcmp(key, "person")) return "人员";
    if (!strcmp(key, "animal")) return "动物";
    if (!strcmp(key, "object")) return "物体";
    return "";
}

/* ================================================================
 * KEY0 设备探测：遍历 /dev/input/eventX，按名字匹配 gpio-keys
 * （gt9147 触摸也是 input 设备，必须用名字过滤避免误绑）
 * 成功返回 fd（已 O_RDONLY|O_NONBLOCK 无所谓，poll 阻塞等待），失败 -1
 * ================================================================ */
static int find_key_dev(void)
{
    DIR *dir = opendir("/dev/input");
    if (!dir) return -1;

    struct dirent *ent;
    int found = -1;
    char path[64];
    char name[256];

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        memset(name, 0, sizeof(name));
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 &&
            strcmp(name, KEY_DEV_NAME) == 0) {
            found = fd;
            break;
        }
        close(fd);
    }
    closedir(dir);
    return found;
}

/* ================================================================
 * 采集线程：KEY0 poll + 周期读传感器 → 共享状态
 * ================================================================ */
static void *collect_thread(void *arg)
{
    (void)arg;
    int key_fd = find_key_dev();

    /* AP3216C / ICM20608（IIO 框架）：探测设备目录并打开通道属性文件 */
    char als_path[96] = {0}, imu_path[96] = {0};
    int als_found = (find_iio_path("ap3216c", als_path, sizeof(als_path)) == 0);
    int imu_found = (find_iio_path("icm20608", imu_path, sizeof(imu_path)) == 0);

    int als_als_fd = -1, als_ps_fd = -1, als_ir_fd = -1;
    int imu_ax_fd = -1, imu_ay_fd = -1, imu_az_fd = -1, imu_t_fd = -1;
    int als_fd = -1, imu_fd = -1;   /* 综合在位标志（沿用原命名） */

    if (als_found) {
        /* 属性名以内核 IIO 命名为准（IIO_INTENSITY → in_intensity_*） */
        als_als_fd = open_iio_attr(als_path, "in_intensity_both_raw");
        als_ps_fd  = open_iio_attr(als_path, "in_proximity_raw");
        als_ir_fd  = open_iio_attr(als_path, "in_intensity_ir_raw");
        als_fd = (als_als_fd >= 0 && als_ps_fd >= 0 && als_ir_fd >= 0) ? 0 : -1;
    }
    if (imu_found) {
        imu_ax_fd = open_iio_attr(imu_path, "in_accel_x_raw");
        imu_ay_fd = open_iio_attr(imu_path, "in_accel_y_raw");
        imu_az_fd = open_iio_attr(imu_path, "in_accel_z_raw");
        imu_t_fd  = open_iio_attr(imu_path, "in_temp_raw");
        imu_fd = (imu_ax_fd >= 0 && imu_ay_fd >= 0 &&
                  imu_az_fd >= 0 && imu_t_fd >= 0) ? 0 : -1;
    }

    if (key_fd < 0)
        printf("[BRIDGE] %s not found (KEY0 disabled; dts key-input okay + insmod key_input.ko?)\n",
               KEY_DEV_NAME);
    if (als_fd < 0)
        printf("[BRIDGE] IIO ap3216c not found (ALS/PS disabled; insmod ap3216c.ko?)\n");
    if (imu_fd < 0)
        printf("[BRIDGE] IIO icm20608 not found (IMU disabled; insmod icm20608.ko?)\n");

    /* 在位标志写入诊断快照 */
    pthread_mutex_lock(&g_shm.lock);
    g_shm.key_ok = (key_fd >= 0);
    g_shm.als_ok = (als_fd >= 0);
    g_shm.imu_ok = (imu_fd >= 0);
    pthread_mutex_unlock(&g_shm.lock);

    int tick = 0;
    int cnt[4] = {0, 0, 0, 0};   /* 各位置去抖计数 */

    while (1) {
        /* --- KEY0（gpio-keys input 流）：poll 等待（100ms 超时兼作循环节拍） --- */
        if (key_fd >= 0) {
            struct pollfd pfd = { .fd = key_fd, .events = POLLIN };
            if (poll(&pfd, 1, POLL_MS) > 0) {
                struct input_event ev;
                /* 每次读一条完整 input_event；EV_KEY+KEY_ARMED+value=1 即按下 */
                while (read(key_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                    if (ev.type == EV_KEY && ev.code == KEY_ARMED) {
                        pthread_mutex_lock(&g_shm.lock);
                        g_shm.key_level = (ev.value == 1);
                        if (ev.value == 1)
                            g_shm.key_toggle++;
                        pthread_mutex_unlock(&g_shm.lock);
                    }
                }
            }
        } else {
            usleep(POLL_MS * 1000);
        }

        tick++;

        /* ================================================================
         * 预警判定：每个安防位置按各自配置（触发源+数据通道+阈值）独立判定。
         * AP3216C 数据 500ms 刷新一次，ICM 数据 200ms 刷新一次；
         * 数据刷新时对所有引用该传感器的位置做去抖判定。
         * ================================================================ */

        /* --- AP3216C 数据刷新（IIO sysfs：ALS/PS/IR 三通道） --- */
        if (als_fd >= 0 && (tick % ALS_PERIOD) == 0) {
            int als = 0, ps = 0, ir = 0;
            if (read_iio_int(als_als_fd, &als) == 0 &&
                read_iio_int(als_ps_fd, &ps) == 0 &&
                read_iio_int(als_ir_fd, &ir) == 0) {

                pthread_mutex_lock(&g_shm.lock);
                g_shm.als_ir = ir;
                g_shm.als_als = als;
                g_shm.als_ps = ps;
                int als_en = g_shm.als_en;
                pthread_mutex_unlock(&g_shm.lock);

                for (int z = 0; z < 4; z++) {
                    if (!als_en) break;
                    const struct zone_warn_cfg *c = &g_cfg.zones[z];
                    if (!c->enabled || c->src != WARN_SRC_AP3216C) continue;
                    int val = (c->channel == AP_DATA_PS) ? ps :
                              (c->channel == AP_DATA_ALS) ? als : ir;
                    int hit = (c->cmp == WARN_CMP_LT) ? (val < c->threshold) :
                              (c->cmp == WARN_CMP_EQ) ? (val == c->threshold) :
                              (val > c->threshold);
                    if (hit) cnt[z]++;
                    else cnt[z] = 0;
                    if (cnt[z] >= g_cfg.debounce) {
                        /* 达到防抖次数：置位一次并重置计数，
                         * 使数据持续超限时能周期性重新置位（冷却结束后立即可再次触发） */
                        cnt[z] = 0;
                        pthread_mutex_lock(&g_shm.lock);
                        g_shm.warn_hit[z]++;
                        pthread_mutex_unlock(&g_shm.lock);
                    }
                }
            }
        }

        /* --- ICM20608 数据刷新（IIO sysfs：accel xyz + temp） --- */
        if (imu_fd >= 0 && (tick % IMU_PERIOD) == 0) {
            int ax = 0, ay = 0, az = 0, temp_raw = 0;
            if (read_iio_int(imu_ax_fd, &ax) == 0 &&
                read_iio_int(imu_ay_fd, &ay) == 0 &&
                read_iio_int(imu_az_fd, &az) == 0 &&
                read_iio_int(imu_t_fd, &temp_raw) == 0) {

                pthread_mutex_lock(&g_shm.lock);
                g_shm.imu_ax = ax;
                g_shm.imu_ay = ay;
                g_shm.imu_az = az;
                int temp_x10 = (int)((double)temp_raw / 326.8 * 10.0 + 250.0);
                g_shm.imu_temp_x10 = temp_x10;
                int imu_en = g_shm.imu_en;
                pthread_mutex_unlock(&g_shm.lock);

                /* 合加速度 ×100 整数近似：|a| ≈ sqrt(ax²+ay²+az²)/32768×16×100 */
                double m2 = (double)ax * ax + (double)ay * ay + (double)az * az;
                long long mag_x100 = (long long)(sqrt(m2) / 32768.0 * 16.0 * 100.0);
                /* 单轴 ×100 绝对值（AX/AY/AZ 通道用） */
                long long ax_x100 = llabs((long long)ax * 100 / 2048);
                long long ay_x100 = llabs((long long)ay * 100 / 2048);
                long long az_x100 = llabs((long long)az * 100 / 2048);

                for (int z = 0; z < 4; z++) {
                    if (!imu_en) break;
                    const struct zone_warn_cfg *c = &g_cfg.zones[z];
                    if (!c->enabled || c->src != WARN_SRC_ICM) continue;
                    long long val;
                    switch (c->channel) {
                    case IMU_DATA_AX:   val = ax_x100; break;
                    case IMU_DATA_AY:   val = ay_x100; break;
                    case IMU_DATA_AZ:   val = az_x100; break;
                    case IMU_DATA_TEMP: val = temp_x10; break;
                    default:            val = mag_x100; break;
                    }
                    int hit = (c->cmp == WARN_CMP_LT) ? (val < c->threshold) :
                              (c->cmp == WARN_CMP_EQ) ? (val == c->threshold) :
                              (val > c->threshold);
                    if (hit) cnt[z]++;
                    else cnt[z] = 0;
                    if (cnt[z] >= g_cfg.debounce) {
                        /* 达到防抖次数：置位一次并重置计数（同上） */
                        cnt[z] = 0;
                        pthread_mutex_lock(&g_shm.lock);
                        g_shm.warn_hit[z]++;
                        pthread_mutex_unlock(&g_shm.lock);
                    }
                }
            }
        }
    }
    return NULL;
}

/* ================================================================
 * LVGL 定时器：主线程消费共享状态 → ui_events_* + 外设控制
 * ================================================================ */
static void bridge_timer_cb(lv_timer_t *t)
{
    (void)t;
    int key_toggle, warn_hit[4];

    pthread_mutex_lock(&g_shm.lock);
    key_toggle = g_shm.key_toggle;  g_shm.key_toggle = 0;
    for (int i = 0; i < 4; i++) {
        warn_hit[i] = g_shm.warn_hit[i];
        g_shm.warn_hit[i] = 0;
    }
    pthread_mutex_unlock(&g_shm.lock);

    /* --- KEY0：屏幕唤醒（重置 UI 空闲计时 + 解除 blank + 事件） --- */
    if (key_toggle > 0) {
        ui_events_user_activity();   /* 统一入口：g_idle_sec=0 + blank 0 */
        ui_events_log("KEY 触发", "系统", "");
    }

    /* --- 各位置预警触发（该分区布防中才生效，带消警冷却） --- */
    time_t now = time(NULL);
    for (int i = 0; i < 4; i++) {
        if (warn_hit[i] <= 0) continue;
        if (!ui_events_zone_is_armed(i)) continue;   /* 分区级布防门槛 */
        /* 冷却：距上次触发 < cooldown_sec 则忽略本次 */
        if (now - g_zone_last_trigger[i] < g_cfg.cooldown_sec) continue;
        g_zone_last_trigger[i] = now;
        ui_events_alarm_trigger_src(i, "SENSOR");
    }

    /* --- 运动粗判命中（当前预览通道对应分区，布防中+冷却） ---
     * 单摄分时切换语义：运动发生在当前预览通道，告警归属其对应分区。
     * 统一入精判管线（08-30 定案）：detector 两段式（本地 SCRFD 初判 →
     * 云端复核定案），结论经下方轮询分发 */
    int mh = cam_feed_get_motion_hits();
    if (mh > 0) {
        int z = ui_events_current_cam_zone();
        if (z >= 0 && ui_events_zone_is_armed(z)
            && now - g_zone_last_trigger[z] >= g_cfg.cooldown_sec) {
            g_zone_last_trigger[z] = now;
            detector_submit(z);
        }
    }

    /* --- 精判结论轮询（统一两级管线，08-30 定案；线程 4 异步产出 200ms 内下发） ---
     * 复核关（纯本地）：worker 仅发 INITIAL——检出人脸→STRANGER / 未检出→INTRUDER（不漏报）
     * 复核开（两级）：
     *   INITIAL: 检出人脸→立即 STRANGER 告警；未检出→"画面变动"轻提醒（等云端定论）
     *   FINAL:   白名单命中→在告警则自动消警；陌生人→不在告警则升级 STRANGER；
     *            非人员（动物/物体/无人）→在告警则自动消警（本地误报），否则仅时间轴；
     *            不可达→初步=未检出则升级 INTRUDER（不漏报），已告警则维持本地结论
     * 自动消警只作用于结论分区（SENSOR 触发的其他分区告警不受影响） */
    {
        int rz = -1, rr = DET_RESULT_NONE, rst = DET_STAGE_INITIAL;
        char name[32];
        int take = detector_poll_result(&rz, &rr, &rst, name, sizeof(name)) && rz >= 0;

        /* 门槛：初步结论仅布防中处理；最终结论在布防中/告警中都处理——
         * 初判告警后分区已转 ZONE_ALARM（非 ARMED），若按布防门槛拦截，
         * 云端白名单/无人结论永远到不了自动消警逻辑（08-30 板测定位）；
         * 撤防（OFFLINE）后两种结论都丢弃 */
        if (take && rst == DET_STAGE_INITIAL && !ui_events_zone_is_armed(rz))
            take = 0;
        if (take && rst == DET_STAGE_FINAL
            && !ui_events_zone_is_armed(rz) && !ui_events_zone_is_alarm(rz))
            take = 0;

        if (take) {
            if (rst == DET_STAGE_INITIAL) {
                g_pre_result = rr;   /* 暂存初步结论（FINAL 定案时参考） */
                if (rr == DET_RESULT_FACE_UNKNOWN) {
                    ui_events_alarm_trigger_src(rz, "STRANGER");   /* 立即告警 */
                } else if (rr == DET_RESULT_NO_FACE) {
                    if (ui_events_cloud_review_on()) {
                        /* 轻提醒不打扰：未检出人脸（可能是光影，也可能人背对镜头），等云端定论 */
                        ui_events_toast("画面变动");
                        ui_events_log("画面变动（未检出人员）", "本地初判", "low");
                    } else {
                        ui_events_alarm_trigger_src(rz, "INTRUDER");
                    }
                }
            } else {   /* DET_STAGE_FINAL：云端复核定案 */
                const char *tw = det_type_word(name);
                switch (rr) {
                case DET_RESULT_FACE_KNOWN: {   /* 白名单命中：误告警自动撤销 */
                    if (ui_events_zone_is_alarm(rz))
                        ui_events_alarm_auto_clear(rz, "云端复核：已授权人员");
                    else
                        ui_events_log("云端复核：已授权人员", "云端复核", "low");
                    break;
                }
                case DET_RESULT_FACE_UNKNOWN: { /* 云端陌生人：不在告警则升级（如背身人形） */
                    if (!ui_events_zone_is_alarm(rz))
                        ui_events_alarm_trigger_src(rz, "STRANGER");
                    break;
                }
                case DET_RESULT_OTHER: {        /* 云端判定非人员：本地误报则撤销 */
                    char t[64];
                    if (tw[0])
                        snprintf(t, sizeof(t), "云端复核：判定为%s活动", tw);
                    else
                        snprintf(t, sizeof(t), "云端复核：判定无人员");
                    if (ui_events_zone_is_alarm(rz))
                        ui_events_alarm_auto_clear(rz, t);
                    else
                        ui_events_log(t, "云端复核", "low");
                    break;
                }
                default: {                      /* 云端不可达：维持/升级本地结论（fail-safe） */
                    if (g_pre_result == DET_RESULT_NO_FACE && !ui_events_zone_is_alarm(rz))
                        ui_events_alarm_trigger_src(rz, "INTRUDER");
                    break;
                }
                }
            }
        }
    }

    /* --- 告警分级声光（设计 2.2：高=快闪+长鸣 中=中闪+断鸣 低=慢闪+短鸣） ---
     * 级别：3=高（窗/周界）2=中（仓库）1=低（门）0=无告警
     * 节拍 200ms：高=LED 每拍翻+持续鸣；中=LED 2 拍翻+2 拍鸣/2 拍停；低=LED 4 拍翻+1 拍鸣/3 拍停 */
    int alarm_lvl = ui_events_zone_alarm_level();

    if (g_led_fd >= 0) {
        if (alarm_lvl > 0) {
            static int blink = 0;
            blink++;
            int period = (alarm_lvl >= 3) ? 1 : (alarm_lvl == 2 ? 2 : 4);
            int led = (blink / 2) % period ? 0 : 1;   /* 按级别周期翻转 */
            if (led != g_led_on) {
                ioctl(g_led_fd, led ? LED_IOC_ON : LED_IOC_OFF);
                g_led_on = led;
            }
        } else {
            /* 无告警：跟随布防状态（布防亮） */
            int on = ui_events_is_armed();
            if (on != g_led_on) {
                ioctl(g_led_fd, on ? LED_IOC_ON : LED_IOC_OFF);
                g_led_on = on;
            }
        }
    }

    /* --- 蜂鸣器：分级鸣叫节奏（静音挂起时只停不响，消警自动解除静音） --- */
    if (g_beep_fd >= 0) {
        if (g_beep_muted && alarm_lvl == 0)
            g_beep_muted = 0;   /* 告警解除，自动恢复跟随 */

        if (!g_beep_muted && alarm_lvl > 0) {
            g_snd_phase++;
            int beep = 1;                                        /* 高：持续长鸣 */
            if (alarm_lvl == 2) beep = (g_snd_phase / 2) % 2;    /* 中：400ms 鸣/停 */
            if (alarm_lvl == 1) beep = (g_snd_phase % 4) == 0;   /* 低：200ms 短鸣/600ms 停 */

            if (beep != g_beep_on) {
                if (lseek(g_beep_fd, 0, SEEK_SET) >= 0) {
                    char c = beep ? '1' : '0';
                    if (write(g_beep_fd, &c, 1) < 0)
                        perror("[BRIDGE] beep write");
                }
                g_beep_on = beep;
            }
        } else if (g_beep_on != 0) {
            if (lseek(g_beep_fd, 0, SEEK_SET) >= 0) {
                char c = '0';
                if (write(g_beep_fd, &c, 1) < 0)
                    perror("[BRIDGE] beep write");
            }
            g_beep_on = 0;
        }
    }
}

/* ================================================================
 * 启动入口
 * ================================================================ */
void dev_bridge_start(void)
{
    if (g_started) return;
    g_started = 1;

    /* 控制输出节点（失败仅禁用该外设） */
    g_led_fd = open(DEV_LED, O_RDWR);
    if (g_led_fd < 0)
        printf("[BRIDGE] %s open failed: %s (LED follow disabled)\n", DEV_LED, strerror(errno));
    g_beep_fd = open(DEV_BEEP, O_WRONLY);
    if (g_beep_fd < 0)
        printf("[BRIDGE] %s open failed: %s (beep follow disabled)\n", DEV_BEEP, strerror(errno));
    g_pwm_fd = open(DEV_PWM, O_RDWR);   /* 仅探测在位（诊断面板显示） */
    g_led_on = g_beep_on = -1;

    /* 传感器采集默认开启 */
    pthread_mutex_lock(&g_shm.lock);
    g_shm.als_en = 1;
    g_shm.imu_en = 1;
    pthread_mutex_unlock(&g_shm.lock);

    /* 采集线程 */
    pthread_t tid;
    if (pthread_create(&tid, NULL, collect_thread, NULL) != 0) {
        printf("[BRIDGE] collect thread create failed\n");
        return;
    }
    pthread_detach(tid);

    /* LVGL 主线程消费定时器（200ms） */
    lv_timer_create(bridge_timer_cb, 200, NULL);
    detector_init();
    printf("[BRIDGE] device bridge started\n");
}

/* ================================================================
 * 诊断快照获取（UI 检测面板用）
 * ================================================================ */
void dev_bridge_get_diag(struct dev_diag *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&g_shm.lock);
    out->key_ok      = g_shm.key_ok;
    out->als_ok      = g_shm.als_ok;
    out->imu_ok      = g_shm.imu_ok;
    out->key_pressed = g_shm.key_level ? 1 : 0;
    out->als_ir      = g_shm.als_ir;
    out->als_als     = g_shm.als_als;
    out->als_ps      = g_shm.als_ps;
    out->imu_ax      = g_shm.imu_ax;
    out->imu_ay      = g_shm.imu_ay;
    out->imu_az      = g_shm.imu_az;
    out->imu_temp_x10 = g_shm.imu_temp_x10;
    out->als_en      = g_shm.als_en;
    out->imu_en      = g_shm.imu_en;
    pthread_mutex_unlock(&g_shm.lock);

    out->led_ok   = (g_led_fd >= 0);
    out->beep_ok  = (g_beep_fd >= 0);
    out->pwm_ok   = (g_pwm_fd >= 0);
    out->pwm_on   = g_pwm_on_cur;
    out->pwm_freq = (g_pwm_fd >= 0) ? g_pwm_freq_cur : 0;
    out->led_on   = (g_led_on == 1);
    out->beep_on  = (g_beep_on == 1);
}

/* ================================================================
 * 预警阈值配置（设置弹窗可调，采集线程实时生效）
 * ================================================================ */
void dev_bridge_get_config(struct dev_config *out)
{
    if (!out) return;
    *out = g_cfg;
}

void dev_bridge_set_config(const struct dev_config *cfg)
{
    if (!cfg) return;
    g_cfg = *cfg;   /* 整表替换（UI 侧先 get 再改再 set） */
    if (g_cfg.debounce < 1) g_cfg.debounce = 1;
    if (g_cfg.debounce > 20) g_cfg.debounce = 20;
    for (int i = 0; i < 4; i++) {
        if (g_cfg.zones[i].threshold < 0) g_cfg.zones[i].threshold = 0;
        if (g_cfg.zones[i].threshold > 2000) g_cfg.zones[i].threshold = 2000;
        if (g_cfg.zones[i].src < WARN_SRC_NONE) g_cfg.zones[i].src = WARN_SRC_NONE;
        if (g_cfg.zones[i].src > WARN_SRC_ICM) g_cfg.zones[i].src = WARN_SRC_ICM;
    }
}

/* ================================================================
 * 测试面板手动控制
 * ================================================================ */

/* 手动开关板载蜂鸣器（同步 g_beep_on 缓存，避免与告警跟随逻辑冲突） */
void dev_bridge_set_beep(int on)
{
    if (g_beep_fd >= 0) {
        if (lseek(g_beep_fd, 0, SEEK_SET) >= 0) {
            char c = on ? '1' : '0';
            if (write(g_beep_fd, &c, 1) < 0)
                perror("[BRIDGE] beep write");
        }
    }
    g_beep_on = on ? 1 : 0;
}

/* 手动开关 PWM 蜂鸣器并设置频率（beep_pwm ioctl） */
void dev_bridge_set_pwm(int on, int freq)
{
    if (g_pwm_fd < 0) return;
    if (freq < 100) freq = 100;
    if (freq > 20000) freq = 20000;

    if (on) {
        int f = freq, d = 50;
        ioctl(g_pwm_fd, BEEP_IOC_SET_FREQ, &f);
        ioctl(g_pwm_fd, BEEP_IOC_SET_DUTY, &d);
        ioctl(g_pwm_fd, BEEP_IOC_ON);
    } else {
        ioctl(g_pwm_fd, BEEP_IOC_OFF);
    }
    g_pwm_on_cur = on ? 1 : 0;   /* 记录开关状态（诊断显示） */
    g_pwm_freq_cur = freq;       /* 记录当前频率（诊断显示） */
}

void dev_bridge_set_beep_mute(int mute)
{
    g_beep_muted = mute ? 1 : 0;
    if (g_beep_muted && g_beep_fd >= 0 && g_beep_on != 0) {
        /* 立即静音 */
        if (lseek(g_beep_fd, 0, SEEK_SET) >= 0) {
            char c = '0';
            if (write(g_beep_fd, &c, 1) < 0)
                perror("[BRIDGE] beep mute write");
        }
        g_beep_on = 0;
    }
}

void dev_bridge_set_als_en(int en)
{
    pthread_mutex_lock(&g_shm.lock);
    g_shm.als_en = en ? 1 : 0;
    pthread_mutex_unlock(&g_shm.lock);
}

void dev_bridge_set_imu_en(int en)
{
    pthread_mutex_lock(&g_shm.lock);
    g_shm.imu_en = en ? 1 : 0;
    pthread_mutex_unlock(&g_shm.lock);
}

int dev_bridge_get_als_en(void)
{
    int v;
    pthread_mutex_lock(&g_shm.lock);
    v = g_shm.als_en;
    pthread_mutex_unlock(&g_shm.lock);
    return v;
}

int dev_bridge_get_imu_en(void)
{
    int v;
    pthread_mutex_lock(&g_shm.lock);
    v = g_shm.imu_en;
    pthread_mutex_unlock(&g_shm.lock);
    return v;
}
