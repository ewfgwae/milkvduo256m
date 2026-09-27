#!/bin/sh
# 开机自启。由 /etc/init.d/S99user 调起(见该脚本里的 $USERDATAPATH/auto.sh 分支)。
#
# 做两件事:
#   1) 把同目录下的 cvirtos.bin 热更到小核(C906L)。免烧 fip.bin、免重启系统:
#      大核把新固件写进小核内存区(0x8FE00000), 小核自己跳到常驻跳板再重跑。
#      所以"改小核代码 -> 只换 /mnt/data/cvirtos.bin" 就能生效。
#   2) 起 lcd_sender mon: 周期采样大核 /proc/stat 的 CPU 占用率, 用 mailbox
#      (LCD_CMD_MON) 发给小核的 LVGL OSD 显示; 回执里带回小核占用率与帧率。
#
# 日志: /tmp/lcd_reload.log (热更), /tmp/lcd_mon.log (监控)。/tmp 重启会清空。
# 注意: 本板 busybox 没有 pkill, 用 killall。
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

	/mnt/data/lcd_sender mon 1000 >/tmp/lcd_mon.log 2>&1 &
fi
