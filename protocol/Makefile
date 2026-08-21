# SPDX-License-Identifier: GPL-2.0
# protocol/ Makefile —— SUMP 工具链（P6-2，手册 5.15.2）
#
# 目标：
#   make        交叉编译（arm-linux-gnueabihf）→ build/protocol/
#   make host   本机编译（PC 自测/演示，--sim 无需板端）
#   make clean  清理
#
# 产物：sump_server（SUMP TCP 服务器）、sump_selftest（协议自测客户端）

CROSS_COMPILE ?= arm-linux-gnueabihf-
BUILD_DIR ?= $(CURDIR)/../build
PROTOCOL_BUILD := $(BUILD_DIR)/protocol
UAPI_DIR := ../core/gpio_event_capture/uapi

CFLAGS := -O2 -Wall -Wextra -std=gnu99 -I$(UAPI_DIR)
LDFLAGS :=

SRCS := capture_source.c event_reconstructor.c sump_server.c
SELFTEST_SRC := sump_selftest.c

all: cross

cross: $(PROTOCOL_BUILD)/sump_server $(PROTOCOL_BUILD)/sump_selftest

# 交叉编译（板端运行）
$(PROTOCOL_BUILD)/sump_server: $(SRCS) capture_source.h event_reconstructor.h \
		$(UAPI_DIR)/edt_capture.h
	@mkdir -p $(PROTOCOL_BUILD)
	$(CROSS_COMPILE)gcc $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

$(PROTOCOL_BUILD)/sump_selftest: $(SELFTEST_SRC)
	@mkdir -p $(PROTOCOL_BUILD)
	$(CROSS_COMPILE)gcc $(CFLAGS) $(SELFTEST_SRC) -o $@ $(LDFLAGS)

# 本机编译（PC 自测，--sim 模式无需板端）
host: $(PROTOCOL_BUILD)/sump_server_host $(PROTOCOL_BUILD)/sump_selftest_host

$(PROTOCOL_BUILD)/sump_server_host: $(SRCS) capture_source.h event_reconstructor.h \
		$(UAPI_DIR)/edt_capture.h
	@mkdir -p $(PROTOCOL_BUILD)
	gcc $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

$(PROTOCOL_BUILD)/sump_selftest_host: $(SELFTEST_SRC)
	@mkdir -p $(PROTOCOL_BUILD)
	gcc $(CFLAGS) $(SELFTEST_SRC) -o $@ $(LDFLAGS)

clean:
	rm -rf $(PROTOCOL_BUILD)

.PHONY: all cross host clean