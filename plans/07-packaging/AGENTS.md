# AGENTS.md — 07 打包发布（sdcard 同步 / 整卡镜像 / 校验）

通用规范见根 `../../AGENTS.md`。

## 结构事实

- `sdcard/rootfs/` **等价于板上 ext4 分区（p2）的根**：`sdcard/rootfs/root/X` = 板上 `/root/X`；
  `sdcard/boot/` = FAT 分区（p1，`/boot`）。
- `image/` 是整卡逐字节镜像的 gzip 分卷（前 897MiB = MBR + FAT128M + ext4 768M）：
  `duo256m-sdcard.img.gz.part-00/01/02`（70/70/56 MiB）+ `CHECKSUMS.txt`。
  解压后 `duo256m-sdcard.img.gz` 的 md5 = `191d92629ed0f166cc1035e69fff2e58`。
- 镜像只覆盖前 897MiB，**任何 ≥1GB 的卡都能写**；想用完剩余空间 `cfdisk` 扩 p2 + `resize2fs`。

## 同步纪律（三处副本，改一处必须想另外两处）

| 内容 | 板上 | 仓库 | 开机来源 |
| --- | --- | --- | --- |
| 小核固件 | `/mnt/data/cvirtos.bin`、`/root/cvirtos.bin` | `sdcard/rootfs/mnt/data/`、`sdcard/rootfs/root/` | auto.sh 每次开机拿 `/mnt/data/` 热更 |
| lcd_sender | `/mnt/data/`、`/root/` | 同上两处 | — |
| lcdcam.sh 等脚本 | `/root/` | `sdcard/rootfs/root/`、`tools/` | — |
| 指南 | `/root/指南/` | `sdcard/rootfs/root/指南/` | — |

- ★ **同名同步**：板上改了文件名，仓库同步改（指南 `readme.txt` 第 7 节的要求）。
- 传完必 `md5sum` 两边比对。
- ★ `fip.bin` / `boot.sd` 只在"必须动引导/内核"时才更新（本仓库当前版本与板上一致）；
  动 fip 前重读根 AGENTS 安全铁律 1。

## 指南的同步命令（板上 → 仓库方向照抄其 readme.txt 第 7 节）

```bash
setsid scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    readme.txt YYYY.M.D-类别-简述.txt root@192.168.42.1:/root/指南/
```

## 坑

- 整卡镜像里的 `cvirtos.bin` 若与 `sdcard/` 里最新版不一致，烧出的卡首次开机会先跑一次旧版
  （README 第 2 节的注）——重制镜像时两处必须同源。
- FAT 分区（p1）在 Windows 下可直接拷贝，ext4（p2）必须走 WSL/镜像，别用 Windows 工具直接写。
