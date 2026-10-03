/*
 * cam2lcd.c —— 摄像头画面 -> 共享内存 -> 小核 LVGL -> ST7789
 *
 * 初始化**逐项照抄** 板上实测可用的 sample_vi_fd:
 *   CVI_MSG_Init -> SAMPLE_TDL_Get_VI_Config -> GetSizeBySensor
 *   -> 3 个 VB 池(尺寸与所绑通道一致) -> VPSS Grp0 两通道(Chn0 1280x720,
 *      Chn1 240x240, dev=1, DUAL) -> SAMPLE_TDL_Init_WM
 * 然后**不用 TDL 推理**, 直接抓 VPSS_CHN1 的帧 -> NV21 转 RGB565(大端)
 * -> 写共享内存 -> mailbox 发 LCD_CMD_CAM。
 *
 * 用法:
 *   cam2lcd --dump out.ppm   抓一帧存 PPM(验证抓帧, 不碰屏)
 *   cam2lcd [--frames N]     持续送显
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

#include "middleware_utils.h"
#include <core/utils/vpss_helper.h>
#include <cvi_comm.h>
#include <cvi_sys.h>
#include <cvi_vi.h>
#include <sample_comm.h>

typedef struct {
	unsigned char  ip_id;
	unsigned char  cmd_id;
	unsigned short block;
	unsigned int   param_ptr;
} cmdqu_t;
#ifndef RTOS_CMDQU_SEND
#define RTOS_CMDQU_SEND  _IOW('R', 0x10, cmdqu_t)
#endif

#define CAM_W   240
#define CAM_H   240
#define CAM_BYTES (CAM_W * CAM_H * 2)

static volatile int g_exit;
static void on_sig(int s) { (void)s; g_exit = 1; }
static void die(const char *m) { perror(m); exit(1); }

/* ---------------- 共享内存 ---------------- */
static struct lcd_shm *g_shm;
static int g_memfd = -1;

static void shm_map(void)
{
	void *p;

	g_memfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (g_memfd < 0) die("open /dev/mem");
	p = mmap(NULL, LCD_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED,
		 g_memfd, (off_t)LCD_SHM_PHYS);
	if (p == MAP_FAILED) die("mmap /dev/mem");
	g_shm = (struct lcd_shm *)p;
	g_shm->ctrl.magic   = LCD_SHM_MAGIC;
	g_shm->ctrl.version = LCD_SHM_VERSION;
}

static void send_cmd(int cmd_id, unsigned int param)
{
	cmdqu_t c;
	int fd = open("/dev/cvi-rtos-cmdqu", O_WRONLY);

	if (fd < 0) { perror("open cmdqu"); return; }
	memset(&c, 0, sizeof(c));
	c.ip_id = 6; c.cmd_id = (unsigned char)cmd_id; c.param_ptr = param;
	if (ioctl(fd, RTOS_CMDQU_SEND, &c) < 0)
		perror("ioctl RTOS_CMDQU_SEND");
	close(fd);
}

/* ---------------- NV21 -> RGB565(大端) ----------------
 * 源尺寸任意: 取源画面**中心正方形**, 最近邻缩放到 CAM_W x CAM_H。 */
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

static void nv21_to_rgb565_be(const VIDEO_FRAME_S *f, uint8_t *dst, int stride)
{
	const uint8_t *yp = f->pu8VirAddr[0];
	const uint8_t *uvp = f->pu8VirAddr[1];
	uint32_t ys = f->u32Stride[0], uvs = f->u32Stride[1];
	uint32_t sw = f->u32Width, sh = f->u32Height;
	uint32_t side = (sw < sh) ? sw : sh;
	uint32_t sx0 = (sw - side) / 2, sy0 = (sh - side) / 2;
	int x, y;

	for (y = 0; y < CAM_H; y++) {
		uint32_t sy = sy0 + (uint32_t)((uint64_t)y * side / CAM_H);
		const uint8_t *yrow = yp + (size_t)sy * ys;
		const uint8_t *uvrow = uvp + (size_t)(sy / 2) * uvs;
		uint8_t *drow = dst + (size_t)y * stride;

		for (x = 0; x < CAM_W; x++) {
			uint32_t sx = sx0 + (uint32_t)((uint64_t)x * side / CAM_W);
			int V = uvrow[(sx / 2) * 2];       /* NV21: V 在前 */
			int U = uvrow[(sx / 2) * 2 + 1];
			uint16_t c = yuv2rgb565(yrow[sx], U, V);

			drow[x * 2]     = (uint8_t)(c >> 8);
			drow[x * 2 + 1] = (uint8_t)(c & 0xFF);
		}
	}
}

/* ---------------- 初始化(照抄 sample_vi_fd) ---------------- */
static SAMPLE_TDL_MW_CONTEXT g_mw;
static VPSS_CHN g_cap_chn = VPSS_CHN1;
static int g_use_vi_direct = 0;      /* 1 = 直接抓 VI 通道(绕开 VPSS) */
static VI_PIPE g_vi_pipe = 0;
static VI_CHN  g_vi_chn  = 0;

/* ★ 绕开 VPSS 的一条路:
 * 实测发现 VPSS 的 CreateGrp 会被内核判 ILLEGAL_PARAM —— 因为 TDL 的
 * middleware_utils.c 里 `VI_VPSS_MODE_S stVIVPSSMode;` **没有 memset**,
 * 只赋了 aenMode[0], 其余是栈垃圾; 内核侧 stVIVPSSMode 一旦被判成 ONLINE,
 * 而 VPSS dev1 的输入又是 MEM(非 ISP), vpss_create_grp 里的校验就会拒绝。
 * 这里改成: VI 走 OFFLINE 模式把 YUV 帧落到内存, 直接从 VI 通道取帧,
 * 完全不用 VPSS; 缩放到 240x240 由 CPU 做(每帧 5.7 万像素, A53 很轻松)。 */
static int vi_direct_init(void)
{
	VI_VPSS_MODE_S m;
	VI_CHN_ATTR_S chnAttr;
	VI_PIPE_ATTR_S pipeAttr;
	SAMPLE_INI_CFG_S ini;
	PIC_SIZE_E enPicSize;
	SIZE_S stSize;
	CVI_S32 ret;

	memset(&ini, 0, sizeof(ini));
	if (SAMPLE_COMM_VI_ParseIni(&ini) != CVI_SUCCESS) {
		printf("解析 /mnt/data/sensor_cfg.ini 失败\n");
		return -1;
	}
	SAMPLE_COMM_VI_GetSensorInfo(&g_mw.stViConfig);
	CVI_VI_SetDevNum(ini.devNum);
	ret = SAMPLE_COMM_VI_IniToViCfg(&ini, &g_mw.stViConfig);
	if (ret != CVI_SUCCESS) { printf("IniToViCfg 失败 %#x\n", ret); return -1; }
	ret = SAMPLE_COMM_VI_GetSizeBySensor(g_mw.stViConfig.astViInfo[0].stSnsInfo.enSnsType,
					    &enPicSize);
	if (ret != CVI_SUCCESS) { printf("GetSizeBySensor 失败\n"); return -1; }
	ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &stSize);
	if (ret != CVI_SUCCESS) { printf("GetPicSize 失败\n"); return -1; }
	printf("传感器: %ux%u\n", stSize.u32Width, stSize.u32Height);

	g_vi_pipe = g_mw.stViConfig.astViInfo[0].stPipeInfo.aPipe[0];
	g_vi_chn  = g_mw.stViConfig.astViInfo[0].stChnInfo.ViChn;

	/* ★ 必须先 SetVPSSModeEx 把 dev0 设成 MEM, 否则默认 ISP 会跟 VI offline 冲突 */
	memset(&m, 0, sizeof(m));
	m.aenMode[0] = VI_OFFLINE_VPSS_OFFLINE;
	ret = CVI_SYS_SetVIVPSSMode(&m);
	printf("SetVIVPSSMode(OFFLINE) ret=%#x\n", ret);

	/* VI: 起 sensor/dev/mipi/pipe */
	ret = SAMPLE_COMM_VI_StartSensor(&g_mw.stViConfig);
	if (ret != CVI_SUCCESS) { printf("StartSensor 失败 %#x\n", ret); return -1; }
	ret = SAMPLE_COMM_VI_StartDev(&g_mw.stViConfig.astViInfo[0]);
	if (ret != CVI_SUCCESS) { printf("StartDev 失败 %#x\n", ret); return -1; }
	ret = SAMPLE_COMM_VI_StartMIPI(&g_mw.stViConfig);
	if (ret != CVI_SUCCESS) { printf("StartMIPI 失败 %#x\n", ret); return -1; }

	memset(&pipeAttr, 0, sizeof(pipeAttr));
	pipeAttr.bYuvSkip = CVI_FALSE;
	pipeAttr.u32MaxW = stSize.u32Width;
	pipeAttr.u32MaxH = stSize.u32Height;
	pipeAttr.enPixFmt = PIXEL_FORMAT_RGB_BAYER_12BPP;
	pipeAttr.enBitWidth = DATA_BITWIDTH_12;
	pipeAttr.stFrameRate.s32SrcFrameRate = -1;
	pipeAttr.stFrameRate.s32DstFrameRate = -1;
	pipeAttr.bNrEn = CVI_TRUE;
	pipeAttr.bYuvBypassPath = CVI_FALSE;
	ret = CVI_VI_CreatePipe(g_vi_pipe, &pipeAttr);
	if (ret != CVI_SUCCESS) { printf("VI_CreatePipe 失败 %#x\n", ret); return -1; }
	ret = CVI_VI_StartPipe(g_vi_pipe);
	if (ret != CVI_SUCCESS) { printf("VI_StartPipe 失败 %#x\n", ret); return -1; }
	ret = SAMPLE_COMM_VI_CreateIsp(&g_mw.stViConfig);
	if (ret != CVI_SUCCESS) { printf("VI_CreateIsp 失败 %#x\n", ret); return -1; }

	memset(&chnAttr, 0, sizeof(chnAttr));
	chnAttr.enPixelFormat  = VI_PIXEL_FORMAT;      /* NV21 */
	chnAttr.enDynamicRange = DYNAMIC_RANGE_SDR8;
	chnAttr.enVideoFormat  = VIDEO_FORMAT_LINEAR;
	chnAttr.enCompressMode = COMPRESS_MODE_NONE;
	chnAttr.stFrameRate.s32SrcFrameRate = -1;
	chnAttr.stFrameRate.s32DstFrameRate = -1;
	ret = CVI_VI_SetChnAttr(g_vi_pipe, g_vi_chn, &chnAttr);
	if (ret != CVI_SUCCESS) { printf("VI_SetChnAttr 失败 %#x\n", ret); return -1; }
	ret = CVI_VI_EnableChn(g_vi_pipe, g_vi_chn);
	if (ret != CVI_SUCCESS) { printf("VI_EnableChn 失败 %#x\n", ret); return -1; }

	printf("VI 直采已启动 (pipe=%d chn=%d)\n", g_vi_pipe, g_vi_chn);
	return 0;
}

static int pipeline_init(void)
{
	SAMPLE_TDL_MW_CONFIG_S cfg;
	PIC_SIZE_E enPicSize;
	SIZE_S snsSize, chn0Size;
	CVI_S32 ret;

	memset(&cfg, 0, sizeof(cfg));

	ret = SAMPLE_TDL_Get_VI_Config(&cfg.stViConfig);
	if (ret != CVI_SUCCESS || cfg.stViConfig.s32WorkingViNum <= 0) {
		printf("取 VI 配置失败(%s)\n", "/mnt/data/sensor_cfg.ini");
		return -1;
	}
	ret = SAMPLE_COMM_VI_GetSizeBySensor(cfg.stViConfig.astViInfo[0].stSnsInfo.enSnsType,
					    &enPicSize);
	if (ret != CVI_SUCCESS) { printf("GetSizeBySensor 失败\n"); return -1; }
	ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &snsSize);
	if (ret != CVI_SUCCESS) { printf("GetPicSize 失败\n"); return -1; }
	printf("传感器: %ux%u\n", snsSize.u32Width, snsSize.u32Height);

	chn0Size.u32Width = 1280; chn0Size.u32Height = 720;   /* 照抄样例 */

	/* VBPool 0 -> VPSS Grp0 Chn0 */
	cfg.stVBPoolConfig.u32VBPoolCount = 3;
	cfg.stVBPoolConfig.astVBPoolSetup[0].enFormat = VI_PIXEL_FORMAT;
	cfg.stVBPoolConfig.astVBPoolSetup[0].u32BlkCount = 5;
	cfg.stVBPoolConfig.astVBPoolSetup[0].u32Width  = chn0Size.u32Width;
	cfg.stVBPoolConfig.astVBPoolSetup[0].u32Height = chn0Size.u32Height;
	cfg.stVBPoolConfig.astVBPoolSetup[0].bBind = true;
	cfg.stVBPoolConfig.astVBPoolSetup[0].u32VpssChnBinding = VPSS_CHN0;
	cfg.stVBPoolConfig.astVBPoolSetup[0].u32VpssGrpBinding = (VPSS_GRP)0;

	/* VBPool 1 -> VPSS Grp0 Chn1 (240x240, 我们要的画面尺寸) */
	cfg.stVBPoolConfig.astVBPoolSetup[1].enFormat = VI_PIXEL_FORMAT;
	cfg.stVBPoolConfig.astVBPoolSetup[1].u32BlkCount = 5;
	cfg.stVBPoolConfig.astVBPoolSetup[1].u32Width  = CAM_W;
	cfg.stVBPoolConfig.astVBPoolSetup[1].u32Height = CAM_H;
	cfg.stVBPoolConfig.astVBPoolSetup[1].bBind = true;
	cfg.stVBPoolConfig.astVBPoolSetup[1].u32VpssChnBinding = VPSS_CHN1;
	cfg.stVBPoolConfig.astVBPoolSetup[1].u32VpssGrpBinding = (VPSS_GRP)0;

	/* VBPool 2 不绑 */
	cfg.stVBPoolConfig.astVBPoolSetup[2].enFormat = VI_PIXEL_FORMAT;
	cfg.stVBPoolConfig.astVBPoolSetup[2].u32BlkCount = 3;
	cfg.stVBPoolConfig.astVBPoolSetup[2].u32Width  = snsSize.u32Width;
	cfg.stVBPoolConfig.astVBPoolSetup[2].u32Height = snsSize.u32Height;
	cfg.stVBPoolConfig.astVBPoolSetup[2].bBind = false;

	/* VPSS Grp0: 两通道 */
	cfg.stVPSSPoolConfig.u32VpssGrpCount = 1;
#ifndef __CV186X__
	cfg.stVPSSPoolConfig.stVpssMode.aenInput[0] = VPSS_INPUT_ISP;
	cfg.stVPSSPoolConfig.stVpssMode.enMode = VPSS_MODE_DUAL;
	cfg.stVPSSPoolConfig.stVpssMode.ViPipe[0] = 0;
	cfg.stVPSSPoolConfig.stVpssMode.aenInput[1] = VPSS_INPUT_MEM;
	cfg.stVPSSPoolConfig.stVpssMode.ViPipe[1] = 0;
#endif
	{
		SAMPLE_TDL_VPSS_CONFIG_S *vc = &cfg.stVPSSPoolConfig.astVpssConfig[0];

		vc->bBindVI = true;
		vc->u32ChnCount = 2;
		vc->u32ChnBindVI = 0;
		VPSS_GRP_DEFAULT_HELPER2(&vc->stVpssGrpAttr,
					 snsSize.u32Width, snsSize.u32Height,
					 VI_PIXEL_FORMAT, 1);
		VPSS_CHN_DEFAULT_HELPER(&vc->astVpssChnAttr[VPSS_CHN0],
					chn0Size.u32Width, chn0Size.u32Height,
					VI_PIXEL_FORMAT, true);
		VPSS_CHN_DEFAULT_HELPER(&vc->astVpssChnAttr[VPSS_CHN1],
					CAM_W, CAM_H, VI_PIXEL_FORMAT, true);
	}

	SAMPLE_TDL_Get_Input_Config(&cfg.stVencConfig.stChnInputCfg);
	cfg.stVencConfig.u32FrameWidth  = chn0Size.u32Width;
	cfg.stVencConfig.u32FrameHeight = chn0Size.u32Height;
	SAMPLE_TDL_Get_RTSP_Config(&cfg.stRTSPConfig.stRTSPConfig);

	printf("初始化中间件(照抄 sample_vi_fd)...\n");
	ret = SAMPLE_TDL_Init_WM(&cfg, &g_mw);
	if (ret != CVI_SUCCESS) {
		printf("init middleware failed! ret=%#x\n", ret);
		return -1;
	}
	printf("中间件已就绪\n");
	return 0;
}

int main(int argc, char **argv)
{
	static uint8_t rgb[CAM_BYTES] __attribute__((aligned(64)));
	int do_dump = 0, max_frames = 0, n = 0, i;
	const char *ppm_path = NULL;
	time_t t0 = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
			do_dump = 1; ppm_path = argv[++i];
		} else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
			max_frames = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--chn") && i + 1 < argc) {
			g_cap_chn = (VPSS_CHN)atoi(argv[++i]);
		} else {
			printf("用法: %s [--dump out.ppm] [--frames N] [--chn 0|1]\n", argv[0]);
			return 1;
		}
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	printf("=== cam2lcd: 摄像头 -> ST7789(%dx%d) ===\n", LCD_W, LCD_H);
	if (vi_direct_init() != 0) return 1;
	if (!do_dump) shm_map();

	while (!g_exit) {
		VIDEO_FRAME_INFO_S fi;
		CVI_S32 ret = CVI_VI_GetChnFrame(g_vi_pipe, g_vi_chn, &fi, 2000);

		if (ret != CVI_SUCCESS) {
			printf("VI_GetChnFrame 失败 %#x\n", ret);
			usleep(200000);
			continue;
		}

		if (n == 0)
			printf("首帧: %ux%u stride=%u/%u\n",
			       fi.stVFrame.u32Width, fi.stVFrame.u32Height,
			       fi.stVFrame.u32Stride[0], fi.stVFrame.u32Stride[1]);

		nv21_to_rgb565_be(&fi.stVFrame, rgb, CAM_W * 2);
		CVI_VI_ReleaseChnFrame(g_vi_pipe, g_vi_chn, &fi);

		if (do_dump) {
			FILE *fp = fopen(ppm_path, "wb");
			if (!fp) die("fopen");
			fprintf(fp, "P6\n%d %d\n255\n", CAM_W, CAM_H);
			for (i = 0; i < CAM_H; i++) {
				int x;
				for (x = 0; x < CAM_W; x++) {
					uint16_t c = (uint16_t)((rgb[i*CAM_W*2 + x*2] << 8) |
								rgb[i*CAM_W*2 + x*2 + 1]);
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

		memcpy(g_shm->fb, rgb, CAM_BYTES);
		g_shm->ctrl.x0 = 0;
		g_shm->ctrl.y0 = (LCD_H - CAM_H) / 2;
		g_shm->ctrl.x1 = CAM_W - 1;
		g_shm->ctrl.y1 = g_shm->ctrl.y0 + CAM_H - 1;
		g_shm->ctrl.flags = 0;
		__sync_synchronize();
		g_shm->ctrl.seq++;
		send_cmd(LCD_CMD_CAM, 0);

		if (n == 0) t0 = time(NULL);
		n++;
		if ((n % 30) == 0) {
			time_t dt = time(NULL) - t0;
			printf("已送 %d 帧 %.1f fps ack=%u err=%u\n",
			       n, dt ? (double)n / (double)dt : 0.0,
			       g_shm->stat.ack_seq, g_shm->stat.err);
		}
		if (max_frames && n >= max_frames) break;
	}

	printf("退出, 共 %d 帧\n", n);
	return 0;
}
