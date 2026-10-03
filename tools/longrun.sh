#!/bin/sh
# 长跑 rtsp2lcd 并周期性采样小核探针, 看流是否稳定、滞留是否收敛
export LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd
DUR=${1:-90}
killall rtsp2lcd 2>/dev/null
sleep 1
: > /tmp/run.log
/root/rtsp2lcd rtsp://127.0.1.1/h264 > /tmp/run.log 2>&1 &
PID=$!
echo "rtsp2lcd pid=$PID 跑 ${DUR}s"
i=0
while [ $i -lt $DUR ]; do
    sleep 15
    i=$((i + 15))
    echo "===== t=${i}s ====="
    sh /root/probe.sh
    echo "  最近日志: $(grep -E '已送' /tmp/run.log | tail -1)"
    echo "  异常行  : $(grep -cE '流结束|停顿|断开' /tmp/run.log) 条"
done
kill $PID 2>/dev/null
wait $PID 2>/dev/null
echo "===== 完整 stdout 尾部 ====="
tail -25 /tmp/run.log
echo "===== dbg 日志尾部 ====="
tail -8 /tmp/rtsp2lcd.log
