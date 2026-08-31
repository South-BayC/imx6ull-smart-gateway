# SPDX-License-Identifier: GPL-2.0
# Top-level Makefile for imx6ull-drivers project
# Orchestrates core (kernel modules) and dts (device tree)
# All build outputs go to build/ — source directories stay clean.

CROSS_COMPILE ?= arm-linux-gnueabihf-
BUILD_DIR := $(CURDIR)/build

all: core dts test protocol

core:
	$(MAKE) -C core BUILD_DIR=$(BUILD_DIR)

dts:
	$(MAKE) -C dts BUILD_DIR=$(BUILD_DIR)

test:
	$(MAKE) -C core BUILD_DIR=$(BUILD_DIR) test

protocol:
	$(MAKE) -C protocol BUILD_DIR=$(BUILD_DIR)

host:
	$(MAKE) -C protocol BUILD_DIR=$(BUILD_DIR) host

clean:
	$(MAKE) -C core clean
	$(MAKE) -C dts clean
	$(MAKE) -C protocol clean
	rm -rf $(BUILD_DIR)
send:
	sudo cp ./build/module/*.ko /home/szh/linux/nfs/rootfs/lib/modules/4.1.15/ -f
	sudo cp ./build/test/* /home/szh/linux/nfs/rootfs/lib/modules/4.1.15/ -f
	sudo cp ./build/protocol/sump_server ./build/protocol/sump_selftest /home/szh/linux/nfs/rootfs/lib/modules/4.1.15/ -f
	sudo cp ./protocol/P6_accept_sump.sh /home/szh/linux/nfs/rootfs/lib/modules/4.1.15/ -f
	sudo chmod +x /home/szh/linux/nfs/rootfs/lib/modules/4.1.15/P6_accept_sump.sh
	sudo cp ./build/dts/*.dtb /home/szh/linux/tftp/ -f
	sudo cp ./build/lvgl/lvgl_gateway /home/szh/linux/nfs/rootfs/usr/bin/ -f
	sudo cp ./models/face.param ./models/face.bin /home/szh/linux/nfs/rootfs/root/ -f
.PHONY: all core dts test protocol host clean
