#include "spi.h"
#include "mmio.h"

#include "arch_helpers.h"       /* flush_dcache_range / inv_dcache_range */
#include "arch_time.h"          /* GetSysTime() = rdtime, 25MHz -> us */

/* DesignWare APB SSI 寄存器 (相对 CVI_SPI2_BASE) */
#define SPI_CTRL0       (CVI_SPI2_BASE + 0x00)
#define SPI_CTRL1       (CVI_SPI2_BASE + 0x04)
#define SPI_SSIENR      (CVI_SPI2_BASE + 0x08)
#define SPI_SER         (CVI_SPI2_BASE + 0x10)
#define SPI_BAUDR       (CVI_SPI2_BASE + 0x14)
#define SPI_TXFTLR      (CVI_SPI2_BASE + 0x18)
#define SPI_RXFTLR      (CVI_SPI2_BASE + 0x1C)
#define SPI_TXFLR       (CVI_SPI2_BASE + 0x20)
#define SPI_RXFLR       (CVI_SPI2_BASE + 0x24)
#define SPI_SR          (CVI_SPI2_BASE + 0x28)
#define SPI_DMACR       (CVI_SPI2_BASE + 0x4C)
#define SPI_DMATDLR     (CVI_SPI2_BASE + 0x50)
#define SPI_DMARDLR     (CVI_SPI2_BASE + 0x54)
#define SPI_DR          (CVI_SPI2_BASE + 0x60)

/* DMACR 位: 和 Linux spi-dw.h 的 SPI_DMA_RDMAE/SPI_DMA_TDMAE 一致 */
#define SPI_DMA_RDMAE   (1u << 0)
#define SPI_DMA_TDMAE   (1u << 1)

/* CTRL0 的 TMOD 字段在 bit[9:8]: 0=收发, 1=只发, 2=只收, 3=EEPROM 读。
 * 本屏只写(MISO 悬空), DMA 送显走"只发"模式 —— 这样 RX FIFO 不会进数据,
 * 也就不会因为 RX FIFO 满而把移位器卡住(收发模式下必须同步把 RX 取走)。 */
#define SPI_CTRL0_TMOD_MASK     0x00000300u
#define SPI_CTRL0_TMOD_TXONLY   0x00000100u

/* SR 位 */
#define SPI_SR_BUSY     (1u << 0)
#define SPI_SR_TFNF     (1u << 1)   /* TX FIFO 不满, 可以继续写 */
#define SPI_SR_RFNE     (1u << 3)   /* RX FIFO 非空 */

#define SPI_FIFO_DEPTH  32
#define SPI_POLL_GUARD  1000000u

/* CTRL0: DFS=7 (8 位帧), FRF=0 (Motorola SPI), TMOD=0 (收发),
 * SCPOL=SCPH=0 (SPI 模式 0)。
 * 这个值和 Linux 侧 dw-spi 给 spidev 编程后实测出来的 CTRL0=0x7 完全一致。 */
#define SPI_CTRL0_MODE0_8BIT    0x00000007u

volatile unsigned long g_spi2_bytes;

/* 大核用 devmem 读这 8 个量看 SPI2-DMA 通路的状态(地址用 nm 查 g_spi2_probe):
 *   [0] 'SDM2' 魔数        [1] DMA 送显次数
 *   [2] DMA 送出的字节数    [3] 最近一次 sysdma_transfer 返回码(0=成功)
 *   [4] 探测到的 FIFO 深度  [5] DMA 失败回退 PIO 的次数
 *   [6] DMATDLR 实际写入值  [7] 突发长度对应的 MSIZE 编码 */
#define SPI2_PROBE_MAGIC    0x53444D32UL        /* 'SDM2' */
volatile unsigned long g_spi2_probe[8];

/* ---- 【临时诊断】异步送显各段耗时, 见 spi.h 的索引说明 -------------- */
#define DMA2_PROBE_MAGIC    0x444D4132UL        /* 'DMA2' */
volatile unsigned long g_dma2_probe[28];

/* 最近一次 sysdma_start 的规模, 供超时快照判断"停在第几个 LLI / 共几个" */
static uint32_t s_last_len, s_last_n;

/* 微秒时间戳(25MHz mtime), 只用于做区间差值 */
static inline uint32_t dma2_us(void)
{
	return (uint32_t)(GetSysTime() / 25u);
}

/* 探测出来的 FIFO 深度, 以及由它推出的 DMA 突发长度(CTL.MSIZE 编码: 0=1项 1=4 2=8 3=16) */
static uint32_t s_spi2_fifo_len = 32u;
static uint32_t s_spi2_msize    = 3u;

/* 当前 CTRL0 里 TMOD 的取值; 0xFFFFFFFF = 还没编程过 */
static uint32_t s_spi2_tmod = 0xFFFFFFFFu;

/* 把"突发多少项"换算成 CTL.MSIZE 编码(1->0, 4->1, 8->2, 16->3)。
 * 和 Linux convert_burst() 的 fls(n)-2 等价, 只是不想为此链 libc。 */
static uint32_t spi2_msize_from_len(uint32_t n)
{
	uint32_t k = 0;

	if (n <= 1u)
		return 0;
	while (n > 1u) {
		n >>= 1;
		k++;
	}
	k -= 1u;
	return (k > 3u) ? 3u : k;
}

void spi2_init(uint32_t baudr)
{
	uint32_t i;

	/* 必须先关 SSIENR 才能改其它寄存器 */
	mmio_write_32(SPI_SSIENR, 0);
	mmio_write_32(SPI_SER, 0);
	mmio_write_32(SPI_CTRL0, SPI_CTRL0_MODE0_8BIT);
	mmio_write_32(SPI_CTRL1, 0);
	mmio_write_32(SPI_BAUDR, baudr & 0xffff);
	mmio_write_32(SPI_RXFTLR, 0);

	/* FIFO 深度用 Linux spi-dw 同样的办法探出来: TXFTLR 的可写范围是 0..深度-1,
	 * 写"深度"会被硬件截到"深度-1", 所以回读值第一次不等于写入值时就是深度。
	 * DMA 的突发放长和 DMATDLR 水位都要用这个值, 不能靠猜(SSIENR=0 时才能做)。 */
	for (i = 1; i < 256u; i++) {
		mmio_write_32(SPI_TXFTLR, i);
		if (i != mmio_read_32(SPI_TXFTLR))
			break;
	}
	s_spi2_fifo_len = (i > 1u) ? i : 32u;
	s_spi2_msize    = spi2_msize_from_len(s_spi2_fifo_len / 2u);

	mmio_write_32(SPI_TXFTLR, 0);

	/* 清掉可能残留的 FIFO 数据 */
	for (i = 0; i < SPI_FIFO_DEPTH && (mmio_read_32(SPI_RXFLR) > 0); i++)
		(void)mmio_read_32(SPI_DR);

	mmio_write_32(SPI_SSIENR, 1);
	g_spi2_bytes = 0;
	s_spi2_tmod  = 0;                       /* CTRL0 刚写的就是 TMOD=0(收发) */

	g_spi2_probe[0] = SPI2_PROBE_MAGIC;
	g_spi2_probe[4] = s_spi2_fifo_len;
	g_spi2_probe[7] = s_spi2_msize;
	flush_dcache_range((uintptr_t)g_spi2_probe, sizeof(g_spi2_probe));
}

/* 切 CTRL0 的 TMOD(PIO 用收发 0, DMA 送显用只发 1)。
 * CTRL0 必须在 SSIENR=0 时改, 所以这里会短暂关一次 SSI; 只在模式真的变了才做,
 * 稳态(一直走 DMA 或一直走 PIO)是零开销的。 */
static void spi2_tmod_set(uint32_t tmod)
{
	if (s_spi2_tmod == tmod)
		return;

	mmio_write_32(SPI_SSIENR, 0);
	mmio_write_32(SPI_CTRL0, (SPI_CTRL0_MODE0_8BIT & ~SPI_CTRL0_TMOD_MASK) | tmod);
	mmio_write_32(SPI_SER, 1);
	mmio_write_32(SPI_SSIENR, 1);
	s_spi2_tmod = tmod;
}

void spi2_cs(int assert)
{
	mmio_write_32(SPI_SER, assert ? 1u : 0u);
}

void spi2_write(const uint8_t *buf, uint32_t len)
{
	uint32_t tx_cnt = 0;                    /* 已写进 DR 的字节数 */
	uint32_t rx_cnt = 0;                    /* 已从 DR 取走的字节数 */
	uint32_t guard = SPI_POLL_GUARD;
	uint32_t t0 = dma2_us(), t1;            /* 【诊断】 */
	uint32_t t2;                            /* 【诊断】 */

	/* PIO 必须走"收发"模式: 每发 1 字节要能从 RX FIFO 取走 1 个。
	 * DMA 送显会把 CTRL0 切成"只发", 这里切回来。 */
	spi2_tmod_set(0);

	/* 小核固件在 FSBL 阶段就起来了, 而 Linux 的 dw-spi 驱动稍后才 probe
	 * spi2@041A0000, probe 过程中会把 SER(片选) 清成 0。片选一旦被清掉,
	 * 之后写进 DR 的字节只会堆在 TX FIFO 里发不出去(实测整屏耗时从 ~55ms
	 * 暴涨到 5834 tick, TXFLR 卡在 0x10)。
	 * Linux 只会在这一个 slave 上 probe 一次, 且没有任何进程用 /dev/spidev0,
	 * 所以每次传输前重断言 SER 就能自动从被抢状态恢复。 */
	mmio_write_32(SPI_SER, 1);

	/* 为什么要这么写:
	 * 原来的实现是"发 1 字节 -> 等它进 RX FIFO -> 取走 -> 再发下一字节",
	 * 于是 TX FIFO 里永远只有 1 个字节在飞, SPI 时钟在字节之间完全空闲,
	 * 每字节耗时 = SPI 移位时间 + CPU 走一圈循环的时间(实测约 30.7% 带宽)。
	 *
	 * 改成"TX 有空位就灌, RX 有数据就取": 每轮只读一次 SR, 两个动作都能做就
	 * 都做。于是 TX FIFO 一直被填到满(32 深), SPI 连续移位不再等 CPU,
	 * 每字节耗时降到 max(SPI 时间, CPU 一圈时间), 而不是两者相加。
	 *
	 * TMOD=0(收发) 下每发出 1 字节 RX FIFO 就进 1 个, 所以必须同步取走;
	 * 但 tx_cnt - rx_cnt 天然不会超过 TX FIFO 深度 32, 而 RX FIFO 也是 32 深,
	 * 不会溢出(RX FIFO 真满时 SSI 自己会停, 不丢数据)。 */
	while ((tx_cnt < len) || (rx_cnt < tx_cnt)) {
		uint32_t sr = mmio_read_32(SPI_SR);
		int did = 0;

		/* TX FIFO 未满 -> 补一个字节进去 */
		if ((tx_cnt < len) && (sr & SPI_SR_TFNF)) {
			mmio_write_32(SPI_DR, buf[tx_cnt]);
			tx_cnt++;
			did = 1;
		}

		/* RX FIFO 非空 -> 收走一个, 给后面腾位置 */
		if ((rx_cnt < tx_cnt) && (sr & SPI_SR_RFNE)) {
			(void)mmio_read_32(SPI_DR);
			rx_cnt++;
			did = 1;
		}

		if (did)
			guard = SPI_POLL_GUARD;     /* 有进展就重置看门狗 */
		else if (--guard == 0)
			break;                      /* SER 被抢等异常, 不要死等 */
	}

	g_spi2_bytes += tx_cnt;
	t1 = dma2_us();                 /* 【诊断】循环结束 */

	/* 等最后一个字节真正移出去 */
	guard = SPI_POLL_GUARD;
	while (mmio_read_32(SPI_SR) & SPI_SR_BUSY) {
		if (--guard == 0)
			break;
	}

	t2 = dma2_us();                 /* 【诊断】 */
	g_dma2_probe[9]++;
	g_dma2_probe[10] += t1 - t0;    /* PIO 灌数据段 */
	g_dma2_probe[11] += t2 - t1;    /* PIO 收尾 BUSY 段 */
}

uint32_t spi2_read_reg(uint32_t off)
{
	return mmio_read_32(CVI_SPI2_BASE + off);
}

/* ==========================================================================
 * sysDMA (DesignWare AXI DMAC, compatible "snps,dmac-bm", 基址 0x04330000)
 *
 * 放在 spi.c 里而不是新建文件: 小核构建树是按目录 GLOB 源文件的
 * (driver/spi/CMakeLists.txt: file(GLOB _SOURCES "src/*.c")), 新增 .c 不会
 * 自动进构建, 得重新 configure —— 之前踩过"改了源码固件却没变"的坑, 所以
 * 通用 DMA 引擎直接写在已有的 spi.c 里。
 *
 * 用途: 给 SPI2 的 TX 挂 DMA。PIO 逐字节轮询既吃 CPU, 又只能和 LVGL 渲染
 * 串行; 挂上 DMA 之后 CPU 在传输期间是空闲的, 才能做"渲染/送显重叠"。
 *
 * 大核侧同一个控制器由 drivers/dma/cvitek/cvitek-dma.c 驱动: 它 reset 过全局
 * 寄存器、dma0chan0..3 是 in_use, 4..7 空闲。所以小核借 **7 号通道**, 并且
 * 只碰本通道自己的寄存器 + clk 门控, 全程不动全局 RESET/CFG。
 *
 * 寄存器偏移/位段/LLI 结构全部对照 cvitek-dma.h:
 *   struct dw_dma_regs / struct dw_dma_chan_regs 字段都是 u64, 偏移按 8 递增;
 *   LLI 是 struct dw_lli (64 字节, 硬件把 LLP 低 6 位当无效 -> 必须 64 字节对齐)。
 * ========================================================================== */

#define SYSDMA_BASE        0x04330000UL

/* 公共寄存器 */
#define DMAC_ID            0x000
#define DMAC_COMPVER       0x008
#define DMAC_CH_EN         0x018

/* 通道寄存器: 第 n 个通道基址 = 0x100 + n*0x100。
 * 注意通道寄存器里 CTL 在 0x18、CFG 在 0x20、LLP 在 0x28 —— 和 LLI 内部的
 * 字段顺序(llp=0x18, ctl=0x20)不一样, 别混。 */
#define CH_OFF(n)          (0x100u + (unsigned)(n) * 0x100u)
#define CH_BLOCK_TS        0x010
#define CH_CFG             0x020
#define CH_LLP             0x028
#define CH_INTSTATUS_EN    0x080
#define CH_INTSTATUS       0x088
#define CH_INTCLEAR        0x098

/* clk 门控: clk 控制器基址 0x03002000, REG_CLK_EN_1=+0x004, clk_sdma_axi=bit1。
 * 大核由 dw_dmac 驱动 clk_enable 打开; 小核要用就自己打开(幂等, 打开后大核对
 * 应的 clk_count 仍是它自己的计数, 互不影响)。 */
#define CLK_BASE           0x03002000UL
#define REG_CLK_EN_1       0x004
#define CLK_SDMA_AXI_BIT   1u

#define SYSDMA_CH          7u       /* 0..3 归 Linux, 4..7 空闲, 取 7 */
#define CH_EN_WE_OFFSET    8        /* CH_EN: [7:0]=使能值, [15:8]=对应写使能 */
#define CH_EN_ABORT_OFFSET 32       /* 中止位, [40]=对应写使能, 见 cvitek-dma.h */
#define CH_EN_ABORT_WE_OFFSET 40

#define CH_INTSTA_BLOCK_TFR_DONE   (1u << 0)
#define CH_INTSTA_DMA_TFR_DONE     (1u << 1)

/* ---- CFG 位段 (cvitek-dma.h 的 DWC_CFG_*) ---- */
#define CFG_SRC_MULTBLK(x)  ((uint64_t)((x) & 0x7u))
#define CFG_DST_MULTBLK(x)  ((uint64_t)((x) & 0x7u) << 2)
#define CFG_TT_FC(x)        ((uint64_t)((x) & 0x7u) << 32)
#define CFG_SRC_PER(x)      ((uint64_t)((x) & 0xffu) << 39)
#define CFG_DST_PER(x)      ((uint64_t)((x) & 0xffu) << 44)
#define CFG_SRC_OSR_LMT(x)  ((uint64_t)((x) & 0xfu) << 55)
#define CFG_DST_OSR_LMT(x)  ((uint64_t)((x) & 0xfu) << 59)

#define TT_FC_M2M           0u      /* DW_DMA_FC_D_M2M */
#define TT_FC_M2P           1u      /* DW_DMA_FC_D_M2P (DMAC 做流控) */
#define MULTBLK_LINK_LIST   3u

/* ---- CTL 位段 (cvitek-dma.h 的 DWC_CTL_*) ----
 * 宽度字段是 log2(字节数): 0=8bit, 1=16bit, 2=32bit。 */
#define CTL_SMS(n)          ((uint64_t)((n) & 1u))
#define CTL_DMS(n)          ((uint64_t)((n) & 1u) << 2)
#define CTL_SRC_WIDTH(n)    ((uint64_t)((n) & 0x7u) << 8)
#define CTL_DST_WIDTH(n)    ((uint64_t)((n) & 0x7u) << 11)
#define CTL_SRC_MSIZE(n)    ((uint64_t)((n) & 0xfu) << 14)
#define CTL_DST_MSIZE(n)    ((uint64_t)((n) & 0xfu) << 18)
#define CTL_DST_FIX         (1ULL << 6)     /* 目的地址不递增(写给外设寄存器) */
#define CTL_SRC_STA_EN      (1ULL << 56)
#define CTL_DST_STA_EN      (1ULL << 57)
#define CTL_IOC_BLT_EN      (1ULL << 58)    /* 块传完触发中断 */
#define CTL_LLI_LAST        (1ULL << 62)
#define CTL_LLI_VALID       (1ULL << 63)

/* 板级 DT(sg2002_milkv_duo256m_glibc_arm64_sd.dts)写得很明确:
 *     dmas = <&dmac 4 1 1>;   请求线 4 = rx: SPI2_RX
 *     dmas = <&dmac 5 1 1>;   请求线 5 = tx: SPI2_TX
 * dma spec 三格是 <request m_master p_master>, m_master = p_master = 1;
 * 内核 dw_dma_of_xlate 把 src_id/dst_id 都取第一格, 所以两处填同一个值。
 *
 * 请求线 4 是 RX、5 才是 TX。这里原来填的是 4: 等于把目的握手挂到了 RX 请求
 * 线上, 而送显走"只发"模式 —— RX FIFO 永远不进数据, 请求线永不置起, 通道就
 * 一直停在"已使能、等握手"的状态(实测 CH_EN.bit7 恒 1、BLOCK_TS 不动),
 * 于是 DMA 一次也没搬成功, 全部回退 PIO。 */
#define DMA_MASTER_MEM      1u
#define DMA_MASTER_PER      1u
#define SPI2_TX_PER_ID      5u

/* 每个 LLI 最多搬多少字节。设备树 dma@0x4330000 的 block_size = 0x400 = 1024,
 * Linux 的 memcpy 路径也是照这个值切块的, 跟着走最稳。 */
#define SYSDMA_CHUNK        1024u
#define SYSDMA_MAX_LLI      192u    /* 192*1024 = 196608 B, 够整屏 153600 B */

struct sysdma_lli {
	uint64_t sar;
	uint64_t dar;
	uint64_t block_ts;
	uint64_t llp;
	uint64_t ctl;
	uint32_t sstat;
	uint32_t dstat;
	uint64_t llp_status;
	uint64_t reserved;
} __attribute__((aligned(64)));

static struct sysdma_lli s_lli[SYSDMA_MAX_LLI] __attribute__((aligned(64)));

/* 大核可以用 devmem 读这几个量看小核 DMA 的状态(地址用 nm 查 g_sysdma_stat) */
#define SYSDMA_STAT_MAGIC   0x53444D41UL        /* 'SDMA' */
volatile unsigned long g_sysdma_stat[8];

static inline void dma_wr32(unsigned long a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t dma_rd32(unsigned long a) { return *(volatile uint32_t *)a; }
static inline void dma_wr64(unsigned long a, uint64_t v) { *(volatile uint64_t *)a = v; }
static inline uint64_t dma_rd64(unsigned long a) { return *(volatile uint64_t *)a; }

static void sysdma_clk_on(void)
{
	unsigned long r = CLK_BASE + REG_CLK_EN_1;

	dma_wr32(r, dma_rd32(r) | (1u << CLK_SDMA_AXI_BIT));
}

/* 中止并关闭通道。只写"使能位=0"是关不掉一个正卡在等外设握手的通道的,
 * 必须同时置 ABORT —— 这段照抄 cvitek-dma.c 的 dwc_chan_disable():
 *     ch_en |= mask<<8 (EN 写使能) | mask<<40 (ABORT 写使能) | mask<<32 (ABORT)
 *     ch_en &= ~mask
 * 通道空闲时什么都不做。返回 0=已是空闲, 0xE5=清不掉(确实有别人在用)。
 *
 * 为什么需要它: DMAC 不随小核固件重载复位。上一次失败留在通道里的
 * "已使能、等握手"脏状态会一直存着, 之后每次传输都会判"通道被占"直接放弃。 */
static uint32_t sysdma_ch_disable(unsigned ch)
{
	unsigned long reg = SYSDMA_BASE + DMAC_CH_EN;
	uint64_t v = dma_rd64(reg);
	uint32_t guard = 100000u;

	if (!(v & ((uint64_t)1 << ch)))
		return 0;

	v |= (uint64_t)1 << (ch + CH_EN_WE_OFFSET);
	v |= (uint64_t)1 << (ch + CH_EN_ABORT_WE_OFFSET);
	v |= (uint64_t)1 << (ch + CH_EN_ABORT_OFFSET);
	v &= ~((uint64_t)1 << ch);
	dma_wr64(reg, v);

	while (dma_rd64(reg) & ((uint64_t)1 << ch))
		if (--guard == 0) {
			g_dma2_probe[6]++;      /* 【诊断】ch_disable 打满 guard */
			return 0xE5;
		}
	return 0;
}

/* 建 LLI 链并把通道拉起来, 不等它搬完。收尾必须配对调 sysdma_wait_done()。
 * dst_fixed!=0 时目的地址固定(写给外设寄存器)。
 * 返回: 0=已启动, 0xE0=长度/链数越界, 0xE1=控制器不可见, 0xE5=通道被占。 */
static uint32_t sysdma_start(uint64_t sar, uint64_t dar, uint32_t len,
			     int dst_fixed, unsigned per_id, int use_handshake)
{
	unsigned long ch = SYSDMA_BASE + CH_OFF(SYSDMA_CH);
	uint32_t n, off, chunk;
	uint64_t ctl, cfg;
	unsigned i;

	if (len == 0)
		return 0;

	n = (len + SYSDMA_CHUNK - 1u) / SYSDMA_CHUNK;
	if (n == 0 || n > SYSDMA_MAX_LLI)
		return 0xE0;

	s_last_len = len;               /* 【诊断】供超时快照记录规模 */
	s_last_n   = n;

	sysdma_clk_on();

	if (dma_rd64(SYSDMA_BASE + DMAC_COMPVER) == 0)
		return 0xE1;                    /* 控制器不可见 */

	/* CTL 照 dw-spi 送显真正走的那条内核路径配 ——
	 * cvitek-dma.c 的 dwc_prep_dma_slave_sg() 里的 DMA_MEM_TO_DEV 分支:
	 *     ctl = DWC_DEFAULT_CTL | DST_WIDTH(reg_width) | DST_FIX
	 *           | SRC_INC | DST_STA_EN | SRC_STA_EN
	 * 其中 DWC_DEFAULT_CTL 的 MSIZE 来自 dma_slave_config 的 maxburst
	 * (经 dwc_config() 的 convert_burst() 编码):
	 *     dst_maxburst = txburst = fifo_len/2 = 8 -> 编码 2 (8 项)
	 *     src_maxburst 没设 = 0                   -> 编码 0 (1 项)
	 *     SMS/DMS 取 m_master = p_master = 1
	 * 【不要加 ARLEN/AWLEN】: 那两个只在 dwc_prep_dma_cyclic(音频循环传输)
	 * 里置, 普通 slave_sg 不置。之前照 cyclic 抄了 ARLEN(7)/AWLEN(0),
	 * 与 DST_MSIZE=8 自相矛盾(AWLEN(0) 要求写侧单拍)。 */
	ctl = CTL_SMS(DMA_MASTER_MEM) | CTL_DMS(DMA_MASTER_PER)
	    | CTL_SRC_WIDTH(0) | CTL_DST_WIDTH(0)
	    | CTL_SRC_MSIZE(use_handshake ? 0u : 3u)
	    | CTL_DST_MSIZE(use_handshake ? s_spi2_msize : 3u)
	    | CTL_SRC_STA_EN | CTL_DST_STA_EN;
	if (dst_fixed)
		ctl |= CTL_DST_FIX;

	cfg = CFG_SRC_MULTBLK(MULTBLK_LINK_LIST) | CFG_DST_MULTBLK(MULTBLK_LINK_LIST)
	    | CFG_TT_FC(use_handshake ? TT_FC_M2P : TT_FC_M2M)
	    | CFG_SRC_OSR_LMT(15) | CFG_DST_OSR_LMT(15);
	if (use_handshake) {
		cfg |= CFG_SRC_PER(per_id) | CFG_DST_PER(per_id);
	}

	for (i = 0, off = 0; off < len; i++) {
		chunk = len - off;
		if (chunk > SYSDMA_CHUNK)
			chunk = SYSDMA_CHUNK;

		s_lli[i].sar        = sar + off;
		s_lli[i].dar        = dst_fixed ? dar : (dar + off);
		s_lli[i].block_ts   = chunk - 1u;
		s_lli[i].llp        = (i + 1u < n) ? (uint64_t)(uintptr_t)&s_lli[i + 1] : 0;
		s_lli[i].ctl        = ctl | CTL_LLI_VALID;
		s_lli[i].sstat      = 0;
		s_lli[i].dstat      = 0;
		s_lli[i].llp_status = 0;
		s_lli[i].reserved   = 0;

		off += chunk;
	}
	s_lli[n - 1].ctl |= CTL_LLI_LAST | CTL_IOC_BLT_EN;
	flush_dcache_range((uintptr_t)s_lli, (uintptr_t)n * sizeof(s_lli[0]));

	/* 通道必须是空闲的。先尝试中止掉可能残留的脏状态(上一次传输中途失败
	 * 会留下"已使能、等握手"的通道), 只有真清不掉才判"被占"。 */
	/* 【诊断】记下"起新传输时上一趟居然还挂着", 这正是超时的直接嫌疑 */
	if (dma_rd64(SYSDMA_BASE + DMAC_CH_EN) & ((uint64_t)1 << SYSDMA_CH))
		g_dma2_probe[12]++;
	if (sysdma_ch_disable(SYSDMA_CH) != 0)
		return 0xE5;

	dma_wr64(ch + CH_INTCLEAR, 0xFFFFFFFFu);
	dma_wr64(ch + CH_INTSTATUS_EN, CH_INTSTA_DMA_TFR_DONE);
	dma_wr64(ch + CH_CFG, cfg);
	dma_wr64(ch + CH_LLP, (uint64_t)(uintptr_t)&s_lli[0]);
	dma_wr64(SYSDMA_BASE + DMAC_CH_EN,
		 ((uint64_t)1 << SYSDMA_CH) |
		 ((uint64_t)1 << (SYSDMA_CH + CH_EN_WE_OFFSET)));
	return 0;
}

/* 轮询通道中断等这一趟搬完, 然后清中断 + 中止并关掉通道。
 * 返回 0=已完成, 0xE2=超时(通道已被 ABORT 关掉)。
 *
 * 不用中断, 绕开中断路由。m2m 会很快返回; 带握手时若外设一直不给请求, 就在
 * guard 耗尽后退出并关通道。guard 取 2e6(约 0.18s), 只是一道兜底 —— 真搬完
 * 的搬运会在下面的"通道已关+LLP=0"上立刻返回, 一般 13ms 就出循环。 */
static uint32_t sysdma_wait_done(void)
{
	unsigned long ch = SYSDMA_BASE + CH_OFF(SYSDMA_CH);
	uint32_t guard = 2000000u;
	uint32_t st = 0;
	uint32_t done;
	uint32_t hw_done = 0;
	uint32_t t0 = dma2_us();        /* 【诊断】 */
	uint32_t t1;

	/* 判"搬完了"有两个依据:
	 *   ① 通道中断 BLOCK_TFR_DONE / DMA_TFR_DONE —— 正常路径;
	 *   ② 硬件其实早就收摊了: CH_EN 位自己清了(设计里搬完自动关通道),
	 *      LLP 也已走到链尾(0), 只是中断状态没读到。
	 * 只认 ① 会把 ② 那些成功的搬运全判成超时(0x484A0007 实测占 65%), 每次
	 * 白等满 guard(~0.7s), 帧率就是这么掉到 1~2 的。
	 * ② 每 1024 圈查一次(多读两个寄存器), 不影响正常路径的开销。 */
	while (guard--) {
		st = (uint32_t)dma_rd64(ch + CH_INTSTATUS);
		if (st & (CH_INTSTA_BLOCK_TFR_DONE | CH_INTSTA_DMA_TFR_DONE))
			break;
		if (((guard & 0x3FFu) == 0) &&
		    !(dma_rd64(SYSDMA_BASE + DMAC_CH_EN) & ((uint64_t)1 << SYSDMA_CH)) &&
		    (dma_rd64(ch + CH_LLP) == 0)) {
			hw_done = 1;
			break;
		}
	}
	done = (st & (CH_INTSTA_BLOCK_TFR_DONE | CH_INTSTA_DMA_TFR_DONE)) || hw_done;

	t1 = dma2_us();                 /* 【诊断】 */
	{
		uint32_t dt = t1 - t0;

		/* 【诊断】超时现场: 在"清中断 + 关通道"之前先把双方状态抓下来,
		 * 否则什么都看不到了。判定用:
		 *   TXFLR 满 + SR.BUSY=1  -> SPI 还在移, 卡在外设没给下一个请求
		 *   TXFLR 空 + SR.BUSY=0  -> SPI 早把 FIFO 倒空, DMA 没再供数据
		 *   LLP 序号 / BLOCK_TS   -> 到底搬了多少, 还是根本没动 */
		if (!done) {
			uint64_t en  = dma_rd64(SYSDMA_BASE + DMAC_CH_EN);
			uint64_t llp = dma_rd64(ch + CH_LLP);
			uint64_t ts  = dma_rd64(ch + CH_BLOCK_TS);
			uintptr_t lp = (uintptr_t)llp;

			g_dma2_probe[13] = (unsigned long)(uint32_t)en;
			g_dma2_probe[14] = (unsigned long)((uint32_t)ts & 0xFFFFu);
			g_dma2_probe[15] =
				(lp >= (uintptr_t)s_lli &&
				 lp < (uintptr_t)(s_lli + SYSDMA_MAX_LLI))
				? (unsigned long)((lp - (uintptr_t)s_lli) / sizeof(s_lli[0]))
				: 0xFFFFFFFFUL;
			g_dma2_probe[16] = s_last_n;
			g_dma2_probe[17] = mmio_read_32(SPI_SR);
			g_dma2_probe[18] = (unsigned long)mmio_read_32(SPI_TXFLR)
					 | ((unsigned long)mmio_read_32(SPI_RXFLR) << 16);
			g_dma2_probe[19] = mmio_read_32(SPI_DMACR);
			g_dma2_probe[20] = mmio_read_32(SPI_CTRL0);
			g_dma2_probe[21] = mmio_read_32(SPI_TXFTLR);
			g_dma2_probe[22] = s_last_len;
			g_dma2_probe[23] = dt;
			g_dma2_probe[26] = (unsigned long)(uint32_t)dma_rd64(ch + CH_INTSTATUS_EN);
			g_dma2_probe[27] = DMA2_PROBE_MAGIC;
		}

		g_dma2_probe[1]++;
		g_dma2_probe[2] += dt;
		if (!done) {
			g_dma2_probe[3]++;
			g_dma2_probe[4] = st;
		} else if (hw_done) {
			g_dma2_probe[24]++;     /* 靠"通道已关+LLP=0"补判完成的次数 */
		}
		if (dt > g_dma2_probe[25])
			g_dma2_probe[25] = dt;
	}

	/* 收尾: 清通道中断 + 中止并关掉通道。
	 * 超时那一路必须靠 ABORT 才停得下来(只清使能位会留下卡住的通道)。 */
	dma_wr64(ch + CH_INTCLEAR, 0xFFFFFFFFu);
	(void)sysdma_ch_disable(SYSDMA_CH);

	return done ? 0u : 0xE2;
}

/* 跑完一整趟(启动 + 等完成), 同步语义。 */
static uint32_t sysdma_transfer(uint64_t sar, uint64_t dar, uint32_t len,
				int dst_fixed, unsigned per_id, int use_handshake)
{
	uint32_t rc = sysdma_start(sar, dar, len, dst_fixed, per_id, use_handshake);

	if (rc != 0)
		return rc;
	return sysdma_wait_done();
}

/* 内存 -> 内存, 走 LLI 链。给自检用; 也顺便验证链式搬大块的能力。 */
uint32_t sysdma_memcpy(void *dst, const void *src, uint32_t len)
{
	uint32_t rc;
	uint32_t n;

	if (len == 0 || ((uintptr_t)src & 3u) || ((uintptr_t)dst & 3u) || (len & 3u))
		return 0xE0;

	n = (len + SYSDMA_CHUNK - 1u) / SYSDMA_CHUNK;

	g_sysdma_stat[0] = SYSDMA_STAT_MAGIC;
	g_sysdma_stat[1] = len;
	g_sysdma_stat[2] = n;

	/* 源数据要真正落到 DRAM(DMA 读的是内存), 目的缓冲的旧 cache 行先作废 */
	flush_dcache_range((uintptr_t)src, len);
	inv_dcache_range((uintptr_t)dst, len);

	rc = sysdma_transfer((uint64_t)(uintptr_t)src, (uint64_t)(uintptr_t)dst,
			     len, 0, 0, 0);

	inv_dcache_range((uintptr_t)dst, len);
	g_sysdma_stat[3] = rc;
	return rc;
}

/* ==========================================================================
 * SPI2 走 DMA 送一块数据(只发模式)
 *
 * 和 spi2_write() 的区别: 数据由 sysDMA 直接灌进 SPI 的 TX FIFO, 小核在这
 * 期间是空闲的 —— 这是"渲染/送显重叠"的前提。小包(命令、窗口参数)
 * 仍走 PIO, 因为 DMA 每次都要重建 LLI 链, 有固定开销。
 *
 * 分成 start/wait 两半: 上层(lv_port_disp.c 的双缓冲)在 start 之后立刻返回去
 * 渲染下一块, 直到下一块要开窗之前才 wait —— 于是送显和渲染在时间上重叠。
 *
 * DMA 配错的后果是安全的: sysdma_start 起不来会直接返回非 0, 这里立刻退回
 * PIO 当场搬完(s_dma_inflight 保持 0, 后面的 wait 是空操作), 屏幕上最多是
 * 这一帧晚了 ~0.2s。
 * ========================================================================== */

/* 1 = 已经 start 过一次还没 wait, 用来保证"同一时刻最多一趟在飞" */
static int s_dma_inflight;

/* 熔断: 连丢 SPI2_DMA_FAIL_MAX 块像素以后就彻底不赌 DMA 了, 一律 PIO。
 * DMA 通路实测"时好时坏", 一旦真超时那块像素就没了; 与其每帧赌一次(赌输
 * 就白等 + 丢画面), 不如退回纯 PIO —— 帧率就是 PIO 的水平(~57fps), 而不是
 * 1~2fps。真超时本来就该是罕见的, 所以这道闸正常永远不会合上。 */
#define SPI2_DMA_FAIL_MAX   3u
static uint32_t s_dma_fail;     /* 连续真超时的次数 */
static int      s_dma_off;      /* 1 = 已熔断, 以后一律 PIO */

void spi2_write_dma_start(const uint8_t *buf, uint32_t len)
{
	uint32_t rc;
	uint32_t t0;
	uint32_t tdlr = s_spi2_fifo_len / 2u;

	if (len == 0)
		return;

	if (s_dma_off) {                /* 已熔断: 直接 PIO 送完, 不再碰 DMA */
		spi2_write(buf, len);
		return;
	}

	t0 = dma2_us();                 /* 【诊断】整个 start 的耗时 */

	/* 只发模式: RX FIFO 不进数据, 不会因为 RX FIFO 满把移位器卡住 */
	spi2_tmod_set(SPI_CTRL0_TMOD_TXONLY);

	/* DMATDLR 水位取 FIFO 深度的一半, 和上面的突发项数一致: 每次请求刚好补
	 * 一个突发, SPI 时钟不会在两次请求之间空转(和 Linux dw-spi 一个思路)。 */
	mmio_write_32(SPI_DMACR, 0);
	mmio_write_32(SPI_DMATDLR, tdlr);
	mmio_write_32(SPI_SER, 1);      /* 同 spi2_write: 片选可能被 Linux 抢过 */
	mmio_write_32(SPI_DMACR, SPI_DMA_TDMAE);

	/* DMA 读的是 DRAM, 像素必须先真正落到内存 */
	flush_dcache_range((uintptr_t)buf, len);

	rc = sysdma_start((uint64_t)(uintptr_t)buf,
			  (uint64_t)(uintptr_t)SPI_DR,
			  len, 1 /* 目的地址固定 */, SPI2_TX_PER_ID, 1 /* 硬件握手 */);

	g_spi2_probe[3] = rc;
	g_spi2_probe[4] = s_spi2_fifo_len;
	g_spi2_probe[6] = tdlr;

	if (rc != 0) {
		/* DMA 没跑起来就退回 PIO, 这一帧至少还能显示出来 */
		g_spi2_probe[5]++;
		mmio_write_32(SPI_DMACR, 0);
		spi2_tmod_set(0);
		spi2_write(buf, len);
		flush_dcache_range((uintptr_t)g_spi2_probe, sizeof(g_spi2_probe));
		return;
	}

	s_dma_inflight = 1;
	g_spi2_probe[1]++;
	g_spi2_probe[2] += len;
	g_spi2_bytes   += len;
	g_dma2_probe[5] += dma2_us() - t0;      /* 【诊断】整个 start 的耗时 */
}

void spi2_write_dma_wait(void)
{
	uint32_t guard;
	uint32_t t0;                    /* 【诊断】 */
	uint32_t rc;

	if (!s_dma_inflight)
		return;                 /* 没在飞(或已收尾过), 空操作 */
	s_dma_inflight = 0;

	rc = sysdma_wait_done();
	mmio_write_32(SPI_DMACR, 0);

	if (rc != 0) {
		/* 真超时: 这一块像素丢了(下次开窗前 FIFO 会被 SSI 复位清掉, 不会
		 * 串到后面的窗口里)。连续丢够次数就熔断, 退回 PIO。 */
		if (++s_dma_fail >= SPI2_DMA_FAIL_MAX && !s_dma_off) {
			s_dma_off = 1;
			g_spi2_probe[5]++;      /* 复用"回退 PIO"计数 */
		}
	} else {
		s_dma_fail = 0;
	}

	/* DMA 传完 != 移位器空: 最后一个字节还在往外移。这里必须等干净,
	 * 因为调用方下一步通常就是发开窗命令(PIO), 插进还没出去的像素流里
	 * 会让画面错位。 */
	t0 = dma2_us();                 /* 【诊断】 */
	guard = SPI_POLL_GUARD;
	while (mmio_read_32(SPI_SR) & SPI_SR_BUSY) {
		if (--guard == 0) {
			g_dma2_probe[8]++;
			break;
		}
	}
	g_dma2_probe[7] += dma2_us() - t0;

	/* 先把诊断量刷出去, 大核才读得到最新值 */
	g_dma2_probe[0] = DMA2_PROBE_MAGIC;
	flush_dcache_range((uintptr_t)g_dma2_probe, sizeof(g_dma2_probe));
	flush_dcache_range((uintptr_t)g_spi2_probe, sizeof(g_spi2_probe));
}

void spi2_write_dma(const uint8_t *buf, uint32_t len)
{
	spi2_write_dma_start(buf, len);
	spi2_write_dma_wait();
}
