#include "lvgl/lvgl.h"
#include "lvgl/src/drivers/sdl/lv_sdl_window.h"
#include "lvgl/src/drivers/sdl/lv_sdl_mouse.h"
#include <unistd.h>
#include <stdio.h>

// 复用主 UI 创建（与 main.c 保持一致，简版）
extern void ui_home_create(lv_obj_t *parent);

int main(void) {
    lv_init();
    lv_sdl_window_create(1024, 600);
    lv_sdl_mouse_create();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B1220), 0);
    ui_home_create(scr);

    printf("[SIM] 1024x600 安防网关模拟器已启动，按 Ctrl+C 退出\n");
    while (1) {
        lv_timer_handler();
        usleep(5000);
    }
    return 0;
}
