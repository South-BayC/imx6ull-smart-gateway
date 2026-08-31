#!/bin/bash
# ============================================================
# imx6ull 双项目 P0-2 环境部署脚本（基础工具 + 开发目录 + 交叉编译链）
# 用法：拷到 Ubuntu 主机后  bash setup_env.sh
# 前置：建议先跑 check_env.sh 盘点；apt 部分需要 sudo 权限
# ============================================================
set -e

echo "========== 1. 安装基础工具（手册 2.1） =========="
sudo apt update
sudo apt install -y \
    build-essential git vim curl wget \
    minicom picocom net-tools \
    nfs-kernel-server tftp-hpa \
    libncurses5-dev libssl-dev \
    bison flex bc u-boot-tools \
    pkg-config libtool autoconf automake \
    python3 python3-pip python3-venv \
    ssh openssh-server
echo "[OK] 基础工具安装完成"

echo
echo "========== 2. 创建开发目录（手册 2.1） =========="
mkdir -p ~/imx6ull/{edge-ai-gateway,imx6ull-drivers,buildroot,linux-imx6ull,rootfs}
echo "[OK] 目录已创建："
ls -la ~/imx6ull/

echo
echo "========== 3. 交叉编译链部署（手册 2.2） =========="
echo "来源选择（务必先核对盘点到的情况）："
echo "  [1] 正点原子资料光盘 gcc-linaro-4.9.4-2017.01（★推荐：与出厂 4.9.88 内核配套）"
echo "  [2] apt 安装 gcc-arm-linux-gnueabihf（版本较新，编译老内核可能报错）"
echo "  [3] 已安装，跳过本步"
read -r -p "输入 1 / 2 / 3： " choice

case "$choice" in
  1)
    # 找到 tar.xz 路径（资料盘通常挂载在 /media 下）
    TARBALL=$(find /media /home /mnt -maxdepth 4 -name "gcc-linaro-4.9.4-2017.01*.tar.xz" 2>/dev/null | head -1)
    if [ -z "$TARBALL" ]; then
        echo "[!] 未自动找到工具链压缩包，请手动指定路径："
        read -r -p "输入 tar.xz 完整路径（或直接回车跳过，稍后手动部署）： " TARBALL
    fi
    if [ -n "$TARBALL" ] && [ -f "$TARBALL" ]; then
        sudo mkdir -p /opt
        sudo tar -xJf "$TARBALL" -C /opt
        TOOLCHAIN_DIR=$(ls -d /opt/gcc-linaro-* 2>/dev/null | head -1)
        if [ -n "$TOOLCHAIN_DIR" ]; then
            grep -q "$TOOLCHAIN_DIR/bin" ~/.bashrc || \
                echo "export PATH=$TOOLCHAIN_DIR/bin:\$PATH" >> ~/.bashrc
            echo "[OK] 工具链解压至 $TOOLCHAIN_DIR，PATH 已写入 ~/.bashrc"
            echo "     执行 source ~/.bashrc 后验证：arm-linux-gnueabihf-gcc --version"
        fi
    else
        echo "[!] 未部署工具链，请手动参考手册 2.2 节完成"
    fi
    ;;
  2)
    sudo apt install -y gcc-arm-linux-gnueabihf
    echo "[OK] apt 版工具链已装（注意：编译 4.9.88 内核可能需额外适配）"
    ;;
  3)
    echo "[跳过] 请自行确认 arm-linux-gnueabihf-gcc --version 正常"
    ;;
  *)
    echo "[!] 无效输入，跳过交叉编译链（请稍后手动处理）"
    ;;
esac

echo
echo "========== P0-2 部署完成 =========="
echo "剩余手动项："
echo "  1) source ~/.bashrc  &&  arm-linux-gnueabihf-gcc --version   # 验证工具链"
echo "  2) 内核源码获取（Gitee: gitee.com/GuangzhouXingyi/ebf_linux_kernel 或资料光盘）—— 见 P0-3"
echo "请把本脚本完整输出贴回给指导会话。"
