#ifndef __LVGL_TASK_H__
#define __LVGL_TASK_H__

#include <stdint.h>

/* 启动小核(C906L)上的 LVGL 任务 —— 屏幕的唯一所有者。
 * 由 comm_main.c 的 main_cvirtos() 在 vTaskStartScheduler() 之前调用。 */
void lvgl_task_start(void);

/* ---- mailbox 通道：大核 <-> 小核 交换监控量 -------------------------- *
 * 大核的 lcd_sender mon 每采一次 /proc/stat 就发一条 LCD_CMD_MON
 * (见 lcd_shm.h 的 LCD_MON_TX_*)，comm_main.c 的 cmdqu 任务收到后调
 * lvgl_mon_set() 存下来，屏上的 OSD 直接取用（不再读共享内存）。 */
void lvgl_mon_set(uint32_t big_pct, uint32_t heartbeat);

/* OSD 取大核的监控量；返回 0 表示大核还没开始采样(heartbeat 为 0)。 */
int  lvgl_mon_get(uint32_t *big_pct, uint32_t *heartbeat);

/* 回执给大核的数字：小核占用率(%) 与 帧率×10。 */
void lvgl_mon_reply(uint32_t *small_pct, uint32_t *fps_x10);

/* ---- 画面模式(摄像头) ------------------------------------------------- *
 * comm_main.c 的 cmdqu 任务收到 LCD_CMD_CAM 时调用。只置一个标志、给帧号 +1,
 * 不碰任何 LVGL 对象 —— LVGL 不是线程安全的, 渲染统一在 LVGL 任务里做
 * (见 lvgl_task.c 的 lvgl_cam_poll())。**不要回 ACK**。 */
void lvgl_cam_submit(void);

#endif /* __LVGL_TASK_H__ */
