obj-m += ap3216c.o

KDIR := /home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay

all:
	make -C $(KDIR) M=$(CURDIR) ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules
	mkdir -p $(BUILD_DIR)/module
	cp ap3216c.ko $(BUILD_DIR)/module/

# P7-4 迁移 IIO 后旧 /dev 接口测试程序已删除（用户态走 IIO sysfs）
test:
	@echo "ap3216c: IIO driver, no standalone test (use /sys/bus/iio/devices/)"

clean:
	make -C $(KDIR) M=$(CURDIR) ARCH=arm clean

.PHONY: all test clean
