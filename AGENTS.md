# AGENTS.md — milkvduo256m 总纲与模块导航

Milk-V **Duo256M**（SG2002）双核 LCD 监控小设备：小核 **C906L（FreeRTOS + LVGL 8.3.11）** 独占
ST7789 240×320 SPI 屏（按 **320×240 横屏**用），大核 **A53（Linux 5.10）** 跑摄像头取流 / RTSP /
CVI_VDEC 硬件解码，把画面经**共享内存（像素）+ IPCM mailbox / cmdqu（命令）**交给小核上屏。

## 文档体系（三处分工，不要互相搬运内容）

| 文档 | 读者 | 管什么 |
| --- | --- | --- |
| 本文件 + `plans/` | AI / 开发者 | 模块导航、编码规范、各模块计划与测试流程 |
| `README.md` | 人（使用者） | 接线、烧卡复现、运行手册（改了行为记得同步它） |
| `sdcard/rootfs/root/指南/` | 板上接手者 | 板上运行记录，编写规范见其 `readme.txt`，随 SD 卡走 |

## 模块导航（★ 改哪块，先读哪个文件夹，不要一次全看）

| # | 模块 | 文件夹 | 管什么 | 状态 |
| --- | --- | --- | --- | --- |
| 01 | 小核固件 | `plans/01-smallcore-firmware/` | cvirtos.bin、热更、cmdqu 分发、LVGL 任务、探针、链接脚本 | 稳定运行 |
| 02 | 大核工具 | `plans/02-bigcore-tools/` | lcd_sender、rtsp2lcd、lcdcam.sh、编译与部署 | 稳定运行 |
| 03 | 摄像头链路 | `plans/03-camera-pipeline/` | sample_vi_fd、RTSP 服务端 quirk、断流诊断、自愈 | 稳定运行 |
| 04 | ST7789 显示 | `plans/04-display-st7789/` | 接线、SPI2 + sysDMA、屏方向、刷新性能 | 稳定运行 |
| 05 | 按钮界面 | `plans/05-ui-button/` | LVGL 中文 UI + 按钮联动摄像头（`lcd_sender cam on/off`） | **待实施** |
| 06 | 触摸 | `plans/06-touch/` | 触摸驱动骨架 + 大核守护进程（芯片型号待用户确认） | **待实施** |
| 07 | 打包发布 | `plans/07-packaging/` | sdcard/ 同步、整卡镜像重制、校验清单 | 稳定运行 |

每个模块文件夹内两份文件：`AGENTS.md`（该模块的事实、规矩、坑）、`PLAN.md`（改动/实施计划 + **测试流程**）。

## 编码原则

- **最小改动**：只碰与当前任务相关的代码，不为将来需求提前抽象、不顺手重构无关文件。
- **简单优先**：能直白写就不包一层；重复两三处好过猜错的抽象。
- **先读后写**：改文件前先读懂它怎么被用；两份必须逐字节一致的头（`lcd_shm.h`）改一处必须同步另一处。
- **小步可验证**：一次只做一件事，每步都能编译通过并按该模块 PLAN.md 的判据验证；失败要能看到
  （探针 / 日志），不要静默吞掉。
- **先计划后动手**：跨核、动协议、动固件的事，先在对应 `plans/*/PLAN.md` 里落计划再实施；
  实施完把结果与实测数字回填到 PLAN 的「实施结果」。

## 安全铁律（每一次动手前先过一遍）

1. **`fip.bin` 是唯一有变砖风险的文件**，不要随手动它。只改小核 C 代码 → 完全不碰 fip，走热更。
2. **改小核固件一律走热更**（`lcd_sender rtos <cvirtos.bin>`，~200ms、不重启、不碰 fip）。
   热更前先清 8 个 cmdqu 槽位并 kill 掉 `lcd_sender` / `rtsp2lcd`，否则 ENOBUFS。
3. **`lcd_sender mon` 与 `rtsp2lcd` 互斥**：两者共用 cmdqu，一起跑必把 8 槽打满，之后永久
   ENOBUFS **且不自愈**，唯一解药是热更一次小核固件。mon 已从开机自启摘除。
4. 改内核 / 重编内核时，`/mnt/system/ko` 下的 .ko 必须用**同一棵内核树**一起重编，否则模块
   能加载但功能静默失效甚至 Oops。
5. 密钥、token、密码一律不写进任何文档。

## 环境与技术栈

- 板：IP `192.168.42.1`，SSH root / milkv。板上**没有编译器**（只有 python3）、**没有 RTC**
  （看 uptime 比 date 可靠）。
- 编译环境：开发机 Windows + **WSL Ubuntu-22.04**（SDK 记作 `~/duo-sdk-v2`，基线提交
  `ad920f839`）。小核编译用 `wsl -u root` 跑（build 树里有 root 属主文件）。
- 本仓库改动以 **overlay** 形式存放在 `src/sdk_overlay/`（按 SDK 相对路径），应用进 SDK 用
  `tools/apply_sdk_overlay.sh ~/duo-sdk-v2`。
- 仓库路径带空格会拆坏 gcc 的 `-I`：编译脚本一律先镜像到无空格目录（`~/wbrepo`），不要在
  带空格路径下直接编。
- 产物：小核 `cvirtos.bin`（**定长 1640640B = 应用区 1638400 + 跳板 2240**，实际内容小靠填充
  补满——体积不变不代表没改动）；大核 `lcd_sender`（静态 aarch64）、`rtsp2lcd`（动态，依赖
  板上 `/mnt/system` 的库）。

## 目录与维护边界

| 位置 | 内容 | 边界 |
| --- | --- | --- |
| `src/sdk_overlay/freertos/…` | 小核源码（spi/lcd_st7789/lvgl_task/comm_main/hotjump/lv_conf/链接脚本…） | 自己维护，按 SDK 相对路径存放 |
| `src/sdk_overlay/build/…`、`linux_5.10/…` | 设备树、defconfig、spidev 上限 | 自己维护 |
| `src/big_core/` | 大核 lcd_sender.c + lcd_shm.h | 自己维护；lcd_shm.h 与小核侧副本逐字节一致 |
| `tools/` | 一键脚本、rtsp2lcd.c、部署/诊断脚本 | 自己维护 |
| `sdcard/` | 可直接拷进 SD 卡的覆盖文件（`rootfs/` = ext4 分区根） | 自己维护，与板上**同名同步** |
| `image/` | 整卡镜像 gzip 分卷 + CHECKSUMS | 大文件不改；重制流程见 `plans/07` |
| `sdcard/rootfs/root/指南/` | 板上运行记录 | 规范见其 `readme.txt`，不要往 readme.txt 写当日记录 |

## 构建与部署（总口径，细节在模块 PLAN）

```bash
# 小核固件（WSL，root）：
tools/build_small_core.sh ~/duo-sdk-v2        # 产物 $SDK/freertos/cvitek/install/bin/cvirtos.bin
# 大核程序（WSL）：
tools/build_big_core.sh ~/duo-sdk-v2          # 产物 ~/lcdhost/lcd_sender
bash ~/wb_build_big.sh                        # rtsp2lcd（脚本自动镜像到 ~/wbrepo 再编）
# 部署到板（默认 192.168.42.1，密码 milkv）：
tools/deploy.sh <板子IP>
```

热更标准流程（详见 `plans/01/PLAN.md`）：killall 两工具 → 清 8 槽（`0x1900400` 起每槽 8 字节写 0）
→ `scp` 固件 → `lcd_sender rtos <固件>` → 探针 `[14]` 核对 build id → 同步 `/mnt/data/cvirtos.bin`
与 `sdcard/` 两份副本。

## 全局坑速查（细节与出处见各模块）

- **探针地址随固件体积变**：每版固件必须 `nm cvirtos.elf | grep g_lvgl_probe` 重查；小核是 RV64，
  `unsigned long` 8 字节，devmem 步长必须按 8。
- **busybox 没有 pkill**；`killall` 按 argv[0] 匹配，打不中 `sh /root/xxx.sh` 这类进程——停脚本一律
  用它的 `stop` 子命令。
- **判断画面真的在动只看探针 `[17]`（大核提交帧号）在涨**；`[16]/[11]/[18]` 在 cmdqu 打满时都会骗人。
- 覆盖板上二进制前先 `killall`，否则 "Text file busy"；传完两边 `md5sum` 比对。
- `make rtos` 会复用陈旧 `.obj`，改了没生效就删掉对应 `.obj` 重编（见 `plans/01`）。
