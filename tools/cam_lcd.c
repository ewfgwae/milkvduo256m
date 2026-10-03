/*
 * cam_lcd.c —— 把摄像头画面送到 ST7789 屏（经小核 LVGL 显示）
 *
 * 数据流:
 *   GC2083 (1920x1080)
 *     -> VI pipe/chn            (PIXEL_FORMAT_NV21 = VI_PIXEL_FORMAT)
 *     -> VPSS Grp0 Chn0         (硬件缩放到 240x240, 自动取中间区域)
 *     -> CVI_VPSS_GetChnFrame   抓帧
 *     -> NV21 -> RGB565(大端)   (CPU 转换; 硬件不做这个)
 *     -> 共享内存 fb 区         (LCD_SHM_PHYS + 128)
 *     -> mailbox LCD_CMD_CAM    通知小核
 *     -> 小核 LVGL 显示
 *
 * 用法:
 *   cam_lcd                     显示到屏(默认)
 *   cam_lcd --dump out.ppm      只抓一帧存 PPM 就退出(验证抓帧, 不碰屏)
 *   cam_lcd --frames N          送 N 帧后退出
 *
 * 注意: 同一时刻只能有一个进程用 VI/VPSS。跑本程序前先确认没有
 *       sample_vi_fd / camera-test.sh 在跑, 否则 VPSS 会报 0xc0068004(已存在)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include "lcd_shm.h"

#include <cvi_comm.h>
#include <cvi_sys.h>
#include <cvi_vi.h>
#include <sample_comm.h>
#include <core/utils/vpss_helper.h>

/* mailbox ioctl（与 lcd_sender.c 一致） */
typedef struct {
	unsigned char  ip_id;
	unsigned char  cmd_id;
	unsigned short block;
	unsigned int   param_ptr;
} cmdqu_t;
#ifndef RTOS_CMDQU_SEND
#define RTOS_CMDQU_SEND  _IOW('R', 0x10, cmdqu_t)
#endif

/* 屏幕上显示的画面尺寸 = 摄像头画面中间那块 */
#define CAM_W   240
#define CAM_H   240
#define CAM_BYTES (CAM_W * CAM_H * 2)

static volatile int g_exit;

static void on_sig(int s) { (void)s; g_exit = 1; }

static void die(const char *m) { perror(m); exit(1); }

/* ==================== 共享内存 ==================== */
static struct lcd_shm *g_shm;
static int g_memfd = -1;

static void shm_map(void)
{
	void *p;

	g_memfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (g_memfd < 0)
		die("open /dev/mem");
	p = mmap(NULL, LCD_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED,
		 g_memfd, (off_t)LCD_SHM_PHYS);
	if (p == MAP_FAILED)
		die("mmap /dev/mem");
	g_shm = (struct lcd_shm *)p;
	g_shm->ctrl.magic   = LCD_SHM_MAGIC;
	g_shm->ctrl.version = LCD_SHM_VERSION;
}

static int cmd_send(int cmd_id, unsigned int param)
{
	cmdqu_t c;
	int fd = open("/dev/cvi-rtos-cmdqu", O_WRONLY);

	if (fd < 0) {
		perror("open /dev/cvi-rtos-cmdqu");
		return -1;
	}
	memset(&c, 0, sizeof(c));
	c.ip_id     = 6;                 /* IP_SYSTEM */
	c.cmd_id    = (unsigned char)cmd_id;
	c.param_ptr = param;
	if (ioctl(fd, RTOS_CMDQU_SEND, &c) < 0) {
		perror("ioctl RTOS_CMDQU_SEND");
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/* ==================== NV21 -> RGB565 ==================== */
/* NV21: Y 平面 + 交错的 VU 平面(注意是 V 在前! NV12 才是 U 在前) */
static inline uint16_t yuv2rgb565(int y, int u, int v)
{
	int c = y - 16;
	int d = u - 128;
	int e = v - 128;
	int r, g, b;

	if (c < 0)
		c = 0;

	r = (298 * c + 409 * e + 128) >> 8;
	g = (298 * c - 100 * d - 208 * e + 128) >> 8;
	b = (298 * c + 516 * d + 128) >> 8;

	if (r < 0) r = 0; else if (r > 255) r = 255;
	if (g < 0) g = 0; else if (g > 255) g = 255;
	if (b < 0) b = 0; else if (b > 255) b = 255;

	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void nv21_to_rgb565_be(const VIDEO_FRAME_S *f, uint8_t *dst, int stride)
{
	const uint8_t *yp  = f->pu8VirAddr[0];
	const uint8_t *uvp0 = f->pu8VirAddr[1];
	uint32_t ys  = f->u32Stride[0];
	uint32_t uvs = f->u32Stride[1];
	int x, y;

	printf("帧: %ux%u  stride Y=%u UV=%u\n",
	       f->u32Width, f->u32Height, ys, uvs);

	for (y = 0; y < CAM_H; y++) {
		const uint8_t *yrow = yp + (size_t)y * ys;
		const uint8_t *uvrow = uvp0 + (size_t)(y / 2) * uvs;
		uint8_t *drow = dst + (size_t)y * stride;

		for (x = 0; x < CAM_W; x++) {
			/* NV21: 偶数字节是 V, 奇数字节是 U */
			int V = uvrow[(x / 2) * 2];
			int U = uvrow[(x / 2) * 2 + 1];
			uint16_t c = yuv2rgb565(yrow[x], U, V);

			drow[x * 2]     = (uint8_t)(c >> 8);      /* 大端 */
			drow[x * 2 + 1] = (uint8_t)(c & 0xFF);
		}
	}
}

/* ==================== 摄像头初始化 ==================== */
static SAMPLE_VI_CONFIG_S g_vi_cfg;
static VPSS_GRP g_grp = 0;
static VPSS_CHN g_chn = VPSS_CHN0;
static VI_PIPE  g_pipe;
static VI_CHN   g_vichn;

static int cam_init(void)
{
	CVI_S32 ret;
	PIC_SIZE_E enPicSize;
	SIZE_S stSize;
	VB_CONFIG_S stVbConf;
	SAMPLE_INI_CFG_S stIniCfg;
	VI_PIPE ViPipe;
	VI_CHN ViChn;
	VI_DEV ViDev = 0;
	VI_PIPE_ATTR_S stPipeAttr;
	VPSS_GRP_ATTR_S stGrpAttr;
	VPSS_CHN_ATTR_S stChnAttr;
	MMF_CHN_S src, dst;

	/* 注: TDL 样例里的 CVI_MSG_Init/Deinit 在 Duo256M(非 CONFIG_DUAL_OS) 下是空函数
	 * (middleware_utils.c 里直接 return 0), 这里不需要调用。 */

	memset(&stIniCfg, 0, sizeof(stIniCfg));

	printf("[1] sizeof(SAMPLE_INI_CFG_S)=%zu sizeof(SAMPLE_VI_CONFIG_S)=%zu\n",
	       sizeof(SAMPLE_INI_CFG_S), sizeof(g_vi_cfg));
	fflush(stdout);
	if (SAMPLE_COMM_VI_ParseIni(&stIniCfg) != CVI_SUCCESS) {
		printf("解析 /mnt/data/sensor_cfg.ini 失败\n");
		return -1;
	}
	printf("[2] ParseIni OK, enSnsType[0]=%d devNum=%d\n",
	       (int)stIniCfg.enSnsType[0], stIniCfg.devNum);
	fflush(stdout);
	SAMPLE_COMM_VI_GetSensorInfo(&g_vi_cfg);
	printf("[3] GetSensorInfo OK\n");
	fflush(stdout);
	CVI_VI_SetDevNum(stIniCfg.devNum);

	ret = SAMPLE_COMM_VI_IniToViCfg(&stIniCfg, &g_vi_cfg);
	printf("[4] IniToViCfg ret=%#x workingViNum=%d snsType=%d ViChn=%d\n",
	       ret, g_vi_cfg.s32WorkingViNum, (int)g_vi_cfg.astViInfo[0].stSnsInfo.enSnsType,
	       (int)g_vi_cfg.astViInfo[0].stChnInfo.ViChn);
	fflush(stdout);
	if (ret != CVI_SUCCESS) { printf("IniToViCfg 失败 %#x\n", ret); return -1; }

	ret = SAMPLE_COMM_VI_GetSizeBySensor(g_vi_cfg.astViInfo[0].stSnsInfo.enSnsType,
					    &enPicSize);
	printf("[5] GetSizeBySensor ret=%#x\n", ret);
	fflush(stdout);
	if (ret != CVI_SUCCESS) { printf("GetSizeBySensor 失败 %#x\n", ret); return -1; }
	ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &stSize);
	if (ret != CVI_SUCCESS) { printf("GetPicSize 失败 %#x\n", ret); return -1; }
	printf("传感器分辨率: %ux%u\n", stSize.u32Width, stSize.u32Height);

	ViPipe = g_vi_cfg.astViInfo[0].stPipeInfo.aPipe[0];
	ViChn  = g_vi_cfg.astViInfo[0].stChnInfo.ViChn;
	g_pipe = ViPipe;
	g_vichn = ViChn;

	/* ---- SYS + VB ----
	 * 两个池:
	 *   pool0 给 VI 用(全分辨率 NV21)
	 *   pool1 ★必须绑到 VPSS 通道上, 否则 GetChnFrame 一直返回
	 *         0xc006800e (EN_ERR_BUF_EMPTY) —— 通道拿不到输出缓冲。
	 *   绑法: VB_CONFIG_S 里没有"绑通道"的字段, 要用 SAMPLE_COMM_SYS_Init
	 *   之后再 CVI_VPSS_AttachVbPool(grp, chn, poolId)。 */
	memset(&stVbConf, 0, sizeof(stVbConf));
	stVbConf.u32MaxPoolCnt = 2;
	stVbConf.astCommPool[0].u32BlkSize =
		COMMON_GetPicBufferSize(stSize.u32Width, stSize.u32Height,
					VI_PIXEL_FORMAT, DATA_BITWIDTH_8,
					COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	stVbConf.astCommPool[0].u32BlkCnt = 4;
	stVbConf.astCommPool[1].u32BlkSize =
		COMMON_GetPicBufferSize(CAM_W, CAM_H,
					VI_PIXEL_FORMAT, DATA_BITWIDTH_8,
					COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	stVbConf.astCommPool[1].u32BlkCnt = 4;
	printf("VB: VI池 blk=%u x4, VPSS池 blk=%u x4\n",
	       stVbConf.astCommPool[0].u32BlkSize,
	       stVbConf.astCommPool[1].u32BlkSize);

	ret = SAMPLE_COMM_SYS_Init(&stVbConf);
	if (ret != CVI_SUCCESS) { printf("SYS_Init 失败 %#x\n", ret); return -1; }

	/* VI/VPSS 模式 —— 照 TDL sample_vi_fd 的可用配置:
	 * aenInput[0]=VPSS_INPUT_ISP + ViPipe[0]=0  -> VPSS dev0 吃 ISP
	 * aenInput[1]=VPSS_INPUT_MEM                -> VPSS dev1 吃内存(本工具不用)
	 * VPSS_MODE_DUAL 下 dev1 才有 3 路输出, 所以组的 u8VpssDev 用 1。 */
	{
		VI_VPSS_MODE_S m;
		VPSS_MODE_S vm;
		memset(&m, 0, sizeof(m));
		m.aenMode[0] = VI_OFFLINE_VPSS_ONLINE;
		CVI_SYS_SetVIVPSSMode(&m);
		memset(&vm, 0, sizeof(vm));
		vm.enMode = VPSS_MODE_DUAL;
		vm.aenInput[0] = VPSS_INPUT_ISP;
		vm.ViPipe[0] = 0;
		vm.aenInput[1] = VPSS_INPUT_MEM;
		vm.ViPipe[1] = 0;
		CVI_SYS_SetVPSSModeEx(&vm);
	}

	/* ---- VI ---- */
	ret = SAMPLE_COMM_VI_StartSensor(&g_vi_cfg);
	if (ret != CVI_SUCCESS) { printf("StartSensor 失败 %#x\n", ret); return -1; }
	ret = SAMPLE_COMM_VI_StartDev(&g_vi_cfg.astViInfo[ViDev]);
	if (ret != CVI_SUCCESS) { printf("StartDev 失败 %#x\n", ret); return -1; }
	ret = SAMPLE_COMM_VI_StartMIPI(&g_vi_cfg);
	if (ret != CVI_SUCCESS) { printf("StartMIPI 失败 %#x\n", ret); return -1; }

	memset(&stPipeAttr, 0, sizeof(stPipeAttr));
	stPipeAttr.bYuvSkip = CVI_FALSE;
	stPipeAttr.u32MaxW = stSize.u32Width;
	stPipeAttr.u32MaxH = stSize.u32Height;
	stPipeAttr.enPixFmt = PIXEL_FORMAT_RGB_BAYER_12BPP;
	stPipeAttr.enBitWidth = DATA_BITWIDTH_12;
	stPipeAttr.stFrameRate.s32SrcFrameRate = -1;
	stPipeAttr.stFrameRate.s32DstFrameRate = -1;
	stPipeAttr.bNrEn = CVI_TRUE;
	stPipeAttr.bYuvBypassPath = CVI_FALSE;
	stPipeAttr.enCompressMode = g_vi_cfg.astViInfo[0].stChnInfo.enCompressMode;

	ret = CVI_VI_CreatePipe(ViPipe, &stPipeAttr);
	if (ret != CVI_SUCCESS) { printf("VI_CreatePipe 失败 %#x\n", ret); return -1; }
	ret = CVI_VI_StartPipe(ViPipe);
	if (ret != CVI_SUCCESS) { printf("VI_StartPipe 失败 %#x\n", ret); return -1; }
	ret = SAMPLE_COMM_VI_CreateIsp(&g_vi_cfg);
	if (ret != CVI_SUCCESS) { printf("VI_CreateIsp 失败 %#x\n", ret); return -1; }
	ret = SAMPLE_COMM_VI_StartViChn(&g_vi_cfg);
	if (ret != CVI_SUCCESS) { printf("VI_StartViChn 失败 %#x\n", ret); return -1; }
	printf("VI 已启动 (pipe=%d chn=%d)\n", ViPipe, ViChn);

	/* ---- VPSS: 组 + 240x240 通道 ---- */
	memset(&stGrpAttr, 0, sizeof(stGrpAttr));
	stGrpAttr.stFrameRate.s32SrcFrameRate = -1;
	stGrpAttr.stFrameRate.s32DstFrameRate = -1;
	stGrpAttr.enPixelFormat = VI_PIXEL_FORMAT;
	stGrpAttr.u32MaxW = stSize.u32Width;
	stGrpAttr.u32MaxH = stSize.u32Height;
	stGrpAttr.u8VpssDev = 0;      /* 用 dev0(吃 ISP); dev1 是吃内存的 */

	ret = CVI_VPSS_CreateGrp(g_grp, &stGrpAttr);
	if (ret != CVI_SUCCESS) { printf("VPSS_CreateGrp 失败 %#x\n", ret); return -1; }
	ret = CVI_VPSS_ResetGrp(g_grp);
	if (ret != CVI_SUCCESS) { printf("VPSS_ResetGrp 失败 %#x\n", ret); return -1; }

	memset(&stChnAttr, 0, sizeof(stChnAttr));
	stChnAttr.u32Width  = CAM_W;
	stChnAttr.u32Height = CAM_H;
	stChnAttr.enVideoFormat = VIDEO_FORMAT_LINEAR;
	stChnAttr.enPixelFormat = VI_PIXEL_FORMAT;
	stChnAttr.stFrameRate.s32SrcFrameRate = -1;
	stChnAttr.stFrameRate.s32DstFrameRate = -1;
	stChnAttr.u32Depth = 2;
	stChnAttr.bMirror = CVI_FALSE;
	stChnAttr.bFlip = CVI_FALSE;

	ret = CVI_VPSS_SetChnAttr(g_grp, g_chn, &stChnAttr);
	if (ret != CVI_SUCCESS) { printf("VPSS_SetChnAttr 失败 %#x\n", ret); return -1; }

	/* ★ 把 pool1 绑到本通道 —— 不绑的话 GetChnFrame 永远 EN_ERR_BUF_EMPTY */
	ret = CVI_VPSS_AttachVbPool(g_grp, g_chn, 1);
	if (ret != CVI_SUCCESS) { printf("VPSS_AttachVbPool 失败 %#x\n", ret); return -1; }
	printf("VPSS 通道已绑定 VB pool 1\n");

	ret = CVI_VPSS_EnableChn(g_grp, g_chn);
	if (ret != CVI_SUCCESS) { printf("VPSS_EnableChn 失败 %#x\n", ret); return -1; }

	src.enModId = CVI_ID_VI;    src.s32DevId = ViPipe; src.s32ChnId = ViChn;
	dst.enModId = CVI_ID_VPSS;  dst.s32DevId = g_grp;  dst.s32ChnId = g_chn;
	ret = CVI_SYS_Bind(&src, &dst);
	if (ret != CVI_SUCCESS) { printf("SYS_Bind(VI->VPSS) 失败 %#x\n", ret); return -1; }

	ret = CVI_VPSS_StartGrp(g_grp);
	if (ret != CVI_SUCCESS) { printf("VPSS_StartGrp 失败 %#x\n", ret); return -1; }

	printf("VPSS 已启动 (grp=%d chn=%d, %dx%d)\n", g_grp, g_chn, CAM_W, CAM_H);
	return 0;
}

/* ==================== 主流程 ==================== */
int main(int argc, char **argv)
{
	static uint8_t rgb[CAM_BYTES] __attribute__((aligned(64)));
	int do_dump = 0, max_frames = 0, i, n = 0;
	const char *ppm_path = NULL;
	time_t t0 = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
			do_dump = 1; ppm_path = argv[++i];
		} else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
			max_frames = atoi(argv[++i]);
		} else {
			printf("用法: %s [--dump out.ppm] [--frames N]\n", argv[0]);
			return 1;
		}
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	printf("=== cam_lcd: 摄像头 -> ST7789(%dx%d 屏幕) ===\n", LCD_W, LCD_H);

	if (cam_init() != 0) {
		printf("摄像头初始化失败\n");
		return 1;
	}

	if (!do_dump)
		shm_map();

	while (!g_exit) {
		VIDEO_FRAME_INFO_S fi;
		CVI_S32 ret = CVI_VPSS_GetChnFrame(g_grp, g_chn, &fi, 2000);

		if (ret != CVI_SUCCESS) {
			printf("GetChnFrame 失败 %#x\n", ret);
			usleep(200000);
			continue;
		}

		nv21_to_rgb565_be(&fi.stVFrame, rgb, CAM_W * 2);
		CVI_VPSS_ReleaseChnFrame(g_grp, g_chn, &fi);

		if (do_dump) {
			FILE *fp = fopen(ppm_path, "wb");
			if (!fp) die("fopen");
			fprintf(fp, "P6\n%d %d\n255\n", CAM_W, CAM_H);
			for (i = 0; i < CAM_H; i++) {
				int x;
				for (x = 0; x < CAM_W; x++) {
					uint16_t c = (uint16_t)((rgb[i * CAM_W * 2 + x * 2] << 8) |
								rgb[i * CAM_W * 2 + x * 2 + 1]);
					uint8_t p[3];
					p[0] = (uint8_t)(((c >> 11) & 0x1F) << 3);
					p[1] = (uint8_t)(((c >> 5) & 0x3F) << 2);
					p[2] = (uint8_t)((c & 0x1F) << 3);
					fwrite(p, 1, 3, fp);
				}
			}
			fclose(fp);
			printf("已保存 %s (%dx%d)\n", ppm_path, CAM_W, CAM_H);
			break;
		}

		/* 画面写进共享内存 fb 区, 竖直居中 */
		memcpy(g_shm->fb, rgb, CAM_BYTES);
		g_shm->ctrl.x0 = 0;
		g_shm->ctrl.y0 = (LCD_H - CAM_H) / 2;
		g_shm->ctrl.x1 = CAM_W - 1;
		g_shm->ctrl.y1 = g_shm->ctrl.y0 + CAM_H - 1;
		g_shm->ctrl.flags = 0;
		__sync_synchronize();
		g_shm->ctrl.seq++;
		cmd_send(LCD_CMD_CAM, 0);

		if (n == 0)
			t0 = time(NULL);
		n++;

		if ((n % 30) == 0) {
			time_t dt = time(NULL) - t0;
			printf("已送 %d 帧  %.1f fps  ack=%u err=%u\n",
			       n, dt ? (double)n / (double)dt : 0.0,
			       g_shm->stat.ack_seq, g_shm->stat.err);
		}
		if (max_frames && n >= max_frames)
			break;
	}

	printf("退出, 共送 %d 帧\n", n);
	return 0;
}
