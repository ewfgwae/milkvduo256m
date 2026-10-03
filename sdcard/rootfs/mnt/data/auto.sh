#!/bin/sh
# 开机自启。由 /etc/init.d/S99user 调起(见该脚本里的 $USERDATAPATH/auto.sh 分支)。
#
# 做两件事:
#   1) 把同目录下的 cvirtos.bin 热更到小核(C906L)。免烧 fip.bin、免重启系统:
#      大核把新固件写进小核内存区(0x8FE00000), 小核自己跳到常驻跳板再重跑。
#      所以"改小核代码 -> 只换 /mnt/data/cvirtos.bin" 就能生效。
#   2) ★ 不再默认起 lcd_sender mon。原因见下面「mon 为什么被摘掉」。
#
# 日志: /tmp/lcd_reload.log (热更)。/tmp 重启会清空。
# 注意: 本板 busybox 没有 pkill, 用 killall。
#
# ============================================================================
# ★ mon 为什么被摘掉(2026-10-03 实测, 别再改回去):
#
#   lcd_sender mon 和 rtsp2lcd(摄像头推屏) **不能同时跑**。两个都用同一条
#   IPCM mailbox / cmdqu 通道, 一起跑会把 8 个槽位打满, 之后所有发送永久报
#       ioctl RTOS_CMDQU_SEND: No buffer space available
#   而且 —— 打满之后**不会自愈**: 把 mon 杀掉、或者只清那 8 个槽位的内存,
#   都救不回来(内核驱动内部的 ring 读写指针和槽位内存是两回事)。
#   实测的唯二解药: 重新热更一次小核固件(下面这段 rtos 分支), 跳板一重跑
#   消费端就复位了。
#
#   现象长这样: mon 自己打印的数值也开始乱(小核占用率 256% 4187% 这种、
#   0.0 FPS), 同时屏上的画面**停在最后一帧** —— 大核提交帧号
#   g_lvgl_probe[17] 不再涨, 但小核还在 33fps 刷同一张图, 所以
#   [11]像素/s 照样是 2.61M, 光看这个会误以为画面还在动。
#
#   要看画面是否真的在动, 认准 [17]大核提交 在涨(约 +30/s), 而不是 [16]/[11]。
#
#   所以: 开机只热更, 不起 mon。要看 CPU/帧率 OSD 就手动跑
#   `/mnt/data/lcd_sender mon 1000`, 而且**必须先停掉 rtsp2lcd**。
#   正常用法是直接跑摄像头推屏:
#       nohup sh /root/lcdcam.sh &      # 停: sh /root/lcdcam.sh stop
# ============================================================================
if [ -x /mnt/data/lcd_sender ]; then
	killall lcd_sender 2>/dev/null
	sleep 1

	# 清 IPCM mailbox 的 8 个槽位。上一条消息若是被 kill -9 强杀留下的
	# "孤儿回执", 槽位会永远停在占用态; 8 个槽位耗尽后任何发送都报 ENOBUFS。
	i=0
	while [ $i -lt 8 ]; do
		devmem $((0x1900400 + i * 8)) 64 0
		i=$((i + 1))
	done

	# 热更小核固件。失败也不影响后面 —— 屏幕上跑的还是 fip.bin 里那版固件。
	if [ -f /mnt/data/cvirtos.bin ]; then
		/mnt/data/lcd_sender rtos /mnt/data/cvirtos.bin >/tmp/lcd_reload.log 2>&1
	fi

	# ---- 想让摄像头画面开机自动上屏? 取消下面两行的注释 ----
	# 注意: 起得比 sensor 驱动/Linux 用户态都晚一点更稳, 所以这里 sleep 8。
	# sleep 8
	# nohup sh /root/lcdcam.sh >/tmp/lcdcam.out 2>&1 &

	# ---- 想看 CPU/帧率 OSD? 换下面这行; 但记住它和摄像头推屏互斥 ----
	# /mnt/data/lcd_sender mon 1000 >/tmp/lcd_mon.log 2>&1 &
fi
