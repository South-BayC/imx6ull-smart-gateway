obj-m += gt9147.o

KDIR := /home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay
BUILD_DIR ?= $(CURDIR)/../../build

all:
	make -C $(KDIR) M=$(CURDIR) ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules
	mkdir -p $(BUILD_DIR)/module
	cp gt9147.ko $(BUILD_DIR)/module/

clean:
	make -C $(KDIR) M=$(CURDIR) ARCH=arm clean

.PHONY: all clean
