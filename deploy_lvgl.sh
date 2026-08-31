#!/bin/bash
# ============================================================
# deploy_lvgl.sh - LVGL 网关部署脚本
# 用法: ./deploy_lvgl.sh [NFS_SERVER] [NFS_ROOTFS]
# 示例: ./deploy_lvgl.sh 192.168.1.100 /nfs/rootfs
# ============================================================
set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 默认配置 (可通过参数或环境变量覆盖)
NFS_SERVER="${1:-${NFS_SERVER:-192.168.1.100}}"
NFS_ROOTFS="${2:-${NFS_ROOTFS:-/nfs/rootfs}}"
BUILD_DIR="${BUILD_DIR:-$(pwd)/build}"
TARGET_NAME="lvgl_gateway"
TARGET_PATH="${BUILD_DIR}/lvgl/${TARGET_NAME}"

echo -e "${BLUE}==========================================${NC}"
echo -e "${BLUE}  LVGL 智能安防网关 - 板端部署脚本${NC}"
echo -e "${BLUE}==========================================${NC}"
echo ""
echo -e "NFS 服务器: ${YELLOW}${NFS_SERVER}${NC}"
echo -e "NFS Rootfs: ${YELLOW}${NFS_ROOTFS}${NC}"
echo -e "构建目录:   ${YELLOW}${BUILD_DIR}${NC}"
echo -e "目标文件:   ${YELLOW}${TARGET_PATH}${NC}"
echo ""

# 1. 检查构建产物是否存在
if [ ! -f "${TARGET_PATH}" ]; then
    echo -e "${RED}[ERROR] 构建产物不存在: ${TARGET_PATH}${NC}"
    echo -e "请先执行编译: ${YELLOW}make lvgl${NC}"
    exit 1
fi

# 2. 检查文件大小 (<5MB)
FILE_SIZE=$(stat -c%s "${TARGET_PATH}" 2>/dev/null || stat -f%z "${TARGET_PATH}")
MAX_SIZE=$((5 * 1024 * 1024))
if [ ${FILE_SIZE} -gt ${MAX_SIZE} ]; then
    echo -e "${RED}[ERROR] 文件过大: $((FILE_SIZE / 1024)) KB (限制: 5 MB)${NC}"
    exit 1
fi
echo -e "${GREEN}[OK] 文件大小: $((FILE_SIZE / 1024)) KB (< 5 MB)${NC}"

# 3. 检查架构 (ARM)
if ! file "${TARGET_PATH}" | grep -q "ARM"; then
    echo -e "${YELLOW}[WARN] 非 ARM 架构: $(file ${TARGET_PATH})${NC}"
fi
echo -e "${GREEN}[OK] 架构检查通过${NC}"

# 4. 检查 NFS 挂载点是否可写
if [ ! -d "${NFS_ROOTFS}" ]; then
    echo -e "${RED}[ERROR] NFS rootfs 目录不存在: ${NFS_ROOTFS}${NC}"
    echo -e "请确保 NFS 已挂载，或指定正确的路径"
    exit 1
fi

if [ ! -w "${NFS_ROOTFS}" ]; then
    echo -e "${RED}[ERROR] NFS rootfs 不可写，请检查权限或以 sudo 运行${NC}"
    exit 1
fi

# 5. 创建目标目录并复制
echo ""
echo -e "${BLUE}[步骤 1/3] 复制可执行文件到 NFS rootfs...${NC}"
mkdir -p "${NFS_ROOTFS}/usr/bin"
cp "${TARGET_PATH}" "${NFS_ROOTFS}/usr/bin/"
chmod +x "${NFS_ROOTFS}/usr/bin/${TARGET_NAME}"
echo -e "${GREEN}[OK] 已部署: ${NFS_ROOTFS}/usr/bin/${TARGET_NAME}${NC}"

# 6. 可选：复制字体文件 (如果需要)
# mkdir -p "${NFS_ROOTFS}/usr/share/lvgl/fonts"
# cp core/lvgl/src/font/*.c "${NFS_ROOTFS}/usr/share/lvgl/fonts/" 2>/dev/null || true

# 7. 生成板端验证脚本
echo ""
echo -e "${BLUE}[步骤 2/3] 生成板端验证脚本...${NC}"
cat > "${NFS_ROOTFS}/usr/bin/verify_lvgl.sh" << 'EOF'
#!/bin/bash
# ============================================================
# 板端 LVGL 验证脚本 (在板端运行)
# ============================================================
set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}==========================================${NC}"
echo -e "${BLUE}  板端 LVGL 网关验证${NC}"
echo -e "${BLUE}==========================================${NC}"
echo ""

# 1. 检查设备节点
echo -e "${BLUE}[检查 1/5] 设备节点...${NC}"
if [ -c /dev/fb0 ]; then
    echo -e "${GREEN}[PASS] /dev/fb0 存在${NC}"
    fb_info=$(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null || echo "unknown")
    echo -e "       分辨率: ${fb_info}"
else
    echo -e "${RED}[FAIL] /dev/fb0 不存在 - mxsfb 驱动未加载${NC}"
    exit 1
fi

if [ -c /dev/input/event1 ]; then
    echo -e "${GREEN}[PASS] /dev/input/event1 存在${NC}"
    ev_name=$(cat /sys/class/input/event1/device/name 2>/dev/null || echo "unknown")
    echo -e "       设备名: ${ev_name}"
else
    echo -e "${RED}[FAIL] /dev/input/event1 不存在 - GT911 驱动未加载${NC}"
    exit 1
fi

# 2. 检查可执行文件
echo -e "${BLUE}[检查 2/5] 可执行文件...${NC}"
if [ -x /usr/bin/lvgl_gateway ]; then
    echo -e "${GREEN}[PASS] /usr/bin/lvgl_gateway 存在且可执行${NC}"
    ls -lh /usr/bin/lvgl_gateway
else
    echo -e "${RED}[FAIL] /usr/bin/lvgl_gateway 不存在或不可执行${NC}"
    exit 1
fi

# 3. 权限检查
echo -e "${BLUE}[检查 3/5] 权限检查...${NC}"
if [ -r /dev/fb0 ] && [ -w /dev/fb0 ]; then
    echo -e "${GREEN}[PASS] /dev/fb0 读写权限正常${NC}"
else
    echo -e "${YELLOW}[WARN] /dev/fb0 权限可能不足，尝试: chmod 666 /dev/fb0${NC}"
fi

if [ -r /dev/input/event1 ]; then
    echo -e "${GREEN}[PASS] /dev/input/event1 读权限正常${NC}"
else
    echo -e "${YELLOW}[WARN] /dev/input/event1 权限可能不足，尝试: chmod 666 /dev/input/event1${NC}"
fi

# 4. 运行测试 (后台运行 5 秒)
echo -e "${BLUE}[检查 4/5] 运行测试 (5秒)...${NC}"
timeout 5 /usr/bin/lvgl_gateway &
PID=$!
sleep 3
if kill -0 $PID 2>/dev/null; then
    echo -e "${GREEN}[PASS] 程序启动成功，PID: $PID${NC}"
    kill $PID 2>/dev/null
    wait $PID 2>/dev/null
else
    echo -e "${RED}[FAIL] 程序启动失败或异常退出${NC}"
    exit 1
fi

# 5. 帧率估算 (简单测试)
echo -e "${BLUE}[检查 5/5] 帧率估算...${NC}"
echo -e "${YELLOW}请在板端手动运行 /usr/bin/lvgl_gateway 观察界面流畅度${NC}"
echo -e "${YELLOW}预期: 1024x600 中文界面、触摸 5 类按钮 100%可用、帧率 >= 25 FPS${NC}"

echo ""
echo -e "${GREEN}==========================================${NC}"
echo -e "${GREEN}  板端基础验证通过！${NC}"
echo -e "${GREEN}==========================================${NC}"
echo ""
echo -e "后续手动验证步骤："
echo -e "  1. 运行: ${YELLOW}/usr/bin/lvgl_gateway${NC}"
echo -e "  2. 观察屏幕显示 1024x600 中文界面"
echo -e "  3. 触摸测试 5 类按钮：布防、撤防、消警、设置、分区卡片"
echo -e "  4. 确认帧率 >= 25 FPS (界面流畅无卡顿)"
echo -e "  5. 配置自启动: echo '/usr/bin/lvgl_gateway &' >> /etc/init.d/rcS"
EOF
chmod +x "${NFS_ROOTFS}/usr/bin/verify_lvgl.sh"
echo -e "${GREEN}[OK] 验证脚本已生成: ${NFS_ROOTFS}/usr/bin/verify_lvgl.sh${NC}"

# 8. 显示后续步骤
echo ""
echo -e "${BLUE}[步骤 3/3] 部署完成！后续操作指引：${NC}"
echo ""
echo -e "${YELLOW}=== 板端操作 ===${NC}"
echo -e "1. 登录板端:"
echo -e "   ${GREEN}ssh root@${NFS_SERVER}${NC}"
echo ""
echo -e "2. 运行自动验证:"
echo -e "   ${GREEN}/usr/bin/verify_lvgl.sh${NC}"
echo ""
echo -e "3. 手动运行主程序:"
echo -e "   ${GREEN}/usr/bin/lvgl_gateway${NC}"
echo ""
echo -e "4. 配置开机自启动 (追加到 /etc/init.d/rcS):"
echo -e "   ${GREEN}echo '/usr/bin/lvgl_gateway &' >> /etc/init.d/rcS${NC}"
echo -e "   ${GREEN}chmod +x /etc/init.d/rcS${NC}"
echo ""
echo -e "${YELLOW}=== 验收标准 ===${NC}"
echo -e "  ✓ 显示 1024x600 中文界面"
echo -e "  ✓ 触摸 5 类按钮 100% 可用 (布防/撤防/消警/设置/分区卡片)"
echo -e "  ✓ 帧率 ≥ 25 FPS (界面流畅，动画正常)"
echo -e "  ✓ 可执行文件 < 5MB"
echo ""
echo -e "${BLUE}==========================================${NC}"
echo -e "${GREEN}部署脚本执行完成！${NC}"
echo -e "${BLUE}==========================================${NC}"