# SPDX-License-Identifier: GPL-2.0
# Makefile for i.MX6U Industrial Data Acquisition (INDAQ) driver
#
# Kernel external module build generates .o in the source directory by design.
# After build, the final .ko is copied to build/driver/ and intermediates
# are cleaned from the source tree — only source files stay in driver/.

obj-m += indaq.o

# Self-test kernel module (separate .ko, not built by default)
# Build manually: make -C <KERNELDIR> M=$PWD obj-m="indaq_selftest.o" modules

# Phase 1-2: core + i2csens; Phase 3: ICM-20608 IMU
# Phase 7: calibration; Phase 9: input subsystem
indaq-objs := indaq_core.o \
              indaq_i2csens.o \
              indaq_imu.o \
              indaq_ringbuf.o \
              indaq_gpioctrl.o \
              indaq_calib.o \
              indaq_input.o \
              indaq_debug.o \
              indaq_pm.o

# IIO subsystem — only if kernel has CONFIG_IIO=y or =m
ifdef CONFIG_IIO
indaq-objs += indaq_iio.o
endif

# indaq_iio_buffer.o — removed: board kernel lacks IIO buffer EXPORT_SYMBOLs
# indaq-objs += indaq_iio_buffer.o

KERNELDIR := /home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay
CROSS_COMPILE ?= arm-linux-gnueabihf-
CURRENT_PATH := $(shell pwd)
name_ko := indaq.ko
BUILD_DIR ?= $(CURDIR)/../build
BUILD_DRV := $(BUILD_DIR)/driver

all:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) ARCH=arm CROSS_COMPILE=$(CROSS_COMPILE) modules
	mkdir -p $(BUILD_DRV)
	cp $(name_ko) $(BUILD_DRV)/
	cp Module.symvers $(BUILD_DRV)/ 2>/dev/null || true
	# Clean intermediates from source dir — only source files remain
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) ARCH=arm clean

clean:
	rm -rf $(BUILD_DRV)

bspcp:
	sudo cp $(BUILD_DRV)/$(name_ko) /home/szh/linux/nfs/rootfs/lib/modules/4.1.15/ -f

.PHONY: all clean bspcp
