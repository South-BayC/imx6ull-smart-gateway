# ============================================================
# 顶层 Makefile - i.MX6ULL 双项目构建入口
# ============================================================
# 子项目：
#   - core/lvgl      : LVGL 智能安防网关 (应用层，位于本仓库)
#   - imx6ull-drivers: 内核驱动 (gt9147, mxsfb 等，位于 Linux 开发机，
#                      不在本仓库中。需另行同步到 KDIR 的源码树)
#
# 说明：
#   本仓库仅包含 LVGL 应用层源码。内核驱动模块 (imx6ull-drivers)
#   属于 Linux 源码树的一部分，不在本仓库内，因此默认 `make` 只构建
#   应用层 (lvgl)。`drivers` 目标是给那些自行拷贝了驱动子目录的主机用的，
#   驱动子目录缺失时会给出提示而不是硬失败。
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
.PHONY: all lvgl lvgl-size lvgl-check lvgl-send drivers drivers-install clean help

# 默认目标：构建本仓库内的所有子项目 (当前仅 LVGL 应用层)
all: lvgl

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
# 内核驱动模块 (gt9147, mxsfb 等)
# 注意：imx6ull-drivers 目录位于 Linux 开发机，不在本仓库。
#       若本机未拷贝该子目录，drivers 目标会给出提示而非硬失败。
# ------------------------------------------------------------------
drivers:
	@if [ -d imx6ull-drivers ]; then \
		echo "=========================================="; \
		echo "Building Kernel Drivers..."; \
		echo "=========================================="; \
		$(MAKE) -C imx6ull-drivers \
			KDIR="$(KDIR)" \
			CROSS_COMPILE="$(CROSS_COMPILE)" \
			modules; \
	else \
		echo "=========================================="; \
		echo "[SKIP] imx6ull-drivers 目录不存在 (位于 Linux 开发机)。"; \
		echo "  本仓库仅包含 LVGL 应用层。内核驱动需在 Linux 源码树中构建。"; \
		echo "=========================================="; \
	fi

# 驱动安装到 NFS
drivers-install:
	@if [ -d imx6ull-drivers ]; then \
		$(MAKE) -C imx6ull-drivers \
			KDIR="$(KDIR)" \
			CROSS_COMPILE="$(CROSS_COMPILE)" \
			modules_install INSTALL_MOD_PATH="$(NFS_ROOTFS)"; \
	else \
		echo "[SKIP] imx6ull-drivers 目录不存在，跳过驱动安装。"; \
	fi

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
	@echo "  make                    构建本仓库内所有子项目 (当前仅 LVGL)"
	@echo "  make lvgl               仅构建 LVGL 网关"
	@echo "  make lvgl-size          显示 LVGL 产物大小详情"
	@echo "  make lvgl-check         LVGL 验收检查 (<5MB, ARM, 可执行)"
	@echo "  make lvgl-send NFS_SERVER=IP NFS_ROOTFS=PATH  部署 LVGL 到 NFS"
	@echo "  make drivers            构建内核驱动 (需 imx6ull-drivers 目录)"
	@echo "  make drivers-install    安装驱动到 NFS rootfs"
	@echo "  make clean              清理所有构建产物"
	@echo "  make help               显示此帮助"
	@echo ""
	@echo "示例:"
	@echo "  make lvgl KDIR=/path/to/kernel CROSS_COMPILE=arm-linux-gnueabihf-"
	@echo "  make lvgl-send NFS_SERVER=192.168.1.100 NFS_ROOTFS=/nfs/rootfs"
	@echo ""
	@echo "注意: imx6ull-drivers (内核驱动) 位于 Linux 开发机，不在本仓库。"
	@echo "      本仓库仅含 LVGL 应用层，`make`/`make lvgl` 不受影响。"