obj-m += led_drv.o

KDIR := /home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay

all:
	make -C $(KDIR) M=$(PWD) ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules
clean:
	make -C $(KDIR) M=$(PWD) ARCH=arm clean
