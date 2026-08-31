#!/bin/bash
# ============================================================
# imx6ull 双项目 P0-1 环境盘点脚本
# 用法： 拷贝到 Ubuntu 主机后  bash check_env.sh
# 用途： 摸清环境现状（Ubuntu 版本/基础工具/交叉编译链/开发目录/正点原子资料）
# ============================================================
echo "========== ① Ubuntu 版本 =========="
lsb_release -a 2>/dev/null || cat /etc/os-release 2>/dev/null
echo

echo "========== ② 基础工具检查 =========="
for cmd in gcc g++ git vim curl wget make cmake minicom picocom net-tools; do
    if command -v $cmd >/dev/null 2>&1; then
        echo "[OK]   $cmd -> $(command -v $cmd)"
    else
        echo "[缺失] $cmd"
    fi
done
echo "--- NFS / TFTP 服务 ---"
dpkg -l 2>/dev/null | grep -E "nfs-kernel-server|tftp-hpa" || echo "[缺失] nfs-kernel-server 或 tftp-hpa"
echo

echo "========== ③ 交叉编译链 =========="
if command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1; then
    echo "[OK]   交叉编译链已存在："
    arm-linux-gnueabihf-gcc --version | head -1
else
    echo "[缺失] arm-linux-gnueabihf-gcc 未安装"
fi
echo "--- /opt 目录内容（检查是否有已解压的工具链） ---"
ls /opt/ 2>/dev/null || echo "(/opt 为空或不存在)"
echo

echo "========== ④ 开发目录 ~/imx6ull =========="
if [ -d ~/imx6ull ]; then
    echo "[OK] ~/imx6ull 已存在，内容如下："
    ls -la ~/imx6ull/
else
    echo "[缺失] ~/imx6ull 不存在（未创建）"
fi
echo

echo "========== ⑤ 正点原子资料 =========="
ls ~/ 2>/dev/null | grep -iE "alientek|imx6ull|资料|embedded|disk|光" || echo "[未找到] 主目录下未发现正点原子资料目录"
echo
echo "========== 盘点完成 =========="
echo "请把以上完整输出贴回给指导会话。"
