#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
capture_stream.py —— 极简 RTSP 客户端, 把 H.264 交织流存成 Annex-B 文件。
作用: 在**不编译任何东西**的前提下, 先在板上验证 rtsp2lcd.c 用到的那套协议流程
      (DESCRIBE -> SETUP(TCP interleaved) -> PLAY -> '$' 交织帧 -> FU-A 重组)
      是否真能拿到完整 NAL; 拿到的 .264 再喂给板上 sample_vdec 验证硬件解码。

★ 关键坑(实测): DESCRIBE 的响应头里带 Content-Length(本例 282), 而 **SDP 正文不在
  同一个 TCP 段里**。如果只读到 "\r\n\r\n" 就返回, 后面的 SDP 会被当成"上一个响应
  的残留", 导致后续 \r\n\r\n / '$' 的定位全部错位 —— 表现就是 PLAY 收不到数据。
  所以必须按 Content-Length 把正文读满再收工。

用法: python3 capture_stream.py [rtsp_url] [out.264] [最多写多少帧]
"""
import os
import socket
import sys
import time

URL = sys.argv[1] if len(sys.argv) > 1 else "rtsp://127.0.1.1/h264"
OUT = sys.argv[2] if len(sys.argv) > 2 else "/tmp/cap.264"
MAX_AU = int(sys.argv[3]) if len(sys.argv) > 3 else 60


def log(*a):
    print(*a, flush=True)


def parse_url(u):
    assert u.startswith("rtsp://"), "只支持 rtsp://"
    rest = u[7:]
    hostport, _, path = rest.partition("/")
    path = "/" + path
    h, _, p = hostport.partition(":")
    return h, (int(p) if p else 554), path


class Rtsp:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=8)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.s.settimeout(6)
        self.buf = b""
        self.pos = 0
        self.cseq = 1
        self.session = ""
        self.sps = b""
        self.pps = b""

    # ---------- 底层 ----------
    def fill(self):
        """读一次, 追加进 buf。返回收到字节数; <=0 表示超时/关闭。"""
        if self.pos:
            self.buf = self.buf[self.pos:]
            self.pos = 0
        try:
            d = self.s.recv(65536)
        except socket.timeout:
            return -1
        if not d:
            return 0
        self.buf += d
        return len(d)

    def find_marker(self):
        """找 buf 里首个 '$' 或 '\\r\\n\\r\\n'。
        返回 (idx, is_dollar); 找不到 (-1, False)。"""
        i = 0
        n = len(self.buf)
        while i < n:
            if self.buf[i] == 0x24:
                return i, True
            if self.buf[i:i + 4] == b"\r\n\r\n":
                return i, False
            i += 1
        return -1, False

    # ---------- RTSP ----------
    def request(self, method, url, extra=""):
        r = "%s %s RTSP/1.0\r\nCSeq: %d\r\n%s%s\r\n" % (
            method, url, self.cseq, self.session, extra)
        self.cseq += 1
        log("  >> %s %s" % (method, url))
        self.s.sendall(r.encode())

        if self.pos:
            self.buf = self.buf[self.pos:]
            self.pos = 0

        hlen = None
        while True:
            idx, is_dollar = self.find_marker()
            if idx >= 0:
                if is_dollar:
                    # 没有响应头(PLAY 常这样), 交织数据从 idx 开始
                    self.pos = idx
                    return self.buf[:idx].decode(errors="replace")
                hlen = idx + 4
                break
            if self.fill() <= 0:
                self.pos = len(self.buf)
                return self.buf.decode(errors="replace")

        # 按 Content-Length 把正文读满
        head = self.buf[:hlen].decode(errors="replace")
        clen = 0
        for line in head.split("\r\n"):
            if line.lower().startswith("content-length:"):
                try:
                    clen = int(line.split(":", 1)[1].strip())
                except ValueError:
                    clen = 0
                break
        while len(self.buf) < hlen + clen:
            if self.fill() <= 0:
                break
        self.pos = min(hlen + clen, len(self.buf))
        return head + self.buf[hlen:self.pos].decode(errors="replace")

    def do_setup(self, url):
        r = self.request("DESCRIBE", url, "Accept: application/sdp\r\n")
        if "200 OK" not in r:
            log("DESCRIBE 失败:\n" + r[:400])
            return False

        ctrl = None
        track = None
        for line in r.split("\r\n"):
            if line.startswith("a=control:"):
                v = line[len("a=control:"):].strip()
                if v == "*":
                    ctrl = "*"
                elif v:
                    track = v
                    if ctrl is None:
                        ctrl = v
        for line in r.split("\r\n"):
            if line.startswith("sprop-parameter-sets="):
                import base64
                v = line[len("sprop-parameter-sets="):].strip()
                a, _, b = v.partition(",")
                try:
                    self.sps = base64.b64decode(a + "==")
                    self.pps = base64.b64decode(b + "==")
                except Exception:
                    pass
                break
        log("  control=%r track=%r  SPS=%d  PPS=%d"
            % (ctrl, track, len(self.sps), len(self.pps)))

        def to_url(v):
            if not v or v == "*":
                return url
            if v.startswith("rtsp://"):
                return v
            if v.startswith("/"):
                h, p, _ = parse_url(url)
                return "rtsp://%s:%d%s" % (h, p, v)
            return url.rstrip("/") + "/" + v

        # ★ 实测: 这个服务端(live555 派生)只有在**会话 URL** 上 SETUP 才会真的出流;
        #   SETUP 到 /track1 也回 200, 但之后一个字节都不给。所以会话 URL 优先。
        cands = [url]
        u2 = to_url(track)
        if u2 != url:
            cands.append(u2)

        r = ""
        for i, base in enumerate(cands):
            r = self.request("SETUP", base,
                             "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
            if "200 OK" in r:
                log("  SETUP 用 %s 成功" % base)
                break
            log("  SETUP %s 失败, 换下一个" % base)
        if "200 OK" not in r:
            log("SETUP 全失败:\n" + r[:400])
            return False
        for line in r.split("\r\n"):
            if line.lower().startswith("session:"):
                self.session = "Session: %s\r\n" % line.split(":", 1)[1].strip().split(";")[0]
                break
        log("  %s" % self.session.strip())

        r = self.request("PLAY", url, "Range: npt=0.000-\r\n")
        log("  PLAY 返回 %d 字节" % len(r))
        return True

    # ---------- 交织包 ----------
    def next_interleaved(self):
        dbg = int(os.environ.get("DBG", "0"))
        it = 0
        while True:
            it += 1
            i = self.buf.find(b"$", self.pos)
            if i == -1:
                if dbg and it < 12:
                    log("    [dbg#%d] 无'$' buflen=%d pos=%d 头64=%s"
                        % (it, len(self.buf), self.pos, self.buf[:64].hex()))
                self.pos = len(self.buf)
                r = self.fill()
                if r <= 0:
                    if dbg:
                        log("    [dbg] fill 返回 %d -> 结束 (buflen=%d)" % (r, len(self.buf)))
                    return None
                continue
            self.pos = i
            while len(self.buf) - self.pos < 4:
                if self.fill() <= 0:
                    if dbg:
                        log("    [dbg] 交织头没凑齐 -> 结束")
                    return None
            plen = (self.buf[self.pos + 2] << 8) | self.buf[self.pos + 3]
            if plen < 12:
                if dbg:
                    log("    [dbg] plen=%d 太小, 跳 1 字节" % plen)
                self.pos += 1
                continue
            while len(self.buf) - self.pos < 4 + plen:
                if self.fill() <= 0:
                    if dbg:
                        log("    [dbg] 包没凑齐(需要 %d, 现有 %d) -> 结束"
                            % (4 + plen, len(self.buf) - self.pos))
                    return None
            ch = self.buf[self.pos + 1]
            pay = self.buf[self.pos + 4:self.pos + 4 + plen]
            self.pos += 4 + plen
            return ch, pay


def main():
    host, port, path = parse_url(URL)
    log("=== 连 %s:%d%s ===" % (host, port, path))
    r = Rtsp(host, port)
    if not r.do_setup(URL):
        return 1

    fp = open(OUT, "wb")
    if r.sps:
        fp.write(b"\x00\x00\x00\x01" + r.sps + b"\x00\x00\x00\x01" + r.pps)

    fu = b""
    fu_on = False
    au = b""
    n_nal = 0
    n_au = 0
    sizes = []
    t0 = time.time()
    marker = 0

    while n_au < MAX_AU:
        got = r.next_interleaved()
        if got is None:
            log("流结束")
            break
        ch, pkt = got
        if ch != 0:
            continue
        hl = (pkt[0] & 0x0F) * 4
        if hl < 12 or len(pkt) < hl + 1:
            continue
        marker = (pkt[1] >> 7) & 1
        pay = pkt[hl:]
        nt = pay[0] & 0x1F

        if 1 <= nt <= 23:
            nal = pay
            n_nal += 1
        elif nt == 28 and len(pay) > 2:
            start = pay[1] & 0x80
            end = pay[1] & 0x40
            if start:
                fu = bytes([(pay[0] & 0xE0) | (pay[1] & 0x1F)]) + pay[2:]
                fu_on = True
            elif fu_on:
                fu += pay[2:]
            if end and fu_on:
                nal = fu
                fu_on = False
                n_nal += 1
            else:
                continue
        else:
            continue

        au += b"\x00\x00\x00\x01" + nal
        if marker:
            fp.write(au)
            sizes.append(len(au))
            au = b""
            n_au += 1
            if n_au % 15 == 0:
                log("  已写 %d 帧 (%d NAL, %.1fs)" % (n_au, n_nal, time.time() - t0))

    fp.close()
    log("完成: %d 帧, %d NAL, 文件 %d 字节" % (n_au, n_nal, os.path.getsize(OUT)))
    if sizes:
        log("每帧字节: 最小 %d 最大 %d 平均 %d" % (
            min(sizes), max(sizes), sum(sizes) // len(sizes)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
