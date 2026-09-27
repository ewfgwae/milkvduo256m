#!/bin/bash
# 部署到板子: 拷 lcd_sender + cvirtos.bin, 清 IPCM 槽位, 热更小核, 读探针验证。
# 用法: tools/deploy.sh [板子IP] [cvirtos.bin路径]
#       默认 IP=192.168.42.1, 固件=<仓库>/sdcard/rootfs/mnt/data/cvirtos.bin
#
# 环境变量:
#   PASS  板子密码, 默认 milkv
#   SKIP_RTOS=1   只拷 lcd_sender, 不热更小核
#
# 注意: 只动 /root 与 /dev/mem, 不碰 fip.bin, 不重启系统。
set -euo pipefail

HOST="${1:-192.168.42.1}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
FW="${2:-$HERE/sdcard/rootfs/mnt/data/cvirtos.bin}"
PASS="${PASS:-milkv}"
SKIP_RTOS="${SKIP_RTOS:-0}"

[ -f "$HERE/build/lcd_sender" ] || { echo "先跑 tools/build_big_core.sh 生成 build/lcd_sender" >&2; exit 1; }
[ "$SKIP_RTOS" = "1" ] || [ -f "$FW" ] || { echo "找不到固件: $FW" >&2; exit 1; }

# ---- 免交互 ssh/scp ----
ASKPASS="$(mktemp)"
printf '#!/bin/sh\necho %s\n' "$PASS" > "$ASKPASS"
chmod 755 "$ASKPASS"
export SSH_ASKPASS="$ASKPASS" SSH_ASKPASS_REQUIRE=force DISPLAY=:0
OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10"
cleanup() { rm -f "$ASKPASS"; }
trap cleanup EXIT

echo "=== 1) scp 到 /root ==="
setsid scp $OPTS "$HERE/build/lcd_sender" "root@$HOST:/root/lcd_sender" </dev/null
if [ "$SKIP_RTOS" != "1" ]; then
	setsid scp $OPTS "$FW" "root@$HOST:/root/cvirtos.bin" </dev/null
	# 开机自启也要用到, 一并更新 /mnt/data
	setsid ssh $OPTS "root@$HOST" "cp -f /root/lcd_sender /mnt/data/lcd_sender && chmod 755 /mnt/data/lcd_sender" </dev/null
fi

echo "=== 2) 板上: 清槽位 + 热更 + 读探针 ==="
setsid ssh $OPTS "root@$HOST" "sh -s" </dev/null <<EOF
set -e
echo "-- 停后台监控(用 SIGTERM, 别用 -9, 否则会留下孤儿回执占满邮箱)"
killall lcd_sender 2>/dev/null || true
sleep 1

echo "-- 清 8 个 IPCM 槽位"
i=0
while [ \$i -lt 8 ]; do
	devmem \$((0x1900400 + i*8)) 64 0
	i=\$((i+1))
done

if [ "$SKIP_RTOS" != "1" ]; then
	echo "-- 热更小核固件"
	/root/lcd_sender rtos /root/cvirtos.bin
	echo "-- 重启后台监控"
	nohup /root/lcd_sender mon 1000 >/tmp/lcd_mon.log 2>&1 &
	sleep 8
else
	echo "-- (SKIP_RTOS=1, 不热更)"
fi

echo "-- 探针 (build 0x484A0008; 换固件后用 nm 重查地址)"
echo -n "buildid = "; devmem 0x8fe6ce90 32
echo -n "fps_x10 = "; devmem 0x8fe6ce58 32
echo -n "tmoN    = "; devmem 0x8fec3958 32
echo -n "PIO回退 = "; devmem 0x8fec3a48 32
EOF

echo
echo "=== 完成 ==="
