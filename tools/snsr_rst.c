/*
 * snsr_rst.c —— Duo256M 摄像头传感器复位脚修复模块
 *
 * 背景(2026-09-30 实测):
 *   Duo256M 的 CAM-GC2083 传感器 RESET 接在 PAD_MIPI_TXP2 = XGPIOC_17
 *   (gpiochip2 全局号 433, 低有效)。cvi_mipi_rx.ko(cif) 的 _init_resource()
 *   里对 "snsr-reset" 的 gpio_request() 失败(或没执行到), 结果这根脚
 *   从头到尾没被设成输出、一直是 0, 传感器被按在复位态, I2C 完全不响应:
 *       i2cdetect -y -r 2   ->  看不到 0x37
 *       camera-test.sh      ->  vi init failed. s32Ret: 0xffffffff
 *   用 sysfs 把 gpio-433 拉高后, 0x37 立刻出现、摄像头测试正常出推理结果。
 *
 * 本模块就干一件事: 启动时把复位脚拉高(释放复位), 卸载时拉低。
 *
 * 编译(对着目标内核树):
 *   make -C <kernel> O=<build> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=<dir> modules
 * 加载: insmod snsr_rst.ko       (可用 gpio=433 覆盖)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/init.h>

static int gpio = 433;          /* XGPIOC_17 = gpiochip2(416) + 17 */
static bool active_low = true;  /* 低有效: 高=释放复位 */
module_param(gpio, int, 0444);
module_param(active_low, bool, 0444);
MODULE_PARM_DESC(gpio, "sensor reset GPIO number (default 433 = XGPIOC_17)");
MODULE_PARM_DESC(active_low, "reset is active-low (default 1)");

static int snsr_rst_init(void)
{
	int val, rc;

	rc = gpio_request(gpio, "snsr-rst-fix");
	if (rc) {
		pr_err("snsr_rst: gpio_request(%d) 失败: %d\n", gpio, rc);
		return rc;
	}

	/* 高有效: 想让复位“释放”, 就要输出 active_low ? 1 : 0 */
	val = active_low ? 1 : 0;

	rc = gpio_direction_output(gpio, val);
	if (rc) {
		pr_err("snsr_rst: gpio_direction_output(%d,%d) 失败: %d\n",
		       gpio, val, rc);
		gpio_free(gpio);
		return rc;
	}

	pr_info("snsr_rst: 传感器复位脚 gpio-%d 已拉高(释放复位), active_low=%d\n",
		gpio, active_low);
	return 0;
}

static void snsr_rst_exit(void)
{
	gpio_set_value(gpio, active_low ? 0 : 1);
	gpio_free(gpio);
	pr_info("snsr_rst: 传感器复位脚 gpio-%d 已释放\n", gpio);
}

module_init(snsr_rst_init);
module_exit(snsr_rst_exit);

MODULE_DESCRIPTION("Duo256M CAM-GC2083 sensor reset fix (drive XGPIOC_17 high)");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("(本会话)");
