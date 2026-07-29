#include <pspaudio.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspjpeg.h>
#include <pspkernel.h>
#include <psputility_modules.h>

#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "yvid_player.h"

#define DISPLAY_WIDTH 480
#define DISPLAY_HEIGHT 272
#define DISPLAY_STRIDE 512
#define MAX_JPEG_BYTES (1024 * 1024)
#define MAX_AUDIO_BYTES (4111 * 2 * 2)

typedef struct __attribute__((packed)) YvidHeader {
    char magic[8];
    uint32_t version;
    uint16_t width;
    uint16_t height;
    uint16_t fps_num;
    uint16_t fps_den;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t reserved;
    uint32_t frame_count;
} YvidHeader;

typedef struct __attribute__((packed)) YvidRecord {
    uint32_t jpeg_bytes;
    uint32_t samples;
    uint32_t pcm_bytes;
    uint32_t pts_ms;
} YvidRecord;

static int read_exact(SceUID fd, void *buffer, int bytes)
{
    int done = 0;
    while (done < bytes) {
        int got = sceIoRead(fd, (unsigned char *)buffer + done, bytes - done);
        if (got <= 0) return -1;
        done += got;
    }
    return 0;
}

int yvid_play(const char *path)
{
    SceUID fd = -1;
    YvidHeader header;
    unsigned char *jpeg = NULL;
    unsigned char *pcm = NULL;
    unsigned char *ycbcr = NULL;
    int ycbcr_capacity = 0;
    int jpeg_ready = 0;
    int av_module_loaded = 0;
    int audio_ready = 0;
    int result = -1;
    uint32_t frame;
    void *buffers[2] = { (void *)0x44000000, (void *)0x44088000 };
    int back = 0;

    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0 || read_exact(fd, &header, sizeof(header)) < 0) goto cleanup;
    if (memcmp(header.magic, "UTUBVID\0", 8) != 0 || header.version != 1) goto cleanup;
    if (header.width != DISPLAY_WIDTH || header.height != DISPLAY_HEIGHT) goto cleanup;
    if ((header.channels != 1 && header.channels != 2) ||
        header.sample_rate < 8000 || header.sample_rate > 48000) goto cleanup;
    if (header.frame_count == 0 || header.frame_count > 1000000) goto cleanup;

    jpeg = memalign(64, MAX_JPEG_BYTES);
    pcm = memalign(64, MAX_AUDIO_BYTES);
    if (!jpeg || !pcm) goto cleanup;

    if (sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC) < 0) goto cleanup;
    av_module_loaded = 1;
    if (sceJpegInitMJpeg() < 0) goto cleanup;
    jpeg_ready = 1;
    if (sceJpegCreateMJpeg(header.width, header.height) < 0) goto cleanup;
    jpeg_ready = 2;

    sceDisplaySetMode(0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    sceDisplaySetFrameBuf(buffers[1], DISPLAY_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_8888,
        PSP_DISPLAY_SETBUF_IMMEDIATE);

    for (frame = 0; frame < header.frame_count; ++frame) {
        YvidRecord record;
        int colour_info = 0;
        int needed;
        int width_height;
        SceCtrlData pad;

        if (read_exact(fd, &record, sizeof(record)) < 0) goto cleanup;
        if (record.jpeg_bytes == 0 || record.jpeg_bytes > MAX_JPEG_BYTES) goto cleanup;
        if (record.samples < 17 || record.samples > 4111) goto cleanup;
        if (record.pcm_bytes != record.samples * header.channels * 2 ||
            record.pcm_bytes > MAX_AUDIO_BYTES) goto cleanup;
        if (read_exact(fd, jpeg, record.jpeg_bytes) < 0 ||
            read_exact(fd, pcm, record.pcm_bytes) < 0) goto cleanup;

        if (header.channels == 1) {
            int16_t *samples = (int16_t *)pcm;
            int sample_index;
            for (sample_index = (int)record.samples - 1; sample_index >= 0; --sample_index) {
                int16_t sample = samples[sample_index];
                samples[sample_index * 2] = sample;
                samples[sample_index * 2 + 1] = sample;
            }
        }

        sceCtrlPeekBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_CIRCLE) {
            result = 0;
            goto cleanup;
        }

        needed = sceJpegGetOutputInfo(jpeg, record.jpeg_bytes, &colour_info, 0);
        if (needed <= 0) goto cleanup;
        if (needed > ycbcr_capacity) {
            free(ycbcr);
            ycbcr = memalign(64, needed);
            if (!ycbcr) goto cleanup;
            ycbcr_capacity = needed;
        }
        width_height = sceJpegDecodeMJpegYCbCr(jpeg, record.jpeg_bytes,
            ycbcr, ycbcr_capacity, 0);
        if (width_height < 0) goto cleanup;
        if (sceJpegCsc(buffers[back], ycbcr, width_height, DISPLAY_STRIDE, colour_info) < 0)
            goto cleanup;

        if (!audio_ready) {
            if (sceAudioSRCChReserve(record.samples, header.sample_rate, 2) < 0) goto cleanup;
            audio_ready = 1;
        }

        sceDisplayWaitVblankStart();
        sceDisplaySetFrameBuf(buffers[back], DISPLAY_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_8888,
            PSP_DISPLAY_SETBUF_NEXTFRAME);
        back ^= 1;
        if (sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, pcm) < 0) goto cleanup;
    }

    result = 0;

cleanup:
    if (audio_ready) {
        /* Let the final DMA block leave the SRC channel before releasing it. */
        sceKernelDelayThread(100000);
        sceAudioSRCChRelease();
    }
    if (jpeg_ready >= 2) sceJpegDeleteMJpeg();
    if (jpeg_ready >= 1) sceJpegFinishMJpeg();
    if (av_module_loaded) sceUtilityUnloadModule(PSP_MODULE_AV_AVCODEC);
    if (fd >= 0) sceIoClose(fd);
    free(ycbcr);
    free(pcm);
    free(jpeg);
    return result;
}
