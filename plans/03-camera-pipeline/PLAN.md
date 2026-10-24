# PLAN.md — 03 摄像头链路：起停流程 / 测试流程 / 断流诊断流程

事实见同目录 `AGENTS.md`。

---

## 一、手动起停流程（调试用；日常用 lcdcam.sh）

```bash
# 1) 起取流服务（等 ~2s 后 netstat -ltn | grep :554 出现监听）
/mnt/system/usr/bin/ai/sample_vi_fd /mnt/cvimodel/scrfd_768_432_int8_1x.cvimodel \
    >/tmp/sample_vi_fd.log 2>&1 &

# 2) 前台推屏（Ctrl-C 停; 停完服务会自杀, 再跑要回第 1 步）
cd /root
LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd \
    /root/rtsp2lcd rtsp://127.0.1.1/h264
```

常驻方式（推荐）：

```bash
nohup sh /root/lcdcam.sh >/dev/null 2>&1 &   # 后台常驻
tail -f /tmp/lcdcam.log                      # 看状态
sh /root/lcdcam.sh stop                      # 停推送（循环 + rtsp2lcd）
sh /root/lcdcam.sh stopall                   # 连取流服务一起收
```

## 二、验证画面真的在动（★ 只认探针 [17]）

`[17]` 的地址 = `g_lvgl_probe 基址 + 17*8`（如 0B 固件基址 0x8fe544a0 → `devmem 0x8fe54528 32`，
基址随固件变，见 `../01/PLAN.md`）。用板上现成脚本更省事：

```bash
sh /root/probe.sh; sleep 5; sh /root/probe.sh    # 对比两次 [17], 应 +30/秒左右
```

判据：`[17]` 在涨 ≈ +30/秒 = 画面在动。`[16]=1`、`[11]≈2.61M`、`[18]` 在涨**都不算数**
（cmdqu 打满时它们照样好看，屏上却是定格）。

## 三、长跑测试流程（改链路后必跑）

```bash
sh /root/longrun.sh 240
```

| 项 | 判据 |
| --- | --- |
| 异常行（`流结束|断开`） | = 0 |
| 「停顿」行 | 信息性，不算异常；频繁出现才查网络 |
| 帧率 | 稳定在源帧率（30fps）附近 |
| `[17]` 速率 | ≈ +30/秒，无长时间停顿 |
| `[drop] 积压` | 4 分钟 ≤ 1 次，触发后自动恢复 |

## 四、断流诊断流程（按顺序做，别跳）

1. **服务端在不在**：`netstat -ltn | grep :554`；`pidof sample_vi_fd`。
   没了 → 看 `/tmp/sample_vi_fd.log` 尾部（客户端断开自杀？模型加载失败？）。
2. **服务端发不发**：`python3 /tmp/rtsp_count.py 70` —— 能稳定计数（~988 kB/s、26fps 量级）
   = 服务端无罪，问题在 rtsp2lcd。
3. **客户端收没收**：看 `/tmp/lcdcam.log` / rtsp2lcd 输出里 `eof/err` 与「停顿」行。
4. **屏为什么不刷**：`sh /root/probe.sh` 两次对比 `[17]`：
   - `[17]` 冻住 + 日志刷 `No buffer space available` → cmdqu 打满，走自愈（下节）。
   - `[17]` 冻住 + 无 ENOBUFS → rtsp2lcd 挂了/没起，`ps | grep rtsp2lcd`。
5. **握手层**：`python3 tools/capture_stream.py`（抓 DESCRIBE/SETUP/PLAY 原始报文，
   对照 `AGENTS.md` 四 quirk 逐条排）。

## 五、自愈验证流程（改 lcdcam.sh 后必跑）

1. **一级（服务死）**：`killall sample_vi_fd` → 计时，≈9 秒内画面恢复、554 重新监听。
2. **二级（cmdqu 打满）**：跑着画面时另开 `lcd_sender mon 1000` 注入 →
   看门狗 ≤6 秒发现 ENOBUFS → 自动热更 → ≈15 秒内 `[17]` 恢复 +30/秒。
3. **停得干净**：`sh /root/lcdcam.sh stop` 后 `ps -o pid,args | grep -E 'lcdcam|rtsp2lcd|sample_vi_fd'`
   只剩（或不含）预期进程；不留孤儿 rtsp2lcd（它会占死单客户端名额）。

## 六、待办清单

- [ ] `plans/05` 落地后，`cam on` 走 `lcdcam.sh start`（新子命令），本模块流程同步更新。
- [ ] 人脸检测框是否已烧进 RTSP 流**未验证**：抓一帧带人脸的 `--dump` 看有无框。
      若要「画面+检测框上屏」再立计划（用户 2026-10-24 拍板先做纯画面）。
