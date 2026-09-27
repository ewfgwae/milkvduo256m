/*
 * lcd_shm.h —— 大核(Linux/A53) 与 小核(FreeRTOS/C906L) 之间的 LCD 共享内存协议
 *
 * ── 通道一：cmdqu (mailbox) —— 传“命令”与“监控量” ────────────────────
 *   大核: open("/dev/cvi-rtos-cmdqu") + ioctl(RTOS_CMDQU_SEND[_WAIT], &cmdqu_t)
 *   小核: prvQueueISR() 按 ip_id 分发 -> prvCmdQuRunTask() 按 cmd_id 处理
 *   ip_id 固定用 IP_SYSTEM(=6)：它已经在路由表里指向 prvCmdQuRunTask，
 *   而 cmd_id 是 7bit 自留区间，所以两端都不用改任何已有枚举。
 *   大核的 CPU 占用率走 LCD_CMD_MON，cmdqu_t.param_ptr 只有 32 位，
 *   所以下行/上行的打包规则见下面的 LCD_MON_TX_* / LCD_MON_RX_* 宏。
 *   大核用 RTOS_CMDQU_SEND_WAIT 发出，小核在同一个 param_ptr 里回执
 *   自己的占用率与帧率。
 *
 * ── 通道二：共享内存 —— 传“像素” ────────────────────────────────────
 *   物理基址 LCD_SHM_PHYS，落在小核预留区(CVIMMAP_FREERTOS_*)内部、
 *   Linux memory 节点(0x80000000+0xfe00000)之外：
 *     大核 mmap("/dev/mem", LCD_SHM_PHYS) 写；小核直接按物理地址读。
 *   缓存一致性靠“分线”解决 —— 控制线(大核写/小核只读) 与
 *   状态线(小核写/大核只读) 各自占满一条 64B cache line，双方都只对
 *   自己“独占写”的那条做 flush，因此永远不会互相覆盖。
 */

#ifndef __LCD_SHM_H__
#define __LCD_SHM_H__

#include <stdint.h>

/* ---- 共享内存布局 ---------------------------------------------------- */
/* 小核预留区 = CVIMMAP_FREERTOS_ADDR(0x8FE00000) + SIZE(0x200000)；
 * 当前固件各段只用到 0x8FF0F540，0x8FFC0000 起是空闲的且 1MB 对齐。 */
#define LCD_SHM_PHYS        0x8FFC0000UL
#define LCD_SHM_MAGIC       0x4C434431UL   /* 'LCD1' */
#define LCD_SHM_VERSION     1u

#define LCD_W               240
#define LCD_H               320
#define LCD_FB_BYTES        (LCD_W * LCD_H * 2)      /* 153600 */

#define LCD_SHM_CTRL_OFF    0      /* 64B: 大核写, 小核只读 */
#define LCD_SHM_STAT_OFF    64     /* 64B: 小核写, 大核只读 */
#define LCD_SHM_FB_OFF      128    /* 起: RGB565 大端显存 */
#define LCD_SHM_BYTES       (LCD_SHM_FB_OFF + LCD_FB_BYTES)

/* ---- cmdqu 约定（cmdqu_t.ip_id = IP_SYSTEM）------------------------- *
 * 取值必须 <= 0x5F: Linux 侧驱动 cv181x_rtos_cmdqu 的 rtos_cmdqu_send()
 * 只放行 ip_id <= 7 且 cmd_id <= 0x5F 的命令, 超出会打印
 *   "invalid-id : ip_id = %d cmd_id = %d"
 * 并返回 -ENOBUFS。0x50..0x5F 已被 SYS_CMD_INFO_* 占用, 所以取 0x40 起。 */
#define LCD_CMD_INIT        0x40   /* 大核: 校验 magic/version；小核回 ACK */
#define LCD_CMD_FLUSH       0x41   /* 按 ctrl 里的矩形把 fb 刷到屏上 */
#define LCD_CMD_FILL        0x42   /* 用 ctrl.color 填满整屏 */
#define LCD_CMD_BL          0x43   /* param_ptr = 0/1 背光 */
#define LCD_CMD_PING        0x44   /* 探活；小核回 ACK */
#define LCD_CMD_ACK         0x45   /* 小核 -> 大核 的回执 cmd_id (仅 INIT/PING 用) */
#define LCD_CMD_MON         0x46   /* 大核 -> 小核 送 CPU 占用率；小核用【同一个
                                    * cmd_id】回执, 并在同一个 param_ptr 里带回
                                    * 自己的数据。
                                    *
                                    * 回执的 cmd_id 必须与请求一致: Linux 驱动
                                    * cv181x_rtos_cmdqu 的 rtos_irq_handler 是按
                                    * 回执的 (ip_id, cmd_id, block) 去 wait_list 里
                                    * 匹配来唤醒 SEND_WAIT 的; 改成别的 cmd_id 会
                                    * 匹配不上, 大核只会等到 "SEND_WAIT timeout"。 */
#define LCD_CMD_RTOS_RELOAD 0x47   /* 大核: 让小核跳到常驻热更跳板, 之后大核原地
                                    * 覆盖 0x8FE00000..0x8FF90000 的应用区, 跳板
                                    * 再做 cache 失效并跳回 _start。见 hotjump.S。
                                    * 小核不回执(它随即停机)。 */

/* ---- 小核固件热更握手 (实现见 task/comm/src/riscv64/hotjump.S) --------
 *   ctrl.reload     @ LCD_RELOAD_REQ_ADDR : 大核 -> 小核。
 *                   写 LCD_RELOAD_MAGIC 表示"新固件已写入 0x8FE00000, 起跳"。
 *   stat.reload_ack @ LCD_RELOAD_ACK_ADDR : 小核 -> 大核。
 *                   1 = 小核已停在跳板, 大核可以开始覆盖应用区。
 *
 * hotjump.S 里用的是同一组数字的 .equ 版本, 改动时两处必须同时改。 */
#define LCD_RELOAD_REQ_ADDR  (LCD_SHM_PHYS + 40)         /* ctrl.reload     */
#define LCD_RELOAD_ACK_ADDR  (LCD_SHM_PHYS + 64 + 24)    /* stat.reload_ack */
#define LCD_RELOAD_MAGIC     0x484F544AUL                /* 'HOTJ' */

/* ---- LCD_CMD_MON 的 param_ptr 打包 ---------------------------------- *
 * cmdqu_t.param_ptr 只有 32 位, 两端都按下面的位段约定收发。 */
/* 下行 大核 -> 小核: [7:0] 大核占用率%, [31:8] heartbeat(每采样 +1, 0 = 没在跑) */
#define LCD_MON_TX(pct, hb)  ((((uint32_t)(hb) & 0xFFFFFFu) << 8) | ((uint32_t)(pct) & 0xFFu))
#define LCD_MON_TX_PCT(p)    ((uint32_t)((p) & 0xFFu))
#define LCD_MON_TX_HB(p)     ((uint32_t)((p) >> 8))
/* 上行 小核 -> 大核: [15:0] 小核占用率%, [31:16] 帧率 ×10 (保留一位小数) */
#define LCD_MON_RX(pct, fps10) ((((uint32_t)(fps10) & 0xFFFFu) << 16) | ((uint32_t)(pct) & 0xFFFFu))
#define LCD_MON_RX_PCT(p)    ((uint32_t)((p) & 0xFFFFu))
#define LCD_MON_RX_FPS10(p)  ((uint32_t)((p) >> 16))

/* ---- 小核运行状态（stat.state）-------------------------------------- */
#define LCD_STATE_BOOT      0u
#define LCD_STATE_READY     1u
#define LCD_STATE_BUSY      2u
#define LCD_STATE_ERROR     3u

/* ---- 控制线：大核写 / 小核只读（64 字节）---------------------------- */
struct lcd_shm_ctrl {
	volatile uint32_t magic;      /*  0: LCD_SHM_MAGIC */
	volatile uint32_t version;    /*  4: LCD_SHM_VERSION */
	volatile uint32_t seq;        /*  8: 大核每提交一帧 +1 */
	volatile uint32_t x0;         /* 12: 刷新矩形（含端点） */
	volatile uint32_t y0;         /* 16 */
	volatile uint32_t x1;         /* 20 */
	volatile uint32_t y1;         /* 24 */
	volatile uint32_t bl;         /* 28: 背光 0/1 */
	volatile uint32_t color;      /* 32: FILL 用的 RGB565（主机字节序） */
	volatile uint32_t flags;      /* 36: bit0=1 强制整屏 */
	volatile uint32_t reload;     /* 40: 热更握手, 写 LCD_RELOAD_MAGIC 起跳 */
	volatile uint32_t reserved[5];/* 44..63: 补齐到 64B */
};

/* ---- 状态线：小核写 / 大核只读（64 字节）---------------------------- */
struct lcd_shm_stat {
	volatile uint32_t state;      /* 40->0: LCD_STATE_* */
	volatile uint32_t ack_seq;    /*  4: 小核已完成的 seq */
	volatile uint32_t frame_cnt;  /*  8: 小核累计刷屏次数 */
	volatile uint32_t last_ticks; /* 12: 上一帧耗时（FreeRTOS tick，见
	                               *     configTICK_RATE_HZ：现为 1000Hz => 1ms/tick） */
	volatile uint32_t last_bytes; /* 16: 上一帧推给 SPI 的字节数 */
	volatile uint32_t err;        /* 20: 出错计数 */
	volatile uint32_t reload_ack; /* 24: 热更握手, 1=小核已停在跳板 */
	volatile uint32_t reserved[9];/* 28..63: 补齐到 64B */
};

struct lcd_shm {
	struct lcd_shm_ctrl ctrl;     /*   0 */
	struct lcd_shm_stat stat;     /*  64 */
	uint8_t  fb[LCD_FB_BYTES];    /* 128 */
};

#endif /* __LCD_SHM_H__ */
