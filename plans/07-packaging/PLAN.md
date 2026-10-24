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

## 四、待办清单

- [ ] plans/05/06 实施完后：README 第 2/3 节的"上电行为"与文件清单要重写一轮，
      整卡镜像重制一次（走本 PLAN 二）。
