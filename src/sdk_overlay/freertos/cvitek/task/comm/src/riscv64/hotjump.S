/*
 * hotjump.S —— 小核(C906L/FreeRTOS)固件"自跳转热更"的常驻跳板
 *
 * 背景：大核(Linux/A53)没有任何办法从外部复位小核 —— reset_c906l() 要写的
 * 启动地址寄存器在安全子系统(SEC_SYS @0x020B0000)，A53 访问一律 Bus error。
 * 所以换个思路：**不重启，让小核固件自己跳到新固件**。
 *
 * 本段代码被链接到固定地址 0x8FF90000（见 scripts/cv181x_lscript.ld 的
 * hotjump_MEM_0），位于小核应用区(0x8FE00000..0x8FF90000)之上、LCD 共享内存
 * (0x8FFC0000)之下。大核热更时只覆盖 0x8FE00000..0x8FF90000，永远碰不到这里，
 * 所以这个跳板是"常驻"的，冷热启动都必须在（随 fip 一起烧进去）。
 *
 * 握手（与 lcd_shm.h 里的 LCD_RELOAD_* 一一对应，改一处必须改两处）：
 *   大核 -> 小核  ctrl.reload     @ 0x8FFC0028：写 LCD_RELOAD_MAGIC 表示"新固件写好了"
 *   小核 -> 大核  stat.reload_ack @ 0x8FFC0058：1 = 已停在跳板，可以开始写
 *
 * 完整流程：
 *   1. 大核在共享内存里清握手，发 cmdqu LCD_CMD_RTOS_RELOAD
 *   2. 应用收到命令后调用 hotjump_entry()（不返回）
 *   3. 本跳板：关中断、换自己的栈、置 reload_ack=1，然后轮询 ctrl.reload
 *   4. 大核看到 reload_ack=1，把新固件经 /dev/mem 写到 0x8FE00000
 *   5. 大核置 ctrl.reload=LCD_RELOAD_MAGIC
 *   6. 本跳板：全 cache 失效，跳到 0x8FE00000
 *   7. 新固件的 _start 按冷启动路径从零初始化
 *
 * cache 指令说明（工具链的汇编器不认 icache.iall/dcache.iall 助记符，
 * 所以统一用 .long 编码；三个都取自本 SDK 已在用的编码）：
 *   .long 0x0295000b  dcache.cpa  a0   把 a0 那行写回 DRAM
 *   .long 0x02a5000b  dcache.ipa  a0   作废 a0 那行（下次从 DRAM 取）
 *   .long 0x0190000b  sync.s
 * 收尾的全 cache 失效用 csrs mcor,0x30013 —— 与 arch/riscv64/src/start.S
 * 冷启动时"invalidate all ... DCACHE,ICACHE"用的是同一个寄存器/同一组位。
 */

	.equ	HOTJUMP_REQ_ADDR, 0x8FFC0028	/* ctrl.reload     (LCD_SHM_PHYS+40)     */
	.equ	HOTJUMP_ACK_ADDR, 0x8FFC0058	/* stat.reload_ack (LCD_SHM_PHYS+64+24)  */
	.equ	HOTJUMP_MAGIC,    0x484F544A	/* 'HOTJ' */

	.section .hotjump, "ax"
	.globl	hotjump_entry
	.type	hotjump_entry, @function
	.align	3

hotjump_entry:
	/* 关中断（M-mode）：mie 清空 + mstatus.MIE=0。之后不再返回。 */
	csrw	mie, zero
	csrc	mstatus, 8
	fence

	/* 换到跳板自己的栈（在 .hotjump 段里，不会被大核覆盖） */
	la	sp, hotjump_stack_top

	/* 先把本核 cache 全部丢弃再放行大核。理由：旧固件的 .data/.bss/heap/stack
	 * 在 cache 里可能还有脏行；若等到大核写完新固件后才被动回写，就会把刚写进去
	 * 的新固件覆盖掉。此处直接失效(不回写)，之后本核再无脏行，大核随便写。
	 * (之后只读 ctrl 那一行，且每轮都先 ipa，不会产生新的脏行。) */
	li	t3, 0x30013
	csrc	mcor, t3
	csrs	mcor, t3

	/* 告诉大核：我停在跳板了，可以开始往 0x8FE00000 写新固件 */
	li	t0, HOTJUMP_ACK_ADDR
	li	t1, 1
	sw	t1, 0(t0)
	mv	a0, t0
	.long	0x0295000b		/* dcache.cpa a0：把 ack 写回 DRAM */
	.long	0x0190000b		/* sync.s */

1:
	/* 轮询大核的 magic；每次先作废本核缓存的这一行再读 */
	li	t0, HOTJUMP_REQ_ADDR
	mv	a0, t0
	.long	0x02a5000b		/* dcache.ipa a0 */
	.long	0x0190000b		/* sync.s */
	lw	t1, 0(t0)
	li	t2, HOTJUMP_MAGIC
	bne	t1, t2, 1b

	/* 新固件已就位：丢掉本核的全部 cache（尤其旧的 I-cache 行），再跳 _start。
	 * 先清后置，保证无论 mcor 是"边沿触发"还是"自清零"都能触发一次失效。 */
	li	t3, 0x30013
	csrc	mcor, t3
	csrs	mcor, t3
	fence.i
	li	t0, 0x8FE00000
	jr	t0

	.size	hotjump_entry, . - hotjump_entry

	.align	4
hotjump_stack:
	.space	2048
	.globl	hotjump_stack_top
hotjump_stack_top:
