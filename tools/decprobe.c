/*
 * decprobe.c —— 列出板上 ffmpeg 支持的所有解码器
 */
#include <stdio.h>
#include <libavcodec/avcodec.h>

int main(void)
{
	const AVCodec *c = NULL;
	void *it = NULL;
	int n = 0, h264 = 0, mjpeg = 0;

	printf("=== 板上 libavcodec 的解码器 ===\n");
	while ((c = av_codec_iterate(&it)) != NULL) {
		if (av_codec_is_decoder(c)) {
			printf("  %-16s %s\n", c->name, c->long_name ? c->long_name : "");
			n++;
			if (c->id == AV_CODEC_ID_H264) h264 = 1;
			if (c->id == AV_CODEC_ID_MJPEG) mjpeg = 1;
		}
	}
	printf("共 %d 个解码器; H264=%d MJPEG=%d\n", n, h264, mjpeg);
	printf("avcodec 版本: %u.%u.%u\n",
	       avcodec_version() >> 16, (avcodec_version() >> 8) & 0xFF, avcodec_version() & 0xFF);
	return 0;
}
