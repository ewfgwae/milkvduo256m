/*
 * camprobe.c —— Duo256M CAM-GC2083 硬件在环探测
 * 编译: aarch64-linux-gnu-gcc -O2 -static -o camprobe camprobe.c
 * 用法: 板上 ./camprobe
 * 干的事:
 *   1) 扫 /dev/i2c-0..4, 每个地址都探测, 找任何应答的从机
 *   2) 对 i2c-2 的 0x37 用多种方式(quick / read byte / read word / write)各试一遍
 *   3) 直接读 GPIOC 寄存器, 报告传感器复位脚(XGPIOC_17)的方向与电平
 *   4) 把复位脚脉冲一次(拉低->拉高), 再扫一遍
 *   5) 读 I2C2 控制器寄存器, 确认控制器状态
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdint.h>

#define I2C2_BASE   0x04020000UL
#define FMUX_BASE   0x03001000UL
#define GPIOC_BASE  0x03022000UL

static int mem_fd = -1;

static volatile uint32_t *map_reg(unsigned long phys)
{
	unsigned long base = phys & ~0xFFFUL;
	unsigned long off  = phys - base;
	void *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, base);
	if (p == MAP_FAILED) {
		perror("mmap");
		return NULL;
	}
	return (volatile uint32_t *)((char *)p + off);
}

/* 自己实现 SMBus 传输, 不依赖 libi2c */
static int smbus_io(int fd, char rw, uint8_t cmd, int size, union i2c_smbus_data *data)
{
	struct i2c_smbus_ioctl_data args;
	args.read_write = rw;
	args.command    = cmd;
	args.size       = size;
	args.data       = data;
	return ioctl(fd, I2C_SMBUS, &args);
}

static int probe_addr(int fd, int addr, const char *how)
{
	union i2c_smbus_data data;
	int r;
	memset(&data, 0, sizeof(data));
	if (!strcmp(how, "quick")) {
		r = smbus_io(fd, I2C_SMBUS_WRITE, 0, I2C_SMBUS_QUICK, NULL);
	} else if (!strcmp(how, "readbyte")) {
		r = smbus_io(fd, I2C_SMBUS_READ, 0, I2C_SMBUS_BYTE, &data);
		if (r == 0) r = data.byte;
	} else if (!strcmp(how, "readword")) {
		r = smbus_io(fd, I2C_SMBUS_READ, 0x00, I2C_SMBUS_WORD_DATA, &data);
		if (r == 0) r = data.word;
	} else if (!strcmp(how, "writebyte0")) {
		data.byte = 0x00;
		r = smbus_io(fd, I2C_SMBUS_WRITE, 0, I2C_SMBUS_BYTE_DATA, &data);
	} else {
		r = -999;
	}
	return r;
}

static int scan(const char *dev)
{
	int fd, addr, found = 0;
	fd = open(dev, O_RDWR);
	if (fd < 0) { printf("  打不开 %s: %s\n", dev, strerror(errno)); return -1; }
	printf("  扫 %s (quick write):\n   ", dev);
	for (addr = 0x03; addr <= 0x77; addr++) {
		if (ioctl(fd, I2C_SLAVE, addr) < 0) continue;
		if (probe_addr(fd, addr, "quick") >= 0) {
			printf(" [0x%02X 应答]", addr);
			found++;
		}
	}
	printf("\n");
	if (!found) printf("   -> 该总线无任何器件应答\n");
	close(fd);
	return found;
}

static void gpio_report(void)
{
	volatile uint32_t *dr  = map_reg(GPIOC_BASE + 0x18); /* SWPORTC_DR  */
	volatile uint32_t *ddr = map_reg(GPIOC_BASE + 0x1C); /* SWPORTC_DDR */
	if (!dr || !ddr) return;
	uint32_t d = *dr, e = *ddr;
	printf("  GPIOC SWPORTC_DR  = 0x%08X   (bit17=%d)\n", d, (d >> 17) & 1);
	printf("  GPIOC SWPORTC_DDR = 0x%08X   (bit17=%d, 1=输出)\n", e, (e >> 17) & 1);
	printf("  => 传感器 RESET(XGPIOC_17, 低有效): %s\n",
	       ((d >> 17) & 1) ? "已释放(高, 正常)" : "处于复位(低)!");
}

static void gpio_pulse_reset(void)
{
	volatile uint32_t *dr  = map_reg(GPIOC_BASE + 0x18);
	volatile uint32_t *ddr = map_reg(GPIOC_BASE + 0x1C);
	if (!dr || !ddr) return;
	uint32_t e = *ddr;
	*ddr = e | (1u << 17);              /* 设为输出 */
	uint32_t d = *dr;
	*dr = d & ~(1u << 17);              /* 拉低 = 复位 */
	usleep(50 * 1000);
	*dr = d | (1u << 17);               /* 拉高 = 释放 */
	usleep(100 * 1000);
	printf("  已脉冲复位(低 50ms -> 高), 现在 DR=0x%08X DDR=0x%08X\n", *dr, *ddr);
}

static void reg_dump(void)
{
	unsigned long regs[] = {0x00, 0x04, 0x6C, 0x70};
	const char *names[] = {"IC_CON", "IC_TAR", "IC_ENABLE", "IC_STATUS"};
	printf("  I2C2 控制器 @0x%lX:\n", I2C2_BASE);
	for (int i = 0; i < 4; i++) {
		volatile uint32_t *r = map_reg(I2C2_BASE + regs[i]);
		if (r) printf("    %-10s = 0x%08X\n", names[i], *r);
	}
	printf("  FMUX(摄像头那几个 pad):\n");
	unsigned long pads[] = {0x1AC, 0x1B0, 0x1B8, 0x1A8};
	const char *pn[] = {"PAD_MIPI_TXM1/IIC2_SDA", "PAD_MIPI_TXP1/IIC2_SCL",
	                    "PAD_MIPI_TXP0/CAM_MCLK0", "PAD_MIPI_TXP2/RESET"};
	for (int i = 0; i < 4; i++) {
		volatile uint32_t *r = map_reg(FMUX_BASE + pads[i]);
		if (r) printf("    %-26s = 0x%08X\n", pn[i], *r);
	}
}

int main(void)
{
	printf("======== Duo256M 摄像头硬件在环探测 ========\n\n");

	mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (mem_fd < 0) { perror("open /dev/mem"); return 1; }

	printf("[1] 引脚复用 / 控制器寄存器\n");
	reg_dump();

	printf("\n[2] 传感器复位脚状态\n");
	gpio_report();

	printf("\n[3] 扫描所有 I2C 总线\n");
	for (int b = 0; b <= 4; b++) {
		char dev[32];
		snprintf(dev, sizeof(dev), "/dev/i2c-%d", b);
		if (access(dev, F_OK)) continue;
		scan(dev);
	}

	printf("\n[4] 对 i2c-2 的 0x37 用多种方式确认\n");
	int fd = open("/dev/i2c-2", O_RDWR);
	if (fd >= 0) {
		if (ioctl(fd, I2C_SLAVE, 0x37) == 0) {
			const char *ways[] = {"quick", "readbyte", "readword", "writebyte0"};
			for (int i = 0; i < 4; i++) {
				errno = 0;
				int r = probe_addr(fd, 0x37, ways[i]);
				printf("    %-11s -> ret=%d errno=%d(%s)\n",
				       ways[i], r, errno, strerror(errno));
			}
		} else {
			printf("    I2C_SLAVE 0x37 失败: %s\n", strerror(errno));
		}
		close(fd);
	} else {
		printf("    打不开 /dev/i2c-2: %s\n", strerror(errno));
	}

	printf("\n[5] 脉冲复位脚后再扫 i2c-2\n");
	gpio_pulse_reset();
	scan("/dev/i2c-2");

	printf("\n[6] 复位后 0x37 再试一次\n");
	fd = open("/dev/i2c-2", O_RDWR);
	if (fd >= 0) {
		ioctl(fd, I2C_SLAVE, 0x37);
		errno = 0;
		int r = probe_addr(fd, 0x37, "readbyte");
		printf("    read_byte -> ret=%d errno=%d(%s)\n", r, errno, strerror(errno));
		close(fd);
	}

	printf("\n======== 探测结束 ========\n");
	return 0;
}
