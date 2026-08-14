# SPDX-License-Identifier: GPL-2.0
# Top-level Makefile for imx6ul-indaq project
# Orchestrates driver (kernel module), app (user-space), and dts (device tree)
# All build outputs go to build/ — source directories stay clean.

CROSS_COMPILE ?= arm-linux-gnueabihf-
BUILD_DIR := $(CURDIR)/build

all: driver app dts ui

driver:
	$(MAKE) -C driver BUILD_DIR=$(BUILD_DIR)

app:
	$(MAKE) -C app CROSS_COMPILE=$(CROSS_COMPILE) BUILD_DIR=$(BUILD_DIR)

dts:
	$(MAKE) -C dts BUILD_DIR=$(BUILD_DIR)
ui:
	make -C app/indaq_ui clean && make -C app/indaq_ui 

bspcp:
	sudo cp $(BUILD_DIR)/driver/indaq.ko /home/szh/linux/nfs/rootfs/lib/modules/4.1.15/ -f

appcp:
	sudo cp $(BUILD_DIR)/app/* /home/szh/linux/nfs/rootfs/lib/modules/4.1.15/ -f
dtbcp:
	sudo cp $(BUILD_DIR)/dts/*.dtb /home/szh/linux/tftp/ -f
uicp:
	sudo  cp app/indaq_ui/indaq_ui /home/szh/linux/nfs/rootfs/lib/modules/4.1.15/ -f

install: bspcp appcp  uicp

clean:
	$(MAKE) -C driver clean
	$(MAKE) -C app clean
	$(MAKE) -C dts clean
	rm -rf $(BUILD_DIR)

.PHONY: all driver app dts bspcp appcp dtbcp install clean
