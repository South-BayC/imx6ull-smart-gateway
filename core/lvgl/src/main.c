#include "lvgl/lvgl.h"
#include "lvgl/src/drivers/display/fb/lv_linux_fbdev.h"
#include "lvgl_port.h"
#include "ui/ui_home.h"
#include "dev_bridge.h"
#include <unistd.h>
#include <stdio.h>

int main(void) {
    if (lvgl_port_init() != 0) {
        printf("[LVGL] port init failed, continue with fallback\n");
    }

    // 创建主屏：深色背景 + 智慧安防网关主页（与模拟器一致）
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B1220), 0);
    ui_home_create(scr);

    // 启动外设桥接（KEY0/AP3216C/ICM20608 采集 → UI；LED/蜂鸣器 ← 状态）
    dev_bridge_start();

    printf("[LVGL] UI ready, entering main loop\n");
    while (1) {
        lv_timer_handler();
        usleep(5000);
    }
    return 0;
}
