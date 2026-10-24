# AGENTS.md — 03 摄像头链路（取流服务 / RTSP / 断流诊断 / 自愈）

通用规范见根 `../../AGENTS.md`；收流解码的客户端侧见 `../02-bigcore-tools/AGENTS.md`。

## 链路事实（接手先背这个）

```
GC2083 (1920x1080)
  └─ sample_vi_fd（大核，/mnt/system/usr/bin/ai/，内部跑 SCRFD 人脸检测）
       └─ 编码 H.264 1280x720，RTSP 服务端监听 554
            └─ rtsp2lcd（TCP interleaved 收流）
                 └─ CVI_VDEC 硬解 → 缩放 320x240 → RGB565 大端 → 共享内存 fb → LCD_CMD_CAM
```

- 起服务（板载现成例子 + 模型参数）：

```bash
/mnt/system/usr/bin/ai/sample_vi_fd /mnt/cvimodel/scrfd_768_432_int8_1x.cvimodel \
    >/tmp/sample_vi_fd.log 2>&1 &
```

- 拉流地址固定 `rtsp://127.0.1.1/h264`（★ 是 **127.0.1.1**，不是 127.0.0.1——服务端自己绑的）。
- 判活：`netstat -ltn | grep :554`；起进程到监听约 **2 秒**。

## sample_vi_fd 的两个决定性行为（写自愈逻辑的依据）

1. **只服务一个客户端**：已有客户端连着时，第二个连接一个字节都收不到
   （日志刷「停顿 N s 无数据(g_rlen=0)」）。
2. **客户端一断它整体退出**（不是保持常驻）：日志依次 `RTSP client disconnected` →
   `CVI_VENC_GetStream failed 0xc007804c` → `Exit encoder/TDL thread` → `destroy middleware` →
   `stop VPSS`，554 随之消失。→ **重连前必须重拉服务**，只重连客户端没用。

## 服务端（live555 v2020.07.21）四个 quirk（自写客户端必看）

1. DESCRIBE 回 `Content-Length: 282` 但 **SDP 正文不在同一个 TCP 段**——必须按长度读满再解析。
2. ★ SETUP 必须发到**会话 URL**（`rtsp://127.0.1.1/h264`）。发到 SDP 里的 `/track1` 也回 200 OK，
   但**一个字节都不给**——状态码全对就是没数据，最坑人的一条。
3. PLAY **不返回响应头**，直接开始推 `$` 交织帧（首字节 0x24，PT=96）——不要傻等 `RTSP/1.0 200 OK`。
4. 会话 timeout=65 秒，客户端**每 20 秒发一次 OPTIONS** 保活（用会话 URL + Session token）。

## 流格式

- H.264 1280x720，RTP PT=96；交织帧 = 1B `0x24` + 1B channel + 2B 大端长度 + RTP 包。
- RTP 头长 = `12 + (pkt[0] & 0x0F) * 4`（csrc 数算错后面全错位）。

## 已量化指标（回归时对照）

- 长跑：4 分钟 **7298 帧、0 断流、稳定 30fps**；去 demo 固件上 60 秒 `[17]` 稳定 **+30.5/秒**。
- 自愈时间：服务被杀 → 恢复 ≈ **9 秒**；cmdqu 打满 → 恢复 ≈ **15 秒**（lcdcam.sh 两级自愈）。
- 积压漂移：约 1%/秒的接收缓冲上涨属正常，rtsp2lcd 的 1.5MB 丢 GOP 兜底盖得住（4 分钟触发 ≤1 次）。

## 坑

- 「屏上画面定格但帧率/像素率都正常」= cmdqu 被打满，**只看 `[17]`**（见根 AGENTS）。
- 客户端断开后 1~2 秒内 554 仍在监听但服务端正在拆，这时连上去秒断——所以 lcdcam.sh
  选择**无条件重拉**而不是探活复用。
- 板上诊断工具：`/tmp/rtsp_count.py`（纯计数客户端，判「服务端不发 vs 客户端不收」）、
  `tools/capture_stream.py`（抓包/握手诊断）。
