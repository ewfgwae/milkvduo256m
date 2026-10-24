# AGENTS.md — 02 大核工具（lcd_sender / rtsp2lcd / lcdcam.sh）

通用规范见根 `../../AGENTS.md`；摄像头服务端细节见 `../03-camera-pipeline/AGENTS.md`。

## 文件地图

| 文件 | 板上位置 | 职责 |
| --- | --- | --- |
| `src/big_core/lcd_sender.c` | `/root/lcd_sender`、`/mnt/data/lcd_sender` | cmdqu + 共享内存的命令行前端：刷屏/背光/探活/热更 |
| `tools/rtsp2lcd.c` | `/root/rtsp2lcd` | RTSP 客户端 + CVI_VDEC 硬解 + 缩放 + RGB565(大端) + 送屏 |
| `tools/lcdcam.sh` | `/root/lcdcam.sh` | 常驻脚本：断线重连 + 两级自愈（服务重拉 / cmdqu 复位） |
| `tools/build_big_core.sh` | — | 编 lcd_sender（WSL） |
| `tools/build_rtsp2lcd.sh`、`~/wb_build_big.sh` | — | 编 rtsp2lcd（先镜像到无空格目录） |
| `tools/deploy.sh` / `deploy_big.sh` | — | scp + 停旧进程 + 覆盖 + md5 校验 |
| `tools/probe.sh` / `longrun.sh` | `/root/` | 读探针 / 长跑采样 |

## lcd_sender 子命令（现状）

| 命令 | 作用 |
| --- | --- |
| `stat` | 打印共享内存 ctrl/stat |
| `init` | 写 magic/version + 发 LCD_CMD_INIT |
| `ping` | LCD_CMD_PING 探活 |
| `bl 0/1` | 背光 |
| `fill <rgb565>` / `bars` / `anim [n]` | 刷屏测试 |
| `mon [ms]` | 采样 /proc/stat 发 LCD_CMD_MON。★ **与 rtsp2lcd 互斥**（见根 AGENTS 安全铁律 3） |
| `rtos <固件>` | 热更小核固件（唯一入口，见 `../01/PLAN.md`） |
| `cam on/off` | **待实施**（`plans/05` Phase 2）：按钮动画 + 起停摄像头链路 |

## ioctl / cmdqu 纪律（新写大核程序必须照抄）

1. ioctl 号**照抄 lcd_sender**，不要自己猜：
   `RTOS_CMDQU_SEND _IOW('r', 1, unsigned long)`、`RTOS_CMDQU_SEND_WAIT _IOW('r', 4, unsigned long)`。
   （错写成 `_IOW('R', 0x10, cmdqu_t)` 会报 EFAULT——驱动把第三参数当指针。）
2. `cmdqu_t` 必须 `memset` 清零且 **`block` 显式置 0**，否则栈垃圾让回执匹配错位。
3. 设备节点 `/dev/cvi-rtos-cmdqu`；常开一个 fd 反复用。
4. 每秒几十帧的路径（CAM）**不回执**；只有低频命令才可用 SEND_WAIT。
5. `g_seq` 续号：新进程启动时从共享内存 `max(ctrl.seq, stat.ack_seq)` 续上，否则「等 ack」立即假成功。

## rtsp2lcd 要点

- 链路：RTSP(TCP interleaved) → CVI_VDEC → 缩放 320×240 → YUV→RGB565 大端 → 写 fb → `LCD_CMD_CAM`。
- 参数：`[url]`、`--dump out.ppm`（抓帧不碰屏）、`--raw out.264`、`--frames N`、`--size WxH`、
  `--pixfmt planar|nv21|nv12`、`--swapuv`、`--convs 1..8`（默认 **2**）、`--convbench N`。
- 判「流断」以 socket eof/err 为准，**不要**拿「多久没数据」当断流（老 bug，曾致 274 帧必断）。
- 积压超 1.5MB 丢弃当前 GOP 等下一个 IDR（长跑防延迟漂移）。
- 性能结论（已定，别再猜）：A53 实际 ~300MHz，瓶颈是纯标量像素循环；`--convs 2` 单帧 ~31ms ≈ 32fps。

## lcdcam.sh 行为（改它前先懂它）

- `sh /root/lcdcam.sh`：前台循环；`stop`：停循环 + rtsp2lcd；`stopall`：连 sample_vi_fd 一起收。
  ★ 别用 `killall lcdcam.sh`（argv[0] 是 `sh`，打不中）；本板没有 pkill。
- 启动时：扫 /proc 收同类循环（只认 **argv[1] 是本脚本路径**，别对整条 cmdline 做 grep——会把
  调用它的 `sh -c` 一起杀掉）→ 收掉 `mon` → 确保取流服务在跑。
- 每轮重连前**无条件重拉** sample_vi_fd（客户端一断服务端自杀，见模块 03）。
- 6 秒看门狗盯日志：新增 `No buffer space available` → `recover_channel()`（收 mon + 清 8 槽 +
  热更小核）——cmdqu 打满的**唯一**解药。
- 日志 `/tmp/lcdcam.log`（超 1MB 自动截尾）；PID 文件 `/tmp/lcdcam.pid`。
- `start` 子命令**待实施**（`plans/05` Phase 2）：防重复启动 + nohup 后台化，给 `lcd_sender cam on` 调。

## 编译三坑（固定）

1. `#error "Unknown Chip Architecture!"` → 缺 `-D__CV181X__`。
2. `-I` 被拆坏 → 仓库路径带空格，先镜像到 `~/wbrepo`。
3. rtsp2lcd 头用 `duo-tdl-examples/include/system`、库用 `~/boardlibs`（板上原版逐字节副本），
   链接 `-lvdec -lsys -lpthread -lrt -lm`。
