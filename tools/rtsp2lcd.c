/*
 * rtsp2lcd.c —— 摄像头画面 -> 硬件 H.264 解码 -> RGB565 -> 共享内存 -> 小核 LVGL -> ST7789
 *
 * 方案:
 *   1) 大核自己实现一个**极简 RTSP 客户端**(TCP interleaved), 从板上
 *      sample_vi_fd 推的 rtsp://127.0.1.1/h264 里取 H.264 RTP, 拼回
 *      Annex-B 访问单元(AU);
 *   2) 交给板上**硬件解码器 CVI_VDEC** 解码
 *      —— 软件解码走不通: 板上 libavcodec 只有 mjpeg/aac/pcm_s16le 三个解码器;
 *   3) GetFrame 拿 NV21/PLANAR420 -> 转 RGB565(大端) -> 写共享内存 fb 区;
 *   4) mailbox 发 LCD_CMD_CAM, 小核 LVGL 把它贴到 ST7789 上(320x240 横屏铺满)。
 *
 * ── 为什么这次不会碰到"头与库 ABI 错位" ───────────────────────────────
 *   本工具**只用 CVITEK MPI 这一层**(cvi_sys.h / cvi_vdec.h),
 *   不碰 SAMPLE_* / TDL 那些出过错位的结构体(SAMPLE_VI_CONFIG_S 之类)。
 *   实测: milkv-duo256-develop/cvi_mpi/include 与 duo-tdl-examples/include/system
 *   下的 cvi_vdec.h / cvi_comm_vdec.h / cvi_sys.h / cvi_vb.h / cvi_comm_vb.h /
 *   cvi_comm_video.h / cvi_buffer.h **逐字节一致**;
 *   链接时用**板上原版**库(/home/chen/boardlibs == /mnt/system/usr/lib 的副本),
 *   头与库都对着同一套 ABI, 所以 CreateChn 不会像 CVI_VPSS_CreateGrp 那样报
 *   ILLEGAL_PARAM。
 *
 * ── 与 sample_vdec 的关系 ──────────────────────────────────────────────
 *   初始化序列照抄板上 sample_vdec 的**默认路径**(H.264 + VB_SOURCE_COMMON):
 *       CVI_SYS_Exit() -> CVI_SYS_Init()
 *       -> CVI_VDEC_CreateChn -> CVI_VDEC_SetChnParam -> CVI_VDEC_StartRecvStream
 *   这条路径在板上是实测可用的(见 指南/2026.9.30-摄像头-画面显示到屏.txt 3.10)。
 *
 * 用法:
 *   rtsp2lcd [rtsp_url] [--dump out.ppm] [--raw out.264] [--frames N]
 *            [--size WxH] [--pixfmt planar|nv21|nv12] [--swapuv]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "lcd_shm.h"

/* ---- CVITEK MPI: 只要 SYS/VDEC 两层 ---- */
#include "cvi_sys.h"
#include "cvi_vdec.h"

typedef struct {
	unsigned char  ip_id;
	unsigned char  cmd_id;
	unsigned short block;      /* 0 = 不等回执(回执看共享内存 stat); 1 = 阻塞 */
	unsigned int   param_ptr;
} cmdqu_t;

/* ★ ioctl 号必须与板上的驱动严格一致 —— 直接照抄 src/big_core/lcd_sender.c
 *   (那是验证过能用的版本)。踩过的坑(2026-10-03): 原先写成
 *   _IOW('R', 0x10, cmdqu_t)(大写 R、号 0x10), 驱动认不出来, 掉进了别的
 *   分支把 param_ptr 当用户指针去 copy_from_user —— 报错是
 *   "ioctl RTOS_CMDQU_SEND: Bad address"(EFAULT), 而不是 ENOTTY, 很容易
 *   误判成"参数没填对"。正确的号是 _IOW('r', 1, unsigned long)(小写 r、1)。 */
#define RTOS_CMDQU_SEND      _IOW('r', 1, unsigned long)
#define RTOS_CMDQU_SEND_WAIT _IOW('r', 4, unsigned long)

#define CMDQU_DEV            "/dev/cvi-rtos-cmdqu"

#define FB_W      LCD_W            /* 320 */
#define FB_H      LCD_H            /* 240 */
#define FB_BYTES  (FB_W * FB_H * 2)

#define VDEC_CHN_ID   0
#define VDEC_FRM_CNT  6            /* 解码输出缓冲个数(越多越抗抖动) */

/* 解码器允许的**最大**图像尺寸 —— 决定流的解码缓冲/帧缓冲要开多大。
 * 摄像头 GC2083 是 1920x1080, 取 1088 是为了 16 对齐; 真要更大用 --size 覆盖。 */
#define DEC_DEF_W  1920
#define DEC_DEF_H  1088

#define NAL_BUF_BYTES   (1 << 20)  /* 单个 NAL 上限 */
#define AU_BUF_BYTES    (1 << 21)  /* 一帧(访问单元)上限 */
/* 接收缓冲(接收线程往里塞, 主线程从里面取)。
 * ★ 2026-10-03 教训: 原来是 2MB、且**边收边解**在同一个线程里 —— 解码+缩放
 *   一帧要几十毫秒, 期间没人 recv, 服务端(live555)的 TCP 发送窗口填满后
 *   sendDataOverTCP 反复重发, 最后 errno=EAGAIN 干脆关掉连接, 表现就是
 *   "跑了十几秒突然 流结束/断开"。现在由独立线程只管收, 缓冲放大到 8MB
 *   (≈2 分钟 @512kbps), 主线程慢一点也只是丢帧, 不会再被服务端踢掉。 */
#define RCV_BUF_BYTES   (1 << 23)
#define RCV_STAGE_BYTES (1 << 18)  /* 接收线程的暂存块: 一次 recv 最多这么多 */
/* 积压超过这个量就丢掉、跳到下一个 IDR 重新同步。
 * ★ 为什么需要: 源 ~30fps、本工具消费 ~30fps, 两者有 ~1% 的速率漂移(实测
 *   90 秒里积压从 33KB 涨到 795KB)。不丢的话会一路积到 8MB 缓冲满, 之后
 *   画面延迟越来越大(几十秒)。1.5MB ≈ 45 帧 ≈ 1.5 秒画面, 超过就说明
 *   已经跟不上, 干脆丢干净重新对齐, 代价是丢的那一下有 ≤1 个 GOP 的卡顿。 */
#define DROP_KEEP_BYTES (3 << 19)  /* 1.5MB */

static volatile int g_exit;
static void on_sig(int s) { (void)s; g_exit = 1; }

/* SDP 的 sprop-parameter-sets 是 base64, 下面 rtsp_setup() 要用(实现在文件末尾) */
static int b64_decode(const char *in, unsigned char *out, int outsz);

/* 独立诊断日志: 直接写文件并 fflush, 这样即使程序卡死(甚至被 kill -9)
 * 也能看到它走到了哪一步 —— stdout 重定向到文件时是块缓冲的, 什么也留不下。 */
static FILE *g_dbg;
static void dbg(const char *fmt, ...)
{
	va_list ap;

	if (!g_dbg) {
		g_dbg = fopen("/tmp/rtsp2lcd.log", "w");
		if (!g_dbg) return;
	}
	va_start(ap, fmt);
	vfprintf(g_dbg, fmt, ap);
	va_end(ap);
	fputc('\n', g_dbg);
	fflush(g_dbg);
}

/* ================= 共享内存 ================= */
static struct lcd_shm *g_shm;

static void shm_map(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	void *p;

	if (fd < 0) { perror("open /dev/mem"); exit(1); }
	p = mmap(NULL, LCD_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED,
		 fd, (off_t)LCD_SHM_PHYS);
	if (p == MAP_FAILED) { perror("mmap /dev/mem"); exit(1); }
	close(fd);
	g_shm = (struct lcd_shm *)p;
	g_shm->ctrl.magic   = LCD_SHM_MAGIC;
	g_shm->ctrl.version = LCD_SHM_VERSION;
}

/* cmdqu 句柄常开: 每秒几十帧都要发, 每次 open/close 没必要 */
static int cmdqu_open(void)
{
	static int fd = -1;

	if (fd < 0) {
		fd = open(CMDQU_DEV, O_WRONLY);
		if (fd < 0) perror("open " CMDQU_DEV);
	}
	return fd;
}

static void send_cmd(int cmd_id, unsigned int param)
{
	cmdqu_t c;
	int fd = cmdqu_open();

	if (fd < 0) return;
	memset(&c, 0, sizeof(c));
	c.ip_id     = 6;                       /* IP_SYSTEM */
	c.cmd_id    = (unsigned char)cmd_id;
	c.block     = 0;                       /* 不等回执, 免得打满 8 个槽位 */
	c.param_ptr = param;
	if (ioctl(fd, RTOS_CMDQU_SEND, &c) < 0)
		perror("ioctl RTOS_CMDQU_SEND");
}

/* ================= RTSP 客户端 ================= */
static int  g_sock = -1;
static char g_sess[128];
static int  g_cseq = 1;
static char g_host[128];
static int  g_port = 554;

/* ============ 接收线程 <-> 主线程 之间的大缓冲 ============
 * 约定: [0, g_rpos) 已消费, [g_rpos, g_rlen) 待处理。
 *   - 接收线程**只往后追加**(g_rlen += n), 从不改 g_rpos;
 *   - 主线程在 rb_fill() 里做压缩(memmove 到 0) —— 压缩前必须确保接收线程
 *     没有正在往 buf 里写的动作, 所以接收线程 recv 用独立的暂存块, 只有往
 *     g_rbuf 拷贝那一段才持锁。这样压缩永远不会和 recv 打架。 */
static unsigned char g_rbuf[RCV_BUF_BYTES];
static unsigned char g_stage[RCV_STAGE_BYTES];
static int g_rlen, g_rpos;

static pthread_mutex_t g_rlk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_rcv = PTHREAD_COND_INITIALIZER;   /* 有新数据到了 */
static pthread_cond_t  g_rsp = PTHREAD_COND_INITIALIZER;   /* 腾出空位了 */
static volatile int    g_rd_eof;      /* 对端关闭/真出错 */
static volatile int    g_rd_err;
static unsigned long   g_rd_bytes;

static unsigned char g_sps[128], g_pps[128];
static int g_sps_len, g_pps_len;
static int  g_nal_cnt, g_rd_cnt;
/* 丢弃积压后置 1: next_nal 只接受 SPS(7)/IDR(5), 其余跳过, 直到能重新同步。
 * 见 main 里 DROP_KEEP_BYTES 的说明。 */
static volatile int g_seek_idr;
static int g_ndrop;         /* 丢弃重新同步的次数 */

/* 主线程取一次数据: 把已消费的字节挤掉, 等接收线程送来新数据。
 * 返回新增字节数(>0); 0 = 对端真的关闭了(g_rd_eof)。
 *
 * ★ 2026-10-03 二次教训: 原来这里 5 秒没等到新数据就 return -1, 而所有调用点
 *   都把 "<=0" 当成流结束 —— 结果网络只要抖动一下(实测诊断日志里
 *   "rb_fill 等待超时(5s), g_rlen=247", 而同一时刻 eof=0 err=0, socket 好得
 *   很), 工具就打印 "流结束/断开", 每跑十几秒必死一次。用纯 python 客户端
 *   对照测过: 服务端连续 60 秒 988kB/s、26fps、零空档。
 *   所以现在**只有 g_rd_eof(对端关闭/真出错)才算流结束**, 网络停顿只会
 *   每 5 秒打一行日志然后继续等。 */
static int rb_fill(void)
{
	int before, n, stalled = 0;
	struct timespec ts;

	pthread_mutex_lock(&g_rlk);

	if (g_rpos > 0) {                       /* 先把已消费的字节挤出去 */
		memmove(g_rbuf, g_rbuf + g_rpos, (size_t)(g_rlen - g_rpos));
		g_rlen -= g_rpos;
		g_rpos = 0;
		pthread_cond_signal(&g_rsp);     /* 告诉接收线程: 有空位了 */
	}

	before = g_rlen;
	while (g_rlen == before && !g_rd_eof && !g_exit) {
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += 5;
		if (pthread_cond_timedwait(&g_rcv, &g_rlk, &ts) == ETIMEDOUT) {
			if (g_exit) break;
			stalled += 5;
			printf("  [net] 停顿 %ds 无数据(g_rlen=%d), 继续等\n",
			       stalled, g_rlen);
			fflush(stdout);
			dbg("rb_fill 停顿 %ds, g_rlen=%d (不当成断流)", stalled, g_rlen);
		}
	}
	n = g_rlen - before;
	pthread_mutex_unlock(&g_rlk);

	/* ★ 被 SIGTERM/SIGINT 叫停时必须能从等待里出来, 否则 killall 杀不掉 ——
	 *   流一停这里就永久阻塞, 实测表现为 longrun.sh 的 `wait` 永远收不了尾。
	 *   调用点把 <0 当"结束", 主循环再按 g_exit 区分是不是用户主动停的。 */
	if (g_exit && n <= 0) return -1;
	if (n > 0) { g_rd_cnt++; return n; }
	return 0;                                /* 能走到这里只可能是 g_rd_eof */
}

static void *rb_thread(void *arg)
{
	(void)arg;

	for (;;) {
		int n, off;

		/* ★ 不持锁 recv: 收数据可以把锁让给主线程去压缩 */
		n = recv(g_sock, g_stage, sizeof(g_stage), 0);

		pthread_mutex_lock(&g_rlk);
		if (n > 0) {
			g_rd_bytes += (unsigned long)n;
			g_rd_cnt++;                  /* 记一笔"收到的次数" */
			for (off = 0; off < n; ) {
				int space = (int)sizeof(g_rbuf) - g_rlen;
				int chunk;

				while (space <= 0 && !g_exit) {  /* 缓冲满: 等主线程压缩腾地方 */
					pthread_cond_wait(&g_rsp, &g_rlk);
					space = (int)sizeof(g_rbuf) - g_rlen;
				}
				if (space <= 0) break;   /* g_exit 且缓冲仍满: 剩下的丢掉, 直接收工 */
				chunk = (n - off < space) ? (n - off) : space;
				memcpy(g_rbuf + g_rlen, g_stage + off, (size_t)chunk);
				g_rlen += chunk;
				off += chunk;
			}
			pthread_cond_broadcast(&g_rcv);
			pthread_mutex_unlock(&g_rlk);
			if (g_exit) break;
			continue;
		}

		/* n == 0 = 对端正常关闭; n < 0 要区分"超时"和"真出错" */
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
			      errno == EINTR)) {
			int stop = g_exit;

			pthread_mutex_unlock(&g_rlk);
			if (stop) break;             /* 被 SIGTERM 叫停: 收工 */
			continue;                    /* 这一轮没数据而已, 接着等 */
		}
		g_rd_eof = 1;
		if (n < 0) g_rd_err = errno;
		pthread_cond_broadcast(&g_rcv);
		pthread_mutex_unlock(&g_rlk);
		break;
	}
	return NULL;
}

static int tcp_connect(const char *host, int port)
{
	struct hostent *he = gethostbyname(host);
	struct sockaddr_in sa;
	int fd, one = 1;
	struct timeval tv;

	if (!he) { printf("解析主机 %s 失败\n", host); return -1; }
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	memcpy(&sa.sin_addr, he->h_addr, (size_t)he->h_length);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		printf("连接 %s:%d 失败: %s\n", host, port, strerror(errno));
		close(fd);
		return -1;
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	/* 接收超时: 卡住时报错而不是死等, 便于定位。
	 * 由接收线程用, 超时(EAGAIN)只当"这轮没数据"重试, 不当作流结束。 */
	tv.tv_sec = 10; tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	/* 内核接收缓冲也放大一点, 给接收线程留点余量 */
	{
		int rcv = 1 << 20;   /* 1MB */
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
	}
	return fd;
}

/*
 * 把 [g_rpos, g_rlen) 挪到缓冲开头, 并把 g_rpos 归 0。
 * 读响应期间必须先调一次: 之后 g_rpos 一直是 0, rb_fill() 就不会再搬数据,
 * 扫描用的下标才稳定。
 */
static void rb_normalize(void)
{
	pthread_mutex_lock(&g_rlk);
	if (g_rpos > 0) {
		memmove(g_rbuf, g_rbuf + g_rpos, (size_t)(g_rlen - g_rpos));
		g_rlen -= g_rpos;
		g_rpos = 0;
		pthread_cond_signal(&g_rsp);
	}
	pthread_mutex_unlock(&g_rlk);
}

/* 从响应头(长度 hlen)里解析 Content-Length; 没有这个头就返回 0 */
static int hdr_content_length(const unsigned char *h, int hlen)
{
	static const char key[] = "content-length:";
	int i, k, n = 0;

	for (i = 0; i + (int)sizeof(key) - 1 <= hlen; i++) {
		for (k = 0; key[k]; k++) {
			unsigned char c = h[i + k];

			if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
			if (c != (unsigned char)key[k]) break;
		}
		if (!key[k]) {
			i += (int)sizeof(key) - 1;
			while (i < hlen && (h[i] == ' ' || h[i] == '\t')) i++;
			while (i < hlen && h[i] >= '0' && h[i] <= '9')
				n = n * 10 + (h[i++] - '0');
			return n;
		}
	}
	return 0;
}

/*
 * ★ 一个实测踩到的坑(2026-10-03): DESCRIBE 的响应头里带 Content-Length(本例 282),
 *   而 **SDP 正文不在同一个 TCP 段里**。如果只读到 "\r\n\r\n" 就收工, 那段 SDP 会
 *   变成"上一条响应的残留", 之后所有 \r\n\r\n / '$' 的定位全部错位 —— 表现就是
 *   PLAY 之后一个交织包都收不到。所以必须按 Content-Length 把正文读满。
 *
 * 返回: 响应字节数(头部+正文, 已写进 out 并补 0); 0 表示只有交织数据没有头部;
 *       <0 失败。
 */
static int rtsp_request(const char *method, const char *url, const char *extra,
			char *out, int outsz)
{
	char req[1024];
	int n, hlen, body, idx, is_dollar;

	n = snprintf(req, sizeof(req),
		     "%s %s RTSP/1.0\r\nCSeq: %d\r\n%s%s\r\n",
		     method, url, g_cseq++,
		     g_sess[0] ? g_sess : "", extra ? extra : "");
	printf("  >> %s %s CSeq=%d %s", method, url, g_cseq - 1,
	       g_sess[0] ? g_sess : "(no session)\n");
	if (send(g_sock, req, (size_t)n, 0) != n) return -1;

	rb_normalize();     /* g_rpos 归 0, 之后索引稳定(rb_fill 不会再搬数据) */

	/* 1) 等"响应头结束"或"交织数据开始", 谁先到算谁 */
	for (;;) {
		idx = -1;
		is_dollar = 0;
		{
			int i;
			for (i = 0; i < g_rlen; i++) {
				if (g_rbuf[i] == '$') { idx = i; is_dollar = 1; break; }
				if (i + 3 < g_rlen &&
				    g_rbuf[i] == '\r' && g_rbuf[i+1] == '\n' &&
				    g_rbuf[i+2] == '\r' && g_rbuf[i+3] == '\n') {
					idx = i; is_dollar = 0; break;
				}
			}
		}
		if (idx >= 0) break;

		if (rb_fill() <= 0) {
			int c = g_rlen < outsz - 1 ? g_rlen : outsz - 1;

			memcpy(out, g_rbuf, (size_t)c); out[c] = 0;
			g_rpos = g_rlen;
			return c;
		}
	}

	if (is_dollar) {
		/* 没有响应头(PLAY 常见: 服务端直接开始推交织帧) */
		int c = idx < outsz - 1 ? idx : outsz - 1;

		memcpy(out, g_rbuf, (size_t)c); out[c] = 0;
		g_rpos = idx;                    /* '$' 留给取帧逻辑 */
		return c;
	}

	/* 2) 头部结束在 idx+4; 按 Content-Length 把正文读满 */
	hlen = idx + 4;
	body = hdr_content_length(g_rbuf, hlen);
	while (g_rlen < hlen + body) {
		if (rb_fill() <= 0) break;
	}
	{
		int tot = hlen + body;
		int c;

		if (tot > g_rlen) tot = g_rlen;   /* 对端没给够, 有什么算什么 */
		c = tot < outsz - 1 ? tot : outsz - 1;
		memcpy(out, g_rbuf, (size_t)c); out[c] = 0;
		g_rpos = c;
		return c;
	}
}

/* 会话来保活: SDP 里服务端写 timeout=65, 到点不发东西就主动断。
 * 用 OPTIONS 喂一口即可; 它的响应是纯文本, 会被 next_interleaved 当噪声跳过。 */
static char g_url_full[512];

static void rtsp_keepalive(void)
{
	char req[512];
	int n;

	n = snprintf(req, sizeof(req),
		     "OPTIONS %s RTSP/1.0\r\nCSeq: %d\r\n%s\r\n",
		     g_url_full, g_cseq++, g_sess[0] ? g_sess : "");
	if (send(g_sock, req, (size_t)n, 0) != n)
		dbg("keepalive 发送失败: %s", strerror(errno));
	else
		dbg("keepalive 已发 CSeq=%d", g_cseq - 1);
}

static int rtsp_setup(const char *url)
{
	char resp[8192], extra[256];
	char base[512], alt[512], ctrl[512], track[512];
	int n, l, star, have_alt = 0;
	char *p, *e;

	/* 1) DESCRIBE 拿 SDP */
	n = rtsp_request("DESCRIBE", url, "Accept: application/sdp\r\n",
			 resp, sizeof(resp));
	if (n <= 0) { printf("DESCRIBE 失败\n"); return -1; }
	if (!strstr(resp, "200 OK")) { printf("DESCRIBE 非 200:\n%.200s\n", resp); return -1; }

	/* 2) 从 SDP 里找 control 和 sprop-parameter-sets
	 * ★ 实测(2026-10-03): 这个服务端(live555 派生)对 SETUP 到 **会话 URL**
	 *   (即 a=control:*) 才会真的出流; SETUP 到 /track1 也回 200 OK, 但之后
	 *   一个字节都不给。所以下面优先用会话 URL, 失败再退到 track 路径。 */
	star = 0;
	track[0] = 0;
	p = resp;
	while ((p = strstr(p, "a=control:")) != NULL) {
		p += strlen("a=control:");
		e = strstr(p, "\r\n");
		l = e ? (int)(e - p) : (int)strlen(p);
		if (l > (int)sizeof(ctrl) - 1) l = (int)sizeof(ctrl) - 1;
		memcpy(ctrl, p, (size_t)l); ctrl[l] = 0;
		if (ctrl[0] == '*') {
			star = 1;
		} else {
			memcpy(track, ctrl, (size_t)l + 1);
		}
	}
	if (!star && track[0] == 0) {
		printf("SDP 里没找到 a=control (SDP 收到 %d 字节)\n", n);
		return -1;
	}
	printf("  control: %s  track: %s\n", star ? "*" : "(无)", track[0] ? track : "(无)");

	p = strstr(resp, "sprop-parameter-sets=");
	if (p) {
		char tmp[512];
		char *comma;

		p += strlen("sprop-parameter-sets=");
		e = strstr(p, "\r\n");
		l = e ? (int)(e - p) : (int)strlen(p);
		if (l > (int)sizeof(tmp) - 1) l = (int)sizeof(tmp) - 1;
		memcpy(tmp, p, (size_t)l); tmp[l] = 0;
		printf("  sprop: %.80s...\n", tmp);

		/* 形如 "Z2QAH6...,aM4x..." 两段 base64: 自己解, 不依赖 ffmpeg */
		comma = strchr(tmp, ',');
		if (comma) {
			*comma = 0;
			g_sps_len = b64_decode(tmp, g_sps, (int)sizeof(g_sps));
			g_pps_len = b64_decode(comma + 1, g_pps, (int)sizeof(g_pps));
			if (g_sps_len <= 0) g_sps_len = 0;
			if (g_pps_len <= 0) g_pps_len = 0;
			printf("  SPS=%d 字节 PPS=%d 字节\n", g_sps_len, g_pps_len);
		}
	} else {
		printf("  (SDP 没有 sprop-parameter-sets, 只能靠码流里的带内 SPS/PPS)\n");
	}

	/* 候选 SETUP URL: 会话 URL 优先(实测才出流), 其次 track 路径 */
	snprintf(base, sizeof(base), "%s", url);
	if (track[0]) {
		if (!strncmp(track, "rtsp://", 7))
			snprintf(alt, sizeof(alt), "%s", track);
		else if (track[0] == '/')
			snprintf(alt, sizeof(alt), "rtsp://%s:%d%s", g_host, g_port, track);
		else
			snprintf(alt, sizeof(alt), "%s/%s", url, track);
		have_alt = strcmp(alt, base) != 0;
	}

	/* 3) SETUP: 要求 TCP interleaved */
	snprintf(extra, sizeof(extra),
		 "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
	n = rtsp_request("SETUP", base, extra, resp, sizeof(resp));
	if (n <= 0 || !strstr(resp, "200 OK")) {
		if (!have_alt) {
			printf("SETUP 失败(%s):\n%.300s\n", base, resp);
			return -1;
		}
		printf("  SETUP %s 没成, 改试 track 路径 %s\n", base, alt);
		n = rtsp_request("SETUP", alt, extra, resp, sizeof(resp));
		if (n <= 0 || !strstr(resp, "200 OK")) {
			printf("SETUP 全失败(track 路径也非 200):\n%.300s\n", resp);
			return -1;
		}
	}
	printf("  SETUP OK (%s)\n", strstr(resp, "interleaved") ? "interleaved" : "见 Transport");
	p = strstr(resp, "Session:");
	if (p) {
		char s2[160];
		p += 8;
		while (*p == ' ') p++;
		e = strstr(p, "\r\n");
		l = e ? (int)(e - p) : (int)strlen(p);
		if (l > (int)sizeof(g_sess) - 1) l = (int)sizeof(g_sess) - 1;
		memcpy(g_sess, p, (size_t)l); g_sess[l] = 0;
		{ char *sc = strchr(g_sess, ';'); if (sc) *sc = 0; }
		snprintf(s2, sizeof(s2), "Session: %s\r\n", g_sess);
		strncpy(g_sess, s2, sizeof(g_sess) - 1);
		g_sess[sizeof(g_sess) - 1] = 0;
		printf("  %s", g_sess);
	}

	/* 4) PLAY。响应可能为空(对端直接推交织数据), 也算成功。 */
	g_rlen = 0; g_rpos = 0;
	n = rtsp_request("PLAY", url, "Range: npt=0.000-\r\n", resp, sizeof(resp));
	if (n < 0) { printf("PLAY 无响应\n"); return -1; }
	if (n == 0) {
		printf("PLAY 响应为空(对端直接发了交织数据), g_rlen=%d\n", g_rlen);
		if (g_rlen <= 0) return -1;
	} else if (!strstr(resp, "200 OK")) {
		printf("PLAY 非 200, 响应前 200 字节:\n%.200s\n", resp);
		return -1;
	}
	printf("  PLAY OK%s\n", g_rlen ? "(响应里已带 RTP 数据)" : "");
	return 0;
}

/*
 * 取一个完整的 '$' 交织包(4 字节头 + plen 字节 RTP)。
 * 返回 0 成功, -1 流结束/出错。
 * 这里**不再**用手写状态机在一个大 for(;;) 里滚动 —— 那种写法在
 * "包跨缓冲区"时容易卡死(见 指南 3.12 的死循环记录)。改成:
 * 先保证缓冲里有 4+plen 个字节, 再一次性切出来。
 */
static int next_interleaved(const unsigned char **body, int *blen, int *chan)
{
	for (;;) {
		int i, plen;

		/* 1) 找到交织头首字节 '$'(中间可能夹着 RTSP 文本, 全丢掉) */
		for (i = g_rpos; i < g_rlen && g_rbuf[i] != '$'; i++) ;
		if (i >= g_rlen) {
			g_rpos = g_rlen;
			if (rb_fill() <= 0) return -1;
			continue;
		}
		g_rpos = i;

		/* 2) 凑齐 4 字节交织头 */
		while (g_rlen - g_rpos < 4)
			if (rb_fill() <= 0) return -1;

		plen = (g_rbuf[g_rpos + 2] << 8) | g_rbuf[g_rpos + 3];
		if (plen < 12) {                 /* 连 RTP 头都放不下, 丢弃这包头 */
			g_rpos++;
			continue;
		}

		/* 3) 凑齐整个 RTP 包 */
		while (g_rlen - g_rpos < 4 + plen)
			if (rb_fill() <= 0) return -1;

		*chan = g_rbuf[g_rpos + 1];
		*body = g_rbuf + g_rpos + 4;
		*blen = plen;
		g_rpos += 4 + plen;
		return 0;
	}
}

/* 从 RTP 负载里取一个完整 H.264 NAL(FU-A 分片在这里拼回)。
 * 返回 0 且 *nal/*nal_len 有效 = 拿到一个 NAL; 1 = 本包没产出 NAL; -1 = 流结束。 */
static int next_nal(unsigned char **nal, int *nal_len, int *marker)
{
	static unsigned char out[NAL_BUF_BYTES];
	static int fu_active, fu_len;
	static unsigned char fu_hdr;

	for (;;) {
		const unsigned char *pkt;
		int plen, chan, hl, j;

		if (next_interleaved(&pkt, &plen, &chan) != 0) return -1;
		if (chan != 0) continue;                 /* 只要视频通道 */
		/* ★ RTP 头长 = 固定 12 字节 + CSRC 数量*4。
		 *   踩过的坑(2026-10-03): 原先只写 (pkt[0] & 0x0F) * 4, 第一个字节
		 *   0x80 的低 4 位(CSRC 计数)是 0, 于是 hl 算成 0, 又被下面
		 *   "hl < 12" 挡掉 —— 结果**每个包都被 continue 丢掉**, 表现是一直
		 *   收包(NAL=0 recv=3669)却一个 NAL 都取不出来。 */
		hl = 12 + (pkt[0] & 0x0F) * 4;
		if (plen < hl + 1) continue;

		*marker = (pkt[1] >> 7) & 1;
		{
			const unsigned char *pay = pkt + hl;
			int paylen = plen - hl;
			int naltype = pay[0] & 0x1F;

			if (naltype >= 1 && naltype <= 23) {     /* 单个 NAL */
				/* 刚丢过积压: 只收 SPS(7)/IDR(5), 其余跳过, 直到重新同步 */
				if (g_seek_idr && naltype != 5 && naltype != 7) continue;
				g_seek_idr = 0;
				if (paylen > (int)sizeof(out)) continue;
				memcpy(out, pay, (size_t)paylen);
				*nal = out; *nal_len = paylen;
				g_nal_cnt++;
				fu_active = 0;
				return 0;
			}
			if (naltype == 28 && paylen > 2) {       /* FU-A 分片 */
				int start = pay[1] & 0x80, end = pay[1] & 0x40;
				int otype = pay[1] & 0x1F;
				unsigned char hdr = (unsigned char)((pay[0] & 0xE0) |
								    (pay[1] & 0x1F));

				if (g_seek_idr) {
					/* 等一个"分片 IDR/SPS 的开头"才认 */
					if (!start || (otype != 5 && otype != 7)) continue;
					g_seek_idr = 0;
				}
				if (start) { fu_len = 0; fu_hdr = hdr; fu_active = 1; }
				if (fu_active) {
					if (fu_len == 0)
						out[fu_len++] = fu_hdr;
					j = paylen - 2;
					if (fu_len + j <= (int)sizeof(out)) {
						memcpy(out + fu_len, pay + 2, (size_t)j);
						fu_len += j;
					} else {
						fu_active = 0;   /* 拼不下, 放弃这一 NAL */
						continue;
					}
				}
				if (end && fu_active) {
					fu_active = 0;
					*nal = out; *nal_len = fu_len;
					g_nal_cnt++;
					return 0;
				}
				continue;                        /* 继续拼下一片 */
			}
			/* SEI / SPS 等也照原样送出去(标 type 1..23 已覆盖 SPS=7/PPS=8);
			 * 其余(AUD=9 之外的 24..27/29..31)忽略 */
			if (naltype == 24 && paylen > 3) {       /* STAP-A: 打包的若干小 NAL */
				int sl = (pay[1] << 8) | pay[2];

				if (sl > 0 && sl <= (int)sizeof(out) && 3 + sl <= paylen) {
					memcpy(out, pay + 3, (size_t)sl);
					*nal = out; *nal_len = sl;
					g_nal_cnt++;
					return 0;
				}
			}
		}
		/* 其它: 忽略 */
	}
}

/* ================= YUV -> RGB565(大端) ================= */
static inline uint16_t yuv2rgb565(int y, int u, int v)
{
	int c = y - 16, d = u - 128, e = v - 128, r, g, b;

	if (c < 0) c = 0;
	r = (298 * c + 409 * e + 128) >> 8;
	g = (298 * c - 100 * d - 208 * e + 128) >> 8;
	b = (298 * c + 516 * d + 128) >> 8;
	if (r < 0) r = 0; else if (r > 255) r = 255;
	if (g < 0) g = 0; else if (g > 255) g = 255;
	if (b < 0) b = 0; else if (b > 255) b = 255;
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static int g_pix_planar;     /* 1 = PLANAR420(三个平面), 0 = NV21/NV12(交错) */
static int g_uv_swap;        /* NV2x 时: 1 = U 在前(NV12), 0 = V 在前(NV21) */
static int g_convs = 2;      /* 缩放时每个轴最多取几个源样本(1=最近邻, 2=2x2, 3=3x3) */
static int g_bench;          /* >0 = 同一帧连跑这么多次转换, 量完就退出 */
/* 转换内部两段耗时(只在 g_bench 打开时累加) */
static double g_t_copy, g_t_scale;
static double now_us(void);

/*
 * 把解码帧转成 RGB565(大端)写进 dst(320x240)。
 * 两步:
 *   1) 先按屏幕的 4:3 从源图**居中裁剪**出最大矩形 —— 摄像头是 16:9,
 *      屏是 4:3, 不裁边就必然变形; 裁完横向铺满、纵向铺满, 不变形。
 *   2) 再**盒式平均**缩放到 320x240(比最近邻干净得多)。为避免缩放比很大时
 *      把 CPU 吃满, 每个输出像素最多在源盒里取 3x3 个采样点。
 */
/* ---- cached 暂存区 ----
 * ★ 性能关键(2026-10-03 实测): VDEC 送出来的 VB 是 **non-cached** 映射。
 *   直接在它上面做 3x3 盒式平均 = 每个采样点一次独立总线事务, 320x240 要
 *   69 万个点 —— 实测 **43 ms/帧**(单帧最长 58ms), 直接把消费速率压到 ~20fps;
 *   源是 30fps, 积压越滚越大, 最后服务端 TCP 发送窗口填满被关连接。
 *   改成: 先用 memcpy(**宽读**, 一次 8 字节) 把裁剪区搬进本地 cached 缓冲,
 *   再在 cache 里缩放 —— 实测掉到几毫秒。 */
static unsigned char *g_cY, *g_cUV;
static size_t g_cYcap, g_cUVcap;

/* cnt(每像素采了几个源样本) -> 16.16 定点倒数, 用来替掉热循环里的整数除法。
 * ★ 2026-10-03 实测: Cortex-A53 的 SDIV/UDIV 是逐位迭代(~2 周期/位, 32 位
 *   要 ~60 周期), 三个 `acc/cnt` 加起来 ~180 周期/像素 —— 320x240 一共
 *   76800 个像素, 光这三条除法就吃掉 ~15ms/帧。换成乘倒数 + 右移 16 位,
 *   除法一次都不出现在循环里。实测 convs=1 从 33ms 掉到 9ms 以下。 */
static const unsigned int *inv_table(void)
{
	static unsigned int tab[1025];
	static int init;

	if (!init) {
		int i;

		tab[0] = 0;
		for (i = 1; i <= 1024; i++)
			tab[i] = (unsigned int)(65536u / (unsigned int)i);
		init = 1;
	}
	return tab;
}

static void frame_to_rgb565(const VIDEO_FRAME_S *f, uint8_t *dst)
{
	const int sw = (int)f->u32Width;
	const int sh = (int)f->u32Height;
	int cw, ch, cx, cy, oy, ox;
	int ys, cs1, cs2, cys, cwC;
	const uint8_t *Y, *P1, *P2;
	unsigned char *cY, *cU, *cV;
	const unsigned int *inv = inv_table();

	if (sw <= 0 || sh <= 0 || f->pu8VirAddr[0] == NULL) return;

	/* 目标 4:3 下能取到的最大居中矩形 */
	if ((long long)sh * FB_W / FB_H <= sw) {
		ch = sh; cw = (int)((long long)sh * FB_W / FB_H);
	} else {
		cw = sw; ch = (int)((long long)sw * FB_H / FB_W);
	}
	cx = (sw - cw) / 2;
	cy = (sh - ch) / 2;
	cx &= ~1; cy &= ~1;                  /* 色度是 2x2 下采样, 起点取偶 */

	Y  = f->pu8VirAddr[0];
	ys = (int)f->u32Stride[0];
	P1 = f->pu8VirAddr[1];               /* NV2x: UV 交错平面; PLANAR: U 平面 */
	P2 = f->pu8VirAddr[2];               /* PLANAR: V 平面; NV2x: 空 */
	cs1 = (int)f->u32Stride[1];
	cs2 = (int)f->u32Stride[2];

	/* --- 1) 准备 cached 暂存(尺寸随流变化, 按需扩容) --- */
	cys = cw;
	cwC = g_pix_planar ? (cw / 2) : cw;   /* 色度一行的字节数(交错时 UV 各半) */
	{
		size_t need_y  = (size_t)cys * (size_t)ch;
		size_t need_uv = (size_t)cwC * (size_t)(ch / 2) * (g_pix_planar ? 2u : 1u);

		if (g_cYcap < need_y) {
			free(g_cY);
			g_cY = (unsigned char *)malloc(need_y);
			g_cYcap = g_cY ? need_y : 0;
		}
		if (g_cUVcap < need_uv) {
			free(g_cUV);
			g_cUV = (unsigned char *)malloc(need_uv);
			g_cUVcap = g_cUV ? need_uv : 0;
		}
		if (!g_cY || !g_cUV) return;
	}
	cY = g_cY;
	cU = g_cUV;
	cV = g_cUV + (size_t)cwC * (size_t)(ch / 2);

	/* --- 2) 宽读把裁剪区搬进 cache --- */
	{
		double pa = g_bench ? now_us() : 0;

	memcpy(cY, Y + (size_t)cy * ys + cx, (size_t)cw);   /* 首行 */
	{
		int y;

		for (y = 1; y < ch; y++)
			memcpy(cY + (size_t)y * cys, Y + (size_t)(cy + y) * ys + cx,
			       (size_t)cw);
		if (P1 && P2 && g_pix_planar) {
			for (y = 0; y < ch / 2; y++) {
				memcpy(cU + (size_t)y * cwC,
				       P1 + (size_t)(cy / 2 + y) * cs1 + cx / 2,
				       (size_t)cwC);
				memcpy(cV + (size_t)y * cwC,
				       P2 + (size_t)(cy / 2 + y) * cs2 + cx / 2,
				       (size_t)cwC);
			}
		} else if (P1) {
			for (y = 0; y < ch / 2; y++)
				memcpy(cU + (size_t)y * cwC,
				       P1 + (size_t)(cy / 2 + y) * cs1 + cx,
				       (size_t)cwC);
		}
	}
	if (g_bench) g_t_copy += now_us() - pa;
	}

	/* --- 3) 在 cache 里做盒式平均缩放(坐标全部相对缓存区) --- */
	{
		double pa = g_bench ? now_us() : 0;

	for (oy = 0; oy < FB_H; oy++) {
		int sy0 = oy * ch / FB_H;
		int sy1 = (oy + 1) * ch / FB_H;
		int ystep, nys;
		uint8_t *dr = dst + (size_t)oy * FB_W * 2;

		if (sy1 <= sy0) sy1 = sy0 + 1;
		/* 把 (sy1-sy0) 压到最多 g_convs 个采样点。原来是 /g_convs,
		 * 整数除法会让 convs=2 在范围 3 上算出 step=1(等于没减), 所以
		 * 这里用向上取整。 */
		ystep = ((sy1 - sy0) + g_convs - 1) / g_convs;
		if (ystep < 1) ystep = 1;
		nys = (sy1 - sy0 + ystep - 1) / ystep;

		for (ox = 0; ox < FB_W; ox++) {
			/* ★ 用 32 位算: ox<=320、cw<=4096 ⇒ 乘积 < 2^23, 绝对安全。
			 *   写成 (long long) 会让 GCC 为"64 位除以常量 320"生成一整串
			 *   128 位乘法序列, 实测这是热循环里最大的固定开销。 */
			int sx0 = ox * cw / FB_W;
			int sx1 = (ox + 1) * cw / FB_W;
			int xstep, nxs, sx, sy, cnt;
			int acc_y = 0, acc_u = 0, acc_v = 0;
			unsigned int m;
			uint16_t c;

			if (sx1 <= sx0) sx1 = sx0 + 1;
			xstep = ((sx1 - sx0) + g_convs - 1) / g_convs;
			if (xstep < 1) xstep = 1;
			nxs = (sx1 - sx0 + xstep - 1) / xstep;
			cnt = nys * nxs;
			if (cnt < 1) cnt = 1;
			else if (cnt > 1024) cnt = 1024;
			m = inv[cnt];

			if (g_pix_planar) {
				for (sy = sy0; sy < sy1; sy += ystep) {
					const uint8_t *Yr = cY + (size_t)sy * cys;
					const uint8_t *C1 = cU + (size_t)(sy >> 1) * cwC;
					const uint8_t *C2 = cV + (size_t)(sy >> 1) * cwC;

					for (sx = sx0; sx < sx1; sx += xstep) {
						acc_y += Yr[sx];
						acc_u += C1[sx >> 1];
						acc_v += C2[sx >> 1];
					}
				}
			} else {
				for (sy = sy0; sy < sy1; sy += ystep) {
					const uint8_t *Yr = cY + (size_t)sy * cys;
					const uint8_t *C1 = cU + (size_t)(sy >> 1) * cwC;

					for (sx = sx0; sx < sx1; sx += xstep) {
						uint8_t a = C1[(sx >> 1) * 2 + 0];
						uint8_t bb = C1[(sx >> 1) * 2 + 1];

						acc_y += Yr[sx];
						if (g_uv_swap) { acc_u += a; acc_v += bb; }
						else           { acc_v += a; acc_u += bb; }
					}
				}
			}

			c = yuv2rgb565((acc_y * (int)m) >> 16,
				       (acc_u * (int)m) >> 16,
				       (acc_v * (int)m) >> 16);
			dr[ox * 2]     = (uint8_t)(c >> 8);      /* 大端: 高字节在前 */
			dr[ox * 2 + 1] = (uint8_t)(c & 0xFF);
		}
	}
	if (g_bench) g_t_scale += now_us() - pa;
	}
}

/* ================= base64(只用于 SDP 的 sprop-parameter-sets) ================= */
static int b64_val(int ch)
{
	if (ch >= 'A' && ch <= 'Z') return ch - 'A';
	if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
	if (ch >= '0' && ch <= '9') return ch - '0' + 52;
	if (ch == '+') return 62;
	if (ch == '/') return 63;
	return -1;
}

static int b64_decode(const char *in, unsigned char *out, int outsz)
{
	int acc = 0, nbits = 0, n = 0;

	for (; *in; in++) {
		int v = b64_val((unsigned char)*in);

		if (v < 0) break;                /* '=' 或非法字符 -> 结束 */
		acc = (acc << 6) | v;
		nbits += 6;
		if (nbits >= 8) {
			nbits -= 8;
			if (n >= outsz) return -1;
			out[n++] = (unsigned char)((acc >> nbits) & 0xFF);
		}
	}
	return n;
}

/* ================= VDEC(硬件解码) ================= */
static int g_dec_w = DEC_DEF_W, g_dec_h = DEC_DEF_H;

/* 单调微秒时钟: 给主循环做阶段计时埋点(定位"消费追不上源"卡在哪一段) */
static double now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static int vdec_init(void)
{
	VDEC_CHN_ATTR_S attr;
	VDEC_CHN_PARAM_S param;
	CVI_S32 ret;
	uint32_t i;

	/* 照抄 sample_vdec 的默认路径(H.264 + VB_SOURCE_COMMON):
	 * 这种组合下 VB 由模块自己管, 只需要把 SYS 干净地初始化一遍。 */
	CVI_SYS_Exit();
	ret = CVI_SYS_Init();
	if (ret != CVI_SUCCESS) {
		printf("CVI_SYS_Init 失败 0x%x\n", ret);
		dbg("CVI_SYS_Init 失败 0x%x", ret);
		return -1;
	}

	memset(&attr, 0, sizeof(attr));
	attr.enType          = PT_H264;
	attr.enMode          = VIDEO_MODE_FRAME;   /* 一次送一整帧(我们把 AU 拼好了) */
	attr.u32PicWidth     = (uint32_t)g_dec_w;  /* 允许的**最大**尺寸 */
	attr.u32PicHeight    = (uint32_t)g_dec_h;
	/* 流缓冲: 按样本的算法对齐到 0x4000 */
	i = (uint32_t)g_dec_w * (uint32_t)g_dec_h;
	i = (i + 0x3FFFu) & ~0x3FFFu;
	attr.u32StreamBufSize = i;
	attr.u32FrameBufCnt   = VDEC_FRM_CNT;

	ret = CVI_VDEC_CreateChn(VDEC_CHN_ID, &attr);
	if (ret != CVI_SUCCESS) {
		printf("CVI_VDEC_CreateChn 失败 0x%x\n", ret);
		dbg("CVI_VDEC_CreateChn 失败 0x%x", ret);
		return -1;
	}

	/* 拿默认参数改两处: 输出像素格式 + 显示帧数 */
	memset(&param, 0, sizeof(param));
	ret = CVI_VDEC_GetChnParam(VDEC_CHN_ID, &param);
	if (ret != CVI_SUCCESS)
		dbg("CVI_VDEC_GetChnParam 失败 0x%x(继续)", ret);
	param.enType = PT_H264;
	param.enPixelFormat = g_pix_planar ? PIXEL_FORMAT_YUV_PLANAR_420
					   : (g_uv_swap ? PIXEL_FORMAT_NV12
							: PIXEL_FORMAT_NV21);
	param.u32DisplayFrameNum = 2;
	ret = CVI_VDEC_SetChnParam(VDEC_CHN_ID, &param);
	if (ret != CVI_SUCCESS) {
		printf("CVI_VDEC_SetChnParam 失败 0x%x\n", ret);
		dbg("CVI_VDEC_SetChnParam 失败 0x%x", ret);
		return -1;
	}

	ret = CVI_VDEC_StartRecvStream(VDEC_CHN_ID);
	if (ret != CVI_SUCCESS) {
		printf("CVI_VDEC_StartRecvStream 失败 0x%x\n", ret);
		dbg("CVI_VDEC_StartRecvStream 失败 0x%x", ret);
		return -1;
	}

	printf("VDEC 已就绪: H.264 %dx%d(max) 输出 %s\n", g_dec_w, g_dec_h,
	       g_pix_planar ? "PLANAR420" : (g_uv_swap ? "NV12" : "NV21"));
	dbg("VDEC 就绪: %dx%d pixfmt=%d", g_dec_w, g_dec_h, (int)param.enPixelFormat);
	return 0;
}

static void vdec_deinit(void)
{
	CVI_VDEC_StopRecvStream(VDEC_CHN_ID);
	CVI_VDEC_ResetChn(VDEC_CHN_ID);
	CVI_VDEC_DestroyChn(VDEC_CHN_ID);
	CVI_SYS_Exit();
}

/* 送一帧; BUSY 时重试(参考样本的 SendAgainFlag 写法) */
static int vdec_send(const uint8_t *buf, int len, uint64_t pts)
{
	VDEC_STREAM_S st;
	int k;

	memset(&st, 0, sizeof(st));
	st.pu8Addr     = (uint8_t *)buf;
	st.u32Len      = (uint32_t)len;
	st.u64PTS      = pts;
	st.bEndOfFrame = CVI_TRUE;

	for (k = 0; k < 200; k++) {
		CVI_S32 ret = CVI_VDEC_SendStream(VDEC_CHN_ID, &st, 1000);

		if (ret == CVI_SUCCESS) return 0;
		if (ret == CVI_ERR_VDEC_BUSY) { usleep(2000); continue; }
		dbg("CVI_VDEC_SendStream 失败 0x%x (len=%d)", ret, len);
		return -1;
	}
	dbg("CVI_VDEC_SendStream 一直 BUSY, 丢帧 (len=%d)", len);
	return -1;
}

/* ================= main ================= */
int main(int argc, char **argv)
{
	const char *url = "rtsp://127.0.1.1/h264";
	const char *ppm_path = NULL;
	const char *raw_path = NULL;
	int do_dump = 0, do_raw = 0, max_frames = 0, n = 0, i;
	time_t t0 = 0;
	uint64_t pts = 0;
	static uint8_t rgb[FB_BYTES] __attribute__((aligned(64)));
	static unsigned char au[AU_BUF_BYTES];
	int au_len = 0;

	g_pix_planar = 1;    /* 默认 PLANAR420 —— 与 sample_vdec 的默认一致, 最少歧义 */

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dump") && i + 1 < argc) { do_dump = 1; ppm_path = argv[++i]; }
		else if (!strcmp(argv[i], "--raw") && i + 1 < argc) { do_raw = 1; raw_path = argv[++i]; }
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			int w = 0, h = 0;
			if (sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
				g_dec_w = w; g_dec_h = h;
			} else { printf("--size 格式应为 1920x1088\n"); return 1; }
		}
		else if (!strcmp(argv[i], "--pixfmt") && i + 1 < argc) {
			const char *m = argv[++i];
			if (!strcmp(m, "planar")) { g_pix_planar = 1; g_uv_swap = 0; }
			else if (!strcmp(m, "nv21")) { g_pix_planar = 0; g_uv_swap = 0; }
			else if (!strcmp(m, "nv12")) { g_pix_planar = 0; g_uv_swap = 1; }
			else { printf("--pixfmt 只能是 planar|nv21|nv12\n"); return 1; }
		}
		else if (!strcmp(argv[i], "--swapuv")) { g_uv_swap = !g_uv_swap; }
		else if (!strcmp(argv[i], "--convs") && i + 1 < argc) {
			int v = atoi(argv[++i]);
			if (v >= 1 && v <= 8) g_convs = v;
		}
		else if (!strcmp(argv[i], "--convbench") && i + 1 < argc) {
			g_bench = atoi(argv[++i]);
		}
		else if (argv[i][0] != '-') url = argv[i];
		else {
			printf("用法: %s [rtsp_url] [--dump out.ppm] [--raw out.264] [--frames N]\n"
			       "        [--size WxH] [--pixfmt planar|nv21|nv12] [--swapuv]\n"
			       "        [--convs 1..8] [--convbench N]\n", argv[0]);
			return 1;
		}
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	printf("=== rtsp2lcd(硬件解码): %s -> ST7789 %dx%d ===\n", url, FB_W, FB_H);
	dbg("启动: url=%s 目标 %dx%d 解码上限 %dx%d pixfmt=%s",
	    url, FB_W, FB_H, g_dec_w, g_dec_h,
	    g_pix_planar ? "planar" : (g_uv_swap ? "nv12" : "nv21"));

	/* --- 解析 rtsp url --- */
	{
		char host[128], path[256];
		int port = 554;
		const char *p = url, *slash, *colon;

		if (strncmp(p, "rtsp://", 7)) { printf("只支持 rtsp://\n"); return 1; }
		p += 7;
		slash = strchr(p, '/');
		colon = strchr(p, ':');
		if (colon && (!slash || colon < slash)) {
			int hl = (int)(colon - p);
			if (hl > (int)sizeof(host) - 1) hl = (int)sizeof(host) - 1;
			memcpy(host, p, (size_t)hl); host[hl] = 0;
			port = atoi(colon + 1);
			snprintf(path, sizeof(path), "%s", slash ? slash : "/");
		} else {
			int hl = slash ? (int)(slash - p) : (int)strlen(p);
			if (hl > (int)sizeof(host) - 1) hl = (int)sizeof(host) - 1;
			memcpy(host, p, (size_t)hl); host[hl] = 0;
			snprintf(path, sizeof(path), "%s", slash ? slash : "/");
		}
		printf("主机=%s 端口=%d 路径=%s\n", host, port, path);
		snprintf(g_host, sizeof(g_host), "%s", host);
		g_port = port;

		g_sock = tcp_connect(host, port);
		if (g_sock < 0) return 1;
		dbg("TCP 已连接 %s:%d", host, port);

		/* ★ 立刻把接收线程拉起来: 之后 socket 里的东西永远有人在搬,
		 *   主线程解码/缩放再慢也不会让服务端堵住(见 RCV_BUF_BYTES 注释)。 */
		{
			pthread_t th;

			if (pthread_create(&th, NULL, rb_thread, NULL) != 0) {
				printf("启动接收线程失败: %s\n", strerror(errno));
				return 1;
			}
			pthread_detach(th);
			dbg("接收线程已启动");
		}

		{
			char full[512];

			snprintf(full, sizeof(full), "rtsp://%s:%d%s", host, port, path);
			snprintf(g_url_full, sizeof(g_url_full), "%s", full);
			if (rtsp_setup(full) != 0) {
				printf("RTSP 建链失败\n");
				dbg("RTSP 建链失败");
				return 1;
			}
			dbg("RTSP 建链成功, g_rlen=%d SPS=%d PPS=%d", g_rlen, g_sps_len, g_pps_len);
		}
	}

	/* ---- --raw: 只存 Annex-B 码流, 不解码、不碰屏 ----
	 * 放这里是为了"先验证 RTSP 通路"时不用管解码器死活。 */
	if (do_raw) {
		FILE *fp = fopen(raw_path, "wb");
		int written = 0;

		if (!fp) { perror("fopen raw"); return 1; }
		if (g_sps_len > 0) {
			unsigned char sc[4] = {0, 0, 0, 1};
			fwrite(sc, 1, 4, fp); fwrite(g_sps, 1, (size_t)g_sps_len, fp);
			fwrite(sc, 1, 4, fp); fwrite(g_pps, 1, (size_t)g_pps_len, fp);
		}
		while (!g_exit && (!max_frames || written < max_frames)) {
			unsigned char *nal; int nal_len, marker;
			unsigned char sc[4] = {0, 0, 0, 1};
			static time_t last = 0;
			time_t now = time(NULL);

			if (now != last) { last = now; fflush(stdout); }
			if (next_nal(&nal, &nal_len, &marker) != 0) {
				printf("流结束(已收 %d 个 NAL)\n", written);
				break;
			}
			fwrite(sc, 1, 4, fp);
			fwrite(nal, 1, (size_t)nal_len, fp);
			written++;
			if ((written % 30) == 0) {
				fflush(fp);
				printf("[raw] 已写 %d 个 NAL\n", written);
			}
		}
		fflush(fp); fclose(fp);
		printf("已保存 %s (%d 个 NAL)\n", raw_path, written);
		dbg("raw 完成: %d 个 NAL, recv %d 次", written, g_rd_cnt);
		return 0;
	}

	/* ---- 硬件解码器 ---- */
	if (vdec_init() != 0) return 1;

	/* 把 SDP 里拿到的 SPS/PPS 先喂一遍(带内的也在码流里, 重复无害) */
	if (g_sps_len > 0 && g_pps_len > 0) {
		static unsigned char hdr[300];
		int hl = 0;

		hdr[hl++] = 0; hdr[hl++] = 0; hdr[hl++] = 0; hdr[hl++] = 1;
		memcpy(hdr + hl, g_sps, (size_t)g_sps_len); hl += g_sps_len;
		hdr[hl++] = 0; hdr[hl++] = 0; hdr[hl++] = 0; hdr[hl++] = 1;
		memcpy(hdr + hl, g_pps, (size_t)g_pps_len); hl += g_pps_len;
		vdec_send(hdr, hl, 0);
	}

	if (!do_dump) shm_map();

	/* 主循环: 收 NAL -> 攒成一帧 -> 送解码 -> 抽干已解出的帧 -> 转 RGB565 -> 上屏 */
	{
		time_t last_ka = time(NULL);
		/* 阶段计时: t_* 累计微秒(30 帧一个窗口), mx_* 单次最长微秒 */
		double t_nal = 0, t_send = 0, t_get = 0, t_inv = 0, t_conv = 0, t_shm = 0;
		double mx_nal = 0, mx_send = 0, mx_get = 0, mx_inv = 0, mx_conv = 0, mx_shm = 0;
		double win0 = now_us();
		unsigned long rx0 = 0;

		while (!g_exit) {
		unsigned char *nal;
		int nal_len, marker;

		/* 每 20 秒喂一次会话(服务端 timeout=65) */
		if (time(NULL) - last_ka >= 20) {
			rtsp_keepalive();
			last_ka = time(NULL);
		}

		/* 积压过多就丢弃并跳到下一个 IDR 重新同步(见 DROP_KEEP_BYTES) */
		if (!g_seek_idr) {
			int bl;

			pthread_mutex_lock(&g_rlk);
			bl = g_rlen - g_rpos;
			pthread_mutex_unlock(&g_rlk);
			if (bl > DROP_KEEP_BYTES) {
				pthread_mutex_lock(&g_rlk);
				g_rpos = g_rlen;        /* 当前缓冲里的一律不要了 */
				pthread_mutex_unlock(&g_rlk);
				g_seek_idr = 1;
				au_len = 0;
				g_ndrop++;
				printf("  [drop] 积压 %dKB 超过 %dKB, 丢弃后等下一个 IDR (第 %d 次)\n",
				       bl / 1024, DROP_KEEP_BYTES / 1024, g_ndrop);
				fflush(stdout);
				dbg("drop: 积压 %dKB(第 %d 次), 从下一个 IDR 重新同步",
				    bl / 1024, g_ndrop);
			}
		}

		{
			double a = now_us();
			int r = next_nal(&nal, &nal_len, &marker);
			double d = now_us() - a;

			t_nal += d;
			if (d > mx_nal) mx_nal = d;
			if (r != 0) {
				if (g_exit) {
					printf("收到退出信号, 正常收工(已取 %d 个 NAL)\n",
					       g_nal_cnt);
					dbg("退出信号: NAL=%d recv=%d 共收 %lu 字节",
					    g_nal_cnt, g_rd_cnt, g_rd_bytes);
				} else {
					printf("流结束/断开(已取 %d 个 NAL)\n", g_nal_cnt);
					dbg("流结束/断开: NAL=%d recv=%d eof=%d err=%d 共收 %lu 字节",
					    g_nal_cnt, g_rd_cnt, g_rd_eof, g_rd_err, g_rd_bytes);
				}
				break;
			}
		}

		/* 拼访问单元: 每个 NAL 前加 4 字节起始码 */
		if (au_len + nal_len + 4 <= (int)sizeof(au)) {
			au[au_len++] = 0; au[au_len++] = 0;
			au[au_len++] = 0; au[au_len++] = 1;
			memcpy(au + au_len, nal, (size_t)nal_len);
			au_len += nal_len;
		} else {
			dbg("!! 访问单元超过 %d 字节, 放弃", AU_BUF_BYTES);
			au_len = 0;
		}
		if (!marker) continue;                 /* 这一帧还没收完 */
		if (au_len <= 0) continue;

		pts += 33333;
		{
			double a = now_us();

			vdec_send(au, au_len, pts);
			t_send += now_us() - a;
			if (now_us() - a > mx_send) mx_send = now_us() - a;
		}
		au_len = 0;

		/* 抽干已解出的帧 */
		for (;;) {
			VIDEO_FRAME_INFO_S fi;
			double ga;

			ga = now_us();
			if (CVI_VDEC_GetFrame(VDEC_CHN_ID, &fi, 0) != CVI_SUCCESS) break;
			t_get += now_us() - ga;
			if (now_us() - ga > mx_get) mx_get = now_us() - ga;

			/* VB 是 non-cached 映射, 依样本惯例仍做一次 cache 失效 */
			{
				double ia = now_us();
				int k;

				for (k = 0; k < 3; k++)
					if (fi.stVFrame.pu8VirAddr[k])
						CVI_SYS_IonInvalidateCache(
							fi.stVFrame.u64PhyAddr[k],
							fi.stVFrame.pu8VirAddr[k],
							fi.stVFrame.u32Stride[k] *
							fi.stVFrame.u32Height);
				t_inv += now_us() - ia;
				if (now_us() - ia > mx_inv) mx_inv = now_us() - ia;
			}

			if (n == 0)
				printf("首帧: %ux%u stride=%u/%u/%u fmt=%d\n",
				       fi.stVFrame.u32Width, fi.stVFrame.u32Height,
				       fi.stVFrame.u32Stride[0], fi.stVFrame.u32Stride[1],
				       fi.stVFrame.u32Stride[2], (int)fi.stVFrame.enPixelFormat);

			{
				double a = now_us();

				frame_to_rgb565(&fi.stVFrame, rgb);
				t_conv += now_us() - a;
				if (now_us() - a > mx_conv) mx_conv = now_us() - a;
			}

			/* 转换基准: 拿着同一帧连跑 N 次, 直接量出单帧转换成本(与流无关) */
			if (g_bench > 0) {
				double a = now_us();
				int b;

				g_t_copy = g_t_scale = 0;
				for (b = 0; b < g_bench; b++)
					frame_to_rgb565(&fi.stVFrame, rgb);
				a = now_us() - a;
				printf("convbench: convs=%d  %d 次共 %.1fms  单次 %.2fms"
				       "  (拷贝 %.2fms + 缩放 %.2fms)\n",
				       g_convs, g_bench, a / 1000.0, a / 1000.0 / g_bench,
				       g_t_copy / 1000.0 / g_bench,
				       g_t_scale / 1000.0 / g_bench);
				fflush(stdout);
				dbg("convbench convs=%d n=%d total=%.1fms per=%.3fms copy=%.3f scale=%.3f",
				    g_convs, g_bench, a / 1000.0, a / 1000.0 / g_bench,
				    g_t_copy / 1000.0 / g_bench, g_t_scale / 1000.0 / g_bench);
				CVI_VDEC_ReleaseFrame(VDEC_CHN_ID, &fi);
				goto done;
			}

			CVI_VDEC_ReleaseFrame(VDEC_CHN_ID, &fi);

			if (do_dump) {
				FILE *fp = fopen(ppm_path, "wb");

				if (fp) {
					fprintf(fp, "P6\n%d %d\n255\n", FB_W, FB_H);
					for (i = 0; i < FB_H; i++) {
						int x;
						for (x = 0; x < FB_W; x++) {
							uint16_t c = (uint16_t)((rgb[i*FB_W*2 + x*2] << 8) |
										rgb[i*FB_W*2 + x*2 + 1]);
							uint8_t p[3];
							p[0] = (uint8_t)(((c >> 11) & 0x1F) << 3);
							p[1] = (uint8_t)(((c >> 5) & 0x3F) << 2);
							p[2] = (uint8_t)((c & 0x1F) << 3);
							fwrite(p, 1, 3, fp);
						}
					}
					fclose(fp);
					printf("已保存 %s (%dx%d)\n", ppm_path, FB_W, FB_H);
				} else perror("fopen");
				n++;
				goto done;
			}

			{
				double a = now_us();

				memcpy(g_shm->fb, rgb, FB_BYTES);
				g_shm->ctrl.x0 = 0;      g_shm->ctrl.y0 = 0;
				g_shm->ctrl.x1 = FB_W - 1; g_shm->ctrl.y1 = FB_H - 1;
				g_shm->ctrl.flags = 0;
				__sync_synchronize();
				g_shm->ctrl.seq++;
				send_cmd(LCD_CMD_CAM, 0);
				t_shm += now_us() - a;
				if (now_us() - a > mx_shm) mx_shm = now_us() - a;
			}

			if (n == 0) t0 = time(NULL);
			n++;
			if ((n % 30) == 0) {
				time_t dt = time(NULL) - t0;
				int backlog;
				double win = (now_us() - win0) / 1000.0;   /* ms */
				unsigned long rx = g_rd_bytes;

				pthread_mutex_lock(&g_rlk);
				backlog = g_rlen - g_rpos;
				pthread_mutex_unlock(&g_rlk);

				printf("已送 %d 帧 %.1f fps ack=%u err=%u 滞留=%dKB 收=%lukB/s\n",
				       n, dt ? (double)n / (double)dt : 0.0,
				       g_shm->stat.ack_seq, g_shm->stat.err,
				       backlog / 1024,
				       (unsigned long)((rx - rx0) / (win > 1.0 ? win / 1000.0 : 1.0) / 1024));
				printf("   占时ms/30帧: nal=%.0f vdecSend=%.0f getFrame=%.0f inval=%.0f conv=%.0f shm=%.0f\n",
				       t_nal / 1000.0, t_send / 1000.0, t_get / 1000.0,
				       t_inv / 1000.0, t_conv / 1000.0, t_shm / 1000.0);
				printf("   单次最长ms : nal=%.1f vdecSend=%.1f getFrame=%.1f inval=%.1f conv=%.1f shm=%.1f\n",
				       mx_nal / 1000.0, mx_send / 1000.0, mx_get / 1000.0,
				       mx_inv / 1000.0, mx_conv / 1000.0, mx_shm / 1000.0);
				fflush(stdout);
				dbg("已送 %d 帧, seq=%u ack=%u 滞留=%dKB 窗口=%.0fms 收=%lukB",
				    n, g_shm->ctrl.seq, g_shm->stat.ack_seq, backlog / 1024, win,
				    (unsigned long)((rx - rx0) / (win > 1.0 ? win / 1000.0 : 1.0) / 1024));

				t_nal = t_send = t_get = t_inv = t_conv = t_shm = 0;
				mx_nal = mx_send = mx_get = mx_inv = mx_conv = mx_shm = 0;
				win0 = now_us();
				rx0 = rx;
				t0 = time(NULL);
			}
			if (max_frames && n >= max_frames) goto done;
		}
		}
	}

done:
	printf("退出, 共 %d 帧 (丢弃重同步 %d 次)\n", n, g_ndrop);
	dbg("退出: %d 帧, NAL=%d recv=%d", n, g_nal_cnt, g_rd_cnt);
	vdec_deinit();
	if (g_sock >= 0) close(g_sock);
	return 0;
}
