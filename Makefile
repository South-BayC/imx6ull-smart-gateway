# ============================================================
# 顶层 Makefile - i.MX6ULL 双项目构建入口
# ============================================================
# 子项目：
#   - core/lvgl      : LVGL 智能安防网关 (本任务 P7-2)
#   - imx6ull-drivers: 内核驱动 (gt9147, mxsfb 等)
# ============================================================

# ------------------------------------------------------------------
# 全局配置 (可被子 Makefile 继承或覆盖)
# ------------------------------------------------------------------
# 内核源码目录
KDIR ?= /home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay

# 交叉编译工具链前缀
CROSS_COMPILE ?= arm-linux-gnueabihf-

# 构建输出根目录
BUILD_DIR ?= $(CURDIR)/build

# ------------------------------------------------------------------
# 伪目标
# ------------------------------------------------------------------
.PHONY: all lvgl drivers clean help

# 默认目标：构建所有子项目
all: lvgl drivers

# ------------------------------------------------------------------
# LVGL 智能安防网关
# ------------------------------------------------------------------
lvgl:
	@echo "=========================================="
	@echo "Building LVGL Gateway..."
	@echo "=========================================="
	@$(MAKE) -C core/lvgl \
		KDIR="$(KDIR)" \
		CROSS_COMPILE="$(CROSS_COMPILE)" \
		BUILD_DIR="$(BUILD_DIR)" \
		all

# LVGL 详细大小检查
lvgl-size:
	@$(MAKE) -C core/lvgl \
		KDIR="$(KDIR)" \
		CROSS_COMPILE="$(CROSS_COMPILE)" \
		BUILD_DIR="$(BUILD_DIR)" \
		size

# LVGL 验收检查
lvgl-check:
	@$(MAKE) -C core/lvgl \
		KDIR="$(KDIR)" \
		CROSS_COMPILE="$(CROSS_COMPILE)" \
		BUILD_DIR="$(BUILD_DIR)" \
		check

# LVGL 部署到 NFS
# 用法: make lvgl-send NFS_SERVER=192.168.1.100 NFS_ROOTFS=/nfs/rootfs
lvgl-send:
	@$(MAKE) -C core/lvgl \
		KDIR="$(KDIR)" \
		CROSS_COMPILE="$(CROSS_COMPILE)" \
		BUILD_DIR="$(BUILD_DIR)" \
		send NFS_SERVER="$(NFS_SERVER)" NFS_ROOTFS="$(NFS_ROOTFS)"

# ------------------------------------------------------------------
# 内核驱动模块 (gt9147 等，已验证不再修改)
# ------------------------------------------------------------------
drivers:
	@echo "=========================================="
	@echo "Building Kernel Drivers..."
	@echo "=========================================="
	@$(MAKE) -C imx6ull-drivers \
		KDIR="$(KDIR)" \
		CROSS_COMPILE="$(CROSS_COMPILE)" \
		modules

# 驱动安装到 NFS
drivers-install:
	@$(MAKE) -C imx6ull-drivers \
		KDIR="$(KDIR)" \
		CROSS_COMPILE="$(CROSS_COMPILE)" \
		modules_install INSTALL_MOD_PATH="$(NFS_ROOTFS)"

# ------------------------------------------------------------------
# 清理所有构建产物
# ------------------------------------------------------------------
clean:
	@echo "Cleaning all build artifacts..."
	@$(MAKE) -C core/lvgl clean 2>/dev/null || true
	@$(MAKE) -C imx6ull-drivers clean 2>/dev/null || true
	@rm -rf $(BUILD_DIR)

# ------------------------------------------------------------------
# 帮助信息
# ------------------------------------------------------------------
help:
	@echo "i.MX6ULL 双项目顶层 Makefile"
	@echo ""
	@echo "全局配置变量:"
	@echo "  KDIR            内核源码目录"
	@echo "  CROSS_COMPILE   交叉编译前缀"
	@echo "  BUILD_DIR       构建输出目录 (默认: ./build)"
	@echo ""
	@echo "主要目标:"
	@echo "  make                    构建所有 (lvgl + drivers)"
	@echo "  make lvgl               仅构建 LVGL 网关"
	@echo "  make lvgl-size          显示 LVGL 产物大小详情"
	@echo "  make lvgl-check         LVGL 验收检查 (<5MB, ARM, 可执行)"
	@echo "  make lvgl-send NFS_SERVER=IP NFS_ROOTFS=PATH  部署 LVGL 到 NFS"
	@echo "  make drivers            仅构建内核驱动"
	@echo "  make drivers-install    安装驱动到 NFS rootfs"
	@echo "  make clean              清理所有构建产物"
	@echo "  make help               显示此帮助"
	@echo ""
	@echo "示例:"
	@echo "  make lvgl KDIR=/path/to/kernel CROSS_COMPILE=arm-linux-gnueabihf-"
	@echo "  make lvgl-send NFS_SERVER=192.168.1.100 NFS_ROOTFS=/nfs/rootfs"