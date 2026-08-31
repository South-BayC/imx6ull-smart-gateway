obj-m += gpio_event_capture.o

# 模块内 tracepoint 需要：define_trace.h 回包含 trace/edt_capture.h
# 时能搜到模块目录（kbuild 默认不加 M 目录到 include 路径）
ccflags-y += -I$(src)

KDIR := /home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay
BUILD_DIR ?= $(CURDIR)/../../build

all:
	make -C $(KDIR) M=$(CURDIR) ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules
	mkdir -p $(BUILD_DIR)/module
	cp gpio_event_capture.ko $(BUILD_DIR)/module/

clean:
	make -C $(KDIR) M=$(CURDIR) ARCH=arm clean

.PHONY: all clean