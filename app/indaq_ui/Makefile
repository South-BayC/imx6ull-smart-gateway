# Makefile - INDAQ UI (LVGL + lv_drivers + libdaq)
#
# Cross-compile for ARM target board:
#   make                     # build indaq_ui binary
#   make clean               # clean build artifacts
#
# Prerequisites:
#   arm-linux-gnueabihf-gcc  in PATH (cross-compiler)
#   Linux build machine

CROSS_COMPILE ?= arm-linux-gnueabihf-
CC  := $(CROSS_COMPILE)gcc
LD  := $(CROSS_COMPILE)ld
AR  := $(CROSS_COMPILE)ar
STRIP := $(CROSS_COMPILE)strip

# Directories
LVGL_DIR    := lvgl
LVGL_SRC    := $(LVGL_DIR)/src
LVDRV_DIR   := lv_drivers
CURDIR      := .

# Compiler flags
CFLAGS  := -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable
CFLAGS  += -std=gnu99
CFLAGS  += -I$(CURDIR) -I$(LVGL_DIR) -I$(LVDRV_DIR)
CFLAGS  += -I$(LVGL_SRC)/core -I$(LVGL_SRC)/draw -I$(LVGL_SRC)/extra
CFLAGS  += -I$(LVGL_SRC)/font -I$(LVGL_SRC)/hal -I$(LVGL_SRC)/misc
CFLAGS  += -I$(LVGL_SRC)/widgets -I$(LVGL_SRC)/layouts
CFLAGS  += -I$(LVDRV_DIR)/display -I$(LVDRV_DIR)/indev
CFLAGS  += -DLV_CONF_INCLUDE_SIMPLE
CFLAGS  += -DLV_LVGL_H_INCLUDE_SIMPLE
CFLAGS  += -march=armv7ve -mfloat-abi=hard -mfpu=neon
CFLAGS  += -D_GNU_SOURCE

LDFLAGS := -lm -lrt

# LVGL source files (all .c under src/ recursively)
LVGL_SRCS := $(shell find $(LVGL_SRC) -name '*.c')
# lv_drivers source files
LVDRV_SRCS := $(LVDRV_DIR)/display/fbdev.c $(LVDRV_DIR)/indev/evdev.c
# Application sources
APP_SRCS := main.c ../libdaq.c

OBJS := $(LVGL_SRCS:.c=.o) $(LVDRV_SRCS:.c=.o) $(APP_SRCS:.c=.o)

TARGET := indaq_ui

.PHONY: all clean

all: $(TARGET)

# Compile rules
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Link
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	$(STRIP) $@
	@echo ""
	@echo "=== Build complete: $(TARGET) ==="
	@echo "File size: $$(ls -lh $(TARGET) | awk '{print $$5}')"
	@echo "Run 'file $(TARGET)' to verify ARM binary"
	@echo ""

# Also provide a 'static' target that links fully statically for
# systems without matching shared libraries on the target rootfs.
static: CFLAGS += -static
static: LDFLAGS += -static
static: all

clean:
	rm -f $(OBJS) $(TARGET)
	@echo "Clean done."
