# SPDX-License-Identifier: GPL-2.0
# Top-level Makefile for imx6ull-drivers project
# Orchestrates core (kernel modules) and dts (device tree)
# All build outputs go to build/ — source directories stay clean.

CROSS_COMPILE ?= arm-linux-gnueabihf-
BUILD_DIR := $(CURDIR)/build

all: core dts test

core:
	$(MAKE) -C core BUILD_DIR=$(BUILD_DIR)

dts:
	$(MAKE) -C dts BUILD_DIR=$(BUILD_DIR)

test:
	$(MAKE) -C core BUILD_DIR=$(BUILD_DIR) test

clean:
	$(MAKE) -C core clean
	$(MAKE) -C dts clean
	rm -rf $(BUILD_DIR)
send:
	sudo cp ./build/module/*.ko /home/szh/linux/nfs/rootfs/lib/modules/4.1.15/ -f
	sudo cp ./build/test/* /home/szh/linux/nfs/rootfs/lib/modules/4.1.15/ -f
	sudo cp ./build/dts/*.dtb /home/szh/linux/tftp/ -f
.PHONY: all core dts test clean
