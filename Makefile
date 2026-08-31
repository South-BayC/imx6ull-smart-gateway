# SPDX-License-Identifier: GPL-2.0
# Top-level Makefile for imx6ull-drivers project
# Orchestrates core (kernel modules), dts (device tree) and lvgl (applications)
# All build outputs go to build/ — source directories stay clean.

CROSS_COMPILE ?= arm-linux-gnueabihf-
BUILD_DIR := $(CURDIR)/build

# 默认构建项目所需的全部文件（内核除外）：
#   core  -> 8 个内核驱动模块 -> build/module/*.ko
#   dts   -> 设备树            -> build/dts/*.dtb
#   lvgl  -> LVGL 应用         -> build/lvgl/lvgl_gateway
all: core dts lvgl

core:
	$(MAKE) -C core BUILD_DIR=$(BUILD_DIR)

dts:
	$(MAKE) -C dts BUILD_DIR=$(BUILD_DIR)

lvgl:
	$(MAKE) -C core/lvgl BUILD_DIR=$(BUILD_DIR)

clean:
	$(MAKE) -C core clean
	$(MAKE) -C dts clean
	$(MAKE) -C core/lvgl clean
	rm -rf $(BUILD_DIR)

# 部署到板端（NFS rootfs + TFTP）
send:
	sudo cp ./build/module/*.ko /home/szh/linux/nfs/rootfs/lib/modules/4.1.15/ -f
	sudo cp ./build/dts/*.dtb /home/szh/linux/tftp/ -f
	sudo cp ./build/lvgl/lvgl_gateway /home/szh/linux/nfs/rootfs/usr/bin/ -f
	sudo cp ./models/face.param ./models/face.bin /home/szh/linux/nfs/rootfs/root/ -f
.PHONY: all core dts lvgl clean send
