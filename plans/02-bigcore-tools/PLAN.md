# PLAN.md — 02 大核工具：改动流程与测试流程

事实与规矩见同目录 `AGENTS.md`。本文件给「改/加工具 → 编译 → 部署 → 测试」的标准流程。

---

## 一、加 / 改 lcd_sender 子命令的流程

1. 改 `src/big_core/lcd_sender.c`：
   - 用法注释（文件头）同步补一行；
   - 子命令挂在 `main()` 的命令表里；走 cmdqu 的新命令号先在 `plans/05` / `lcd_shm.h` 里定好，
     并同步小核侧 `comm_main.c` 分发（协议两份副本必须一致）；
   - 需要起/停板上服务时用 `system("sh /root/xxx.sh ...")`，**不要**在 C 里 fork 一堆细节。
2. 编译（WSL）：

```bash
tools/build_big_core.sh ~/duo-sdk-v2        # 产物 ~/lcdhost/lcd_sender（静态）
```

3. 部署（★ 先 killall 再覆盖，否则 "Text file busy"；传完 md5 两边比对）：

```bash
ssh root@192.168.42.1 'killall lcd_sender 2>/dev/null'
scp ~/lcdhost/lcd_sender root@192.168.42.1:/root/lcd_sender.new
ssh root@192.168.42.1 'chmod +x /root/lcd_sender.new; mv -f /root/lcd_sender.new /root/lcd_sender'
md5sum ~/lcdhost/lcd_sender && ssh root@192.168.42.1 'md5sum /root/lcd_sender'
# 同步 /mnt/data 副本（开机链路用的那份）:
ssh root@192.168.42.1 'cp -f /root/lcd_sender /mnt/data/lcd_sender'
```

4. 仓库副本同步：`sdcard/rootfs/root/lcd_sender`、`sdcard/rootfs/mnt/data/lcd_sender`。

## 二、改 / 加 rtsp2lcd 的流程

1. 改 `tools/rtsp2lcd.c`（RTSP 状态机、解码、转换三段解耦，别搅在一起）。
2. 编译（WSL，脚本自动镜像到无空格目录）：

```bash
bash ~/wb_build_big.sh        # 产物 ~/wbbuild/rtsp2lcd
```

3. 部署同上（`killall rtsp2lcd` → `.new` → `mv` → md5），副本同步 `sdcard/rootfs/root/rtsp2lcd`。
4. 改了收流/转换逻辑，必须跑「四、性能与长跑」再收工。

## 三、冒烟测试流程（每次部署后）

1. **通路**：`lcd_sender ping` 出两行（ctrl.magic = 0x4C434431）。
2. **刷屏**：`lcd_sender fill 0xF800` 屏变红、`bars` 出 8 彩条（背光开着的前提）。
3. **互斥检查**：`pidof lcd_sender` 为空（mon 没在跑）才允许起 rtsp2lcd / lcdcam。
4. **rtsp2lcd 单帧**：`rtsp2lcd rtsp://127.0.1.1/h264 --frames 30` 正常退出、无 ENOBUFS。
5. **抓帧不碰屏**（验证转换颜色）：`--dump /tmp/f.ppm` 取回 PC 看，白色物体是白色 = 无偏色、
   通道没反、RGB565 大端正确。

## 四、性能与长跑测试流程

1. **转换基准**（改转换代码必跑）：

```bash
LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd \
  /root/rtsp2lcd rtsp://127.0.1.1/h264 --convbench 200 --convs 2
```

   判据：单帧 ≤ ~31ms（≥ 30fps，压过源帧率）；拷贝段 ~2.5ms（内存访问不是瓶颈的证据）。

2. **长跑**（改收流/自愈逻辑必跑）：

```bash
sh /root/longrun.sh 240
```

   判据：异常行（`流结束|断开`）= 0；帧率稳定在源帧率附近；`[17]` 每秒 ≈ +30。

3. **自愈注入**（改 lcdcam.sh 必跑）：
   - 杀 `sample_vi_fd` → ~9 秒内画面恢复（重拉服务 + 客户端重连），554 重新监听；
   - 注入 `lcd_sender mon 1000` 制造 jam → 看门狗 ~2s 发现 → 热更 ~200ms → ~15 秒内 `[17]` 恢复 +30/秒。

## 五、待办清单

- [ ] `lcd_sender cam on|off` + `lcdcam.sh start`（`plans/05` Phase 2，命令号 0x49/0x4A 已预定）。
- [ ] `plans/06` 的大核守护进程 `cam_btnd`（轮询共享内存 `stat.ui_event`，**不占 cmdqu**）。
