# duo256m-lcd-rtos

Milk-V **Duo256M**（SG2002）双核 LCD 监控小设备：**小核 C906L（FreeRTOS）** 直驱
**ST7789 240×320 SPI 屏**跑 LVGL，**大核 A53（Linux）** 把 CPU 占用率实时投到屏上做 OSD。

- 小核：LVGL `lv_demo_benchmark` 跑分画面 + 右下角 OSD，半屏双缓冲 + **sysDMA 送显**，
  跑分实测 **约 82 FPS**（240×320 / SPI ~46.8 MHz）。跑分跑完画面静止后，LVGL 只在脏区
  变化时刷屏（每秒刷一次 OSD），小核 CPU 占用接近 0
- 大核：`lcd_sender` 命令行工具（刷色/彩条/动画/背光/**热更小核固件**）
- 大小核通讯：**IPCM mailbox（cmdqu）传命令** + **共享内存传像素**，不经过 Linux 的 spidev
- 小核固件支持**热更**：改完只换一个 `cvirtos.bin`，不烧 `fip.bin`、不重启

---

## 1. 硬件

| 项目 | 说明 |
|---|---|
| 板卡 | Milk-V Duo256M（SG2002，256MB，SD 卡启动） |
| 屏幕 | ST7789 240×320 SPI（无触摸） |
| 电平 | **只能 3.3V**，切勿接 5V |

### 接线（Duo256M 40PIN）

大核用户态例子用 spidev，小核 RTOS 直接操作 SPI2 寄存器，**两者用的是同一组物理引脚**：

| 屏幕 | 功能 | Duo256M 引脚（GP） | 物理脚 | SG2002 焊盘 |
|---|---|---|---|---|
| SCL | SPI2 时钟 | GP6 | 9 | SD1_CLK |
| SDA | SPI2 数据出（MOSI） | GP7 | 10 | SD1_CMD |
| CS | 片选 | GP9 | 12 | SD1_D3 |
| DC | 数据/命令 | GP20 | 26 | XGPIOA[27] |
| RES | 复位 | GP21 | 27 | XGPIOA[26] |
| BLK | 背光 | GP16 | 21 | XGPIOA[23] |
| VCC | 电源 | 3V3(OUT) | 36 | — |
| GND | 地 | GND | 任意 | — |

说明：

- GP6~GP9 默认就是 SPI2 功能（由 FSBL 的 `cvi_board_init.c` 做 pinmux），不用额外配置。
- `SPI2_SDI`（GP8，脚 11）屏不回读，可不接。
- 小核侧 **CS 靠 SPI2 的 SER 位自动片选**，所以固件里不占一根 GPIO；接上更稳。
- 历史上本工程用过**软件模拟 SPI**（SCL=GP18/脚24、SDA=GP19/脚25、CS=GP17/脚22），
  现版本已改为硬件 SPI2（SCL/SDA 移到脚 9/10），DC/RES/BLK 没变。

---

## 2. 最快复现：把整卡镜像写进 SD 卡

`image/` 下是设备上那张 SD 卡的**逐字节镜像**（前 897 MiB：MBR + FAT 128M + ext4 768M），
gzip 后分卷存放（避开 GitHub 单文件 100MB 限制）：

```
image/duo256m-sdcard.img.gz.part-00   70 MiB
image/duo256m-sdcard.img.gz.part-01   70 MiB
image/duo256m-sdcard.img.gz.part-02   58 MiB
image/CHECKSUMS.txt                   校验值（已验证与板子逐文件一致）
```

镜像里的 `fip.bin` / `boot.sd` / `auto.sh` / `cvirtos.bin` / `lcd_sender` 已与板子实测
md5 逐一比对一致，ext4 分区 `e2fsck -fn` 无错。

### 2.1 合并 + 解压（WSL / Linux）

```bash
cat duo256m-sdcard.img.gz.part-* > duo256m-sdcard.img.gz
gunzip -k duo256m-sdcard.img.gz          # 得到 duo256m-sdcard.img（897 MiB）
md5sum duo256m-sdcard.img.gz             # 应 = 03f8e97a82f7096db895baf02cd0ab62
```

### 2.2 写卡

**Linux / WSL（推荐，先确认盘符千万别写错）**：

```bash
lsblk                                     # 找到读卡器对应的设备，比如 /dev/sdX
sudo dd if=duo256m-sdcard.img of=/dev/sdX bs=4M conv=fsync status=progress
sync
```

**Windows**：把分卷合并成 `.img.gz` 后，直接用 **balenaEtcher** 选这个 `.gz` 写卡即可
（Etcher 支持 gzip，不用手动解压）。合并命令（PowerShell）：

```powershell
cd image
cmd /c copy /b duo256m-sdcard.img.gz.part-00+duo256m-sdcard.img.gz.part-01+duo256m-sdcard.img.gz.part-02 duo256m-sdcard.img.gz
```

插卡上电，屏幕应立即显示 LVGL 界面 + CPU 占用率 OSD。

> 卡的容量：镜像只有 897 MiB，**任何 ≥1GB 的卡都能写**（写完整卡会多出未分配空间，
> 不影响启动；想用完剩余空间可用 `cfdisk`/`parted` 扩 p2 后再 `resize2fs`）。

---

## 3. 已有卡增量更新：只用 `sdcard/` 覆盖

不想整卡重写、只想升级到本版本时，把 `sdcard/` 里的文件覆盖到卡的对应分区：

| 仓库里的路径 | 拷到卡的哪里 | 作用 |
|---|---|---|
| `sdcard/boot/fip.bin` | FAT 分区（p1，`/boot`） | FSBL + u-boot + 小核固件（BLCP_2ND 段）+ 常驻热更跳板 |
| `sdcard/boot/boot.sd` | FAT 分区（p1，`/boot`） | Linux 内核 + 设备树（含 SPI2 的 DMA 请求线映射） |
| `sdcard/rootfs/mnt/data/auto.sh` | ext4 分区（p2）`/mnt/data/` | 开机自启：热更小核固件 + 起 CPU 监控 |
| `sdcard/rootfs/mnt/data/cvirtos.bin` | ext4 分区（p2）`/mnt/data/` | 小核固件（build id `0x484A0008`），开机时热更进小核 |
| `sdcard/rootfs/mnt/data/lcd_sender` | ext4 分区（p2）`/mnt/data/` | 大核 aarch64 静态程序（含 `rtos` 热更子命令） |
| `sdcard/rootfs/root/*` | ext4 分区（p2）`/root/` | 同一批文件的副本 + 大核用户态例子，供手动调试 |
| `sdcard/rootfs/etc/init.d/S99user` | ext4 分区（p2）`/etc/init.d/` | 原厂启动脚本（**未改**），它负责调用 `/mnt/data/auto.sh` |

`S99user` 里关键的一段（原厂就有，不需要你改）：

```sh
if [ -f $USERDATAPATH/auto.sh ]; then
        usleep 30000
        . $USERDATAPATH/auto.sh &
        exit 1
fi
```

> ⚠️ **`fip.bin` 是唯一有变砖风险的文件**（本设备没有 USB 转串口/烧录按钮等恢复手段）。
> 判断是否要动它很简单：**只改小核 C 代码 → 完全不用碰 fip**，走第 5 节的"热更"即可，
> 因为 fip 里的常驻跳板支持原地覆盖小核应用区。只有改了 `hotjump.S` 本身才需要重烧 fip。

在 WSL 里往卡上拷（假定卡挂在 `/mnt/card`，FAT 分区挂 `/mnt/card/boot`）：

```bash
sudo cp -a sdcard/rootfs/. /mnt/card/            # 根分区内容
sudo cp -a sdcard/boot/.   /mnt/card/boot/       # boot 分区内容
sync
```

---

## 4. 目录结构

```
duo256m-lcd-rtos/
├── README.md                  本文
├── image/                     整卡镜像（gzip 分卷）
├── sdcard/                    可直接拷进 SD 卡的覆盖文件（见第 3 节）
│   ├── boot/                  -> FAT 分区
│   └── rootfs/                -> ext4 分区
├── src/
│   ├── sdk_overlay/           小核/内核改动，按 duo-sdk-v2 的相对路径存放
│   │   ├── freertos/cvitek/…
│   │   ├── build/boards/…     设备树、defconfig
│   │   └── linux_5.10/…       spidev 缓冲上限
│   ├── big_core/              大核程序源码 lcd_sender.c + lcd_shm.h
│   └── examples/              大核用户态例子 st7789/ 与 lvgl_port/（wiringX + spidev）
├── tools/                     一键脚本（见第 5 节）
└── docs/
    └── readme_board.txt       板上 /root/指南/readme.txt 的副本：完整开发指令与排障记录
```

---

## 5. 从源码编译

> 编译环境是 **WSL2 Ubuntu 22.04**（真机/虚拟机同理），SDK 目录下文记作 `$SDK`。

### 5.1 准备

```bash
# 1) SDK 与本版本一致的基线提交：ad920f839 (cvi_mpi: support st7701sn 2 lane lcd)
cd ~ && git clone <duo-buildroot-sdk 仓库> duo-sdk-v2
cd duo-sdk-v2 && git checkout ad920f839

# 2) 把本仓库的改动覆盖进 SDK（包含小核新增文件、设备树、defconfig、spidev 上限）
tools/apply_sdk_overlay.sh ~/duo-sdk-v2
```

### 5.2 编小核固件

```bash
tools/build_small_core.sh ~/duo-sdk-v2
# 产物：$SDK/freertos/cvitek/install/bin/cvirtos.bin
```

脚本内部走的是：

```bash
cd ~/duo-sdk-v2
source build/envsetup_milkv.sh milkv-duo256m-glibc-arm64-sd
cd build && make rtos
```

> **坑**：`make rtos` 有时会复用已存在的 `.obj`，改完源码重编却发现行为没变。
> 增量重编某一层的可靠做法（以 `lvgl_task.c` 为例）：
>
> ```bash
> cd $SDK/freertos/cvitek/build/task
> rm -f lvgl/CMakeFiles/lvgl.dir/lvgl_task.c.obj
> cmake --build . --target install && cmake --build . --target cvirtos.bin
> ```
>
> 另外：如果之前用 `root` 编过，build 树里会有 root 属主文件，普通用户编会报
> `Error writing to build log: Permission denied`，直接用 `sudo`/root 跑即可。

### 5.3 编大核程序

```bash
tools/build_big_core.sh ~/duo-sdk-v2
# 产物：~/lcdhost/lcd_sender（静态链接，避免板上缺库）
```

### 5.4 部署到板子

```bash
tools/deploy.sh <板子IP>          # 默认 192.168.42.1，密码 milkv
```

做的事：`scp` 新的 `cvirtos.bin` 和 `lcd_sender` 到板上 → 清 IPCM 槽位 →
`lcd_sender rtos` 热更小核 → 读探针验证。

**日常改小核代码的标准流程**（不碰 fip、不重启）：

```bash
scp $SDK/freertos/cvitek/install/bin/cvirtos.bin root@192.168.42.1:/root/cvirtos.bin
ssh root@192.168.42.1 'i=0; while [ $i -lt 8 ]; do devmem $((0x1900400+i*8)) 64 0; i=$((i+1)); done
                       /root/lcd_sender rtos /root/cvirtos.bin'
```

热更成功会打印：

```
固件 /root/cvirtos.bin: 1640640 字节 = 应用区 1638400 + 常驻跳板 2240 (跳板不重写)
/dev/mem 写入 + 读回校验通过
热更完成: 新固件已运行 (state=1, ~200 ms)        <- 退出码 0
```

### 5.5 编大核用户态例子（可选）

`src/examples/st7789/`、`src/examples/lvgl_port/` 是**大核 Linux 侧**的参考实现
（wiringX 软件 SPI / spidev，用来和大核、小核的刷屏性能做对比），产物就是板上
`/root/st7789_*_duo256m` 与 `/root/lvgl_demo`。它们需要 duo-examples 的编译环境：

```bash
git clone https://github.com/milkv-duo/duo-examples.git
cd duo-examples && source envsetup.sh          # 选 2 = Duo256M，选 riscv/arm
# 然后把 src/examples/st7789 拷到 duo-examples 下，按原工程的方式 make 即可
```

---

## 6. 运行时

### 6.1 开机流程

```
/etc/init.d/S99user           (原厂)
  └─ /mnt/system/ko/loadsystemko.sh
  └─ /mnt/data/auto.sh        (本工程)
       ├─ killall lcd_sender ; 清 8 个 IPCM 槽位
       ├─ lcd_sender rtos /mnt/data/cvirtos.bin    # 热更小核固件
       └─ lcd_sender mon 1000 &                    # 每秒采样大核 CPU 占用率发给小核
```

日志：`/tmp/lcd_reload.log`（热更）、`/tmp/lcd_mon.log`（监控）。`/tmp` 是 tmpfs，重启清空。

整套功能 = 3 个文件：`/boot/fip.bin`、`/boot/boot.sd`、`/mnt/data/{auto.sh,cvirtos.bin,lcd_sender}`。

想让屏上的 LVGL 跑分重新跑一遍：随便热更一次小核固件即可（小核重启就重建界面）：

```bash
ssh root@192.168.42.1 '/root/lcd_sender rtos /root/cvirtos.bin'
```

### 6.2 `lcd_sender` 子命令

| 命令 | 作用 |
|---|---|
| `lcd_sender stat` | 打印共享内存 ctrl/stat |
| `lcd_sender init` | 上电初始化屏幕（设背光、发 `LCD_CMD_INIT`） |
| `lcd_sender ping` | 发 `LCD_CMD_PING` 测大小核通讯通路 |
| `lcd_sender bl 1` | 背光开（`0` = 关） |
| `lcd_sender fill 0xF800` | 整屏纯色（RGB565） |
| `lcd_sender bars` | 画 8 条彩条 |
| `lcd_sender anim 300` | 移动白条，局部刷新，跑 300 帧 |
| `lcd_sender mon 1000` | 每秒采样 `/proc/stat`，把大核 CPU 占用率发给小核 OSD，并接回小核占用率与帧率 |
| `lcd_sender rtos <固件>` | **热更小核固件**（见 5.4） |

### 6.3 读小核探针（确认屏上跑的是哪一版、有没有掉帧）

探针地址**随固件体积变化**，换固件后必须重新查：

```bash
~/duo-sdk-v2/host-tools/gcc/riscv64-elf-x86_64/bin/riscv64-unknown-elf-nm \
    ~/duo-sdk-v2/freertos/cvitek/install/bin/cvirtos.elf | grep -E 'g_lvgl_probe|g_dma2_probe|g_spi2_probe'
```

对应 **build id `0x484A0008`** 的固件：

| 地址 | 含义 |
|---|---|
| `0x8fe6ce90` | `g_lvgl_probe[14]` 固件 build id，应为 `0x484A0008` |
| `0x8fe6ce30` | `[2]` 主循环圈数（隔几秒读两次，在涨 = 小核在跑） |
| `0x8fe6ce58` | `[7]` 帧率 ×10（例 `0x33A` = 82.6 FPS） |
| `0x8fe6ce60` | `[8]` 累计帧数 |
| `0x8fe6ce40` | `[4]` 小核 CPU 占用率 % |
| `0x8fec3958` | `g_dma2_probe[3]` 送显 DMA 真超时次数，**应为 0** |
| `0x8fec3a08` | `g_dma2_probe[25]` 单次等待最长耗时 us |
| `0x8fec3a48` | `g_spi2_probe[5]` DMA 失败回退 PIO 次数，**应为 0** |

```bash
devmem 0x8fe6ce90 32      # build id
devmem 0x8fe6ce58 32      # fps x10
devmem 0x8fec3958 32      # tmoN
```

### 6.4 已知故障与坑

- **`No buffer space available` / ping 发不出去**：IPCM mailbox 只有 8 个槽位
  （`0x1900400` 起，每槽 8 字节）。后台 `lcd_sender` 被 `kill -9` 强杀会留下"孤儿回执"，
  槽位永不释放，8 个占满后任何发送都报 `ENOBUFS`。清空即可：

  ```bash
  i=0; while [ $i -lt 8 ]; do devmem $((0x1900400+i*8)) 64 0; i=$((i+1)); done
  ```

  停 `lcd_sender` 优先用 `killall lcd_sender`（SIGTERM，正常收尾）。

- **帧率掉到 1~2 FPS（已修，见 build `0x484A0008`）**：sysDMA 搬完后
  `CH_INTSTATUS` 的中断位**约 35% 概率读不到**，硬件其实已收摊；只认中断位会把成功搬运
  判成超时、白等满 `guard`。修法是判据两条并用 —— 中断位 **或**（`DMAC_CH_EN` 通道位 = 0
  且 `CH_LLP` = 0），另加连续超时熔断回退 PIO。修改点在 `driver/spi/src/spi.c`。

- **`make rtos` 复用陈旧对象**导致改了没生效，见 5.2。

- **SPI2 的 DMA 请求线**必须是 `tx = 5`（板级 DTS `dmas = <&dmac 4 …> <&dmac 5 …>`，
  4 = `CVI_SPI2_RX`、5 = `CVI_SPI2_TX`），填错会让 DMA 通道永远卡在"已使能、等握手"。
  自查：`cat /proc/device-tree/sysdma_remap/ch-remap`，槽位 4 应为 `0x14`、槽位 5 应为 `0x15`。

- FreeRTOS tick 已从 200Hz 提到 **1000Hz**（`FreeRTOSConfig.h`），所以按 tick 算时间的代码
  单位是 **1ms/tick**。

---

## 7. 本版本的关键改动（相对原厂 SDK）

`src/sdk_overlay/` 里就是全部，逐条：

| 文件 | 改动 |
|---|---|
| `freertos/cvitek/driver/spi/src/spi.c` | **新增**：小核直驱 SPI2 + sysDMA 送显，含超时熔断与双判据完成检测 |
| `freertos/cvitek/driver/CMakeLists.txt` | 加入 `spi` 子目录 |
| `freertos/cvitek/task/comm/src/riscv64/lcd_st7789.c\|h` | **新增**：ST7789 驱动（含 `lcd_blit_start/wait` 异步送显） |
| `freertos/cvitek/task/comm/src/riscv64/lcd_shm.h` | **新增**：大小核共享内存协议 + `cmdqu` 命令号 |
| `freertos/cvitek/task/comm/src/riscv64/hotjump.S` | **新增**：常驻热更跳板（固定在 `0x8FF90000`） |
| `freertos/cvitek/task/comm/src/riscv64/sysdma_test.c` | **新增**：启动时 sysDMA 自检 |
| `freertos/cvitek/task/comm/src/riscv64/comm_main.c` | 挂上 `cmdqu` 命令分发（`LCD_CMD_*`） |
| `freertos/cvitek/task/lvgl/*` | **新增**：LVGL 移植 + 半屏双缓冲 + OSD 任务 |
| `freertos/cvitek/task/CMakeLists.txt` | 加入 `lvgl` 子目录 |
| `freertos/cvitek/task/main/CMakeLists.txt` | 入口调整 |
| `freertos/cvitek/scripts/cv181x_lscript.ld` | 新增 `.hotjump` 与 `.lvgl_fb`(NOLOAD) 段，应用区让出 `0x70000` |
| `freertos/cvitek/kernel/include/riscv64/FreeRTOSConfig.h` | tick 200→1000Hz；打开运行时间统计 |
| `freertos/cvitek/driver/gpio/src/gpio.c` | GPIO 输出改读-改-写（同一端口挂多根线时不互相踩） |
| `build/boards/cv181x/…/…_arm64_sd.dts` | 给 `&spi2` 加 DMA 请求线 + `sysdma_remap` 槽位 4/5 改给 SPI2 |
| `build/boards/cv181x/…/linux/…_defconfig` | 内核配置相应打开 |
| `linux_5.10/drivers/spi/spidev.c` | `bufsiz` 4096 → 262144（大核用户态刷屏才够用） |

内存布局（热更为什么安全）：

```
0x8FE00000 .. 0x8FF90000   小核应用区（1638400 B，热更时原地覆盖）
0x8FF90000 .. 0x8FFC0000   常驻热更跳板 .hotjump（永不覆盖）
0x8FF94000 .. 0x8FFC0000   LVGL 双缓冲 .lvgl_fb（NOLOAD，不进固件镜像）
0x8FFC0000 ..              LCD 共享内存（ctrl 64B + stat 64B + fb 153600B）
```

---

## 8. 参考

- 板上完整开发指令、内存布局、fip 重烧流程与排障记录：`docs/readme_board.txt`
  （板子上的副本在 `/root/指南/readme.txt`）
- Milk-V Duo256M 文档：<https://milkv.io/docs/duo/getting-started/duo256m>
- duo-examples：<https://github.com/milkv-duo/duo-examples>

## 许可

本仓库中来自 Milk-V / CVITEK 的代码（`src/sdk_overlay/`）遵循其原始许可；
应用层代码（`src/big_core/`、`src/examples/` 等）随本仓库一并提供。
