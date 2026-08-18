#!/bin/sh
# P3_accept.sh —— P3 验收③④⑥⑦ 一键执行（板端 Buildroot）
# 用法: ./P3_accept.sh [每阶段时长s]   默认 5s
# 要求: 与 edt_capture_test / edt_capture_bench 同目录；驱动已加载
# 说明: 每阶段都会提示"请持续按键 + 碰触 pin17(GPIO_01)~VCC3.3"，脚本自动跑 N 秒

DUR=${1:-5}
DEV=/dev/edt_capture0
PASS=0; FAIL=0

echo "===== P3 验收脚本 (每阶段 ${DUR}s) ====="
echo ""
echo "操作说明（每阶段都要做）:"
echo "  ① 按 KEY0 几次        -> ch0 事件（按下+松开=2个）"
echo "  ② 杜邦线一端插 JP6 丝印 GPIO_01(pin17),"
echo "     另一端碰 VCC3.3 几次  -> ch1 事件（接触+断开=2个）"
echo "     ※ 注意: 该引脚无上拉（电平偏低/浮空），碰 GND 无效，"
echo "       必须碰 VCC3.3/高电平才有上升沿！"
echo "  （若只有 ch0 或只有 ch1，该模式判 FAIL）"
echo ""

[ -e "$DEV" ] || { echo "[FAIL] $DEV 不存在，先 insmod gpio_event_capture.ko"; exit 1; }
[ -x ./edt_capture_test ] || { echo "[FAIL] 找不到 ./edt_capture_test（请与脚本同目录）"; exit 1; }
[ -x ./edt_capture_bench ] || { echo "[FAIL] 找不到 ./edt_capture_bench"; exit 1; }

# ---------- ③ 三模式交互 ----------
echo ""
echo "[③] 三模式交互 --- 请持续按键 + 碰触 pin17(GPIO_01)~VCC3.3 ..."
for mode in "" "--nonblock" "--poll"; do
    name=${mode:-block}
    ./edt_capture_test $mode > /tmp/t_$name.log 2>&1 &
    PID=$!
    sleep $DUR
    kill $PID 2>/dev/null
    wait $PID 2>/dev/null
    C0=$(grep -c 'ch=0' /tmp/t_$name.log 2>/dev/null); C0=${C0:-0}
    C1=$(grep -c 'ch=1' /tmp/t_$name.log 2>/dev/null); C1=${C1:-0}
    echo "  $name 模式: ch0=$C0 事件, ch1=$C1 事件"
    if [ "$C0" -gt 0 ] && [ "$C1" -gt 0 ]; then
        echo "  [PASS] $name 双通道事件正常"
        PASS=$((PASS+1))
    else
        echo "  [FAIL] $name 缺通道事件 (需按键+碰触 pin17)"
        FAIL=$((FAIL+1))
    fi
done

# ---------- ④ 序列号守恒 ----------
echo ""
echo "[④] 序列号守恒 --- 请持续按键 + 碰触 pin17(GPIO_01)~VCC3.3 ..."
./edt_capture_bench --duration $DUR --report $DUR > /tmp/bench.log 2>&1
grep -E '守恒|丢失率' /tmp/bench.log
if grep -q '守恒(irq+snapshot == queued+dropped): OK' /tmp/bench.log; then
    echo "  [PASS] 序列号守恒 OK"
    PASS=$((PASS+1))
else
    echo "  [FAIL] 序列号守恒 MISMATCH"
    FAIL=$((FAIL+1))
fi

# ---------- ⑥ ftrace ----------
echo ""
echo "[⑥] ftrace tracepoint --- 请持续按键 + 碰触 pin17(GPIO_01)~VCC3.3 ..."
mount -t debugfs none /sys/kernel/debug 2>/dev/null
TR=/sys/kernel/debug/tracing
for ev in irq enqueue drop read; do
    echo 1 > $TR/events/edt_capture/edt_capture_$ev/enable 2>/dev/null
done
echo > $TR/trace
echo 1 > $TR/tracing_on
# 关键：IRQ handler 检查 running，running=0 时事件不 trace！
# 必须后台跑 nonblock 模式 START 驱动，trace 期间用户操作才有真实事件
./edt_capture_test --nonblock > /dev/null 2>&1 &
PID6=$!
sleep $DUR
echo 0 > $TR/tracing_on
kill $PID6 2>/dev/null
wait $PID6 2>/dev/null
N=$(grep -c 'edt_capture' $TR/trace 2>/dev/null)
N=${N:-0}
echo "  trace 输出 $N 行"
if [ "$N" -gt 0 ]; then
    echo "  [PASS] ftrace 4 tracepoint 可观测"
    PASS=$((PASS+1))
    head -15 $TR/trace
else
    echo "  [FAIL] ftrace 无输出（检查 events/edt_capture 是否存在）"
    FAIL=$((FAIL+1))
fi
for ev in irq enqueue drop read; do
    echo 0 > $TR/events/edt_capture/edt_capture_$ev/enable 2>/dev/null
done

# ---------- ⑦ rmmod ----------
echo ""
echo "[⑦] rmmod 干净卸载验证"
if lsmod | grep -q gpio_event_capture; then
    rmmod gpio_event_capture
    sleep 1
    if [ -e "$DEV" ]; then
        echo "  [FAIL] /dev/edt_capture0 未消失"
        FAIL=$((FAIL+1))
    elif lsmod | grep -q gpio_event_capture; then
        echo "  [FAIL] lsmod 仍有残留"
        FAIL=$((FAIL+1))
    else
        echo "  [PASS] rmmod 干净（/dev 消失、lsmod 无残留）"
        PASS=$((PASS+1))
    fi
else
    echo "  [FAIL] 驱动未加载，无法验证 rmmod"
    FAIL=$((FAIL+1))
fi

echo ""
echo "===== P3 验收结果: PASS=$PASS FAIL=$FAIL ====="
echo "（重新加载驱动: insmod /lib/modules/4.1.15/gpio_event_capture.ko）"