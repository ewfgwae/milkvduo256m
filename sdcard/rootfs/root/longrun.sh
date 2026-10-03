#!/bin/sh
# 长跑 rtsp2lcd 并周期性采样小核探针, 看流是否稳定、滞留是否收敛
export LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd
DUR=${1:-90}
# ★ 必须先清干净: 只 killall lcdcam.sh 会留下它拉起的孤儿 rtsp2lcd,
#   而 sample_vi_fd 的 live555 服务端只能服务一个客户端 —— 第二个连接
#   收不到任何数据, 日志会刷满 "停顿 N s 无数据(g_rlen=0)"。
killall lcdcam.sh 2>/dev/null
killall rtsp2lcd 2>/dev/null
sleep 1
: > /tmp/run.log
# 断线自动重连(与 lcdcam.sh 同款循环), 否则一次意外退出就只剩半个测试
( while :; do
    /root/rtsp2lcd rtsp://127.0.1.1/h264 >> /tmp/run.log 2>&1
    echo "[longrun] rtsp2lcd 退出(rc=$?), 2 秒后重连" >> /tmp/run.log
    sleep 2
  done ) &
LOOP=$!
echo "rtsp2lcd 循环 pid=$LOOP 跑 ${DUR}s"
i=0
while [ $i -lt $DUR ]; do
    sleep 15
    i=$((i + 15))
    echo "===== t=${i}s ====="
    sh /root/probe.sh
    echo "  最近日志: $(grep -E '已送' /tmp/run.log | tail -1)"
    # 「停顿」是信息性的(网络抖一下, 继续等), 不算异常; 只有真断才算
    echo "  异常行  : $(grep -cE '流结束|断开' /tmp/run.log) 条"
    echo "  停顿行  : $(grep -cE '停顿' /tmp/run.log) 条 (信息性, 非异常)"
done
kill $LOOP 2>/dev/null
killall rtsp2lcd 2>/dev/null
sleep 1
echo "===== 完整 stdout 尾部 ====="
tail -25 /tmp/run.log
echo "===== dbg 日志尾部 ====="
tail -8 /tmp/rtsp2lcd.log
