# SPDX-License-Identifier: GPL-2.0
# Makefile for compiling imx6ull-drivers device tree
# Output goes to BUILD_DIR/dts/
#
# DTS files use #include (C preprocessor directive), so dtc alone won't work.
# We replicate the kernel's two-step flow:
#   1. cpp  — expand #include / #define
#   2. dtc  — compile preprocessed .dts into .dtb

KERNELDIR := /home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay
DTS := imx6ull-southbay-emmc.dts
BUILD_DIR ?= $(CURDIR)/../build
BUILD_DTS := $(BUILD_DIR)/dts
DTS_TMP  := $(BUILD_DTS)/$(DTS:.dts=.dts.tmp)
DTB := $(BUILD_DTS)/$(DTS:.dts=.dtb)

# Use kernel's dtc (not system dtc — may not be installed)
DTC := $(KERNELDIR)/scripts/dtc/dtc

# C preprocessor (system cpp — no cross-compile needed for DTS)
CPP := cpp

# Flags matching kernel's cmd_dtc
CPP_FLAGS := -nostdinc -E -P -undef -D__DTS__ -x assembler-with-cpp
CPP_INC   := -I $(KERNELDIR)/arch/arm/boot/dts -I $(KERNELDIR)/include

all: $(DTB)

# Step 1: preprocess #include / #define
$(DTS_TMP): $(DTS)
	mkdir -p $(BUILD_DTS)
	$(CPP) $(CPP_FLAGS) $(CPP_INC) -o $@ $<

# Step 2: compile to dtb
$(DTB): $(DTS_TMP)
		$(DTC) -I dts -O dtb -o $@ \
			-i $(KERNELDIR)/arch/arm/boot/dts/ \
			-i $(KERNELDIR)/include/ \
			$<

clean:
	rm -rf $(BUILD_DTS)

.PHONY: all clean
