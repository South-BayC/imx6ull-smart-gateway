#!/bin/sh
# P4_accept.sh —— P4-B PWM 蜂鸣器验收（手册 5.13.4 PWM 项）一键执行（板端 Buildroot）
# 用法: ./P4_accept.sh
# 要求: 与 beep_test 同目录；无源蜂鸣器已接 GPIO1_IO04(PWM3_OUT 官方引脚) + GND
# 说明: 频率/占空比/开关的核心验收靠"耳朵听"，每步交互确认 y/n
#       AP3216C/ICM20608 各有独立测试程序（ap3216c_test/icm20608_test）验收，本脚本仅覆盖 beep_pwm

DEV=/dev/beep_pwm0
KO=beep_pwm
PASS=0; FAIL=0

echo "===== P4-B PWM 蜂鸣器验收 ====="
echo "操作准备: 无源蜂鸣器 正极→GPIO1_IO04(PWM3_OUT 官方引脚), 负极→GND"
echo ""

# ---------- 前置检查 ----------
[ -x ./beep_test ] || { echo "[FAIL] 找不到 ./beep_test（请与脚本同目录）"; exit 1; }
if [ ! -e "$DEV" ]; then
    echo "加载 ${KO}.ko ..."
    insmod /lib/modules/4.1.15/${KO}.ko 2>/dev/null || insmod ./${KO}.ko
    sleep 1
fi
[ -e "$DEV" ] || { echo "[FAIL] $DEV 不存在，insmod 失败？"; exit 1; }

# 交互确认辅助: 描述 -> y/n
ask() {
    printf "  $1 (y/n): "
    read ans
    [ "$ans" = "y" ] || [ "$ans" = "Y" ]
}

# ---------- ① 设备节点 + PWM 子系统 ----------
echo "[①] 设备节点与 PWM 子系统可见"
if [ -e "$DEV" ]; then
    echo "  /dev/beep_pwm0 存在 [OK]"
else
    echo "  [FAIL] $DEV 不存在"
    FAIL=$((FAIL+1))
fi
if ls /sys/class/pwm/pwmchip* >/dev/null 2>&1; then
    echo "  /sys/class/pwm: $(ls /sys/class/pwm/ | tr '\n' ' ')"
    echo "  [PASS] PWM 子系统硬件层可见"
    PASS=$((PASS+1))
else
    echo "  [FAIL] /sys/class/pwm 无 pwmchip"
    FAIL=$((FAIL+1))
fi

# ---------- ② 频率可控 ----------
echo "[②] 频率可控（500Hz 低沉 → 2000Hz 尖锐）"
# 注意: 驱动配置/开关分离设计——仅设 freq/duty 不启动 PWM，须带 --on 首次启动
./beep_test --on --freq 500 --duty 50 || { echo "  [FAIL] 500Hz 设置失败"; FAIL=$((FAIL+1)); }
echo "  当前应为低沉『嗡嗡』声（500Hz）"
if ask "听到低沉声且与上一步对比音调有变化？"; then
    ./beep_test --freq 2000 --duty 50 || { echo "  [FAIL] 2000Hz 设置失败"; FAIL=$((FAIL+1)); }
    echo "  当前应为尖锐『吱吱』声（2000Hz）"
    if ask "听到尖锐声且音调明显变高？"; then
        echo "  [PASS] 频率可控"
        PASS=$((PASS+1))
    else
        echo "  [FAIL] 2000Hz 听感异常"
        FAIL=$((FAIL+1))
    fi
else
    echo "  [FAIL] 500Hz 听感异常（检查接线/insmod）"
    FAIL=$((FAIL+1))
fi

# ---------- ③ 占空比可控 ----------
echo "[③] 占空比可控（50% 响 → 10% 小声）"
./beep_test --on --freq 1000 --duty 50
echo "  当前应为 1000Hz 较响"
./beep_test --freq 1000 --duty 10
echo "  当前应为 1000Hz 明显变小（占空比 10%）"
if ask "占空比 50%→10% 声音明显变小？"; then
    echo "  [PASS] 占空比可控"
    PASS=$((PASS+1))
else
    echo "  [FAIL] 占空比听感无变化"
    FAIL=$((FAIL+1))
fi

# ---------- ④ 开关可控 ----------
echo "[④] 开关可控（--on/--off 与 duty=0 静音）"
./beep_test --on --freq 1000 --duty 50
echo "  当前应响（1000Hz）"
if ask "当前在响？"; then
    ./beep_test --off
    echo "  --off 已执行，应静音"
    if ask "已静音？"; then
        echo "  [PASS] 开关可控"
        PASS=$((PASS+1))
    else
        echo "  [FAIL] --off 未静音"
        FAIL=$((FAIL+1))
    fi
else
    echo "  [FAIL] ON 状态无声音"
    FAIL=$((FAIL+1))
fi
./beep_test --duty 0
echo "  duty=0 已执行（静音备用手段）"

# ---------- ⑤ rmmod 五件套 ----------
echo "[⑤] rmmod 干净卸载验证"
if lsmod | grep -q $KO; then
    rmmod $KO
    sleep 1
    if [ -e "$DEV" ]; then
        echo "  [FAIL] /dev/beep_pwm0 未消失"
        FAIL=$((FAIL+1))
    elif lsmod | grep -q $KO; then
        echo "  [FAIL] lsmod 仍有残留"
        FAIL=$((FAIL+1))
    else
        echo "  [PASS] rmmod 干净（/dev 消失、lsmod 无残留、PWM 停止输出）"
        PASS=$((PASS+1))
    fi
else
    echo "  [FAIL] 驱动未加载，无法验证 rmmod"
    FAIL=$((FAIL+1))
fi

echo ""
echo "===== P4-B PWM 验收结果: PASS=$PASS FAIL=$FAIL ====="
echo "（重新加载驱动: insmod /lib/modules/4.1.15/beep_pwm.ko）"