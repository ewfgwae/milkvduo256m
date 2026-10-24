# PLAN.md — 07 打包发布：增量更新 / 整卡重制 / 校验流程

事实与纪律见同目录 `AGENTS.md`。

---

## 一、增量更新流程（已有卡升到当前版本，不重刷整卡）

1. 确认 `sdcard/` 里的文件就是想部署的版本（固件/工具/脚本的来源与 md5）。
2. 卡挂到 WSL（假定 `/mnt/card`，FAT 挂 `/mnt/card/boot`）：

```bash
sudo cp -a sdcard/rootfs/. /mnt/card/            # 根分区内容
sudo cp -a sdcard/boot/.   /mnt/card/boot/       # boot 分区内容(如未变可跳过)
sync
```

3. 或者板子在手，直接 ssh 覆盖（固件走热更立即生效，见 `../01/PLAN.md` 一.5-7）。
4. 验证：`md5sum` 抽查固件与工具；重启一次板子，开机行为正确（新版 UI/黑屏指纹 + 探针 build id）。

## 二、整卡镜像重制流程（大版本发布才做；上一次是 0c14f34）

前置：板上已把新版固件/工具/指南全部就位并验证；`/root` 下无临时垃圾。

1. **清板**：删掉 `/root` 下的实验残留（历史上清过"文件名内嵌控制字符的监控残留 + 旧固件实验文件"）。
2. **修 fs**：`e2fsck -fy <卡p2>`，确保 ext4 干净。
3. **取卡**：读卡器挂 WSL，找到设备（`lsblk`，**千万核对盘符**），取前 897MiB：

```bash
sudo dd if=/dev/sdX of=duo256m-sdcard.img bs=4M count=2243 conv=fsync status=progress
```

4. **分卷压缩**（避开 GitHub 单文件 100MB 限制）：

```bash
gzip -k -9 duo256m-sdcard.img
split -b 70MiB duo256m-sdcard.img.gz duo256m-sdcard.img.gz.part-
```

5. **校验清单**：更新 `image/CHECKSUMS.txt`（各分卷 + 合并后 .gz 的 md5/sha256）；
   README 第 2 节的 md5 值同步改。
6. **逐文件核对**：挂载镜像（loop）比对 `fip.bin/boot.sd/S99user/auto.sh/cvirtos.bin/lcd_sender`
   与板上实测 md5 一致。
7. 提交 git（分卷是二进制，确认 `.gitattributes` 处理正常）。

## 三、烧卡验证流程（发布前必做一次"新卡开箱"）

```bash
# WSL/Linux:
cat duo256m-sdcard.img.gz.part-* > duo256m-sdcard.img.gz
gunzip -k duo256m-sdcard.img.gz
md5sum duo256m-sdcard.img.gz        # 应 = CHECKSUMS.txt 里的值
sudo dd if=duo256m-sdcard.img of=/dev/sdX bs=4M conv=fsync status=progress && sync
# Windows: 合并成 .gz 后直接 balenaEtcher 选 .gz 写卡亦可
```

上电验收（按当前版本口径，plans/05 落地后以「UI 待机页」替代黑屏项）：
- [ ] 起来到 Linux、`/root/lcd_sender ping` 通；
- [ ] 上电屏指纹正确（当前：纯黑 + 右下 OSD；新版：按钮 UI 待机页）；
- [ ] 探针 `[14]` = 当前 build id（地址用当版基址）；
- [ ] `nohup sh /root/lcdcam.sh &` 出画面、`[17]` +30/秒；
- [ ] 重启一次，行为一致（auto.sh 热更的固件就是新版）。

## 四、fip.bin 更换小核固件载荷（BLCP_2ND）流程

背景：fip.bin 里打包着一份小核固件（BLCP_2ND，上电 → auto.sh 热更之前屏上跑的就是它）。
2026-10-25 前它一直是带跑分 demo 的老版（0x484A0009），冷启前 ~40s 屏上出"LVGL 测试"，
用户要求换掉 → 首次执行本流程（产物 md5 `c207b636…`，板上 /boot 备份
`fip.bak.0x484A0009`）。工具：SDK 自带 `fsbl/plat/cv181x/fiptool.py`（fip 格式：
PARAM1/BLCP/BL2 + PARAM2(各段 CRC/偏移) + DDR_PARAM/BLCP_2ND/MONITOR/LOADER_2ND；
BLCP_2ND 不压缩、512 对齐、CRC16-CCITT）。

**三道关卡，任何一关不过就停手**：

1. **往返保真**：`fiptool.py genfip --OLD_FIP 旧fip /tmp/rt.bin`，`cmp` 必须逐字节一致
   ——证明工具对现有 fip 的读/写完全确定（含 LZMA 的 u-boot 载荷原样保留、无隐藏段被丢）。
2. **载荷体检**：按 PARAM2 的 BLCP_2ND_LOADADDR/SIZE/RUNADDR 提取旧载荷，
   `crc_hqx` 与 TOC 的 CKSUM 对上、RUNADDR = 0x8FE00000、新旧固件补齐 512 后同尺寸
   （1640640 → 0x190A00）、跳板段（应用区后 2240B）与新版逐字节一致。
3. **差异审计**：`genfip --OLD_FIP 旧 --BLCP_2ND 新固件 --BLCP_2ND_RUNADDR 0x8FE00000`
   出新 fip；逐字节 diff，差异只允许三处——载荷区、BLCP_2ND_CKSUM（载荷 CRC）、
   PARAM2_CKSUM（PARAM2 表 CRC，因前者而变）；总尺寸不变（1928192）。

```bash
# 生成(WSL, SDK 在 ~/duo-sdk-v2):
python3 ~/duo-sdk-v2/fsbl/plat/cv181x/fiptool.py genfip \
    --OLD_FIP sdcard/boot/fip.bin \
    --BLCP_2ND ~/duo-sdk-v2/freertos/cvitek/install/bin/cvirtos.bin \
    --BLCP_2ND_RUNADDR 0x8FE00000 fip_new.bin
```

部署与验证：
1. scp 上板 → `cp /boot/fip.bin /boot/fip.bak.<旧build>` 备份 → `cp 新 /boot/fip.bin` →
   `sync` → 两边 md5 对上。
2. **纯 fip 验证**（判据最硬）：`mv /mnt/data/cvirtos.bin /root/` 后重启——auto.sh 无固件
   可热更，屏上跑的就是纯 fip 载荷；探针 `[14]` 应 = 新 build id、无 `/tmp/lcd_reload.log`。
   （2026-10-25 实测：`[14]=0x484A000F`、启动页 501fps 转圈。）
3. 恢复 `/mnt/data/cvirtos.bin` 再重启，auto.sh 热更 + ui 链路回归正常。
4. 最坏情况（fip 坏到起不来）：读卡器整卡重写（`image/` 或备份）。

## 五、待办清单

- [ ] plans/05/06 实施完后：README 第 2/3 节的"上电行为"与文件清单要重写一轮，
      整卡镜像重制一次（走本 PLAN 二）。
