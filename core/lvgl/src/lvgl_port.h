#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#define LVGL_PORT_ERR_OK           0
#define LVGL_PORT_ERR_FBDEV_OPEN  -4
#define LVGL_PORT_ERR_EVDEV_OPEN  -5

int lvgl_port_init(void);
void lvgl_port_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_PORT_H */
