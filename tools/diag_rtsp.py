#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""diag_rtsp.py —— 诊断: 完整打印 RTSP 各步响应, 并 dump PLAY 之后真正收到的字节。"""
import base64
import socket
import sys
import time

URL = sys.argv[1] if len(sys.argv) > 1 else "rtsp://127.0.1.1/h264"


def parse_url(u):
    rest = u[7:]
    hostport, _, path = rest.partition("/")
    path = "/" + path
    h, _, p = hostport.partition(":")
    return h, int(p) if p else 554, path


def hd(b, n=400):
    b = b[:n]
    return " ".join("%02X" % c for c in b)


def asc(b, n=400):
    b = b[:n]
    return "".join(chr(c) if 32 <= c < 127 else "." for c in b)


host, port, path = parse_url(URL)
s = socket.create_connection((host, port), timeout=8)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
s.settimeout(4)

state = {"cseq": 1, "sess": "", "pos": 0, "buf": b""}


def fill():
    if state["pos"]:
        state["buf"] = state["buf"][state["pos"]:]
        state["pos"] = 0
    try:
        d = s.recv(65536)
    except socket.timeout:
        return b""
    state["buf"] += d
    return d


def req(method, url, extra=""):
    r = "%s %s RTSP/1.0\r\nCSeq: %d\r\n%s%s\r\n" % (
        method, url, state["cseq"], state["sess"], extra)
    state["cseq"] += 1
    print("\n>>>>>>>>>> %s %s" % (method, url))
    s.sendall(r.encode())

    # 一直读到 \r\n\r\n 或 '$'
    while True:
        i = state["buf"].find(b"\r\n\r\n", state["pos"])
        d = state["buf"].find(b"$", state["pos"])
        if d != -1 and (i == -1 or d < i):
            hdr = state["buf"][:d]
            state["pos"] = d
            return hdr
        if i != -1:
            state["pos"] = i + 4
            return state["buf"][:i + 4]
        more = fill()
        if not more:
            return state["buf"]


print("=== 连 %s:%d ===" % (host, port))
r = req("DESCRIBE", URL, "Accept: application/sdp\r\n")
print("<<< DESCRIBE 响应 (%d 字节):" % len(r))
print(r.decode(errors="replace"))

sdp = r.decode(errors="replace")
ctrl = "*"
for line in sdp.split("\r\n"):
    if line.startswith("a=control:"):
        ctrl = line[len("a=control:"):].strip()
        break
for line in sdp.split("\r\n"):
    if line.startswith("sprop-parameter-sets="):
        v = line[len("sprop-parameter-sets="):].strip()
        a, _, b = v.partition(",")
        try:
            print("  SPS b64=%s -> %d 字节" % (a, len(base64.b64decode(a + "=="))))
            print("  PPS b64=%s -> %d 字节" % (b, len(base64.b64decode(b + "=="))))
        except Exception as ex:
            print("  base64 解码失败:", ex)
        break

if ctrl == "*":
    base = URL
elif ctrl.startswith("rtsp://"):
    base = ctrl
elif ctrl.startswith("/"):
    base = "rtsp://%s:%d%s" % (host, port, ctrl)
else:
    base = URL + "/" + ctrl

r = req("SETUP", base, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
print("<<< SETUP 响应 (%d 字节):" % len(r))
print(r.decode(errors="replace"))
for line in r.decode(errors="replace").split("\r\n"):
    if line.lower().startswith("session:"):
        state["sess"] = "Session: %s\r\n" % line.split(":", 1)[1].strip().split(";")[0]

r = req("PLAY", URL, "Range: npt=0.000-\r\n")
print("<<< PLAY 响应 (%d 字节):" % len(r))
print(r.decode(errors="replace"))

print("\n=== PLAY 之后 4 秒内收到的字节 ===")
got = b""
t0 = time.time()
while time.time() - t0 < 4:
    try:
        d = s.recv(65536)
        if not d:
            print("  对端关闭")
            break
        got += d
        if len(got) > 600:
            break
    except socket.timeout:
        print("  (4 秒内无数据)")
        break

print("共收到 %d 字节" % len(got))
if got:
    print("HEX :", hd(got, 192))
    print("ASCII:", asc(got, 192))
s.close()
