obj-m += beep_pwm.o

KDIR := /home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay
BUILD_DIR ?= $(CURDIR)/../../build

all:
	make -C $(KDIR) M=$(CURDIR) ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules
	mkdir -p $(BUILD_DIR)/module
	cp beep_pwm.ko $(BUILD_DIR)/module/

clean:
	make -C $(KDIR) M=$(CURDIR) ARCH=arm clean

.PHONY: all clean