#!/bin/sh
# ============================================================================
# lcdcam.sh —— 在 Milk-V Duo256M 上把 GC2083 摄像头画面持续送到 ST7789 屏。
#
# 用法(板上):
#   sh /root/lcdcam.sh              # 前台跑, Ctrl-C 停
#   nohup sh /root/lcdcam.sh &      # 后台常驻
#   sh /root/lcdcam.sh stop         # 停推送(循环 + rtsp2lcd)
#   sh /root/lcdcam.sh stopall      # 全停(连取流服务 sample_vi_fd 一起收)
#
# 做什么:
#   1) 确保取流服务在跑(sample_vi_fd 提供 rtsp://127.0.1.1/h264)
#   2) 循环跑 rtsp2lcd; 万一它退了(服务端重启/网络断), 2 秒后自动重连
#   3) ★ 每次重连前先把取流服务重新拉起(见下面「为什么必须重启取流服务」)
#
# ★ 为什么必须重启取流服务(本机实测, 别省这一步):
#   /mnt/system/usr/bin/ai/sample_vi_fd 是 demo 性质的服务端, **只能服务一个客户端,
#   而且客户端一断开它自己就整体退出**! 日志里依次出现:
#       RTSP client disconnected from: 127.0.1.1
#       CVI_VENC_GetStream failed with 0xc007804c!
#       Send Output Frame NG, ret=c007804c
#       Exit encoder thread / Exit TDL thread / destroy middleware
#       stop VPSS (0) / isp_3aLib_exit ...
#   也就是编码线程跟着客户端一起死, 整个中间件被销毁, 554 端口直接消失。
#   所以: rtsp2lcd 一退出, 就**不能**指望原来的 554 还在 —— 必须 killall 旧的、
#   重新拉一个新的, 等 554 重新监听, 再让 rtsp2lcd 连。
#   (只做「端口探活、没掉就复用」是不够的: 客户端断开后有 1~2 秒 554 仍在监听
#    但服务端正在拆, 这时连上去会秒断, 会看到 rtsp2lcd 飞快地反复重启。)
#   → 因此本脚本在每轮重连前无条件 restart_vi_fd(), 用 ~3 秒换确定性。
#
# ★ 和 lcd_sender mon 互斥(2026-10-03 实测):
#   `lcd_sender mon` 和 rtsp2lcd 走同一条 cmdqu 通道, 一起跑会把 8 个槽位打满,
#   之后所有发送永久报 "No buffer space available", 且**不自愈**(杀 mon、只清槽位
#   内存都没用, 内核 ring 指针不跟着回退)。屏上的表现是**画面停在最后一帧**——
#   大核提交帧号 [17] 不再涨, 但小核还在 33fps 刷同一张图, 所以 [11]像素/s 照样
#   2.61M, 光看它会误判"画面还在动"。唯一解药是热更一次小核固件复位消费端。
#   本脚本因此: 启动时先把 mon 收掉; 运行中派一个 6 秒轮询的看门狗盯日志,
#   一旦发现新增 ENOBUFS 就自动 recover_channel()(收掉 mon + 清槽 + 热更)。
#
# ★ 怎么停(踩过坑):
#   本脚本是 shell 循环。busybox 的 killall 是按 argv[0] 名字匹配的, 而这个进程的
#   名字是 `sh`(命令行是 `sh /root/lcdcam.sh`), 所以 `killall lcdcam.sh` **打不中它**!
#   更糟的是只 `killall rtsp2lcd` 也没用 —— 循环会立刻再起一个; 而如果只把脚本本身
#   干掉、留下它拉起的 rtsp2lcd 孤儿, 那个孤儿还占着 sample_vi_fd 的 RTSP 连接
#   (服务端只服务一个客户端), 之后你自己手动起的 rtsp2lcd 会一个字节都收不到,
#   日志刷满 "停顿 N s 无数据(g_rlen=0)"。所以停要按下面这个顺序:
#       sh /root/lcdcam.sh stop
#   等价于:
#       kill $(cat /tmp/lcdcam.pid)     # 先停循环, 否则会再起一个
#       killall rtsp2lcd                # 再停当前那一轮
#
# 依赖: 小核固件已带 LCD_CMD_CAM 画面模式(build id >= 0x484A000A);
#       大核 /root/rtsp2lcd 已就位。
# ============================================================================

URL=${URL:-rtsp://127.0.1.1/h264}
BIN=${BIN:-/root/rtsp2lcd}
LOG=${LOG:-/tmp/lcdcam.log}
PIDFILE=${PIDFILE:-/tmp/lcdcam.pid}
VIFD=${VIFD:-/mnt/system/usr/bin/ai/sample_vi_fd}
MODEL=${MODEL:-/mnt/cvimodel/scrfd_768_432_int8_1x.cvimodel}
VIFD_LOG=${VIFD_LOG:-/tmp/sample_vi_fd.log}
VIFD_WAIT=${VIFD_WAIT:-30}          # 等 554 的最长秒数
export LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd

# ---- ★ 收掉别的 lcdcam.sh 循环 ----
# 本板 busybox **没有 pkill**, 而 `killall lcdcam.sh` 也打不中(这个进程的 argv[0]
# 是 `sh`, 不是 `lcdcam.sh`)。两个循环同时在跑会很坑: 它们各自拉一个 rtsp2lcd,
# 而 sample_vi_fd 的服务端只服务一个客户端 —— 第二个连接一个字节都收不到,
# 日志刷满 "停顿 N s 无数据(g_rlen=0)"。
# 做法: 扫 /proc 的 cmdline, 只认 **argv[1] 就是本脚本路径** 的进程。
# ★ 别偷懒改成对整条 cmdline 做 grep 'lcdcam.sh' —— 那样会把"命令行里恰好出现
#   lcdcam.sh 字样"的进程一起杀掉, 比如从 ssh 进来执行 `sh /root/lcdcam.sh stop`
#   的那层 `sh -c '...'`; 结果是脚本把自己连同调用者一起杀掉, 命令中途断掉。
kill_other_loops() {
	_k=0
	while [ $_k -lt 5 ]; do
		_found=0
		for _p in /proc/[0-9]*; do
			_ppid=${_p#/proc/}
			[ "$_ppid" = "$$" ] && continue
			[ -n "$OLDPID" ] && [ "$_ppid" = "$OLDPID" ] && continue
			[ -r "$_p/cmdline" ] || continue
			_argv2=$(tr '\000' '\n' <"$_p/cmdline" 2>/dev/null | sed -n 2p)
			case "$_argv2" in
			*/lcdcam.sh)
				echo "[lcdcam] 收掉 lcdcam.sh 循环 pid=$_ppid" | tee -a "$LOG"
				kill "$_ppid" 2>/dev/null
				_found=1 ;;
			esac
		done
		[ $_found -eq 0 ] && break
		sleep 1
		_k=$((_k + 1))
	done
}

# ---- stop 子命令 ----
if [ "$1" = "stop" ] || [ "$1" = "stopall" ]; then
	if [ -f "$PIDFILE" ]; then
		OLDPID=$(cat "$PIDFILE")
		kill "$OLDPID" 2>/dev/null    # 先停循环, 否则它马上又起一个 rtsp2lcd
		rm -f "$PIDFILE"
		echo "[lcdcam] 已停循环 pid=$OLDPID"
	fi
	kill_other_loops                # pidfile 丢了的兜底
	killall rtsp2lcd 2>/dev/null
	if [ "$1" = "stopall" ]; then
		killall sample_vi_fd 2>/dev/null
	fi
	sleep 1
	echo "[lcdcam] 剩余进程:"
	ps -o pid,args 2>/dev/null | grep -E 'lcdcam|rtsp2lcd|sample_vi_fd' | grep -v grep
	echo "[lcdcam] (上面为空即已停干净)"
	exit 0
fi

echo $$ > "$PIDFILE"

kill_other_loops
sleep 1
# 旧循环可能已经留下它的 rtsp2lcd 孤儿(占着 RTSP 连接), 一并收掉
killall rtsp2lcd 2>/dev/null
sleep 1

# ---- ★ 先清掉 lcd_sender mon ----
# mon 和 rtsp2lcd 都走同一条 cmdqu 通道, 一起跑会把 8 个槽位打满, 之后所有发送
# 永久报 "No buffer space available", 而且不自愈(详见 auto.sh 里的长注释)。
# 所以这里先把 mon 收掉。注意 busybox 的 killall 按 argv[0] 匹配, mon 实例的
# argv[0] 就是 lcd_sender, 打得中; `lcd_sender rtos` 是短命进程, 不冲突。
if pidof lcd_sender >/dev/null 2>&1; then
	echo "[lcdcam] 检测到 lcd_sender mon 在跑, 先停掉(它会和推屏抢 cmdqu 打满槽位)" | tee -a "$LOG"
	killall lcd_sender 2>/dev/null
	sleep 1
	i=0
	while [ $i -lt 8 ]; do
		a=$((0x1900400 + i * 8)); devmem $(printf 0x%x $a) 64 0
		i=$((i + 1))
	done
fi

# ---- 取流服务管理 ----
# 554 在监听吗
port_ready() { netstat -ltn 2>/dev/null | grep -q ':554'; }

# ★ 复位 cmdqu 通道: 槽位被打满后唯一的解药是热更一次小核(跳板重跑=消费端复位)。
#   只清槽位内存没用 —— 内核驱动内部的 ring 读写指针还停在满的位置上。
recover_channel() {
	echo "[lcdcam] ★★ cmdqu 通道被打满, 热更小核固件复位消费端  $(date '+%H:%M:%S')" | tee -a "$LOG"
	killall rtsp2lcd 2>/dev/null
	# 顺手把 mon 也收了 —— 它才是打满槽位的元凶, 不收掉复完马上又被打满
	killall lcd_sender 2>/dev/null
	sleep 1
	i=0
	while [ $i -lt 8 ]; do
		a=$((0x1900400 + i * 8)); devmem $(printf 0x%x $a) 64 0
		i=$((i + 1))
	done
	if [ -x /mnt/data/lcd_sender ] && [ -f /mnt/data/cvirtos.bin ]; then
		/mnt/data/lcd_sender rtos /mnt/data/cvirtos.bin >>"$LOG" 2>&1
		echo "[lcdcam] 热更完成, 等 3 秒让小核站稳" | tee -a "$LOG"
	else
		echo "[lcdcam] 找不到 /mnt/data/lcd_sender 或 cvirtos.bin, 无法自动复位!" | tee -a "$LOG"
		echo "[lcdcam] 手动解药: 清 8 槽后 /mnt/data/lcd_sender rtos /mnt/data/cvirtos.bin" | tee -a "$LOG"
	fi
	sleep 3
}

# 重启取流服务: 杀旧的 -> 等端口释放 -> 起新的 -> 等 554 回来
restart_vi_fd() {
	echo "[lcdcam] 重启取流服务 sample_vi_fd  $(date '+%H:%M:%S')" | tee -a "$LOG"
	killall sample_vi_fd 2>/dev/null
	# 等旧实例彻底退出、554 释放
	i=0
	while [ $i -lt 40 ]; do
		port_ready || break
		i=$((i + 1)); sleep 0.5
	done
	killall -9 sample_vi_fd 2>/dev/null   # 顽固残留补一刀
	sleep 0.5
	"$VIFD" "$MODEL" >"$VIFD_LOG" 2>&1 &
	# 等 554 重新监听
	i=0
	while [ $i -lt $((VIFD_WAIT * 2)) ]; do
		port_ready && break
		i=$((i + 1)); sleep 0.5
	done
	if port_ready; then
		echo "[lcdcam] 取流服务就绪, 554 已监听 (等了 $((i / 2))s)" | tee -a "$LOG"
		sleep 1     # 让 RTSP 处理线程站稳, 免得起得快但连上秒断
		return 0
	else
		echo "[lcdcam] 警告: 等了 ${VIFD_WAIT}s 554 仍未监听, 看 $VIFD_LOG" | tee -a "$LOG"
		tail -5 "$VIFD_LOG" >>"$LOG" 2>/dev/null
		return 1
	fi
}

vi_fd_healthy() { port_ready && pidof sample_vi_fd >/dev/null 2>&1; }

# 首次起服务(如果还没在跑)
if vi_fd_healthy; then
	echo "[lcdcam] 取流服务已在跑, 复用"
else
	restart_vi_fd
fi

echo "[lcdcam] 开始推送 $URL -> ST7789 320x240"
n=0
while :; do
	n=$((n + 1))

	# 日志别无限涨: 超过 1MB 就只留最后 200 行
	if [ -f "$LOG" ] && [ "$(wc -c <"$LOG" 2>/dev/null || echo 0)" -gt 1048576 ]; then
		tail -200 "$LOG" >"$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG"
	fi

	echo "[lcdcam] 第 $n 次启动 rtsp2lcd  $(date '+%H:%M:%S')" | tee -a "$LOG"
	nb_last=$(grep -c "No buffer" "$LOG" 2>/dev/null); [ -z "$nb_last" ] && nb_last=0
	t0=$(date +%s)
	"$BIN" "$URL" >>"$LOG" 2>&1 &
	cli=$!

	# ★ 监督: rtsp2lcd 挨了 jam 也不会自己退(它只是丢帧, 屏上停在最后一帧),
	#   所以必须主动盯着 —— 只要本轮日志里新增了 "No buffer space available",
	#   就判定 cmdqu 被打满, 把客户端收掉去走复位。
	jam=0
	while kill -0 "$cli" 2>/dev/null; do
		sleep 6
		kill -0 "$cli" 2>/dev/null || break
		nb_now=$(grep -c "No buffer" "$LOG" 2>/dev/null); [ -z "$nb_now" ] && nb_now=0
		if [ "$nb_now" -gt "$nb_last" ]; then
			jam=1
			echo "[lcdcam] 发现 cmdqu ENOBUFS(本轮 +$((nb_now - nb_last)) 行), 通道被打满" | tee -a "$LOG"
			kill "$cli" 2>/dev/null
			break
		fi
	done
	wait "$cli" 2>/dev/null
	rc=$?
	t1=$(date +%s)
	dur=$((t1 - t0))
	echo "[lcdcam] rtsp2lcd 退出 rc=$rc, 跑了 ${dur}s" | tee -a "$LOG"

	if [ $jam -eq 1 ]; then
		recover_channel
		sleep 2
		continue
	fi

	if [ $dur -ge 5 ]; then
		# 曾经稳定跑过 -> 是它把服务端带走的(客户端断开=服务端自杀), 必须重拉
		sleep 2
		restart_vi_fd
	elif vi_fd_healthy; then
		# 秒退且服务端没被碰 -> 多半是连不上/地址问题, 别去动服务端
		echo "[lcdcam] 客户端秒退但取流服务健康, 只重试客户端" | tee -a "$LOG"
		sleep 3
	else
		# 秒退且服务端也确实没了 -> 重拉
		sleep 2
		restart_vi_fd
	fi
done
