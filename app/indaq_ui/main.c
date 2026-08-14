/*
 * main.c - INDAQ Monitor LVGL Display Application
 *
 * Displays all 15 feature categories on 1024x600 LCD via fbdev:
 *   Sensor raw, accel physical, gyro physical, temperature,
 *   ALS, device core status, submodule init, calibration,
 *   ring buffer, PM state, input/tap, GPIO, IIO, debugfs, health.
 *
 * Build:  arm-linux-gnueabihf-gcc ... (see Makefile)
 * Run:    ./indaq_ui                (target board, /dev/fb0)
 *
 * Prerequisites: /dev/indaq loaded, debugfs mounted.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* LVGL core */
#include "lvgl/lvgl.h"

/* lv_drivers */
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"

/* INDAQ userspace API */
#include "../libdaq.h"

/* ================================================================
 *  CONFIGURATION
 * ================================================================ */

#define INDAQ_DEVICE     "/dev/indaq"
#define DAQ_UPDATE_MS    200      /* refresh sensor data every 200ms */
#define LVGL_TICK_MS     5        /* LVGL timer handler interval */
#define STYLE_BG         lv_color_hex(0x1a1a2e)
#define STYLE_ACCENT     lv_color_hex(0x0f3460)
#define STYLE_OK         lv_color_hex(0x2ecc71)
#define STYLE_WARN       lv_color_hex(0xf39c12)
#define STYLE_ERR        lv_color_hex(0xe74c3c)
#define STYLE_TEXT       lv_color_hex(0xcccccc)
#define STYLE_TEXT_HL    lv_color_hex(0xffffff)

/* ================================================================
 *  GLOBAL STATE
 * ================================================================ */

struct monitor_ctx {
    /* device */
    int daq_fd;
    struct indaq_info info;
    struct indaq_sample sample;
    int daq_ok;

    /* header */
    lv_obj_t *lbl_title;

    /* ======== sensor panel (left) ======== */
    lv_obj_t *lbl_als_line;     /* "ALS: 12345 (12.3 lux)  PS: 12345  IR: 12345  OBJ: YES" */
    lv_obj_t *lbl_accel_line;   /* "X: +0.023 g  Y: -0.001 g  Z: +1.002 g" */
    lv_obj_t *lbl_gyro_line;    /* "GX: +12  GY: -5  GZ: +3  Cal: YES" */
    lv_obj_t *lbl_temp_line;    /* "25.3 C" */

    /* ======== system panel (right) ======== */
    lv_obj_t *lbl_core_line;    /* "Capture: RUNNING  Rate: 100 Hz  Samples: 12345" */
    lv_obj_t *lbl_ringbuf_line; /* "Count: 123 / 4096  Usage: 3%" */
    lv_obj_t *lbl_calib_line;   /* "Gyro: CALIBRATED  Off: 0/0/0  Accel: CALIBRATED" */
    lv_obj_t *lbl_pm_line;      /* "PM: active" */
    lv_obj_t *lbl_gpio_line;    /* "GPIO: N/A" */
    lv_obj_t *lbl_tap_line;     /* "Tap: 0" */
    lv_obj_t *lbl_iio_line;     /* "IIO: registered" */
    lv_obj_t *lbl_debugfs_line; /* "DebugFS: OK" */
    lv_obj_t *lbl_health_line;  /* "Health: IMU OK" */
};

static struct monitor_ctx ctx;

/* ================================================================
 *  DEBUGFS READERS
 * ================================================================ */

/* Read a simple u32 from a debugfs file. Returns -1 on failure. */
static int debugfs_read_u32(const char *path)
{
    FILE *f = fopen(path, "r");
    unsigned long val;
    if (!f) return -1;
    if (fscanf(f, "%lu", &val) != 1) { fclose(f); return -1; }
    fclose(f);
    return (int)val;
}

/* Read a line from a debugfs file into buf. Returns 0 on success. */
static int debugfs_read_line(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)size, f)) { fclose(f); return -1; }
    /* strip newline */
    size_t len = strlen(buf);
    if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
    fclose(f);
    return 0;
}

/* ================================================================
 *  UI CREATION HELPERS
 * ================================================================ */

/* Create a section-header label */
static lv_obj_t *create_hdr(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, STYLE_ACCENT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    return lbl;
}

/* Create a data-line label */
static lv_obj_t *create_line(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, STYLE_TEXT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    return lbl;
}

/* ================================================================
 *  CREATE SENSOR PANEL (left 60%)
 * ================================================================ */

static void create_sensor_panel(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_row(parent, 1, 0);

    create_hdr(parent, "-- AP3216C --");
    ctx.lbl_als_line = create_line(parent, "ALS: ---  PS: ---  IR: ---  OBJ: ---");

    create_hdr(parent, "-- ACCELEROMETER --");
    ctx.lbl_accel_line = create_line(parent, "X: ---  Y: ---  Z: ---");

    create_hdr(parent, "-- GYROSCOPE --");
    ctx.lbl_gyro_line = create_line(parent, "GX: ---  GY: ---  GZ: ---  Cal: ---");

    create_hdr(parent, "-- TEMPERATURE --");
    ctx.lbl_temp_line = create_line(parent, "--.- C");
}

/* ================================================================
 *  CREATE SYSTEM PANEL (right 40%)
 * ================================================================ */

static void create_system_panel(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_row(parent, 1, 0);

    create_hdr(parent, "-- CORE --");
    ctx.lbl_core_line    = create_line(parent, "Capture: ---");
    create_hdr(parent, "-- RING BUFFER --");
    ctx.lbl_ringbuf_line = create_line(parent, "Count: --- / ---  Usage: ---");
    create_hdr(parent, "-- CALIBRATION --");
    ctx.lbl_calib_line   = create_line(parent, "Gyro: ---  Off: ---  Accel: ---");
    create_hdr(parent, "-- SYSTEM --");
    ctx.lbl_pm_line      = create_line(parent, "PM: ---");
    ctx.lbl_gpio_line    = create_line(parent, "GPIO: ---");
    ctx.lbl_tap_line     = create_line(parent, "Tap: ---");
    ctx.lbl_iio_line     = create_line(parent, "IIO: ---");
    ctx.lbl_debugfs_line = create_line(parent, "DebugFS: ---");
    ctx.lbl_health_line  = create_line(parent, "Health: ---");
}

/* ================================================================
 *  CREATE HEADER BAR
 * ================================================================ */

static void create_header(lv_obj_t *parent)
{
    lv_obj_t *hdr = lv_obj_create(parent);
    lv_obj_set_size(hdr, LV_HOR_RES_MAX, 40);
    lv_obj_set_style_bg_color(hdr, STYLE_ACCENT, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);

    ctx.lbl_title = lv_label_create(hdr);
    lv_label_set_text(ctx.lbl_title, "INDAQ MONITOR v1.0.0");
    lv_obj_set_style_text_color(ctx.lbl_title, STYLE_TEXT_HL, 0);
    lv_obj_set_style_text_font(ctx.lbl_title, &lv_font_montserrat_20, 0);
    lv_obj_align(ctx.lbl_title, LV_ALIGN_LEFT_MID, 10, 0);
}

/* ================================================================
 *  DATA UPDATE (called by LVGL timer)
 * ================================================================ */

static void update_sensor_data(lv_timer_t *timer)
{
    (void)timer;
    struct monitor_ctx *c = &ctx;

    /* --- Read sensor sample (batch-drain to skip stale IMU backlog) --- */
    if (c->daq_ok) {
        struct indaq_sample batch[32];
        ssize_t n = indaq_read(c->daq_fd, batch, 32);
        if (n > 0) {
            c->sample = batch[n - 1];   /* take the LATEST sample only */
        } else if (n == -EAGAIN) {
            goto do_system_status;
        } else {
            indaq_close(c->daq_fd);
            c->daq_fd = indaq_open(INDAQ_DEVICE);
            c->daq_ok = (c->daq_fd >= 0);
            if (c->daq_ok) indaq_start(c->daq_fd);
        }
        static int icnt = 0;
        if (++icnt % 5 == 0)
            indaq_get_info(c->daq_fd, &c->info);
    }

    /* ======== AP3216C ======== */
    {
        float lux = c->sample.als * 0.01f;
        int obj  = (c->sample.ps > 100);
        lv_label_set_text_fmt(c->lbl_als_line,
            "ALS: %.1f(lux)[%u]  PS: %u(counts)[%u]  IR: %u(counts)[%u]  OBJ: %s",
            (double)lux, c->sample.als,
            c->sample.ps, c->sample.ps,
            c->sample.ir, c->sample.ir,
            obj ? "YES" : "NO");
    }

    /* ======== Accelerometer ======== */
    {
        int16_t r[3] = { c->sample.ax, c->sample.ay, c->sample.az };
        lv_label_set_text_fmt(c->lbl_accel_line,
            "X: %+.3f(g)[%d]  Y: %+.3f(g)[%d]  Z: %+.3f(g)[%d]",
            (double)(r[0] / 2048.0f), r[0],
            (double)(r[1] / 2048.0f), r[1],
            (double)(r[2] / 2048.0f), r[2]);
    }

    /* ======== Gyroscope + Temperature ======== */
    {
        int16_t rg[3] = { c->sample.gx, c->sample.gy, c->sample.gz };
        /* Gyro: ±2000°/s, sensitivity 16.4 LSB/(°/s) */
        /* Temp: ICM-20608 formula: raw / 333.87 + 25 */
        float tc = c->sample.temp / 333.87f + 25.0f;
        lv_label_set_text_fmt(c->lbl_gyro_line,
            "GX: %.1f(dps)[%d]  GY: %.1f(dps)[%d]  GZ: %.1f(dps)[%d]  Cal: ---",
            (double)(rg[0] / 16.4f), rg[0],
            (double)(rg[1] / 16.4f), rg[1],
            (double)(rg[2] / 16.4f), rg[2]);
        lv_label_set_text_fmt(c->lbl_temp_line, "%.1f(C)[%d]",
            (double)tc, c->sample.temp);
    }

    /* ======== Ring Buffer ======== */
    {
        int cap = debugfs_read_u32("/sys/kernel/debug/indaq/ringbuf_capacity");
        int cnt = debugfs_read_u32("/sys/kernel/debug/indaq/ringbuf_count");
        if (cap > 0 && cnt >= 0) {
            int pct = (cnt * 100) / cap;
            lv_label_set_text_fmt(c->lbl_ringbuf_line,
                "Count: %d / %d  Usage: %d%%", cnt, cap, pct);
        }
    }

    /* ======== Calibration ======== */
    {
        char buf[128];
        int  cal_ok = 0;
        int  ox = 0, oy = 0, oz = 0, acal = 0;
        if (debugfs_read_line("/sys/kernel/debug/indaq/calib_params",
                              buf, sizeof(buf)) == 0 && strstr(buf, "yes")) {
            cal_ok = 1;
            FILE *f = fopen("/sys/kernel/debug/indaq/calib_params","r");
            if (f) {
                char line[64];
                while (fgets(line, sizeof(line), f)) {
                    (void)sscanf(line, "gyro_offset.x: %d", &ox);
                    (void)sscanf(line, "gyro_offset.y: %d", &oy);
                    (void)sscanf(line, "gyro_offset.z: %d", &oz);
                    if (strstr(line, "accel_calibrated: yes")) acal = 1;
                }
                fclose(f);
            }
        }
        lv_label_set_text_fmt(c->lbl_calib_line,
            "Gyro: %s  Off: %d/%d/%d  Accel: %s",
            cal_ok ? "CALIBRATED" : "not cal",
            ox, oy, oz,
            acal ? "CALIBRATED" : "not cal");
    }

do_system_status:
    /* ======== System Status (from stats) ======== */
    {
        char line[128];
        FILE *f = fopen("/sys/kernel/debug/indaq/stats", "r");
        int pm_sus = -1, gpio_ok = -1, tap_cnt = -1;
        int imu_ok = 0, iio_found = 0;
        int dbg_ok = (f != NULL);

        if (f) {
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "imu_ready:"))
                    imu_ok = !!strstr(line, "yes");
                if (sscanf(line, "pm_suspended: %d", &pm_sus) == 1) pm_sus = !!pm_sus;
                if (sscanf(line, "gpio_trigger: %d", &gpio_ok) == 1) gpio_ok = !!gpio_ok;
                if (sscanf(line, "tap_count: %d", &tap_cnt) == 1) {}
            }
            fclose(f);
        }

        lv_label_set_text_fmt(c->lbl_core_line,
            "Capture: %s  Rate: %u Hz  Samples: %u",
            c->daq_ok ? "RUNNING" : "N/A",
            c->info.sampling_rate, c->info.total_samples);

        lv_label_set_text_fmt(c->lbl_pm_line,
            "PM: %s",  pm_sus < 0 ? "N/A" : (pm_sus ? "SUSPENDED" : "active"));
        lv_label_set_text_fmt(c->lbl_gpio_line,
            "GPIO: %s", gpio_ok < 0 ? "--" : (gpio_ok ? "active" : "N/A"));
        lv_label_set_text_fmt(c->lbl_tap_line,
            "Tap: %s", tap_cnt < 0 ? "N/A" : "");
        if (tap_cnt >= 0)
            lv_label_set_text_fmt(c->lbl_tap_line, "Tap: %d", tap_cnt);

        /* IIO */
        f = fopen("/sys/bus/iio/devices/iio:device0/name", "r");
        if (f) {
            char name[32];
            if (fgets(name, sizeof(name), f) && strstr(name, "indaq"))
                iio_found = 1;
            fclose(f);
        }
        lv_label_set_text_fmt(c->lbl_iio_line,
            "IIO: %s", iio_found ? "registered" : "N/A");

        lv_label_set_text_fmt(c->lbl_debugfs_line,
            "DebugFS: %s", dbg_ok ? "OK" : "N/A");
        lv_label_set_text_fmt(c->lbl_health_line,
            "Health: %s", imu_ok ? "IMU OK" : "N/A");
    }
}

/* ================================================================
 *  MAIN
 * ================================================================ */

int main(void)
{
    struct monitor_ctx *c = &ctx;
    memset(c, 0, sizeof(*c));

    printf("INDAQ Monitor UI starting...\n");

    /* ---- Open INDAQ device ---- */
    c->daq_fd = indaq_open(INDAQ_DEVICE);
    c->daq_ok = (c->daq_fd >= 0);
    if (c->daq_ok) {
        indaq_get_info(c->daq_fd, &c->info);
        /* Start capture — required for IMU/I2C data to flow into ring buffer */
        if (indaq_start(c->daq_fd) == 0) {
            printf("Capture started.\n");
        } else {
            printf("WARNING: indaq_start() failed — no sensor data.\n");
        }
        printf("INDAQ opened: version 0x%08x, rate=%u Hz\n",
               c->info.version, c->info.sampling_rate);
    } else {
        printf("WARNING: Cannot open %s. Display will show N/A.\n",
               INDAQ_DEVICE);
    }

    /* ---- Initialize LVGL ---- */
    lv_init();

    /* ---- fbdev display init ---- */
    fbdev_init();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf[LV_HOR_RES_MAX * 30];  /* 30-line buffer */
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, LV_HOR_RES_MAX * 30);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf   = &draw_buf;
    disp_drv.flush_cb   = fbdev_flush;
    disp_drv.hor_res    = LV_HOR_RES_MAX;
    disp_drv.ver_res    = LV_VER_RES_MAX;
    lv_disp_drv_register(&disp_drv);

    /* ---- evdev touch init ---- */
    evdev_init();

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = evdev_read;
    lv_indev_drv_register(&indev_drv);

    printf("LVGL initialized. Creating UI...\n");

    /* ---- Create main screen layout ---- */
    /* All containers explicitly positioned and sized — no flex at top level */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, STYLE_BG, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* Header row */
    create_header(scr);

    /* Left: sensor panel (60%) — direct child of screen */
    lv_obj_t *left_panel = lv_obj_create(scr);
    lv_obj_set_pos(left_panel, 0, 40);
    lv_obj_set_size(left_panel, LV_HOR_RES_MAX * 60 / 100,
                    LV_VER_RES_MAX - 40);
    lv_obj_set_style_bg_color(left_panel, STYLE_BG, 0);
    lv_obj_set_style_border_width(left_panel, 0, 0);
    lv_obj_set_style_radius(left_panel, 0, 0);
    lv_obj_set_style_pad_all(left_panel, 0, 0);
    lv_obj_clear_flag(left_panel, LV_OBJ_FLAG_SCROLLABLE);
    create_sensor_panel(left_panel);

    /* Right: system panel (40%) — direct child of screen */
    {   int rx = LV_HOR_RES_MAX * 60 / 100;
        lv_obj_t *right_panel = lv_obj_create(scr);
        lv_obj_set_pos(right_panel, rx, 40);
        lv_obj_set_size(right_panel, LV_HOR_RES_MAX - rx,
                        LV_VER_RES_MAX - 40);
        lv_obj_set_style_bg_color(right_panel, STYLE_BG, 0);
        lv_obj_set_style_border_width(right_panel, 0, 0);
        lv_obj_set_style_radius(right_panel, 0, 0);
        lv_obj_set_style_pad_all(right_panel, 0, 0);
        lv_obj_clear_flag(right_panel, LV_OBJ_FLAG_SCROLLABLE);
        create_system_panel(right_panel);
    }

    /* ---- Create update timer ---- */
    lv_timer_create(update_sensor_data, DAQ_UPDATE_MS, NULL);

    printf("Entering LVGL main loop...\n");

    /* ---- Main loop ---- */
    while (1) {
        lv_tick_inc(LVGL_TICK_MS);
        lv_timer_handler();
        usleep(LVGL_TICK_MS * 1000);
    }

    /* (Never reached) */
    indaq_close(c->daq_fd);
    return 0;
}
