/*
 * abi_probe.c —— 量出「TDL 头 vs 板上库」SAMPLE_VI_CONFIG_S 的字段错位
 *
 * 手法: 库函数 IniToViCfg 会把 s32WorkingViNum 写成 1。
 *       我们先把整块 buffer 填 0xAA, 调完库函数后扫哪些字节变成了 0/1,
 *       就能反推出库实际用的字段偏移; 与头文件的 offsetof 一比, 差值即错位量。
 */
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#include <cvi_comm.h>
#include <sample_comm.h>

static SAMPLE_VI_CONFIG_S g_cfg;
static SAMPLE_INI_CFG_S   g_ini;

int main(void)
{
	int i, off_sns = -1;

	printf("=== 头文件认为的布局 ===\n");
	printf("sizeof(SAMPLE_INI_CFG_S)      = %zu\n", sizeof(SAMPLE_INI_CFG_S));
	printf("sizeof(SAMPLE_VI_INFO_S)      = %zu\n", sizeof(SAMPLE_VI_INFO_S));
	printf("sizeof(SAMPLE_VI_CONFIG_S)    = %zu\n", sizeof(SAMPLE_VI_CONFIG_S));
	printf("sizeof(SAMPLE_SNS_INFO_S)     = %zu\n", sizeof(SAMPLE_SENSOR_INFO_S));
	printf("sizeof(SAMPLE_DEV_INFO_S)     = %zu\n", sizeof(SAMPLE_DEV_INFO_S));
	printf("sizeof(SAMPLE_PIPE_INFO_S)    = %zu\n", sizeof(SAMPLE_PIPE_INFO_S));
	printf("sizeof(SAMPLE_CHN_INFO_S)     = %zu\n", sizeof(SAMPLE_CHN_INFO_S));
	printf("sizeof(SAMPLE_SNAP_INFO_S)    = %zu\n", sizeof(SAMPLE_SNAP_INFO_S));
	printf("offsetof(astViInfo)           = %zu\n", offsetof(SAMPLE_VI_CONFIG_S, astViInfo));
	printf("offsetof(as32WorkingViId)     = %zu\n", offsetof(SAMPLE_VI_CONFIG_S, as32WorkingViId));
	printf("offsetof(s32WorkingViNum)     = %zu\n", offsetof(SAMPLE_VI_CONFIG_S, s32WorkingViNum));
	printf("VI_MAX_DEV_NUM                = %d\n", VI_MAX_DEV_NUM);
	printf("\n");

	/* 用头文件的布局把 ini 填对 */
	memset(&g_ini, 0, sizeof(g_ini));
	g_ini.enSource = VI_PIPE_FRAME_SOURCE_DEV;
	g_ini.devNum = 1;
	g_ini.enSnsType[0] = GCORE_GC2083_MIPI_2M_30FPS_10BIT;
	g_ini.enWDRMode[0] = WDR_MODE_NONE;
	g_ini.s32BusId[0] = 2;
	g_ini.s32SnsI2cAddr[0] = 0x37;
	g_ini.as16LaneId[0][0] = 1;
	g_ini.as16LaneId[0][1] = 0;
	g_ini.as16LaneId[0][2] = 2;
	g_ini.as16LaneId[0][3] = -1;
	g_ini.as16LaneId[0][4] = -1;

	printf("=== 我自己填的 ini(绕开 ParseIni) ===\n");
	printf("devNum=%d enSnsType[0]=%d busId=%d\n",
	       g_ini.devNum, (int)g_ini.enSnsType[0], g_ini.s32BusId[0]);

	/* 先看这个枚举值板上库认不认 */
	{
		PIC_SIZE_E ps;
		CVI_S32 r = SAMPLE_COMM_VI_GetSizeBySensor(g_ini.enSnsType[0], &ps);
		printf("GetSizeBySensor(gc2083=%d) ret=%#x\n", (int)g_ini.enSnsType[0], r);
	}

	/* 关键: 把整块 buffer 填 0xAA, 调库函数, 看它到底写了哪几个字节 */
	memset(&g_cfg, 0xAA, sizeof(g_cfg));
	{
		CVI_S32 r = SAMPLE_COMM_VI_IniToViCfg(&g_ini, &g_cfg);
		printf("IniToViCfg ret=%#x\n", r);
	}
	printf("s32WorkingViNum (头认为偏移 %zu) = %d\n",
	       offsetof(SAMPLE_VI_CONFIG_S, s32WorkingViNum),
	       g_cfg.s32WorkingViNum);

	printf("\n=== 被库写过的字节(值不是 0xAA 的位置) ===\n");
	{
		unsigned char *p = (unsigned char *)&g_cfg;
		int shown = 0;
		for (i = 0; i < (int)sizeof(g_cfg) && shown < 40; i++) {
			if (p[i] != 0xAA) {
				printf("  off %4d: 0x%02X\n", i, p[i]);
				shown++;
			}
		}
		if (shown == 0)
			printf("  (一个字节都没写! 说明库认为入参非法或结构不匹配)\n");
	}

	/* 找 1 出现的位置(workingViNum 应为 1) */
	printf("\n=== 值为 1 的位置(候补 s32WorkingViNum) ===\n");
	{
		unsigned char *p = (unsigned char *)&g_cfg;
		int n = 0;
		for (i = 0; i < (int)sizeof(g_cfg); i++) {
			if (p[i] == 1) { printf("  off %4d == 1\n", i); n++; }
		}
		if (!n) printf("  (没有任何字节是 1)\n");
	}
	(void)off_sns;
	return 0;
}
