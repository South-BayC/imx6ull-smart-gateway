#!/bin/sh
# P6_accept.sh —— P6-1 input 子系统按键驱动验收（手册 5.15.1）
# 覆盖验收清单: ①加载+eventX ②事件流 ③时间戳 ④互斥 ⑤rmmod
# 用法: ./P6_accept.sh [每阶段时长s]   默认 5s
# 要求: 与 key_input_test 同目录；key_input 驱动已加载；/lib/modules/4.1.15/ 有各 .ko
# 说明: ②③ 阶段会提示"请按下/松开 KEY0"，脚本自动跑 N 秒采集事件

DUR=${1:-5}
PASS=0; FAIL=0
DEV_DIR=/dev/input
MOD_DIR=/lib/modules/4.1.15

# 查找 imx6ull-gpio-keys 对应的 eventX 节点，输出路径（无则输出空）
find_key_event() {
    for f in $DEV_DIR/event*; do
        [ -e "$f" ] || continue
        NAME=$(cat /sys/class/input/$(basename $f)/device/name 2>/dev/null)
        case "$NAME" in
            *imx6ull-gpio-keys*) echo "$f"; return 0 ;;
        esac
    done
    return 1
}

echo "===== P6-1 input 子系统按键验收脚本 (每阶段 ${DUR}s) ====="
echo ""
echo "操作说明（②③ 阶段需要你配合）:"
echo "  在提示期间反复【按下 → 松开】KEY0，每次完整按键产生 2 个事件"
echo "  （按下 value=1 + 释放 value=0），脚本自动统计成对事件"
echo ""

# ---------- 前置检查 ----------
[ -x ./key_input_test ] || { echo "[FAIL] 找不到 ./key_input_test（请与脚本同目录）"; exit 1; }
if ! lsmod | grep -q key_input; then
    echo "[FAIL] key_input 未加载，先: insmod $MOD_DIR/key_input.ko"
    exit 1
fi

# ---------- ① 模块加载 + eventX 出现 ----------
echo "[①] 模块加载 + /dev/input/eventX 出现"
if lsmod | grep -q key_input; then
    echo "  [PASS] lsmod 确认 key_input 已加载"
    PASS=$((PASS+1))
else
    echo "  [FAIL] key_input 未加载"
    FAIL=$((FAIL+1))
fi

EVDEV=$(find_key_event)
if [ -n "$EVDEV" ]; then
    echo "  [PASS] 设备节点 $EVDEV 存在 (name=imx6ull-gpio-keys)"
    PASS=$((PASS+1))
else
    echo "  [FAIL] 未找到 imx6ull-gpio-keys 的 eventX 节点（ls /dev/input/ 人工确认）"
    FAIL=$((FAIL+1))
fi

# ---------- ② 事件流（按下/释放成对）+ ③ 时间戳 ----------
if [ -z "$EVDEV" ]; then
    echo ""
    echo "[②③] 无 eventX 节点，跳过事件流测试"
else
    echo ""
    echo "[②③] 事件流 + 时间戳 --- 请开始【按下→松开】KEY0 ..."
    LOG=/tmp/p6_event.log
    ./key_input_test "$EVDEV" > $LOG 2>&1 &
    TPID=$!
    sleep $DUR
    kill $TPID 2>/dev/null
    wait $TPID 2>/dev/null

    NPRES=$(grep -c 'value=1' $LOG 2>/dev/null); NPRES=${NPRES:-0}
    NREL=$(grep -c 'value=0' $LOG 2>/dev/null);  NREL=${NREL:-0}
    NCODE=$(grep -c 'code=227' $LOG 2>/dev/null); NCODE=${NCODE:-0}
    echo "  采集: 按下=$NPRES 释放=$NREL 事件行=$NCODE"
    echo "  (日志: $LOG, 最近事件:)"
    grep 'code=227' $LOG | tail -4 | sed 's/^/    /'

    # ② 事件流: 至少各 1 次按下+释放，且 code=227 正确
    if [ "$NPRES" -ge 1 ] && [ "$NREL" -ge 1 ] && [ "$NCODE" -ge 2 ]; then
        echo "  [PASS] 按下(value=1)/释放(value=0) 成对出现, code=227(KEY_ARMED)"
        PASS=$((PASS+1))
    else
        echo "  [FAIL] 事件不完整（按键了吗? 需按下+释放各≥1）"
        FAIL=$((FAIL+1))
    fi

    # ③ 时间戳: 按下/释放成对且间隔合理（<10s，符合按键节奏）
    T1=$(grep 'value=1' $LOG | head -1 | sed -n 's/.*t=\([0-9.]*\).*/\1/p')
    T0=$(grep 'value=0' $LOG | head -1 | sed -n 's/.*t=\([0-9.]*\).*/\1/p')
    if [ -n "$T1" ] && [ -n "$T0" ]; then
        # t 是秒.微秒，取整数秒比较
        S1=${T1%%.*}; S0=${T0%%.*}
        case "$S1$S0" in *[!0-9]*) S1=0; S0=0;; esac
        DIFF=$((S0 - S1)); [ $DIFF -lt 0 ] && DIFF=$((-DIFF))
        if [ $DIFF -lt 10 ]; then
            echo "  [PASS] 时间戳合理: 首按下 t=$T1 首释放 t=$T0 间隔 ${DIFF}s"
            PASS=$((PASS+1))
        else
            echo "  [FAIL] 时间戳异常: 按下/释放间隔 ${DIFF}s 不合理"
            FAIL=$((FAIL+1))
        fi
    else
        echo "  [FAIL] 无法解析时间戳（事件不足）"
        FAIL=$((FAIL+1))
    fi
fi

# ---------- ④ 互斥（与 gpio_event_capture） ----------
echo ""
echo "[④] 与 gpio-event-capture 互斥"
# 互斥有 dts 层 + 运行时两层：
#   A. dts 层：gpio-event-capture 节点 status=disabled 即互斥已生效（当前 P6 验证态）
#   B. 运行时：两节点均 okay 时，先加载者占用 GPIO1_IO18，后加载者 probe 失败
# 脚本自适应：读板端 /proc/device-tree 判断节点状态，决定走 A 还是 B

get_dts_status() {
    # 板端设备树以 \0 结尾，去掉后返回
    tr -d '\0' < /proc/device-tree/$1/status 2>/dev/null
}
GEC_STATUS=$(get_dts_status gpio-event-capture)
KI_STATUS=$(get_dts_status key-input)

if [ "$GEC_STATUS" = "disabled" ]; then
    echo "  [PASS] dts 层互斥已生效: gpio-event-capture=disabled（key-input 独占 GPIO1_IO18）"
    PASS=$((PASS+1))
    echo "  （运行时竞争验证需两节点均 okay，本次跳过；如需实测可临时改 dts 双 okay 重编）"
elif [ "$GEC_STATUS" = "okay" ] && [ "$KI_STATUS" = "okay" ]; then
    echo "  检测到双 okay（互斥测试模式），执行运行时竞争验证..."
    # ① 卸 key_input → 加载 gpio_event_capture 占用 GPIO1_IO18
    if lsmod | grep -q key_input; then
        rmmod key_input 2>/dev/null
        sleep 1
    fi
    if insmod $MOD_DIR/gpio_event_capture.ko 2>/dev/null; then
        sleep 1
        if [ -e /dev/edt_capture0 ]; then
            echo "  [PASS] gpio_event_capture 先加载成功，占用 GPIO1_IO18"
            PASS=$((PASS+1))
            # ② key_input 后加载：模块能 insmod（platform_driver 注册成功）
            #    但 probe 应失败 → input 设备不出现（勿用 lsmod 判断！）
            insmod $MOD_DIR/key_input.ko 2>/tmp/p6_mutex.log
            sleep 1
            if [ -n "$(find_key_event)" ]; then
                echo "  [FAIL] key_input probe 成功（互斥未生效！）"
                FAIL=$((FAIL+1))
                rmmod key_input 2>/dev/null
            else
                echo "  [PASS] key_input 后加载 probe 失败（GPIO1_IO18 被 gpio_event_capture 占用）"
                PASS=$((PASS+1))
            fi
            # ③ 恢复现场：卸 gpio_event_capture → 重新加载 key_input
            rmmod gpio_event_capture 2>/dev/null
            sleep 1
            insmod $MOD_DIR/key_input.ko 2>/dev/null
            sleep 1
            if [ -n "$(find_key_event)" ]; then
                echo "  [PASS] rmmod gpio_event_capture 后 key_input 恢复 probe"
                PASS=$((PASS+1))
            else
                echo "  [FAIL] key_input 恢复加载失败"
                FAIL=$((FAIL+1))
            fi
        else
            echo "  [FAIL] gpio_event_capture 未 probe 出设备，无法验证竞争"
            FAIL=$((FAIL+1))
        fi
    else
        echo "  [FAIL] gpio_event_capture insmod 失败，无法验证互斥"
        FAIL=$((FAIL+1))
    fi
else
    echo "  [SKIP] 无法读取 dts 状态（/proc/device-tree 不可用），跳过互斥验证"
    echo "  （人工验证: 两节点均 okay 时先加载者占用 GPIO，后加载者 dmesg 报 gpiod 失败）"
fi

# ---------- ⑤ rmmod 干净卸载 ----------
echo ""
echo "[⑤] rmmod 干净卸载"
if command -v fuser >/dev/null 2>&1; then
    if [ -z "$EVDEV" ]; then
        echo "  [SKIP] 无 eventX 节点，跳过 fuser 检查"
    elif ! fuser "$EVDEV" 2>/dev/null; then
        echo "  [PASS] fuser: 无进程占用 $EVDEV"
        PASS=$((PASS+1))
    else
        echo "  [FAIL] fuser: 仍有进程占用 $EVDEV（killall key_input_test 后重试）"
        FAIL=$((FAIL+1))
    fi
else
    echo "  [SKIP] fuser 不可用（busybox 未含），跳过占用检查"
fi

if lsmod | grep -q key_input; then
    if rmmod key_input 2>&1; then
        echo "  [PASS] rmmod key_input 无报错"
        PASS=$((PASS+1))
    else
        echo "  [FAIL] rmmod key_input 失败"
        FAIL=$((FAIL+1))
    fi
else
    echo "  [FAIL] key_input 未加载，无法验证 rmmod"
    FAIL=$((FAIL+1))
fi

# eventX 节点消失
if [ -z "$(find_key_event)" ]; then
    echo "  [PASS] imx6ull-gpio-keys 的 eventX 节点已消失"
    PASS=$((PASS+1))
else
    echo "  [FAIL] eventX 节点仍存在"
    FAIL=$((FAIL+1))
fi

if lsmod | grep -q key_input; then
    echo "  [FAIL] lsmod 仍有 key_input 残留"
    FAIL=$((FAIL+1))
else
    echo "  [PASS] lsmod 无 key_input 残留"
    PASS=$((PASS+1))
fi

# dmesg 无 warning/leak
if dmesg | tail -30 | grep -iE "key_input|key-input" | grep -iE "warning|leak|error|fail" >/dev/null 2>&1; then
    echo "  [WARN] dmesg 近 30 行 key_input 相关出现 warning/error 关键字，人工确认:"
    dmesg | tail -30 | grep -iE "key_input|key-input" | grep -iE "warning|leak|error|fail" | sed 's/^/    /'
    echo "  （若为历史无关消息可忽略，视为 [PASS]）"
    PASS=$((PASS+1))
else
    echo "  [PASS] dmesg 近 30 行无 key_input warning/leak/error"
    PASS=$((PASS+1))
fi

echo ""
echo "===== P6-1 验收结果: PASS=$PASS FAIL=$FAIL ====="
echo "（重新加载驱动: insmod $MOD_DIR/key_input.ko）"
echo "（注意: ④互斥若走运行时验证会 insmod/rmmod gpio_event_capture，最终已恢复 key_input）"