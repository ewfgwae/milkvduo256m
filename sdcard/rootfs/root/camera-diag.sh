#!/bin/sh
# ============================================================================
#  camera-diag.sh  ——  Milk-V Duo256M CAM-GC2083 摄像头"不工作"分层自检
#  位置: 板上 /root/camera-diag.sh      (仓库: sdcard/rootfs/root/camera-diag.sh)
#  用法: 板上 root 下直接跑   /root/camera-diag.sh
#  作用: 一条命令把"到底是硬件没接好, 还是软件/配置不对"分开, 不用猜。
#  依据: 2026-09-30 实测(见 /root/指南/2026.9.30-摄像头-无法出图.txt)。
# ============================================================================

echo "==================================================================="
echo " Duo256M 摄像头分层自检 (GC2083)   $(date 2>/dev/null)"
echo "==================================================================="

FAIL=0

# ---------------------------------------------------------------- 0. 驱动模块
echo
echo "--- 0. 驱动模块有没有崩过 (第一道门, 必查) ---"
OOPS=$(dmesg 2>/dev/null | grep -cE 'Unable to handle kernel|Internal error: Oops')
CRASHED=$(dmesg 2>/dev/null | grep -cE '\[snsr_i2c\]')
if [ "$OOPS" -gt 0 ]; then
	echo "    !! 内核里有 $OOPS 条 Oops —— 驱动模块崩过"
	dmesg 2>/dev/null | grep -E 'Unable to handle kernel|Internal error: Oops|pc :|lr :' | head -6
	dmesg 2>/dev/null | grep -E 'init_module.*\[snsr_i2c\]|platform_device_register' | head -4
	echo "    !! 见 /root/指南/2026.9.30-摄像头-无法出图.txt 第六章: 旧 ko 与重编内核不匹配,"
	echo "       必须用同一棵内核树重编 /mnt/system/ko 下的模块, 否则摄像头必然读不到。"
	FAIL=1
else
	echo "    没有 Oops, 驱动模块正常加载"
fi
if ls /sys/bus/platform/drivers/snsr_i2c/ 2>/dev/null | grep -q 'snsr_i2c'; then
	echo "    snsr_i2c 平台设备已注册, 驱动已绑定"
else
	echo "    !! snsr_i2c 驱动没有绑定平台设备"
	FAIL=1
fi

# ---------------------------------------------------------------- 1. 软件面
echo
echo "--- 1. 传感器配置 /mnt/data/sensor_cfg.ini (应为 GC2083, bus_id=2) ---"
if [ -f /mnt/data/sensor_cfg.ini ]; then
	ls -l /mnt/data/sensor_cfg.ini
	grep -E 'name|bus_id|sns_i2c_addr|lane_id|mipi_dev' /mnt/data/sensor_cfg.ini
else
	echo "!! /mnt/data/sensor_cfg.ini 不存在"
	FAIL=1
fi

# ---------------------------------------------------------------- 2. I2C 定位面
echo
echo "--- 2. 摄像头 I2C 引脚复用 (FMUX, PINMUX_BASE 0x03001000) ---"
echo "    期望值: IIC2_SDA/SCL = 4, CAM_MCLK0 = 4, 传感器 RESET = 3"
p_txm1=$(devmem 0x030011AC 32)   # PAD_MIPI_TXM1 -> IIC2_SDA (GP10)
p_txp1=$(devmem 0x030011B0 32)   # PAD_MIPI_TXP1 -> IIC2_SCL (GP11)
p_txp0=$(devmem 0x030011B8 32)   # PAD_MIPI_TXP0 -> CAM_MCLK0
p_txp2=$(devmem 0x030011A8 32)   # PAD_MIPI_TXP2 -> XGPIOC_17 (传感器 RESET)
printf '    PAD_MIPI_TXM1 (IIC2_SDA) = %s\n' "$p_txm1"
printf '    PAD_MIPI_TXP1 (IIC2_SCL) = %s\n' "$p_txp1"
printf '    PAD_MIPI_TXP0 (CAM_MCLK0)= %s\n' "$p_txp0"
printf '    PAD_MIPI_TXP2 (RESET)    = %s\n' "$p_txp2"
if [ "$p_txm1" != "0x00000004" ] || [ "$p_txp1" != "0x00000004" ]; then
	echo "    !! I2C2 引脚复用不是 4 —— SoC 侧引脚没切到 I2C2"
	FAIL=1
fi

# ---------------------------------------------------------------- 3. 器件应答面
echo
echo "--- 3. 扫 I2C 总线找摄像头 (GC2083 地址 0x37) ---"
echo "    i2c-1 = 0x4010000 (GP4/GP5)   i2c-2 = 0x4020000 (摄像头, GP10/GP11)"
echo "    先扫摄像头那条(i2c-2), 每条限量 20 秒 —— i2c-1 上有个桩设备会让扫描很慢"
FOUND=""
for b in 2 1 3; do
	[ -e /dev/i2c-$b ] || continue
	echo "    --- i2c-$b ---"
	timeout 20 i2cdetect -y -r $b 2>&1 | sed -n '2,9p'
	if timeout 20 i2cdetect -y -r $b 2>/dev/null | grep -qE '(^| )37( |$)'; then
		FOUND=$b
	fi
done

# ---------------------------------------------------------------- 3.5 复位脚
echo
echo "--- 3.5 传感器复位脚 XGPIOC_17 (gpio-433) ---"
GS=$(cat /sys/kernel/debug/gpio 2>/dev/null | grep 'gpio-433')
if [ -z "$GS" ]; then
	echo "    !! gpio-433 没有任何驱动占用 —— cif 驱动没能申请到复位脚"
	echo "       现象: 复位脚没人驱动, 传感器被按在复位态, I2C 必然扫不到 0x37"
	echo "       原因: 板上 cvi_mipi_rx.ko 是旧预编译产物, 与重编内核 ABI 错位,"
	echo "             gpio 申请静默失败(实测返回 -EBUSY)。修法见指南第六章。"
	FAIL=1
else
	echo "    $GS"
	case "$GS" in
	*"out hi"*)
		echo "    -> 复位已释放, 传感器应能应答"
		;;
	*"in  lo"*|*"in hi"*)
		echo "    -> 复位脚已由 cif 驱动接管, 但此刻尚未释放(引脚是 input)。"
		echo "       这是**正常**的: 复位由用户态初始化时经 ioctl 释放,"
		echo "       必须跑 camera-test.sh 才会拉高。跑完再看这里应变成 out hi。"
		;;
	*"out lo"*)
		echo "    !! 复位脚被主动拉低 —— 传感器处于复位态, 不该常驻这样"
		FAIL=1
		;;
	esac
fi

echo
echo "==================================================================="
if [ -n "$FOUND" ]; then
	echo " 结论: 摄像头在 i2c-$FOUND 上应答了 (0x37)"
	echo "       -> 接线/供电/复位 正常, 可以跑 camera-test.sh"
else
	echo " 结论: 此刻没有扫到 0x37"
	echo "       -> 若第 3.5 步显示复位脚是 'in lo'(未释放), 属正常:"
	echo "          复位要等 camera-test.sh 初始化时才释放, 请直接跑它再判断。"
	echo "       -> 若第 3.5 步显示 gpio-433 没被占用, 那是驱动没修好(见上面提示)。"
	echo "       -> 若跑完 camera-test.sh 仍不出图, 再按第五章查硬件。"
fi
echo "==================================================================="

# ---------------------------------------------------------------- 4. 内核日志
echo
echo "--- 4. 内核日志里的摄像头相关消息 ---"
echo "    (已滤掉 i2c_designware 4010000.i2c 的刷屏: 那是 i2c-1, 不是摄像头)"
dmesg 2>/dev/null | grep -iE 'sensor|snsr|gc2083|mipi|cif|vi_open|vi_release' \
	| grep -v 'i2c_designware' | tail -20
echo "    ★ 注意: \"family ID request : receive error\" 与摄像头无关!"
echo "      它出自 cvi_thermal.c, 是向内核 netlink 问 \"thermal_event\" 这个 genl 家族,"
echo "      属于温控组件的正常报错(见指南 2026.9.30-摄像头-无法出图.txt 8.4), 不要拿它当摄像头故障证据。"

# ---------------------------------------------------------------- 5. 汇总
echo
echo "==================================================================="
if [ "$FAIL" -ne 0 ]; then
	echo " 总判定: 不通过 —— 驱动模块/配置层就有问题 (退出码 1)"
	echo "         先按第 0 步的提示处理(见 /root/指南/2026.9.30-摄像头-无法出图.txt 第六章):"
	echo "         旧 ko 与重编内核不匹配时必须重编 /mnt/system/ko 下的模块。"
	exit 1
elif [ -n "$FOUND" ]; then
	echo " 总判定: 通过 —— 摄像头已应答, 可以跑 camera-test.sh"
	exit 0
else
	echo " 总判定: 驱动层(含复位脚)已就绪, 但脚本启动时还没扫到 0x37"
	echo "         ★ 这不一定是故障: 复位脚要等 camera-test.sh 初始化时才释放,"
	echo "           脚本启动的瞬间本来就扫不到。请接着做下面两步再下结论:"
	echo "           ① 跑 camera-test.sh, 看到 'GC2083 1080P 30fps 10bit LINE Init OK!'"
	echo "              和 'rtsp://...' 就是好的;"
	echo "           ② 跑的过程中再看一次 gpio-433, 应变成 'out hi'。"
	echo "         只有 camera-test.sh 也失败, 才按第五章查硬件。"
	exit 1
fi
