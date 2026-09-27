/* sysdma_test.c —— Step0 排雷: 验证小核(C906L)能不能驱动 sysDMA
 *
 * sysDMA = DesignWare AXI DMAC (compatible "snps,dmac-bm"), 基址 0x04330000。
 *
 * ==== 第一版(ABI 0x484A0002)的结论 ====
 * 读 DMAC_ID 得到 0, 于是判成"控制器不可见"(0xE1)。但拿同样几个寄存器去大核
 * 对读, 数值一模一样:
 *     大核: ID=0x00000000 COMPVER=0x3130312A CFG=0x00000003 CH_EN=0x00000000
 *     小核: ID=0x00000000 COMPVER=0x3130312A CFG=0x00000003 CH_EN=0x00000000
 * 说明这块 SoC 的 ID 本来就是 0, 是小核的判据写错了 —— 小核访问 0x04330000
 * 完全正常。
 *
 * ==== 本版(ABI 0x484A0003)改了什么 ====
 *   1. 存在性判据从 DMAC_ID 换成 DMAC_COMPVER(非 0 且非全 F);
 *   2. 【重要】不再碰全局寄存器: 原来先写 DMAC_RESET=1 再写 DMAC_CFG=0 会
 *      把 Linux 的 dw_dmac 驱动正在用的控制器给关了(大核实测 CFG=0x3, 即
 *      EN|INT_EN 已置位)。改成只用通道自己的寄存器。
 *   3. 用**空闲通道**(默认 6 号): 大核实测 dma0chan0..3 的 in_use=1,
 *      4..7 为空, 所以借 6 号。启动前先查该通道 CH_EN 位, 若已被别人占用就
 *      直接放弃(结果码 0xE5)。
 *   4. 收尾把通道关掉并清中断, 不留垃圾状态给大核。
 *
 * 为什么先做"内存到内存": 内存到内存不需要外设请求线, 也不需要改
 * sysdma_remap 寄存器, 能一次性排除"小核能不能把这个控制器驱动起来"这个
 * 最大未知数。
 *
 * 结论全部写进 g_dma_probe[16], 大核用 `devmem <g_dma_probe> 64` 逐字读:
 *   [0]  DMAC_ID(本 SoC 恒 0)  [1]  DMAC_COMPVER(存在性判据)
 *   [2]  DMAC_CFG (只读)       [3]  DMAC_CH_EN (改前)
 *   [4]  clk 门控寄存器(改前)  [5]  clk 门控寄存器(改后)
 *   [6]  ch_INTSTATUS          [7]  DMAC_CH_EN (传输后)
 *   [8]  源地址                [9]  目的地址
 *   [10] 搬运字节数            [11] 不一致字节数
 *   [12] 轮询圈数(看是否超时)  [13] 结果码
 *   [14] 'DMA0' 魔数头         [15] 'STEP' 魔数尾
 *
 * 结果码 [13]: 0=成功且数据完全正确; 0xE1=控制器不可见; 0xE2=等完成超时;
 *              0xE3=完成了但数据不对; 0xE5=目标通道已被占用。
 */
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "arch_helpers.h"       /* flush_dcache_range / inv_dcache_range */

/* ---------------- sysDMA 寄存器 (基址 0x04330000) ---------------- */
#define SYSDMA_BASE        0x04330000UL

/* 公共寄存器 */
#define DMAC_ID            0x000
#define DMAC_COMPVER       0x008
#define DMAC_CFG           0x010
#define DMAC_CH_EN         0x018

/* 通道寄存器: 第 n 个通道基址 = 0x100 + n*0x100 */
#define CH_OFF(n)          (0x100u + (unsigned)(n) * 0x100u)
#define CH_SAR             0x000
#define CH_DAR             0x008
#define CH_BLOCK_TS        0x010
#define CH_CFG             0x020
#define CH_LLP             0x028
#define CH_INTSTATUS_EN    0x080
#define CH_INTSTATUS       0x088
#define CH_INTCLEAR        0x098

/* 借哪个通道。0..3 被大核 in_use, 4..7 空, 取 6 号。 */
#define CH_SEL             6u

/* CH_EN 的位段: [7:0]=使能值, [15:8]=对应写使能 */
#define CH_EN_WE_OFFSET    8

/* clk 门控: clk 控制器基址 0x03002000, REG_CLK_EN_1=+0x004, clk_sdma_axi=bit1。
 * 这个门控不是 CLK_IS_CRITICAL。大核那边由 dw_dmac 驱动 clk_enable 打开;
 * 小核要用就自己开 —— 打开是幂等的, 打开后大核那边也不受影响。 */
#define CLK_BASE           0x03002000UL
#define REG_CLK_EN_1       0x004
#define CLK_SDMA_AXI_BIT   1u

/* 通道中断状态位 */
#define CH_INTSTA_BLOCK_TFR_DONE   (1u << 0)
#define CH_INTSTA_DMA_TFR_DONE     (1u << 1)

/* CTL(64bit) / CFG(64bit) 常量, 取自 cvitek-dma.c 的 dwc_prep_dma_memcpy +
 * DWC_DEFAULT_CTL / dwc_initialize 的内存到内存分支:
 *   SMS=DMS=0, SRC/DST_INC, 32bit 宽, MSIZE=4, SRC/DST_STA_EN,
 *   单块直接寄存器模式 -> SHADOWREG_OR_LLI_VALID|LAST 都要置。
 *   TT_FC=0(M2M), HS_SEL 保持硬件(0), SRC/DST_PER=0,
 *   SRC/DST_OSR_LMT=15, MULTBLK_TYPE=LINK_LIST(3), CH_PRIOR=0。 */
#define CTL_SINGLE_BLK     0xC300000000111200ULL
#define CH_CFG_M2M         0x7F8000000000000FULL

/* LLI 描述符: 64 字节, 必须 64 字节对齐(硬件把 LLP 低 6 位当无效)。
 * 字段偏移和通道寄存器偏移不一样 —— 这里 llp=0x18/ctl=0x20。 */
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

#define DMA_TEST_LEN   4096

static uint8_t  s_src[DMA_TEST_LEN] __attribute__((aligned(64)));
static uint8_t  s_dst[DMA_TEST_LEN] __attribute__((aligned(64)));
static struct sysdma_lli s_lli __attribute__((aligned(64)));

volatile unsigned long g_dma_probe[16];

/* 用 unsigned long 传地址: RV64 上指针是 64 位, 用 uint32_t 会截断 */
static inline void wr32(unsigned long a, uint32_t v)  { *(volatile uint32_t *)a = v; }
static inline uint32_t rd32(unsigned long a)          { return *(volatile uint32_t *)a; }
static inline void wr64(unsigned long a, uint64_t v)  { *(volatile uint64_t *)a = v; }
static inline uint64_t rd64(unsigned long a)          { return *(volatile uint64_t *)a; }

static void dma_probe_flush(void)
{
	flush_dcache_range((uintptr_t)g_dma_probe, sizeof(g_dma_probe));
}

void sysdma_selftest(void)
{
	volatile unsigned long *p = g_dma_probe;
	const unsigned long ch = SYSDMA_BASE + CH_OFF(CH_SEL);
	unsigned long clk_reg = CLK_BASE + REG_CLK_EN_1;
	uint64_t id, compver, cfg_now, chen_before, chen_now;
	uint32_t ch_int = 0;
	uint32_t guard;
	int i, bad = 0;

	for (i = 0; i < 16; i++)
		p[i] = 0;
	p[14] = 0x444D4130UL;           /* 'DMA0' */
	p[15] = 0x53544550UL;           /* 'STEP' */
	dma_probe_flush();

	/* ---- 1) 打开 clk_sdma_axi 门控 (幂等, 不动别的位) ---- */
	p[4] = rd32(clk_reg);
	wr32(clk_reg, (uint32_t)p[4] | (1u << CLK_SDMA_AXI_BIT));
	p[5] = rd32(clk_reg);
	dma_probe_flush();

	/* ---- 2) 只读: 控制器在不在。这块 SoC 的 ID 恒 0, 所以用 COMPVER 做判据 ---- */
	id          = rd64(SYSDMA_BASE + DMAC_ID);
	compver     = rd64(SYSDMA_BASE + DMAC_COMPVER);
	cfg_now     = rd64(SYSDMA_BASE + DMAC_CFG);
	chen_before = rd64(SYSDMA_BASE + DMAC_CH_EN);
	p[0] = (unsigned long)id;
	p[1] = (unsigned long)compver;
	p[2] = (unsigned long)cfg_now;
	p[3] = (unsigned long)chen_before;
	dma_probe_flush();

	if (compver == 0 || compver == ~(uint64_t)0) {
		p[13] = 0xE1;           /* 控制器不可见 / 不可访问 */
		dma_probe_flush();
		return;
	}

	/* ---- 3) 目标通道别被人占着 (CH_EN 对应位为 1 说明正在跑) ---- */
	if (chen_before & (1UL << CH_SEL)) {
		p[13] = 0xE5;
		dma_probe_flush();
		return;
	}

	/* ---- 4) 准备数据 ---- */
	for (i = 0; i < DMA_TEST_LEN; i++) {
		s_src[i] = (uint8_t)(i * 7 + 3);
		s_dst[i] = 0;
	}
	flush_dcache_range((uintptr_t)s_src, sizeof(s_src));
	flush_dcache_range((uintptr_t)s_dst, sizeof(s_dst));

	/* ---- 5) 建单块 LLI, 然后 clean 到 DRAM (DMA 读的是内存里的真值) ---- */
	s_lli.sar        = (uint64_t)(uintptr_t)s_src;
	s_lli.dar        = (uint64_t)(uintptr_t)s_dst;
	s_lli.block_ts   = (DMA_TEST_LEN / 4) - 1;      /* 32bit 宽 -> 项数-1 */
	s_lli.llp        = 0;                           /* 单块, 无后继 */
	s_lli.ctl        = CTL_SINGLE_BLK;
	s_lli.sstat      = 0;
	s_lli.dstat      = 0;
	s_lli.llp_status = 0;
	s_lli.reserved   = 0;
	flush_dcache_range((uintptr_t)&s_lli, sizeof(s_lli));

	/* ---- 6) 清本通道中断 / 中断使能 / 通道 CFG / LLP ---- */
	wr64(ch + CH_INTCLEAR, 0xFFFFFFFFu);
	wr64(ch + CH_INTSTATUS_EN, CH_INTSTA_DMA_TFR_DONE);
	wr64(ch + CH_CFG, CH_CFG_M2M);
	wr64(ch + CH_LLP, (uint64_t)(uintptr_t)&s_lli);

	/* ---- 7) 启动本通道: 使能位=1, 写使能位=1 ---- */
	wr64(SYSDMA_BASE + DMAC_CH_EN,
	     ((uint64_t)1 << CH_SEL) | ((uint64_t)1 << (CH_SEL + CH_EN_WE_OFFSET)));

	/* ---- 8) 轮询本通道中断状态 (不用中断, 绕开中断路由的未知数) ---- */
	guard = 20000000u;
	while (guard--) {
		ch_int = (uint32_t)rd64(ch + CH_INTSTATUS);
		if (ch_int & (CH_INTSTA_BLOCK_TFR_DONE | CH_INTSTA_DMA_TFR_DONE))
			break;
	}
	chen_now = rd64(SYSDMA_BASE + DMAC_CH_EN);
	p[6]  = (unsigned long)ch_int;
	p[7]  = (unsigned long)chen_now;
	p[12] = 20000000u - guard;
	dma_probe_flush();

	/* ---- 9) 收尾: 关本通道 + 清中断 (无论成败都做) ---- */
	wr64(ch + CH_INTCLEAR, 0xFFFFFFFFu);
	wr64(SYSDMA_BASE + DMAC_CH_EN, (uint64_t)1 << (CH_SEL + CH_EN_WE_OFFSET));

	if (!(ch_int & (CH_INTSTA_BLOCK_TFR_DONE | CH_INTSTA_DMA_TFR_DONE))) {
		p[13] = 0xE2;           /* 等完成超时 */
		dma_probe_flush();
		return;
	}

	/* ---- 10) 收结果: 先作废目的缓冲的 cache 行, 读到的才是 DMA 写的真值 ---- */
	inv_dcache_range((uintptr_t)s_dst, sizeof(s_dst));
	for (i = 0; i < DMA_TEST_LEN; i++)
		if (s_dst[i] != s_src[i])
			bad++;

	p[8]  = (unsigned long)(uintptr_t)s_src;
	p[9]  = (unsigned long)(uintptr_t)s_dst;
	p[10] = DMA_TEST_LEN;
	p[11] = (unsigned long)bad;
	p[13] = bad ? 0xE3 : 0x0;
	dma_probe_flush();
}
