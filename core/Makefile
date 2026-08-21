# SPDX-License-Identifier: GPL-2.0
# Kernel modules orchestrator — dispatches to each driver subdirectory.
# Add new drivers to SUBDIRS as they are created (led, relay, key, ...).

SUBDIRS := led key_event gpio_event_capture beep_pwm ap3216c icm20608 key_input

all:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d BUILD_DIR=$(BUILD_DIR) || exit 1; done

# 编译各模块应用层测试程序到 build/test/
test:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d BUILD_DIR=$(BUILD_DIR) test || exit 1; done

clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d clean || exit 1; done

.PHONY: all test clean