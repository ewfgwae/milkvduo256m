#!/bin/sh
# ============================================================================
# lcdcam.sh —— 在 Milk-V Duo256M 上把 GC2083 摄像头画面持续送到 ST7789 屏。
#
# 用法(板上):
#   sh /root/lcdcam.sh              # 前台跑, Ctrl-C 停
#   nohup sh /root/lcdcam.sh &      # 后台常驻
#   killall lcdcam.sh rtsp2lcd      # 停
#
# 做什么:
#   1) 确认取流服务在跑(sample_vi_fd 提供 rtsp://127.0.1.1/h264)
#   2) 循环跑 rtsp2lcd; 万一它退了(服务端重启/网络断), 2 秒后自动重连
#
# 依赖: 小核固件已带 LCD_CMD_CAM 画面模式(build id 0x484A000A);
#       大核 /root/rtsp2lcd 已就位。
# ============================================================================

URL=${URL:-rtsp://127.0.1.1/h264}
BIN=${BIN:-/root/rtsp2lcd}
LOG=${LOG:-/tmp/lcdcam.log}
export LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd

# 取流服务(摄像头 -> H.264 RTSP)。没有就拉一个起来。
if ! pidof sample_vi_fd >/dev/null 2>&1; then
	echo "[lcdcam] sample_vi_fd 没在跑, 拉起取流服务"
	/mnt/system/usr/bin/ai/sample_vi_fd /mnt/cvimodel/scrfd_768_432_int8_1x.cvimodel \
		>/tmp/sample_vi_fd.log 2>&1 &
	# 等它把 RTSP 端口(554) 起来
	i=0
	while [ $i -lt 30 ]; do
		netstat -ltn 2>/dev/null | grep -q ':554' && break
		i=$((i + 1)); sleep 1
	done
	echo "[lcdcam] 等待 ${i}s, RTSP 端口就绪"
fi

echo "[lcdcam] 开始推送 $URL -> ST7789 320x240"
n=0
while :; do
	n=$((n + 1))
	echo "[lcdcam] 第 $n 次启动 rtsp2lcd  $(date '+%H:%M:%S')" | tee -a "$LOG"
	"$BIN" "$URL" >>"$LOG" 2>&1
	rc=$?
	echo "[lcdcam] rtsp2lcd 退出 rc=$rc, 2 秒后重连" | tee -a "$LOG"
	sleep 2
done
