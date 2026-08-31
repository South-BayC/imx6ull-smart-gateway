# SPDX-License-Identifier: GPL-2.0
# Kernel modules orchestrator — dispatches to each driver subdirectory.
# Add new drivers to SUBDIRS as they are created (led, relay, key, ...).
# 注：LVGL 应用由顶层 Makefile 的 `lvgl` 目标独立构建（core/lvgl），不在此处。
#
# build 产物分工（源码目录保持干净）：
#   结果  .ko  → $(BUILD_DIR)/module/            （由各驱动 Makefile 拷贝）
#   过程  .o/.mod.*/.*.cmd/.tmp_versions/Module.symvers/modules.order
#                → $(BUILD_DIR)/cache/<模块>/     （本 Makefile 编译后统一收件）

SUBDIRS := led key_event gpio_event_capture beep_pwm ap3216c icm20608 key_input gt9147
BUILD_DIR ?= $(CURDIR)/../build
CACHE := $(BUILD_DIR)/cache

all:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d BUILD_DIR=$(BUILD_DIR) || exit 1; done
	@$(MAKE) cache-collect

# 编译过程文件收件到 build/cache/<模块>/；Kbuild 默认在 M= 目录就地生成中间产物，
# 这里统一搬离，保证 make 结束后各源码目录只剩源码。glob 不匹配时静默跳过。
# 注意：结果 .ko 已由各驱动 Makefile 拷贝到 build/module/，源码目录残留的 .ko 直接删除。
cache-collect:
	@mkdir -p $(CACHE)
	@for d in $(SUBDIRS); do \
		mkdir -p $(CACHE)/$$d; \
		for f in $$d/*.o $$d/*.mod.c $$d/*.mod.o $$d/.[!.]*.cmd $$d/Module.symvers $$d/modules.order; do \
			[ -e "$$f" ] && mv -f "$$f" $(CACHE)/$$d/; \
		done; \
		[ -d "$$d/.tmp_versions" ] && mv -f "$$d/.tmp_versions" $(CACHE)/$$d/; \
		rm -f $$d/*.ko; \
	done

clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d clean || exit 1; done
	rm -rf $(CACHE)
	rm -f Module.symvers modules.order
	rm -rf .tmp_versions

.PHONY: all clean cache-collect
