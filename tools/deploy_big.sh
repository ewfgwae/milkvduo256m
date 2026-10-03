#!/bin/bash
# 部署刚编好的 rtsp2lcd 到板子
export SSH_ASKPASS=/tmp/wb/ap.sh SSH_ASKPASS_REQUIRE=force DISPLAY=:0
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
SCP="scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
F=${1:-/root/rtsp2lcd}
cat //wsl.localhost/Ubuntu-22.04/home/chen/wbbuild/rtsp2lcd > /tmp/wb/rtsp2lcd
echo "本地: $(md5sum /tmp/wb/rtsp2lcd | cut -d' ' -f1)  $(stat -c%s /tmp/wb/rtsp2lcd) 字节"
$SCP /tmp/wb/rtsp2lcd root@192.168.42.1:$F.new </dev/null 2>&1 | grep -v -E 'WARNING|post-quantum|store now|upgraded|^\*\*'
$SSH root@192.168.42.1 "killall rtsp2lcd 2>/dev/null; chmod +x $F.new; mv -f $F.new $F; md5sum $F" </dev/null 2>&1 | grep -E '^[0-9a-f]{32}'
