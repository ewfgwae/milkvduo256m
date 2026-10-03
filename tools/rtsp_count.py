#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""纯计数 RTSP 客户端: 只走 TCP interleaved, 统计包/字节/帧(RTP marker), 跑固定秒数。
用途: 判断 rtsp://127.0.1.1/h264 服务端到底是"每连接限量"还是"一直发"。"""
import socket
import sys
import time

HOST, PORT = "127.0.1.1", 554
URL = "rtsp://127.0.1.1:554/h264"
DUR = int(sys.argv[1]) if len(sys.argv) > 1 else 60

s = socket.create_connection((HOST, PORT), timeout=10)
s.settimeout(10)


def read_resp(sock):
    buf = b""
    while b"\r\n\r\n" not in buf:
        d = sock.recv(4096)
        if not d:
            return buf, b""
        buf += d
    h, _, rest = buf.partition(b"\r\n\r\n")
    cl = 0
    for line in h.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            cl = int(line.split(b":")[1].strip())
    while len(rest) < cl:
        d = sock.recv(cl - len(rest))
        if not d:
            break
        rest += d
    return h, rest


def req(method, url, cseq, extra=""):
    r = "%s %s RTSP/1.0\r\nCSeq: %d\r\n%s\r\n" % (method, url, cseq, extra)
    s.sendall(r.encode())


req("OPTIONS", URL, 1)
h, b = read_resp(s)
print("OPTIONS ->", h.split(b"\r\n")[0].decode(errors="replace"))

req("DESCRIBE", URL, 2, "Accept: application/sdp\r\n")
h, sdp = read_resp(s)
print("DESCRIBE ->", h.split(b"\r\n")[0].decode(errors="replace"),
      "sdp", len(sdp), "B")

# ★ quirk: SETUP 必须发到会话 URL, 发 /track1 会 200 但一个字节都不给
req("SETUP", URL, 3,
    "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
h, _ = read_resp(s)
sess = ""
for line in h.split(b"\r\n"):
    if line.lower().startswith(b"session:"):
        # ★ session 只是那个 token, 后面的 ";timeout=65" 是参数, 回填时必须剥掉
        sess = line.split(b":", 1)[1].strip().decode().split(";")[0]
print("SETUP ->", h.split(b"\r\n")[0].decode(errors="replace"), "session", sess)

# PLAY 不返回响应头, 直接开始交织数据
s.sendall(("PLAY %s RTSP/1.0\r\nCSeq: 4\r\nSession: %s\r\n\r\n"
           % (URL, sess)).encode())

t0 = time.time()
nbytes = 0
npkt = 0
nfrm = 0
first = None
last_report = t0
buf = b""
gaps = []          # 记录 >0.5s 的空档
last_data = t0

while time.time() - t0 < DUR:
    try:
        d = s.recv(65536)
    except socket.timeout:
        print("[%6.1fs] ** recv 超时 10s (服务端 10 秒没发任何字节)" % (time.time() - t0))
        gaps.append(time.time() - last_data)
        break
    if not d:
        print("[%6.1fs] ** 对端关闭连接 (EOF)" % (time.time() - t0))
        break
    now = time.time()
    if now - last_data > 0.5:
        gaps.append(now - last_data)
    last_data = now
    if first is None:
        first = now
    nbytes += len(d)
    buf += d

    # 切交织包: '$' + chan(1) + len(2) + payload
    i = 0
    while True:
        p = buf.find(b"$", i)
        if p < 0 or len(buf) - p < 4:
            break
        plen = (buf[p + 2] << 8) | buf[p + 3]
        if len(buf) - p - 4 < plen:
            break
        payload = buf[p + 4:p + 4 + plen]
        npkt += 1
        if plen >= 12:
            m = payload[1] & 0x80      # RTP marker = 一帧结束
            if m:
                nfrm += 1
        i = p + 4 + plen
    buf = buf[i:]

    if now - last_report >= 5:
        el = now - t0
        print("[%6.1fs] 包=%-6d 帧=%-6d 收=%6.0f kB/s  均值=%6.0f kB/s"
              % (el, npkt, nfrm, nbytes / el / 1024, nbytes / el / 1024))
        last_report = now

el = time.time() - t0
if npkt == 0 and nbytes:
    print("PLAY 后收到的前 %d 字节: %r" % (min(nbytes, 200), buf[:200]))
print("---- 总计: %.1fs  包=%d  帧=%d  字节=%d  均=%.0f kB/s  空档(>0.5s)=%s"
      % (el, npkt, nfrm, nbytes, nbytes / max(el, .001) / 1024,
         ["%.1f" % g for g in gaps]))
if nfrm:
    print("    平均帧率 = %.1f fps" % (nfrm / el))
s.close()
