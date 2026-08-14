/**
 * @file lv_drv_conf.h
 * Configuration file for lv_drivers v8.3.0
 * INDAQ: fbdev display + evdev touch
 */

/* clang-format off */
#ifndef LV_DRV_CONF_H
#define LV_DRV_CONF_H

#include "lv_conf.h"

/*********************
 *  DELAY INTERFACE
 *********************/
#define LV_DRV_DELAY_INCLUDE  <stdint.h>
#define LV_DRV_DELAY_US(us)  /* delay_us(us) */
#define LV_DRV_DELAY_MS(ms)  /* delay_ms(ms) */

/*********************
 *  DISPLAY INTERFACE
 *********************/
/* Not used with fbdev; these are for direct SPI/parallel LCD control */
#define LV_DRV_DISP_INCLUDE         <stdint.h>
#define LV_DRV_DISP_CMD_DATA(val)
#define LV_DRV_DISP_RST(val)

#define LV_DRV_DISP_SPI_CS(val)
#define LV_DRV_DISP_SPI_WR_BYTE(data)
#define LV_DRV_DISP_SPI_WR_ARRAY(adr, n)

#define LV_DRV_DISP_PAR_CS(val)
#define LV_DRV_DISP_PAR_SLOW
#define LV_DRV_DISP_PAR_FAST
#define LV_DRV_DISP_PAR_WR_WORD(data)
#define LV_DRV_DISP_PAR_WR_ARRAY(adr, n)

/***************************
 *  INPUT DEVICE INTERFACE
 ***************************/
/* Not used with evdev */
#define LV_DRV_INDEV_INCLUDE     <stdint.h>
#define LV_DRV_INDEV_RST(val)
#define LV_DRV_INDEV_IRQ_READ    0
#define LV_DRV_INDEV_SPI_CS(val)
#define LV_DRV_INDEV_SPI_XCHG_BYTE(data) 0
#define LV_DRV_INDEV_I2C_START
#define LV_DRV_INDEV_I2C_STOP
#define LV_DRV_INDEV_I2C_RESTART
#define LV_DRV_INDEV_I2C_WR(data)
#define LV_DRV_INDEV_I2C_READ(last_read) 0

/*********************
 *  DISPLAY DRIVERS
 *********************/

/* Disable SDL (desktop simulation) */
#define USE_SDL      0
#define USE_SDL_GPU  0

/* Disable deprecated monitor driver */
#define USE_MONITOR  0

/* Disable Windows drivers */
#define USE_WINDOWS  0
#define USE_WIN32DRV 0

/* Disable GTK */
#define USE_GTK      0

/* Disable Wayland (no compositor on target) */
#define USE_WAYLAND  0

/* Disable raw LCD controller drivers */
#define USE_SSD1963  0
#define USE_R61581   0
#define USE_ST7565   0
#define USE_GC9A01   0
#define USE_UC1610   0
#define USE_SHARP_MIP 0
#define USE_ILI9341  0

/* Enable Linux framebuffer display */
#define USE_FBDEV    1
#if USE_FBDEV
#  define FBDEV_PATH "/dev/fb0"
#endif

/* Disable FreeBSD fbdev */
#define USE_BSD_FBDEV 0

/* Disable DRM/KMS */
#define USE_DRM      0

/*********************
 *  INPUT DEVICES
 *********************/
#define USE_XPT2046   0
#define USE_FT5406EE8 0
#define USE_AD_TOUCH  0
#define USE_MOUSE     0
#define USE_MOUSEWHEEL 0
#define USE_LIBINPUT  0
#define USE_BSD_LIBINPUT 0

/* Enable evdev touch input (Goodix GT9147) */
#define USE_EVDEV     1
#define USE_BSD_EVDEV 0
#if USE_EVDEV || USE_BSD_EVDEV
#  define EVDEV_NAME   "/dev/input/event1"
#  define EVDEV_SWAP_AXES         0
#  define EVDEV_CALIBRATE         0
#  if EVDEV_CALIBRATE
#    define EVDEV_HOR_MIN         0
#    define EVDEV_HOR_MAX      4096
#    define EVDEV_VER_MIN         0
#    define EVDEV_VER_MAX      4096
#  endif
#endif

/* Disable XKB (no keyboard needed) */
#define USE_XKB      0

/* Disable SDL keyboard */
#define USE_KEYBOARD 0

#endif /*LV_DRV_CONF_H*/
