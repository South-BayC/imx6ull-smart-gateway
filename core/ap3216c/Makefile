obj-m += ap3216c.o

KDIR := /home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay

all:
	make -C $(KDIR) M=$(CURDIR) ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules
	mkdir -p $(BUILD_DIR)/module
	cp ap3216c.ko $(BUILD_DIR)/module/

# 应用层测试程序 → build/test/
test:
	mkdir -p $(BUILD_DIR)/test
	arm-linux-gnueabihf-gcc -o $(BUILD_DIR)/test/ap3216c_test test/ap3216c_test.c -I uapi

clean:
	make -C $(KDIR) M=$(CURDIR) ARCH=arm clean

.PHONY: all test clean
