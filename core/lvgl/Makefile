# ============================================================
# core/lvgl/Makefile - LVGL 智能安防网关交叉编译构建脚本
# ============================================================
# 目标：生成 build/lvgl/lvgl_gateway (<5MB, 可执行)
# 交叉编译链：arm-linux-gnueabihf-gcc (Linaro 4.9.4 或兼容版本)
# 依赖内核源码：KDIR=/home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay
# 运行环境：i.MX6ULL + mxsfb fbdev (/dev/fb0) + GT911 evdev (/dev/input/event1)
# ============================================================

# ------------------------------------------------------------------
# 用户可覆盖的配置变量 (命令行传入或环境变量)
# ------------------------------------------------------------------
# 内核源码目录 (用于获取头文件，如 linux/fb.h, linux/input.h 等)
KDIR ?= /home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay

# 交叉编译工具链前缀
CROSS_COMPILE ?= arm-linux-gnueabihf-

# 构建输出目录 (相对于项目根目录)
BUILD_DIR ?= $(CURDIR)/../build

# 目标可执行文件名
TARGET := lvgl_gateway

# ------------------------------------------------------------------
# 工具链定义
# ------------------------------------------------------------------
CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)gcc
STRIP   := $(CROSS_COMPILE)strip
SIZE    := $(CROSS_COMPILE)size
OBJCOPY := $(CROSS_COMPILE)objcopy

# ------------------------------------------------------------------
# 编译/链接标志
# ------------------------------------------------------------------
# C 标准：C99，启用常用警告，优化等级 Os (体积优先)
CFLAGS  := -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
           -Os -ffunction-sections -fdata-sections \
           -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L

# 包含路径：
#  1. LVGL 核心头文件
#  2. 项目 lv_conf.h
#  3. 项目源码目录 (src/, src/ui/, src/font/)
#  4. 内核头文件 (fbdev, evdev 等)
INCLUDES := -I$(CURDIR)/lvgl/src \
            -I$(CURDIR) \
            -I$(CURDIR)/src \
            -I$(CURDIR)/src/ui \
            -I$(CURDIR)/src/font \
            -I$(KDIR)/include \
            -I$(KDIR)/arch/arm/include \
            -I$(KDIR)/arch/arm/include/generated \
            -I$(KDIR)/arch/arm/include/uapi \
            -I$(KDIR)/include/uapi

# 链接标志：启用 gc-sections 移除未使用代码，静态链接减少依赖
LDFLAGS := -Wl,--gc-sections -static -lpthread -lrt -lm

# ------------------------------------------------------------------
# 源文件收集
# ------------------------------------------------------------------

# LVGL 核心源文件 (lvgl/src/ 下所有 .c 文件，排除 demos、examples、tests)
LVGL_CORE_SRCS := $(shell find $(CURDIR)/lvgl/src -name "*.c" \
    ! -path "*/demos/*" \
    ! -path "*/examples/*" \
    ! -path "*/tests/*" \
    ! -path "*/.git/*" \
    2>/dev/null | sort)

# 项目源文件：src/*.c (含 main.c, lvgl_port.c，排除 test_*.c 测试文件)
PROJECT_SRCS := $(filter-out $(CURDIR)/src/test_%.c,$(wildcard $(CURDIR)/src/*.c))

# UI 模块源文件：src/ui/*.c
UI_SRCS := $(wildcard $(CURDIR)/src/ui/*.c)

# 字体源文件：src/font/*.c (lv_font_SHSC_16.c, lv_font_SHSC_20.c)
FONT_SRCS := $(wildcard $(CURDIR)/src/font/*.c)

# 所有源文件合并
LVGL_SRCS := $(LVGL_CORE_SRCS) $(PROJECT_SRCS) $(UI_SRCS) $(FONT_SRCS)

# 目标文件列表 (放在 BUILD_DIR/obj/ 下)
OBJ_DIR := $(BUILD_DIR)/obj
OBJS := $(patsubst $(CURDIR)/%.c,$(OBJ_DIR)/%.o,$(LVGL_SRCS))

# 最终输出路径
OUTPUT_DIR := $(BUILD_DIR)/lvgl
TARGET_PATH := $(OUTPUT_DIR)/$(TARGET)

# ------------------------------------------------------------------
# 伪目标
# ------------------------------------------------------------------
.PHONY: all clean install send size check help

# 默认目标
all: $(TARGET_PATH)

# ------------------------------------------------------------------
# 构建规则
# ------------------------------------------------------------------

# 创建输出目录
$(OUTPUT_DIR) $(OBJ_DIR):
	@mkdir -p $@

# 编译单个 .c -> .o
$(OBJ_DIR)/%.o: $(CURDIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# 链接生成最终可执行文件
$(TARGET_PATH): $(OBJS) | $(OUTPUT_DIR)
	@echo "  LD      $@"
	@$(LD) $(OBJS) $(LDFLAGS) -o $@
	@echo "  STRIP   $@"
	@$(STRIP) --strip-unneeded $@ 2>/dev/null || true
	@echo ""
	@echo "=========================================="
	@echo "Build successful: $(TARGET_PATH)"
	@$(SIZE) $@
	@echo "=========================================="

# ------------------------------------------------------------------
# 辅助目标
# ------------------------------------------------------------------

# 显示目标文件大小详情
size: $(TARGET_PATH)
	@echo "=== 文件大小 ==="
	@ls -lh $(TARGET_PATH)
	@echo ""
	@echo "=== 段大小 ==="
	@$(SIZE) -A $(TARGET_PATH)
	@echo ""
	@echo "=== 依赖库 (动态链接检查) ==="
	@$(CROSS_COMPILE)readelf -d $(TARGET_PATH) 2>/dev/null | grep NEEDED || echo "  静态链接 (无动态依赖)"

# 检查产物是否符合要求 (<5MB, 可执行)
check: $(TARGET_PATH)
	@echo "=== 验收检查 ==="
	@FILE_SIZE=$$(stat -c%s $(TARGET_PATH) 2>/dev/null || stat -f%z $(TARGET_PATH)); \
	MAX_SIZE=$$((5 * 1024 * 1024)); \
	if [ $$FILE_SIZE -le $$MAX_SIZE ]; then \
		echo "  [PASS] 文件大小: $$(($$FILE_SIZE / 1024)) KB (< 5 MB)"; \
	else \
		echo "  [FAIL] 文件大小: $$(($$FILE_SIZE / 1024)) KB (>= 5 MB)"; \
		exit 1; \
	fi
	@if file $(TARGET_PATH) | grep -q "executable"; then \
		echo "  [PASS] 可执行文件格式正确"; \
	else \
		echo "  [FAIL] 非可执行文件"; \
		exit 1; \
	fi
	@if file $(TARGET_PATH) | grep -q "ARM"; then \
		echo "  [PASS] ARM 架构 (arm-linux-gnueabihf)"; \
	else \
		echo "  [WARN] 架构检查: $$(file $(TARGET_PATH))"; \
	fi
	@echo "  [PASS] 所有检查通过"

# 清理构建产物
clean:
	@echo "  CLEAN   $(BUILD_DIR)"
	@rm -rf $(BUILD_DIR)

# 部署到 NFS rootfs (需配置 NFS_SERVER 和 NFS_ROOTFS 环境变量)
# 用法: make send NFS_SERVER=192.168.1.100 NFS_ROOTFS=/nfs/rootfs
send: $(TARGET_PATH)
	@if [ -z "$(NFS_SERVER)" ] || [ -z "$(NFS_ROOTFS)" ]; then \
		echo "用法: make send NFS_SERVER=<IP> NFS_ROOTFS=<路径>"; \
		echo "示例: make send NFS_SERVER=192.168.1.100 NFS_ROOTFS=/nfs/rootfs"; \
		exit 1; \
	fi
	@echo "=== 部署到板端 NFS ==="
	@echo "目标: $(NFS_SERVER):$(NFS_ROOTFS)"
	@mkdir -p $(NFS_ROOTFS)/usr/bin
	@cp $(TARGET_PATH) $(NFS_ROOTFS)/usr/bin/
	@echo "  已复制: $(TARGET_PATH) -> $(NFS_ROOTFS)/usr/bin/"
	@echo ""
	@echo "=== 板端验证步骤 ==="
	@echo "1. 登录板端: ssh root@$(NFS_SERVER)"
	@echo "2. 检查设备节点:"
	@echo "   ls -l /dev/fb0"
	@echo "   ls -l /dev/input/event1"
	@echo "3. 运行程序:"
	@echo "   /usr/bin/lvgl_gateway"
	@echo "4. 或配置自启动 (/etc/init.d/rcS 添加):"
	@echo "   echo '/usr/bin/lvgl_gateway &' >> /etc/init.d/rcS"

# 显示帮助信息
help:
	@echo "core/lvgl/Makefile - LVGL 智能安防网关交叉编译"
	@echo ""
	@echo "可配置变量:"
	@echo "  KDIR            内核源码目录 (默认: /home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay)"
	@echo "  CROSS_COMPILE   交叉编译前缀 (默认: arm-linux-gnueabihf-)"
	@echo "  BUILD_DIR       构建输出目录 (默认: ../build)"
	@echo ""
	@echo "常用目标:"
	@echo "  make                    编译生成 lvgl_gateway"
	@echo "  make size               显示详细大小信息"
	@echo "  make check              验收检查 (<5MB, 可执行, ARM)"
	@echo "  make clean              清理构建产物"
	@echo "  make send NFS_SERVER=IP NFS_ROOTFS=PATH  部署到 NFS"
	@echo "  make help               显示此帮助"

# ------------------------------------------------------------------
# 依赖文件自动生成 (可选，启用后需 make clean 重新编译)
# ------------------------------------------------------------------
# DEPFLAGS = -MMD -MP -MF $(@:.o=.d)
# -include $(OBJS:.o=.d)