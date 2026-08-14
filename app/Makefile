# SPDX-License-Identifier: GPL-2.0
# Makefile for INDAQ user-space test programs and libdaq
# Output goes to BUILD_DIR/app/

CROSS_COMPILE ?= arm-linux-gnueabihf-
CC := $(CROSS_COMPILE)gcc
AR := $(CROSS_COMPILE)ar
BUILD_DIR ?= $(CURDIR)/../build
BUILD_APP := $(BUILD_DIR)/app

# Sources
LIBDAQ_SRC := libdaq.c
LIBDAQ_OBJ := libdaq.o

# Test programs (standalone, no libdaq dependency)
TARGETS := indaq_test

# Libdaq link test
LIBDAQ_TEST := test_libdaq

# NFS rootfs install path
INSTALL_DIR := /home/szh/linux/nfs/rootfs/lib/modules/4.1.15

# Default target must be first
all: libdaq.a libdaq.so $(TARGETS) $(BUILD_APP)/$(LIBDAQ_TEST)

# Build libdaq as static library
libdaq.a: $(LIBDAQ_SRC)
	mkdir -p $(BUILD_APP)
	$(CC) -c -o $(BUILD_APP)/$(LIBDAQ_OBJ) $^ -Wall -fPIC
	$(AR) rcs $(BUILD_APP)/$@ $(BUILD_APP)/$(LIBDAQ_OBJ)

# Build libdaq as shared library (optional, for Python ctypes)
libdaq.so: $(LIBDAQ_SRC)
	$(CC) -shared -fPIC -o $(BUILD_APP)/$@ $^ -Wall

# Build each test program (standalone, no libdaq dependency)
$(TARGETS): %: %.c
	mkdir -p $(BUILD_APP)
	$(CC) -o $(BUILD_APP)/$@ $^ -Wall

# Build libdaq link test (links against libdaq.so)
$(BUILD_APP)/$(LIBDAQ_TEST): $(LIBDAQ_TEST).c $(BUILD_APP)/libdaq.so
	mkdir -p $(BUILD_APP)
	$(CC) -o $@ $< -I$(CURDIR) -L$(BUILD_APP) -ldaq -Wall

# Phony: 'make test_libdaq' runs the real build
.PHONY: $(LIBDAQ_TEST)
$(LIBDAQ_TEST): $(BUILD_APP)/$(LIBDAQ_TEST)

install: all
	sudo cp $(BUILD_APP)/indaq_test $(INSTALL_DIR)/
	sudo cp $(BUILD_APP)/test_libdaq $(INSTALL_DIR)/
	sudo cp $(BUILD_APP)/libdaq.so $(INSTALL_DIR)/../
	sudo cp $(BUILD_APP)/libdaq.a $(INSTALL_DIR)/../

clean:
	rm -rf $(BUILD_APP)

.PHONY: all install clean
