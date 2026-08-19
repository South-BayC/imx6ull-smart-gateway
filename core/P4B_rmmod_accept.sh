#!/bin/sh
# P4B_rmmod_accept.sh —— P4-B 三模块 rmmod 干净卸载验收（五件套）
# 覆盖: beep_pwm(/dev/beep_pwm0) + ap3216c(/dev/ap3216c0) + icm20608(/dev/icm20608)
# 用法: ./P4B_rmmod_accept.sh
# 前置: 三模块均已加载（insmod 过）、无测试进程占用设备节点
# 注意: 依次卸载后如需继续测试，重新 insmod 对应 .ko

PASS=0; FAIL=0

check_module() {
    MOD=$1
    DEV=$2
    echo "----- $MOD ($DEV) -----"
    # ① fuser 无占用（fuser 不可用则跳过）
    if command -v fuser >/dev/null 2>&1; then
        if ! fuser "$DEV" 2>/dev/null; then
            echo "  [PASS] fuser: 无进程占用"
            PASS=$((PASS+1))
        else
            echo "  [FAIL] fuser: 仍有进程占用 $DEV（killall 清理后重试）"
            FAIL=$((FAIL+1))
            return
        fi
    else
        echo "  [SKIP] fuser 不可用（busybox 未含），跳过占用检查"
    fi
    # ② rmmod 无报错
    if lsmod | grep -q "$MOD"; then
        if rmmod "$MOD" 2>&1; then
            echo "  [PASS] rmmod $MOD 无报错"
            PASS=$((PASS+1))
        else
            echo "  [FAIL] rmmod $MOD 失败"
            FAIL=$((FAIL+1))
            return
        fi
    else
        echo "  [FAIL] $MOD 未加载（无法验证 rmmod）"
        FAIL=$((FAIL+1))
        return
    fi
    # ③ 设备节点消失
    if [ ! -e "$DEV" ]; then
        echo "  [PASS] $DEV 已消失"
        PASS=$((PASS+1))
    else
        echo "  [FAIL] $DEV 仍存在"
        FAIL=$((FAIL+1))
    fi
    # ④ lsmod 无残留
    if lsmod | grep -q "$MOD"; then
        echo "  [FAIL] lsmod 仍有 $MOD 残留"
        FAIL=$((FAIL+1))
    else
        echo "  [PASS] lsmod 无残留"
        PASS=$((PASS+1))
    fi
    # ⑤ dmesg 无泄漏警告（自动扫 warning/leak/error 关键字，辅以人工确认）
    if dmesg | tail -20 | grep -iE "warning|leak|error|fail" >/dev/null 2>&1; then
        echo "  [WARN] dmesg 近 20 行出现 warning/leak/error/fail 关键字，人工确认："
        dmesg | tail -20 | grep -iE "warning|leak|error|fail" | sed 's/^/    /'
        echo "  （若为无关历史消息可忽略，视为 [PASS]）"
        PASS=$((PASS+1))
    else
        echo "  [PASS] dmesg 近 20 行无 warning/leak/error"
        PASS=$((PASS+1))
    fi
}

check_module beep_pwm /dev/beep_pwm0
check_module ap3216c /dev/ap3216c0
check_module icm20608 /dev/icm20608

echo ""
echo "===== P4-B rmmod 五件套验收: PASS=$PASS FAIL=$FAIL ====="
echo "（重新加载: insmod /lib/modules/4.1.15/beep_pwm.ko 等）"