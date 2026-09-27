/* lcd_sender.c —— 大核(Linux/A53)侧: 通过 cmdqu + 共享内存驱动小核刷屏
 *
 * 架构: 大核做识别/渲染 -> 把像素写进共享内存 -> 用 mailbox 发一条"刷屏"命令
 *       -> 小核(C906L)收到命令后读共享内存, 经 SPI2 推给 ST7789。
 *
 * 两个通道:
 *   1) cmdqu(mailbox): /dev/cvi-rtos-cmdqu + ioctl(RTOS_CMDQU_SEND)
 *   2) 共享内存:       /dev/mem mmap @ LCD_SHM_PHYS (见 lcd_shm.h, O_SYNC => 非缓存映射)
 *
 * 用法:
 *   lcd_sender stat              打印 ctrl/stat
 *   lcd_sender init              写 magic/version/背光, 发 LCD_CMD_INIT
 *   lcd_sender ping              发 LCD_CMD_PING
 *   lcd_sender bl 0|1            背光开关
 *   lcd_sender fill <rgb565十六进制>   整屏纯色(走共享内存 color 字段)
 *   lcd_sender bars              画 8 条彩条, 整屏刷新一次
 *   lcd_sender anim [帧数]       移动白条, 只做局部刷新
 *   lcd_sender mon [间隔ms]      采样 /proc/stat, 用 mailbox(LCD_CMD_MON) 把大核
 *                                CPU 占用率发给小核, 小核 OSD 直接显示;
 *                                回执里带回小核自己的占用率与帧率 (默认 1000ms)
 *   lcd_sender rtos <file>       免烧 fip 热更小核固件: 让小核跳进常驻跳板 ->
 *                                原地覆盖应用区 -> 跳回 _start。详见 do_rtos()。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <signal.h>
#include <time.h>

#include "lcd_shm.h"

/* ---------------- cmdqu 用户态接口 ---------------- */
#define CMDQU_DEV            "/dev/cvi-rtos-cmdqu"
#define RTOS_CMDQU_SEND      _IOW('r', 1, unsigned long)
#define RTOS_CMDQU_SEND_WAIT _IOW('r', 4, unsigned long)

struct valid_t {
	unsigned char linux_valid;
	unsigned char rtos_valid;
} __attribute__((packed));

typedef union resv_t {
	struct valid_t valid;
	unsigned short mstime;      /* 0 = 不阻塞 */
} resv_t;

typedef struct cmdqu_t {
	unsigned char ip_id;
	unsigned char cmd_id : 7;
	unsigned char block  : 1;
	resv_t        resv;
	unsigned int  param_ptr;
} __attribute__((packed)) __attribute__((aligned(8))) cmdqu_t;

#define IP_SYSTEM            6

/* ---------------- 全局状态 ---------------- */
static volatile struct lcd_shm *g_shm;
static int      g_cmd_fd = -1;
static uint32_t g_seq;

static void die(const char *msg)
{
	perror(msg);
	exit(1);
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void shm_setup(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);   /* O_SYNC -> 非缓存映射 */
	if (fd < 0)
		die("open /dev/mem");
	void *p = mmap(NULL, LCD_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fd, (off_t)LCD_SHM_PHYS);
	if (p == MAP_FAILED)
		die("mmap /dev/mem");
	close(fd);
	g_shm = (volatile struct lcd_shm *)p;

	/* 关键: 每次运行都是新进程, g_seq 必须从共享内存里"已经"的编号续上,
	 * 否则 ++g_seq 会退回到小核早已 ack 过的旧值, 导致"等 ack"立即返回 0ms。 */
	g_seq = g_shm->ctrl.seq > g_shm->stat.ack_seq ? g_shm->ctrl.seq
						      : g_shm->stat.ack_seq;
}

static void cmd_setup(void)
{
	g_cmd_fd = open(CMDQU_DEV, O_RDWR);
	if (g_cmd_fd < 0)
		die("open " CMDQU_DEV);
}

static int cmd_send(int cmd_id, unsigned int param)
{
	cmdqu_t c;

	memset(&c, 0, sizeof(c));
	c.ip_id     = IP_SYSTEM;
	c.cmd_id    = cmd_id;
	c.block     = 0;
	c.resv.mstime = 0;              /* 不阻塞, 回执看共享内存 stat */
	c.param_ptr = param;

	if (ioctl(g_cmd_fd, RTOS_CMDQU_SEND, &c) < 0) {
		perror("ioctl RTOS_CMDQU_SEND");
		return -1;
	}
	return 0;
}

/* 提交一帧: seq++, 发 FLUSH, 等小核把它做完 */
static int flush_and_wait(void)
{
	uint32_t want = ++g_seq;
	double t0;

	g_shm->ctrl.seq = want;
	__sync_synchronize();

	if (cmd_send(LCD_CMD_FLUSH, 0) != 0)
		return -1;

	t0 = now_ms();
	while (g_shm->stat.ack_seq != want) {
		if (now_ms() - t0 > 2000.0) {
			fprintf(stderr, "等 ack 超时 (want=%u got=%u state=%u err=%u)\n",
				want, g_shm->stat.ack_seq,
				g_shm->stat.state, g_shm->stat.err);
			return -1;
		}
	}
	return (int)(now_ms() - t0);
}

/* ---------------- 渲染辅助 ---------------- */
static inline void put_px(uint32_t x, uint32_t y, uint16_t c)
{
	uint32_t o = ((uint32_t)y * LCD_W + x) * 2u;
	g_shm->fb[o]     = (uint8_t)(c >> 8);   /* RGB565 大端 */
	g_shm->fb[o + 1] = (uint8_t)(c & 0xFF);
}

/* 背景渐变: 横向 R 渐变, 纵向 G 渐变 */
static uint16_t bg_color(uint32_t x, uint32_t y)
{
	uint32_t r = x * 31 / (LCD_W - 1);
	uint32_t g = y * 63 / (LCD_H - 1);
	return (uint16_t)((r << 11) | (g << 5) | 0x10);
}

static void draw_bg_rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
	uint32_t x, y;

	for (y = y0; y <= y1; y++)
		for (x = x0; x <= x1; x++)
			put_px(x, y, bg_color(x, y));
}

/* ---------------- 子命令 ---------------- */
static void do_stat(void)
{
	printf("ctrl: magic=0x%08X version=%u seq=%u bl=%u color=0x%04X flags=%u\n",
	       g_shm->ctrl.magic, g_shm->ctrl.version, g_shm->ctrl.seq,
	       g_shm->ctrl.bl, g_shm->ctrl.color & 0xFFFF, g_shm->ctrl.flags);
	printf("stat: state=%u ack_seq=%u frame_cnt=%u last_ticks=%u last_bytes=%u err=%u\n",
	       g_shm->stat.state, g_shm->stat.ack_seq, g_shm->stat.frame_cnt,
	       g_shm->stat.last_ticks, g_shm->stat.last_bytes, g_shm->stat.err);
}

static void do_init(void)
{
	g_shm->ctrl.magic   = LCD_SHM_MAGIC;
	g_shm->ctrl.version = LCD_SHM_VERSION;
	g_shm->ctrl.bl      = 1;
	g_shm->ctrl.flags   = 0;
	__sync_synchronize();

	if (cmd_send(LCD_CMD_INIT, 0) != 0)
		return;

	usleep(200 * 1000);
	do_stat();
	/* INIT 后小核会把 stat.state 写成 LCD_STATE_READY(1) */
	printf("=> INIT %s\n",
	       g_shm->stat.state == LCD_STATE_READY ? "已生效(小核读到 magic 并回写了状态)"
						   : "未见效(state 不是 READY)");
}

static void do_fill(uint16_t color)
{
	uint32_t want = ++g_seq;

	g_shm->ctrl.color = color;
	g_shm->ctrl.seq   = want;
	__sync_synchronize();

	if (cmd_send(LCD_CMD_FILL, 0) != 0)
		return;

	double t0 = now_ms();
	while (g_shm->stat.ack_seq != want) {
		if (now_ms() - t0 > 2000.0) {
			fprintf(stderr, "FILL 等 ack 超时 (want=%u got=%u)\n",
				want, g_shm->stat.ack_seq);
			return;
		}
	}
	printf("FILL 0x%04X 完成, 耗时 %.1f ms, frame_cnt=%u\n",
	       color, now_ms() - t0, g_shm->stat.frame_cnt);
}

static void do_bars(void)
{
	static const uint16_t bars[8] = {
		0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xFF1F, 0xF800, 0x001F, 0x0000
	};
	uint32_t x, y;
	int t0;

	for (y = 0; y < LCD_H; y++)
		for (x = 0; x < LCD_W; x++)
			put_px(x, y, bars[x * 8 / LCD_W]);

	g_shm->ctrl.flags = 1;                  /* 整屏 */
	__sync_synchronize();

	t0 = flush_and_wait();
	if (t0 >= 0)
		printf("bars 整屏刷新完成, 耗时 %d ms, frame_cnt=%u last_bytes=%u\n",
		       t0, g_shm->stat.frame_cnt, g_shm->stat.last_bytes);
}

static void do_anim(int frames)
{
	const uint32_t BW = 12;                 /* 白条宽 */
	uint32_t x, y, xold, xnew;
	int k;

	printf("先铺背景(整屏一次)...\n");
	draw_bg_rect(0, 0, LCD_W - 1, LCD_H - 1);
	g_shm->ctrl.flags = 1;
	flush_and_wait();

	g_shm->ctrl.flags = 0;                  /* 之后都走局刷 */
	xold = 0;
	for (k = 0; k < frames; k++) {
		xnew = (uint32_t)((k * 6) % (LCD_W - BW - 8)) + 4;

		uint32_t lo = xold < xnew ? xold : xnew;
		uint32_t hi = (xold + BW - 1 > xnew + BW - 1 ? xold + BW - 1 : xnew + BW - 1);
		int ms;

		/* 恢复 [lo, hi] 条带的背景, 再画上新位置的白条 */
		draw_bg_rect(lo, 0, hi, LCD_H - 1);
		for (y = 0; y < LCD_H; y++)
			for (x = xnew; x < xnew + BW; x++)
				put_px(x, y, 0xFFFF);

		g_shm->ctrl.x0 = lo;
		g_shm->ctrl.y0 = 0;
		g_shm->ctrl.x1 = hi;
		g_shm->ctrl.y1 = LCD_H - 1;
		__sync_synchronize();

		ms = flush_and_wait();
		if (ms < 0)
			return;
		printf("\r帧 %3d  局刷矩形 x=[%3u,%3u] 宽 %3u  往返 %d ms  ack=%u",
		       k, lo, hi, hi - lo + 1, ms, g_shm->stat.ack_seq);
		fflush(stdout);

		xold = xnew;
	}
	printf("\nanim 结束, 共 %u 帧, 共享内存 frame_cnt=%u\n", frames, g_shm->stat.frame_cnt);
}

/* ---------------- 大核占用率采样 (给小核屏幕用) ---------------- */

/* 读 /proc/stat 第一行, 出 busy 与 total。单位是内核的 jiffies 计数。 */
static int read_cpu_stat(unsigned long long *busy, unsigned long long *total)
{
	unsigned long long u, n, s, idle, iowait, irq, softirq, steal;
	char line[256];
	FILE *f = fopen("/proc/stat", "r");
	int got;

	if (!f)
		return -1;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);

	if (strncmp(line, "cpu ", 4) != 0)
		return -1;
	got = sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
		     &u, &n, &s, &idle, &iowait, &irq, &softirq, &steal);
	if (got < 4)
		return -1;

	*total = u + n + s + idle + iowait + irq + softirq + steal;
	*busy  = *total - idle - iowait;
	return 0;
}

/* 采样值用一条 LCD_CMD_MON 发给小核, 顺便收小核的回执。
 * 回执写在 cmdqu_t.param_ptr 里: 小核占用率 + 帧率×10 (见 lcd_shm.h)。
 * 返回 0 表示成功, *reply 里是小核的回执。 */
static int mon_send(uint32_t pct, uint32_t hb, uint32_t *reply)
{
	cmdqu_t c;

	memset(&c, 0, sizeof(c));
	c.ip_id       = IP_SYSTEM;
	c.cmd_id      = LCD_CMD_MON;
	c.block       = 0;
	c.resv.mstime = 200;            /* 最多等小核 200ms */
	c.param_ptr   = LCD_MON_TX(pct, hb);

	if (ioctl(g_cmd_fd, RTOS_CMDQU_SEND_WAIT, &c) < 0) {
		perror("ioctl RTOS_CMDQU_SEND_WAIT");
		return -1;
	}
	*reply = c.param_ptr;
	return 0;
}

/* 收尾: 发一条 heartbeat=0 的 MON, 小核就把大核那一栏显示成 n/a。
 * (以前靠清共享内存的 magic 实现, 现在没有共享内存了, 语义不变。
 * 启动时也发一次, 免得小核那边还留着上一轮进程的旧数。) */
static void mon_clear(void)
{
	uint32_t rx;

	mon_send(0, 0, &rx);
}

static void mon_on_signal(int sig)
{
	(void)sig;
	mon_clear();
	_exit(0);
}

static void do_mon(int interval_ms)
{
	unsigned long long pb = 0, pt = 0;
	uint32_t hb = 0;
	int first = 1;

	signal(SIGINT,  mon_on_signal);
	signal(SIGTERM, mon_on_signal);
	signal(SIGHUP,  mon_on_signal);

	printf("mon: 每 %d ms 采一次 /proc/stat, 用 mailbox(LCD_CMD_MON) 发给小核 (Ctrl-C 退出)\n",
	       interval_ms);

	mon_clear();            /* 先把上一轮残留清掉, 小核立刻回到 n/a */

	for (;;) {
		unsigned long long b, t;
		uint32_t pct = 0, rx = 0;

		if (read_cpu_stat(&b, &t) == 0) {
			if (!first && t > pt) {
				pct = (uint32_t)(((b - pb) * 100ull) / (t - pt));
				if (pct > 100u)
					pct = 100u;
			}
			pb = b;
			pt = t;
			first = 0;
		}

		if (mon_send(pct, ++hb, &rx) == 0)
			printf("\r大核 CPU %3u%%   hb %3u   |   小核 %3u%%   %u.%u FPS   ",
			       pct, hb, (unsigned)LCD_MON_RX_PCT(rx),
			       (unsigned)(LCD_MON_RX_FPS10(rx) / 10),
			       (unsigned)(LCD_MON_RX_FPS10(rx) % 10));
		else
			printf("\r大核 CPU %3u%%   hb %3u   |   小核回执失败   ", pct, hb);
		fflush(stdout);

		usleep((useconds_t)interval_ms * 1000);
	}
}

/* 逐个变体试 ioctl, 找出驱动真正接受的组合 */
static void raw_send(unsigned long req, int ip_id, int cmd_id, int block,
		     unsigned short mstime, unsigned param, const char *tag)
{
	cmdqu_t c;
	int r;

	memset(&c, 0, sizeof(c));
	c.ip_id      = (unsigned char)ip_id;
	c.cmd_id     = cmd_id;
	c.block      = block;
	c.resv.mstime = mstime;
	c.param_ptr  = param;

	errno = 0;
	r = ioctl(g_cmd_fd, req, &c);
	printf("  %-36s ret=%d errno=%d(%s)\n", tag, r, errno,
	       r < 0 ? strerror(errno) : "ok");
}

static void do_scan(void)
{
	int ip;
	char t[64];

	printf("--- A: RTOS_CMDQU_SEND, ip_id = 0..7, cmd=0x74 ---\n");
	for (ip = 0; ip < 8; ip++) {
		snprintf(t, sizeof(t), "SEND ip_id=%d", ip);
		raw_send(RTOS_CMDQU_SEND, ip, LCD_CMD_PING, 0, 0, 0, t);
	}
	printf("--- B: 不同的 mstime/block ---\n");
	raw_send(RTOS_CMDQU_SEND, IP_SYSTEM, LCD_CMD_PING, 1, 0xFFFF, 0, "SEND ip=6 block=1 mstime=0xffff");
	raw_send(RTOS_CMDQU_SEND, IP_SYSTEM, LCD_CMD_PING, 0, 0xFFFF, 0, "SEND ip=6 mstime=0xffff");
	raw_send(RTOS_CMDQU_SEND, IP_SYSTEM, LCD_CMD_PING, 0, 100, 0, "SEND ip=6 mstime=100");
	printf("--- C: SEND_WAIT(_IOW r,4) ---\n");
	raw_send(RTOS_CMDQU_SEND_WAIT, IP_SYSTEM, LCD_CMD_PING, 1, 100, 0, "SEND_WAIT ip=6 mstime=100");
	usleep(200 * 1000);
	do_stat();
}

/* ---------------- 小核固件热更 (常驻跳板 + 原地覆盖) ----------------
 *
 * 为什么能免烧: 小核固件自己被链接在应用区 0x8FE00000 静态地址上, 而在一段
 * 固定高地址 0x8FF90000 里常驻着一小块"跳板"(hotjump.S)。热更时先让运行中的
 * 小核跳进跳板(它不再访问应用区), 大核用 /dev/mem 把新固件的 .bin 原样铺回
 * 0x8FE00000, 跳板再做一次全 cache 失效并跳回 _start —— 相当于软件触发了一次
 * 冷启动。全程不动 fip.bin, 所以失败了最多重启一次板子就能回到旧固件。
 *
 * 握手地址/魔数见 lcd_shm.h 的 LCD_RELOAD_* 与 hotjump.S, 三处必须一致。 */
#define RTOS_APP_PHYS   0x8FE00000UL
#define RTOS_APP_LIMIT  0x8FF90000UL            /* 之上是常驻跳板, 不许碰 */
#define RTOS_APP_MAX    (RTOS_APP_LIMIT - RTOS_APP_PHYS)
/* cold-boot 用的 cvirtos.bin 是"平铺"镜像: 前面是应用区, 末尾 0x8FF90000 起
 * 还带着那份常驻跳板。热更只允许写应用区, 所以超出的这部分要丢掉。 */
#define RTOS_HJ_SLOT    0x1000UL                /* 跳板预留槽位(实际只用 0x900) */

static uint8_t *load_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long n;

	if (!f) {
		perror("打开固件");
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	n = ftell(f);
	rewind(f);
	if (n <= 0) {
		fprintf(stderr, "固件为空或不可读: %s\n", path);
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)n);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "读固件失败\n");
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return buf;
}

static int do_rtos(const char *path)
{
	size_t len = 0, i, bad;
	uint8_t *buf;
	volatile uint8_t *q;
	void *p;
	int fd, rc = 1;
	double t0;

	buf = load_file(path, &len);
	if (!buf)
		return 1;

	/* 尾部那份常驻跳板不属于应用区, 不能一起写回去(会覆盖正在执行的跳板) */
	if (len > (size_t)RTOS_APP_MAX) {
		size_t extra = len - (size_t)RTOS_APP_MAX;
		if (extra > (size_t)RTOS_HJ_SLOT) {
			fprintf(stderr, "固件 %zu 字节, 去掉跳板槽位后仍超应用区上限 %lu 字节\n",
				len, (unsigned long)RTOS_APP_MAX);
			free(buf);
			return 1;
		}
		printf("固件 %s: %zu 字节 = 应用区 %lu + 常驻跳板 %zu (跳板不重写)\n",
		       path, len, (unsigned long)RTOS_APP_MAX, extra);
		len = (size_t)RTOS_APP_MAX;
	} else {
		printf("固件 %s: %zu 字节 (应用区 %lu 字节上限, 余 %lu)\n",
		       path, len, (unsigned long)RTOS_APP_MAX,
		       (unsigned long)(RTOS_APP_MAX - len));
	}

	/* 1) 清握手, 再发命令让小核跳进常驻跳板 */
	g_shm->ctrl.reload     = 0;
	g_shm->stat.reload_ack = 0;
	__sync_synchronize();

	if (cmd_send(LCD_CMD_RTOS_RELOAD, 0) != 0)
		goto out;

	t0 = now_ms();
	while (g_shm->stat.reload_ack != 1u) {
		if (now_ms() - t0 > 3000.0) {
			fprintf(stderr, "小核 3s 内未进入跳板 (reload_ack=%u state=%u)\n",
				g_shm->stat.reload_ack, g_shm->stat.state);
			goto out;
		}
	}
	printf("小核已停在跳板 (%.1f ms), 开始往 0x%08lX 写 %zu 字节\n",
	       now_ms() - t0, (unsigned long)RTOS_APP_PHYS, len);

	/* 2) 覆盖应用区。O_SYNC => 非缓存映射, 写完即到 DRAM */
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("open /dev/mem");
		goto out;
	}
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		 (off_t)RTOS_APP_PHYS);
	if (p == MAP_FAILED) {
		perror("mmap 0x8FE00000");
		close(fd);
		goto out;
	}
	memcpy(p, buf, len);
	msync(p, len, MS_SYNC);

	/* 3) 读回校验。此刻小核还停在跳板, 不符就不放行 —— 重启即可回到旧固件 */
	q = (volatile uint8_t *)p;
	for (bad = 0, i = 0; i < len; i++)
		if (q[i] != buf[i])
			bad++;
	munmap(p, len);
	close(fd);
	if (bad) {
		fprintf(stderr, "!! 写入后读回有 %zu 字节不符, 已中止 (未放行起跳)\n", bad);
		goto out;
	}
	printf("/dev/mem 写入 + 读回校验通过\n");

	/* 4) 放行: 跳板全 cache 失效后跳回 _start */
	g_shm->ctrl.reload = (uint32_t)LCD_RELOAD_MAGIC;
	__sync_synchronize();
	printf("已置 ctrl.reload=0x%08lX, 等新固件启动...\n",
	       (unsigned long)LCD_RELOAD_MAGIC);

	/* 5) 新固件从 _start 跑一遍, 起来后不会自己碰共享内存; 由大核反复发
	 *    LCD_CMD_INIT 直到它回写 state。INIT 是幂等的, 重发无害。 */
	g_shm->ctrl.magic   = LCD_SHM_MAGIC;
	g_shm->ctrl.version = LCD_SHM_VERSION;
	g_shm->ctrl.bl      = 1;
	g_shm->ctrl.flags   = 0;
	__sync_synchronize();

	usleep(500 * 1000);
	t0 = now_ms();
	for (;;) {
		uint32_t st;
		cmd_send(LCD_CMD_INIT, 0);
		usleep(200 * 1000);
		st = g_shm->stat.state;
		if (st == LCD_STATE_READY || st == LCD_STATE_BUSY) {
			printf("热更完成: 新固件已运行 (state=%u, %.1f ms)\n",
			       st, now_ms() - t0);
			rc = 0;
			break;
		}
		if (now_ms() - t0 > 8000.0) {
			fprintf(stderr, "!! 新固件 8s 内未起来 (state=%u err=%u); "
					"固件应仍有问题, 重启板子即回旧固件\n",
				st, g_shm->stat.err);
			break;
		}
	}
	do_stat();

out:
	free(buf);
	return rc;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "stat";

	shm_setup();

	if (!strcmp(cmd, "stat")) {
		do_stat();
		return 0;
	}

	cmd_setup();

	if (!strcmp(cmd, "mon")) {          /* 走 mailbox, 不再写共享内存 */
		do_mon(argc > 2 ? atoi(argv[2]) : 1000);
		return 0;
	}

	if (!strcmp(cmd, "init")) {
		do_init();
	} else if (!strcmp(cmd, "scan")) {
		do_scan();
	} else if (!strcmp(cmd, "ping")) {
		cmd_send(LCD_CMD_PING, 0);
		usleep(200 * 1000);
		printf("PING 已发出\n");
		do_stat();
	} else if (!strcmp(cmd, "bl")) {
		int on = argc > 2 ? atoi(argv[2]) : 1;
		g_shm->ctrl.bl = (uint32_t)on;
		__sync_synchronize();
		cmd_send(LCD_CMD_BL, (unsigned int)(on ? 1 : 0));
		printf("背光 -> %d\n", on);
	} else if (!strcmp(cmd, "fill")) {
		uint16_t c = argc > 2 ? (uint16_t)strtoul(argv[2], NULL, 16) : 0xF800;
		do_fill(c);
	} else if (!strcmp(cmd, "bars")) {
		do_bars();
	} else if (!strcmp(cmd, "anim")) {
		do_anim(argc > 2 ? atoi(argv[2]) : 60);
	} else if (!strcmp(cmd, "rtos")) {
		if (argc < 3) {
			fprintf(stderr, "用法: lcd_sender rtos <cvirtos.bin>\n");
			return 2;
		}
		{
			int rc = do_rtos(argv[2]);
			close(g_cmd_fd);
			return rc;
		}
	} else {
		fprintf(stderr, "未知子命令: %s\n", cmd);
		return 2;
	}

	close(g_cmd_fd);
	return 0;
}
