/*
 * Small progressive MP4 player for UTUBBU.
 * AVC/AAC decoder setup derives from PMPlayer Advance/OpenTube (GPL-2.0).
 */
#include <pspaudio.h>
#include <pspaudiocodec.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspmpeg.h>
#include <pspmpegbase.h>
#include <psputility_avmodules.h>
#include <psputility_modules.h>

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>

#include "mp4_player.h"
#include "diagnostic.h"
#include "media_engine.h"
#include "network.h"

#define MP4_MAX_TRACKS 4
#define HANDLER_VIDEO 0x76696465U
#define HANDLER_AUDIO 0x736f756eU
#define CODEC_AVC1    0x61766331U
#define CODEC_MP4A    0x6d703461U

typedef struct StscEntry { uint32_t first_chunk, samples_per_chunk; } StscEntry;
typedef struct SttsEntry { uint32_t count, delta; } SttsEntry;
typedef struct Mp4Track {
    uint32_t handler, codec, timescale, track_id;
    uint64_t next_tick;
    int width, height, sample_rate, channels, aac_object_type, aac_core_rate;
    int nal_prefix;
    unsigned char *sps, *pps;
    int sps_size, pps_size;
    uint32_t sample_count, default_size;
    uint32_t *sizes, *offsets, *times;
    uint32_t sync_count, *sync_samples;
    uint32_t chunk_count, *chunks;
    uint32_t stsc_count; StscEntry *stsc;
    uint32_t stts_count; SttsEntry *stts;
} Mp4Track;
typedef struct Mp4File { Mp4Track tracks[MP4_MAX_TRACKS]; int count; Mp4Track *video, *audio; } Mp4File;

typedef struct Mp4AvcNal {
    void *sps; int sps_size; void *pps; int pps_size; int nal_prefix;
    void *nal; int nal_size; int mode;
} Mp4AvcNal;
typedef struct Mp4AvcInfo { int u0,u1,width,height,u4,u5,u6,u7,u8,u9; } Mp4AvcInfo;
typedef struct Mp4AvcYuv { void *b[8]; int u0,u1,u2; } Mp4AvcYuv;
typedef struct Mp4AvcDetail {
    int u0,u1,u2,u3; Mp4AvcInfo *info; int u5,u6,u7,u8,u9,u10;
    Mp4AvcYuv *yuv; int u12,u13,u14,u15,u16,u17,u18,u19,u20,u21,u22,u23;
} Mp4AvcDetail;
typedef struct Mp4AvcCsc { int height,width,mode0,mode1; void *b[8]; } Mp4AvcCsc;

extern int sceMpegGetAvcNalAu(SceMpeg *, Mp4AvcNal *, SceMpegAu *);
extern int sceMpegAvcDecodeDetail2(SceMpeg *, Mp4AvcDetail **);
extern int sceMpegBaseCscAvc(void *, int, int, Mp4AvcCsc *);
extern int sceMpegQueryMemSizeUtubbu(int);
extern int sceMpegCreateUtubbu(SceMpeg *, void *, int, SceMpegRingbuffer *, int, int, int);
extern int sceUtilityLoadAvModule(int);
extern int sceUtilityUnloadAvModule(int);

typedef struct Player {
    Mp4File file;
    Mp4File audio_file;
    Mp4File *parse_file;
    char path[2][256];
    SceUID fd[2];
    int fd_size[2], read_slot, fragmented[2];
    uint32_t fragment_pos[2];
    volatile int stop, error;
    volatile int audio_done, video_done, video_ready, audio_ready;
    volatile uint32_t audio_time_ms;
    volatile int video_index, audio_index;
    volatile int video_dropped;
    volatile int audio_decode_errors;
    volatile int paused;
    volatile unsigned int seek_generation;
    volatile uint32_t seek_target_ms;
    unsigned int control_buttons;
    int software_video;
    SceUID audio_thread, video_thread;
    SceUID io_mutex, codec_mutex;
} Player;
static Player player;

void utubbu_mp4_request_stop(void)
{ player.stop=1; }

static uint32_t be32(const unsigned char *p)
{ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint64_t be64(const unsigned char *p)
{ return ((uint64_t)be32(p)<<32)|be32(p+4); }
static uint16_t be16(const unsigned char *p) { return (uint16_t)((p[0]<<8)|p[1]); }

static int stream_slot_for(int slot)
{ return slot==1&&!strcmp(player.path[0],player.path[1])?0:slot; }

static int available_bytes(const char *path, int slot)
{
    int downloaded = 0, total = 0, error = 0;
    SceIoStat stat;
    int stream_slot = stream_slot_for(slot);
    utubbu_stream_state_slot(stream_slot, &downloaded, &total, &error);
    int actual = 0;
    (void)total; (void)error;
    if (sceIoGetstat(path, &stat) >= 0) actual = (int)stat.st_size;
    /* Su Memory Stick sceIoGetstat puo restare a zero finche il writer non
       chiude il file. downloaded invece cresce solo dopo sceIoWrite riuscita. */
    if (downloaded > actual) return downloaded;
    return actual;
}

static int read_at_slot(Player *p, int slot, uint32_t offset, void *data, uint32_t bytes)
{
    int active = 0, downloaded, total, error, available = 0;
    int stream_slot=stream_slot_for(slot);
    SceCtrlData pad;
    while (!p->stop) {
        active = utubbu_stream_state_slot(stream_slot, &downloaded, &total,&error);
        available = available_bytes(p->path[slot], slot);
        if (available >= (int)(offset + bytes)) break;
        if (!active) return error ? error : -20;
        sceCtrlPeekBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_CIRCLE) { p->stop = 1; return -21; }
        sceKernelDelayThread(20000);
    }
    if (p->stop) return -21;
    sceKernelWaitSema(p->io_mutex, 1, NULL);
    /* Un file aperto durante il download conserva la vecchia EOF su PSP.
       Riaprilo soltanto quando la lettura richiesta supera quella EOF. */
    if (p->fd[slot] >= 0 && (uint32_t)p->fd_size[slot] < offset + bytes) {
        sceIoClose(p->fd[slot]);
        p->fd[slot] = -1;
    }
    if (p->fd[slot] < 0) {
        p->fd[slot] = sceIoOpen(p->path[slot], PSP_O_RDONLY, 0);
        if (p->fd[slot] < 0) { sceKernelSignalSema(p->io_mutex, 1); return -22; }
        p->fd_size[slot] = available;
    }
    if (sceIoLseek32(p->fd[slot], offset, PSP_SEEK_SET) < 0) {
        sceKernelSignalSema(p->io_mutex, 1); return -23;
    }
    active = sceIoRead(p->fd[slot], data, bytes) == (int)bytes ? 0 : -24;
    if (active < 0) utubbu_log("read fail offset", (int)offset);
    sceKernelSignalSema(p->io_mutex, 1);
    return active;
}

static int read_at(Player *p, uint32_t offset, void *data, uint32_t bytes)
{ return read_at_slot(p, p->read_slot, offset, data, bytes); }

static void wait_initial_buffer(Player *p)
{
    int active,downloaded,total,error;
    uint32_t first_video=p->file.video->offsets[0];
    uint32_t first_audio=p->audio_file.audio->offsets[0];
    /* A dual-stream compatibility retry must become visible quickly.  Its
       two downloaders continue filling in parallel after playback starts. */
    uint32_t reserve=!strcmp(p->path[0],p->path[1])?256*1024:128*1024;
    uint32_t video_target=first_video+reserve,audio_target=first_audio+reserve;
    utubbu_log("video buffer target",(int)video_target);utubbu_log("audio buffer target",(int)audio_target);
    for(;;){
        active=utubbu_stream_state_slot(0,&downloaded,&total,&error);
        if(downloaded>=(int)video_target||!active||p->stop)break;
        sceKernelDelayThread(20000);
    }
    for(;;){
        active=utubbu_stream_state_slot(stream_slot_for(1),&downloaded,&total,&error);
        if(downloaded>=(int)audio_target||!active||p->stop)break;
        sceKernelDelayThread(20000);
    }
    utubbu_log("video buffer ready",available_bytes(p->path[0],0));
    utubbu_log("audio buffer ready",available_bytes(p->path[1],1));
}

static int read_box(Player *p, uint32_t at, uint32_t *size, uint32_t *type)
{
    unsigned char h[8];
    int read_result=read_at(p, at, h, 8);
    if (read_result < 0) { utubbu_log("box read fail",read_result);utubbu_log("box read at",(int)at);return -1; }
    *size = be32(h); *type = be32(h + 4);
    if(at==0){utubbu_log("first box size",(int)*size);utubbu_log("first box type",(int)*type);}
    return *size >= 8 ? 0 : -1;
}

static int parse_stsd(Player *p, Mp4Track *t, uint32_t start, uint32_t bytes)
{
    unsigned char head[96];
    uint32_t entry_size, pos, end;
    if (bytes < 16 || read_at(p, start, head, bytes < sizeof(head) ? bytes : sizeof(head)) < 0) return -1;
    entry_size = be32(head + 8); t->codec = be32(head + 12);
    if (entry_size < 16 || entry_size + 8 > bytes) return -1;
    if (t->codec == CODEC_AVC1) {
        t->width = be16(head + 8 + 32); t->height = be16(head + 8 + 34);
        pos = start + 8 + 86; end = start + 8 + entry_size;
        while (pos + 8 <= end) {
            uint32_t s, type; unsigned char *avcc; uint32_t q; int count;
            if (read_box(p, pos, &s, &type) < 0 || pos + s > end) break;
            if (type != 0x61766343U) { pos += s; continue; }
            avcc = malloc(s - 8); if (!avcc) return -1;
            if (read_at(p, pos + 8, avcc, s - 8) < 0) { free(avcc); return -1; }
            if (s < 16) { free(avcc); return -1; }
            t->nal_prefix = (avcc[4] & 3) + 1; q = 6;
            count = avcc[5] & 31;
            if (count < 1 || q + 2 > s - 8) { free(avcc); return -1; }
            t->sps_size = be16(avcc + q); q += 2;
            if (q + t->sps_size + 3 > s - 8) { free(avcc); return -1; }
            t->sps = memalign(64, (t->sps_size + 63) & ~63U); if (!t->sps) { free(avcc); return -1; }
            memset(t->sps, 0, (t->sps_size + 63) & ~63U);
            memcpy(t->sps, avcc + q, t->sps_size); q += t->sps_size;
            count = avcc[q++];
            if (count < 1 || q + 2 > s - 8) { free(avcc); return -1; }
            t->pps_size = be16(avcc + q); q += 2;
            if (q + t->pps_size > s - 8) { free(avcc); return -1; }
            t->pps = memalign(64, (t->pps_size + 63) & ~63U); if (!t->pps) { free(avcc); return -1; }
            memset(t->pps, 0, (t->pps_size + 63) & ~63U);
            memcpy(t->pps, avcc + q, t->pps_size); free(avcc); return 0;
        }
    } else if (t->codec == CODEC_MP4A) {
        unsigned char *esds=NULL;
        t->channels = be16(head + 8 + 24);
        t->sample_rate = (int)(be32(head + 8 + 32) >> 16);
        t->aac_object_type=2;t->aac_core_rate=t->sample_rate;
        pos=start+8+36;end=start+8+entry_size;
        while(pos+8<=end){
            uint32_t s,type,q;
            if(read_box(p,pos,&s,&type)<0||pos+s>end)break;
            if(type==0x65736473U&&s>12){
                esds=malloc(s-8);if(!esds)return -1;
                if(read_at(p,pos+8,esds,s-8)<0){free(esds);return -1;}
                for(q=4;q+2<s-8;q++)if(esds[q]==5){
                    uint32_t at=q+1,length=0;int n=0;unsigned char b;
                    do{if(at>=s-8)break;b=esds[at++];length=(length<<7)|(b&127);n++;}while((b&128)&&n<4);
                    if(at<s-8&&length){
                        static const int aac_rates[16]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350,0,0,0};
                        int rate_index;
                        t->aac_object_type=esds[at]>>3;
                        if(at+1<s-8){
                            rate_index=((esds[at]&7)<<1)|(esds[at+1]>>7);
                            if(aac_rates[rate_index])t->aac_core_rate=aac_rates[rate_index];
                        }
                        break;
                    }
                }
                free(esds);break;
            }
            pos+=s;
        }
    }
    return 0;
}

static int parse_table(Player *p, Mp4Track *t, uint32_t type, uint32_t start, uint32_t bytes)
{
    unsigned char h[12]; uint32_t i, count, base, n, j, stride; unsigned char *raw;
    if (bytes < 8 || read_at(p, start, h, bytes < 12 ? bytes : 12) < 0) return -1;
    if (type == 0x7374737aU) {
        if (bytes < 12) return -1; t->default_size = be32(h + 4); t->sample_count = be32(h + 8);
        if (t->sample_count > 1000000) return -1;
        if (!t->sample_count) return 0;
        t->sizes = malloc(t->sample_count * 4); if (!t->sizes) { utubbu_log("stsz sizes alloc",(int)t->sample_count);return -1; }
        if (t->default_size) for (i=0;i<t->sample_count;i++) t->sizes[i]=t->default_size;
        else {
            raw=malloc(4096); if(!raw){utubbu_log("table scratch alloc",1);return -1;}
            for(base=0;base<t->sample_count;base+=1024){
                n=t->sample_count-base;if(n>1024)n=1024;
                if(read_at(p,start+12+base*4,raw,n*4)<0){free(raw);return -1;}
                for(j=0;j<n;j++)t->sizes[base+j]=be32(raw+j*4);
            }
            free(raw);
        }
    } else if (type == 0x7374636fU) {
        count=be32(h+4); t->chunk_count=count; t->chunks=malloc(count*4); raw=malloc(4096);
        if(!t->chunks||!raw)return -1;
        for(base=0;base<count;base+=1024){n=count-base;if(n>1024)n=1024;if(read_at(p,start+8+base*4,raw,n*4)<0){free(raw);return -1;}for(j=0;j<n;j++)t->chunks[base+j]=be32(raw+j*4);}free(raw);
    } else if (type == 0x73747363U) {
        count=be32(h+4); t->stsc_count=count; t->stsc=malloc(count*sizeof(StscEntry)); stride=12;raw=malloc(4080);
        if(!t->stsc||!raw)return -1;
        for(base=0;base<count;base+=340){n=count-base;if(n>340)n=340;if(read_at(p,start+8+base*stride,raw,n*stride)<0){free(raw);return -1;}for(j=0;j<n;j++){t->stsc[base+j].first_chunk=be32(raw+j*stride);t->stsc[base+j].samples_per_chunk=be32(raw+j*stride+4);}}free(raw);
    } else if (type == 0x73747473U) {
        count=be32(h+4); t->stts_count=count; t->stts=malloc(count*sizeof(SttsEntry)); stride=8;raw=malloc(4096);
        if(!t->stts||!raw)return -1;
        for(base=0;base<count;base+=512){n=count-base;if(n>512)n=512;if(read_at(p,start+8+base*stride,raw,n*stride)<0){free(raw);return -1;}for(j=0;j<n;j++){t->stts[base+j].count=be32(raw+j*stride);t->stts[base+j].delta=be32(raw+j*stride+4);}}free(raw);
    } else if (type == 0x73747373U) {
        count=be32(h+4);t->sync_count=count;t->sync_samples=malloc(count*4);raw=malloc(4096);
        if((count&&!t->sync_samples)||!raw)return -1;
        for(base=0;base<count;base+=1024){n=count-base;if(n>1024)n=1024;if(read_at(p,start+8+base*4,raw,n*4)<0){free(raw);return -1;}for(j=0;j<n;j++){uint32_t sample=be32(raw+j*4);t->sync_samples[base+j]=sample?sample-1:0;}}free(raw);
    }
    return 0;
}

static int is_container(uint32_t type)
{
    return type==0x6d6f6f76U||type==0x7472616bU||type==0x6d646961U||
           type==0x6d696e66U||type==0x7374626cU;
}

static int parse_boxes(Player *p, uint32_t start, uint32_t end, Mp4Track *track)
{
    uint32_t pos=start;
    while(pos+8<=end){
        uint32_t size,type,content,bytes; unsigned char b[24]; Mp4Track *child=track; int status;
        if(read_box(p,pos,&size,&type)<0){utubbu_log("box header fail",(int)pos);return -1;}
        if(pos+size>end){utubbu_log("box bounds fail",(int)type);return -1;}
        content=pos+8; bytes=size-8;
        if(type==0x7472616bU){if(p->parse_file->count>=MP4_MAX_TRACKS)return -1;child=&p->parse_file->tracks[p->parse_file->count++];}
        if(is_container(type)){status=parse_boxes(p,content,pos+size,child);if(status<0){utubbu_log("box recurse fail",(int)type);return -1;}}
        else if(track&&type==0x746b6864U){
            if(bytes<24||read_at(p,content,b,24)<0){utubbu_log("tkhd fail",(int)pos);return -1;}
            track->track_id=b[0]?be32(b+20):be32(b+12);
        }
        else if(track&&type==0x6d646864U){
            uint32_t need=bytes<24?bytes:24;
            if(bytes<20||read_at(p,content,b,need)<0){utubbu_log("mdhd fail",(int)pos);return -1;}
            if(b[0]){
                if(bytes<24){utubbu_log("mdhd v1 short",(int)bytes);return -1;}
                track->timescale=be32(b+20);
            }else track->timescale=be32(b+12);
        }
        else if(track&&type==0x68646c72U){if(bytes<12||read_at(p,content,b,12)<0){utubbu_log("hdlr fail",(int)pos);return -1;}track->handler=be32(b+8);}
        else if(track&&type==0x73747364U){status=parse_stsd(p,track,content,bytes);utubbu_log("stsd result",status);if(status<0)return -1;}
        else if(track&&(type==0x7374737aU||type==0x7374636fU||type==0x73747363U||type==0x73747473U||type==0x73747373U))
            {status=parse_table(p,track,type,content,bytes);if(status<0){utubbu_log("table fail",(int)type);return -1;}}
        pos+=size;
    }
    return 0;
}

static int build_track(Mp4Track *t)
{
    uint32_t chunk,sample=0,entry=0,i,k,offset; uint64_t tick=0;
    if(!t->sample_count||!t->chunks||!t->stsc||!t->timescale)return -1;
    t->offsets=malloc(t->sample_count*4);t->times=malloc(t->sample_count*4);
    if(!t->offsets||!t->times)return -1;
    for(chunk=0;chunk<t->chunk_count&&sample<t->sample_count;chunk++){
        while(entry+1<t->stsc_count&&t->stsc[entry+1].first_chunk<=chunk+1)entry++;
        offset=t->chunks[chunk];
        for(k=0;k<t->stsc[entry].samples_per_chunk&&sample<t->sample_count;k++){
            t->offsets[sample]=offset;offset+=t->sizes[sample++];
        }
    }
    if(sample!=t->sample_count)return -1;
    sample=0;
    for(i=0;i<t->stts_count;i++)for(k=0;k<t->stts[i].count&&sample<t->sample_count;k++){
        t->times[sample++]=(uint32_t)(tick*1000/t->timescale);tick+=t->stts[i].delta;
    }
    t->next_tick=tick;
    return sample==t->sample_count?0:-1;
}

static int parse_mp4(Player *p, Mp4File *file, int slot, uint32_t required_handler)
{
    uint32_t pos=0,size,type; int i; int known;
    memset(file,0,sizeof(*file));p->parse_file=file;p->read_slot=slot;
    for(;;){
        if(read_box(p,pos,&size,&type)<0)return -30;
        if(type==0x6d6f6f76U){if(parse_boxes(p,pos+8,pos+size,NULL)<0)return -31;break;}
        pos+=size; if(pos>16*1024*1024)return -32;
    }
    for(i=0;i<file->count;i++){
        Mp4Track *t=&file->tracks[i];
        known=(t->handler==HANDLER_VIDEO&&t->codec==CODEC_AVC1)||(t->handler==HANDLER_AUDIO&&t->codec==CODEC_MP4A);
        if(known&&t->sample_count&&build_track(t)<0)return -33;
        if(known&&!t->sample_count)p->fragmented[slot]=1;
        if(t->handler==HANDLER_VIDEO&&t->codec==CODEC_AVC1)file->video=t;
        if(t->handler==HANDLER_AUDIO&&t->codec==CODEC_MP4A)file->audio=t;
    }
    p->fragment_pos[slot]=pos+size;
    if(required_handler==HANDLER_VIDEO)return file->video?0:-34;
    return file->audio?0:-34;
}

static int append_fragment(Player *p, int slot, Mp4Track *t, uint32_t moof_at,
    uint32_t moof_size)
{
    unsigned char *box=NULL,*trun=NULL;uint32_t q,r,s,type,flags,n,i,at;
    uint32_t default_duration=0,default_size=0,data_at=0;uint64_t base_time=0,tick;
    int has_base_time=0;
    uint32_t *new_sizes,*new_offsets,*new_times,old=t->sample_count;
    int32_t data_offset=0;
    if(moof_size<16||moof_size>1024*1024)return -1;
    box=malloc(moof_size);if(!box)return -1;
    if(read_at_slot(p,slot,moof_at,box,moof_size)<0){free(box);return -1;}
    q=8;
    while(q+8<=moof_size){
        s=be32(box+q);type=be32(box+q+4);if(s<8||q+s>moof_size)break;
        if(type==0x74726166U){
            unsigned char *candidate_trun=NULL;uint32_t candidate_track=0;
            uint32_t candidate_duration=0,candidate_size=0;uint64_t candidate_time=0;
            int candidate_has_time=0;
            r=q+8;
            while(r+8<=q+s){
                uint32_t z=be32(box+r),kind=be32(box+r+4);unsigned char *v=box+r+8;
                if(z<8||r+z>q+s)break;
                if(kind==0x74666864U&&z>=16){
                    uint32_t f=be32(v)&0xffffffU;at=8;
                    candidate_track=be32(v+4);
                    if(f&1)at+=8;if(f&2)at+=4;
                    if((f&8)&&at+4<=z-8){candidate_duration=be32(v+at);at+=4;}
                    if((f&0x10)&&at+4<=z-8){candidate_size=be32(v+at);at+=4;}
                }else if(kind==0x74666474U&&z>=16){
                    if(v[0]){if(z>=20){candidate_time=be64(v+4);candidate_has_time=1;}}
                    else{candidate_time=be32(v+4);candidate_has_time=1;}
                }else if(kind==0x7472756eU&&z>=20){candidate_trun=v;}
                r+=z;
            }
            if(candidate_track==t->track_id&&candidate_trun){
                trun=candidate_trun;default_duration=candidate_duration;
                default_size=candidate_size;base_time=candidate_time;
                has_base_time=candidate_has_time;
            }
        }
        q+=s;
    }
    /* A combined YouTube file alternates video and audio traf boxes.  A moof
       belonging only to the other track is valid and must simply be skipped. */
    if(!trun){free(box);return 0;}
    flags=be32(trun)&0xffffffU;n=be32(trun+4);at=8;
    if(!n||n>10000||old+n>1000000){free(box);return -1;}
    if(flags&1){data_offset=(int32_t)be32(trun+at);at+=4;}
    if(flags&4)at+=4;
    new_sizes=realloc(t->sizes,(old+n)*4);if(!new_sizes){free(box);return -1;}t->sizes=new_sizes;
    new_offsets=realloc(t->offsets,(old+n)*4);if(!new_offsets){free(box);return -1;}t->offsets=new_offsets;
    new_times=realloc(t->times,(old+n)*4);if(!new_times){free(box);return -1;}t->times=new_times;
    data_at=(uint32_t)((int32_t)moof_at+data_offset);
    /* Some Google combined fragments omit tfdt or repeat a local zero-based
       decode time.  Never let a track timeline move behind its prior end. */
    tick=has_base_time&&base_time>=t->next_tick?base_time:t->next_tick;
    if(old<1000){
        utubbu_log("fragment track",(int)t->track_id);
        utubbu_log("fragment first ms",(int)(tick*1000/t->timescale));
        utubbu_log("fragment samples",(int)n);
    }
    for(i=0;i<n;i++){
        uint32_t duration=default_duration,size=default_size;
        if(flags&0x100){if(at+4>moof_size){free(box);return -1;}duration=be32(trun+at);at+=4;}
        if(flags&0x200){if(at+4>moof_size){free(box);return -1;}size=be32(trun+at);at+=4;}
        if(flags&0x400)at+=4;if(flags&0x800)at+=4;
        if(!size||!duration||at>moof_size){free(box);return -1;}
        t->sizes[old+i]=size;t->offsets[old+i]=data_at;
        t->times[old+i]=(uint32_t)((uint64_t)tick*1000/t->timescale);
        data_at+=size;tick+=duration;
    }
    t->next_tick=tick;t->sample_count=old+n;free(box);return 1;
}

static int ensure_sample(Player *p,int slot,Mp4Track *t,uint32_t index)
{
    unsigned char h[8];uint32_t size,type,pos;
    if(index<t->sample_count)return 1;
    if(!p->fragmented[slot])return 0;
    pos=p->fragment_pos[slot];
    while(!p->stop){
        if(read_at_slot(p,slot,pos,h,8)<0)return 0;
        size=be32(h);type=be32(h+4);if(size<8)return -1;
        if(type==0x6d6f6f66U){
            int appended=append_fragment(p,slot,t,pos,size);
            if(appended<0)return -1;
            p->fragment_pos[slot]=pos+size;
            if(appended&&index<t->sample_count)return 1;
            pos+=size;continue;
        }
        pos+=size;p->fragment_pos[slot]=pos;
    }
    return 0;
}

static void free_track(Mp4Track *t)
{
    free(t->sps);free(t->pps);free(t->sizes);free(t->offsets);free(t->times);
    free(t->sync_samples);free(t->chunks);free(t->stsc);free(t->stts);memset(t,0,sizeof(*t));
}

static int sample_for_time(const Mp4Track *t,uint32_t target_ms)
{
    uint32_t low=0,high;
    if(!t||!t->sample_count||!t->times)return 0;
    high=t->sample_count;
    while(low+1<high){uint32_t middle=low+(high-low)/2;if(t->times[middle]<=target_ms)low=middle;else high=middle;}
    return (int)low;
}

static int sync_sample_for(const Mp4Track *t,int sample)
{
    uint32_t low=0,high;
    if(!t||!t->sync_count||!t->sync_samples)return sample;
    high=t->sync_count;
    while(low+1<high){uint32_t middle=low+(high-low)/2;if(t->sync_samples[middle]<=(uint32_t)sample)low=middle;else high=middle;}
    if(t->sync_samples[low]>(uint32_t)sample)return 0;
    return (int)t->sync_samples[low];
}

static int ensure_time_available(Player *p,int slot,Mp4Track *t,uint32_t target_ms)
{
    while(!p->stop&&p->fragmented[slot]&&
          (!t->sample_count||t->times[t->sample_count-1]<target_ms)){
        uint32_t old=t->sample_count;
        int status=ensure_sample(p,slot,t,old);
        if(status<=0||t->sample_count<=old)break;
    }
    return sample_for_time(t,target_ms);
}

typedef struct Vertex { unsigned short u,v; float x,y,z; } Vertex;
static unsigned int __attribute__((aligned(16))) gu_list[65536];

static void draw_piece(void *image,int stride,int texture_width,int texture_height,
    int screen_x,int screen_width,int pixel_format)
{
    int tx=0;
    sceGuTexMode(pixel_format,0,0,0);sceGuTexImage(0,512,512,stride,image);
    sceGuTexFunc(GU_TFX_REPLACE,GU_TCC_RGB);sceGuTexFilter(GU_LINEAR,GU_LINEAR);sceGuTexWrap(GU_CLAMP,GU_CLAMP);
    while(tx<texture_width){
        int next=tx+16; Vertex *v; int x0,x1;
        if(next>texture_width)next=texture_width;
        x0=screen_x+tx*screen_width/texture_width;x1=screen_x+next*screen_width/texture_width;
        v=(Vertex*)sceGuGetMemory(2*sizeof(Vertex));
        v[0].u=tx;v[0].v=0;v[0].x=x0;v[0].y=1;v[0].z=0;
        v[1].u=next;v[1].v=texture_height;v[1].x=x1;v[1].y=271;v[1].z=0;
        sceGuDrawArray(GU_SPRITES,GU_TEXTURE_16BIT|GU_VERTEX_32BITF|GU_TRANSFORM_2D,2,NULL,v);tx=next;
    }
}

static void video_gu_init(void)
{
    sceGuInit();sceGuStart(GU_DIRECT,gu_list);sceGuDrawBuffer(GU_PSM_8888,(void*)0,512);
    sceGuDispBuffer(480,272,(void*)0x88000,512);sceGuOffset(2048-240,2048-136);
    sceGuViewport(2048,2048,480,272);sceGuScissor(0,0,480,272);sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_TEXTURE_2D);sceGuClearColor(0);sceGuClear(GU_COLOR_BUFFER_BIT);sceGuFinish();sceGuSync(0,0);
    sceDisplayWaitVblankStart();sceGuDisplay(GU_TRUE);
}

static void show_frame(void *rgb,int width,int height)
{
    int left_width;
    sceKernelDcacheWritebackInvalidateAll();sceGuStart(GU_DIRECT,gu_list);sceGuClearColor(0);sceGuClear(GU_COLOR_BUFFER_BIT);
    if(width>480){left_width=480*480/width;draw_piece(rgb,768,480,height,0,left_width,GU_PSM_8888);draw_piece((unsigned char*)rgb+480*4,768,width-480,height,left_width,480-left_width,GU_PSM_8888);}
    else draw_piece(rgb,512,width,height,0,480,GU_PSM_8888);
    sceGuFinish();sceGuSync(0,0);sceDisplayWaitVblankStart();sceGuSwapBuffers();
}

static void show_frame_565(void *rgb,int width,int height)
{
    int left_width;
    sceKernelDcacheWritebackInvalidateAll();sceGuStart(GU_DIRECT,gu_list);sceGuClearColor(0);sceGuClear(GU_COLOR_BUFFER_BIT);
    if(width>480){left_width=480*480/width;draw_piece(rgb,768,480,height,0,left_width,GU_PSM_5650);draw_piece((unsigned char*)rgb+480*2,768,width-480,height,left_width,480-left_width,GU_PSM_5650);}
    else draw_piece(rgb,512,width,height,0,480,GU_PSM_5650);
    sceGuFinish();sceGuSync(0,0);sceDisplayWaitVblankStart();sceGuSwapBuffers();
}

/* 1 = vero IDR, 2 = recovery_point SEI senza IDR, 0 = frame dipendente. */
static int sample_decoder_epoch(const unsigned char *data, uint32_t size, int prefix)
{
    uint32_t at=0;int kind=0;
    if(prefix<1||prefix>4)return 0;
    while(at+(uint32_t)prefix<size){
        uint32_t n=0;int j,type;
        for(j=0;j<prefix;j++)n=(n<<8)|data[at+j];
        at+=(uint32_t)prefix;
        if(!n||at+n>size)return 0;
        type=data[at]&31;
        if(type==5)return 1;
        if(type==6&&n>1){
            /* Il NAL 6 e' un contenitore SEI, non necessariamente un keyframe.
               Solo payload_type 6 indica recovery_point. I normali SEI
               user_data (payload_type 5) non devono azzerare Media Engine. */
            uint32_t p=at+1,end=at+n;
            int payload_type=0;
            while(p<end&&data[p]==0xff){payload_type+=255;p++;}
            if(p<end){payload_type+=data[p];if(payload_type==6)kind=2;}
        }
        at+=n;
    }
    return kind;
}

static int sample_to_annexb(Mp4Track *t,const unsigned char *sample,
    uint32_t size,unsigned char *out,uint32_t capacity,int first)
{
    uint32_t at=0,used=0;
    if(first){
        if(used+8+(uint32_t)t->sps_size+(uint32_t)t->pps_size>capacity)return -1;
        out[used++]=0;out[used++]=0;out[used++]=0;out[used++]=1;
        memcpy(out+used,t->sps,t->sps_size);used+=(uint32_t)t->sps_size;
        out[used++]=0;out[used++]=0;out[used++]=0;out[used++]=1;
        memcpy(out+used,t->pps,t->pps_size);used+=(uint32_t)t->pps_size;
    }
    while(at+(uint32_t)t->nal_prefix<=size){
        uint32_t n=0;int j;
        for(j=0;j<t->nal_prefix;j++)n=(n<<8)|sample[at+j];
        at+=(uint32_t)t->nal_prefix;
        if(!n||at+n>size||used+4+n>capacity)return -1;
        out[used++]=0;out[used++]=0;out[used++]=0;out[used++]=1;
        memcpy(out+used,sample+at,n);used+=n;at+=n;
    }
    return at==size?(int)used:-1;
}

static int clamp_u8(int value)
{
    if(value<0)return 0;
    if(value>255)return 255;
    return value;
}

static void yuv420_to_abgr(const AVFrame *frame,unsigned int *dst,int width,int height)
{
    int y;
    for(y=0;y<height;y++){
        const unsigned char *src_y=frame->data[0]+y*frame->linesize[0];
        const unsigned char *src_u=frame->data[1]+(y>>1)*frame->linesize[1];
        const unsigned char *src_v=frame->data[2]+(y>>1)*frame->linesize[2];
        unsigned int *row=dst+y*512;int x;
        for(x=0;x<width;x++){
            int c=(int)src_y[x]-16,d=(int)src_u[x>>1]-128,e=(int)src_v[x>>1]-128;
            int r,g,b;if(c<0)c=0;
            r=clamp_u8((298*c+409*e+128)>>8);
            g=clamp_u8((298*c-100*d-208*e+128)>>8);
            b=clamp_u8((298*c+516*d+128)>>8);
            row[x]=0xff000000U|((unsigned int)b<<16)|((unsigned int)g<<8)|(unsigned int)r;
        }
    }
}

static int video_thread_software(SceSize args,void *argp)
{
    Player*p=&player;Mp4Track*t=p->file.video;AVCodec*codec=NULL;
    AVCodecContext*ctx=NULL;AVFrame*frame=NULL;void*sample=NULL,*packet=NULL,*rgb=NULL;
    int i,result=-80,got=0,packet_bytes,decoded,first_picture=0,gu_initialized=0,seek_reset=0;
    unsigned int seen_seek=0;
    uint32_t packet_capacity=0,decode_total_us=0,convert_total_us=0;
    (void)args;(void)argp;
    utubbu_log("software video thread",0);utubbu_log("video width",t->width);utubbu_log("video height",t->height);
    if(t->width>480||t->height>272){result=-81;goto done;}
    avcodec_register_all();codec=avcodec_find_decoder(CODEC_ID_H264);
    utubbu_log("software h264 codec",codec?1:0);if(!codec){result=-82;goto done;}
    ctx=avcodec_alloc_context();frame=avcodec_alloc_frame();rgb=memalign(64,512*272*4);
    if(!ctx||!frame||!rgb){result=-83;goto done;}
    ctx->width=t->width;ctx->height=t->height;
    result=avcodec_open(ctx,codec);utubbu_log("software h264 open",result);if(result<0){result=-84;goto done;}
    memset(rgb,0,512*272*4);video_gu_init();gu_initialized=1;p->video_ready=1;result=0;
    for(i=0;!p->stop;i++){
        uint32_t sample_capacity,start_us,elapsed;int ready;
        while(p->paused&&!p->stop)sceKernelDelayThread(10000);
        if(p->stop)break;
        if(seen_seek!=p->seek_generation){
            unsigned int generation=p->seek_generation;
            int requested=ensure_time_available(p,0,t,p->seek_target_ms);
            i=sync_sample_for(t,requested);seen_seek=generation;seek_reset=1;
            avcodec_flush_buffers(ctx);
        }
        p->video_index=i;ready=ensure_sample(p,0,t,(uint32_t)i);
        if(ready<=0){result=ready<0?-87:0;break;}
        sample_capacity=(t->sizes[i]+63)&~63U;free(sample);sample=memalign(64,sample_capacity);
        if(!sample){result=-88;break;}memset(sample,0,sample_capacity);
        if(read_at_slot(p,0,t->offsets[i],sample,t->sizes[i])<0){result=-89;break;}
        if(packet_capacity<t->sizes[i]+t->sps_size+t->pps_size+64+FF_INPUT_BUFFER_PADDING_SIZE){
            packet_capacity=t->sizes[i]+t->sps_size+t->pps_size+64+FF_INPUT_BUFFER_PADDING_SIZE;
            free(packet);packet=memalign(64,(packet_capacity+63)&~63U);if(!packet){result=-90;break;}
        }
        packet_bytes=sample_to_annexb(t,sample,t->sizes[i],packet,packet_capacity,i==0||seek_reset);
        if(packet_bytes<0){result=-91;break;}
        memset((unsigned char*)packet+packet_bytes,0,FF_INPUT_BUFFER_PADDING_SIZE);
        start_us=sceKernelGetSystemTimeLow();got=0;
        decoded=avcodec_decode_video(ctx,frame,&got,packet,packet_bytes);
        seek_reset=0;
        elapsed=sceKernelGetSystemTimeLow()-start_us;decode_total_us+=elapsed;
        if(i==0||i==30||i==300){utubbu_log("software decode frame",i);utubbu_log("software decode us",elapsed);}
        if(decoded<0){utubbu_log("software decode error",decoded);result=-92;break;}
        if(got){
            uint32_t convert_start,convert_elapsed;
            while(!p->stop&&!p->paused&&seen_seek==p->seek_generation&&!p->audio_ready&&i>0)sceKernelDelayThread(1000);
            while(!p->stop&&!p->paused&&seen_seek==p->seek_generation&&p->audio_ready&&t->times[i]>(uint64_t)p->audio_time_ms+10)sceKernelDelayThread(1000);
            while(p->paused&&!p->stop&&seen_seek==p->seek_generation)sceKernelDelayThread(10000);
            if(seen_seek!=p->seek_generation){i--;continue;}
            if(p->audio_ready&&t->times[i]+80<p->audio_time_ms){p->video_dropped++;continue;}
            convert_start=sceKernelGetSystemTimeLow();
            yuv420_to_abgr(frame,(unsigned int*)rgb,t->width,t->height);
            convert_elapsed=sceKernelGetSystemTimeLow()-convert_start;convert_total_us+=convert_elapsed;
            if(i==0||i==30||i==300){utubbu_log("software convert frame",i);utubbu_log("software convert us",convert_elapsed);}
            sceKernelDcacheWritebackInvalidateAll();show_frame(rgb,t->width,t->height);
            if(!first_picture){first_picture=1;utubbu_log("software first picture",i);}
        }
        {SceCtrlData pad;sceCtrlPeekBufferPositive(&pad,1);if(pad.Buttons&PSP_CTRL_CIRCLE)p->stop=1;}
    }
done:
    if(gu_initialized)sceGuTerm();if(ctx)avcodec_close(ctx);
    utubbu_log("software decode total us",decode_total_us);utubbu_log("video late total",p->video_dropped);
    utubbu_log("software convert total us",convert_total_us);
    utubbu_log("video result",result);free(sample);free(packet);free(rgb);if(frame)av_free(frame);if(ctx)av_free(ctx);
    if(result&&!p->stop)p->error=result;p->video_done=1;sceKernelExitDeleteThread(0);return 0;
}

static int video_thread_hardware(SceSize args, void *argp)
{
    Player *p=&player;Mp4Track*t=p->file.video;SceMpeg mpeg;SceMpegRingbuffer ring;
    SceMpegAvcMode avc_mode;
    void *mpeg_mem=NULL,*ddr=NULL,*au_mem=NULL,*sample=NULL,*sps_pps=NULL,*rgb=NULL;SceMpegAu*au=NULL;int i,mem_size,pics=0,result=-40,mpeg_mode,decode_stride,pps_offset,sps_pps_capacity,rgb_height,waiting_for_idr=0,consecutive_decode_errors=0,seek_reset=0;
    unsigned int seen_seek=0;
    (void)args;(void)argp;memset(&mpeg,0,sizeof(mpeg));memset(&ring,0,sizeof(ring));
    utubbu_log("video thread", 0);
    result=sceMpegInit();utubbu_log("sceMpegInit",result);if(result<0)goto done;
    utubbu_log("video width",t->width);utubbu_log("video height",t->height);
    if(t->width>720||t->height>480){result=-47;utubbu_log("video too large",result);goto finish;}
    mpeg_mode=(t->width>480||t->height>272)?5:4;
    decode_stride=t->width>480?768:512;
    /* BaseCsc writes complete 16-line macroblock rows.  Wide mode reference
       players reserve a full 768x480 surface; allocating only the visible
       360 lines lets the first 640x360 picture overrun the RGB buffer. */
    rgb_height=t->width>480?480:272;
    utubbu_log("video mpeg mode",mpeg_mode);utubbu_log("video output stride",decode_stride);
    utubbu_log("video mpeg stride",512);
    mem_size=sceMpegQueryMemSizeUtubbu(mpeg_mode);utubbu_log("sceMpegQueryMemSize new",mem_size);if(mem_size<0)goto finish;
    mem_size=(mem_size&~15)+16;
    mpeg_mem=memalign(64,mem_size);ddr=memalign(0x400000,0x200000);au=memalign(64,64);
    /* The PSP MPEG firmware expects the avcC parameter sets as one compact
       block.  Align the allocation, not the PPS start: a 64-byte hole between
       SPS and PPS can leave the wide (mode 5) decoder waiting forever. */
    pps_offset=t->sps_size;
    sps_pps_capacity=(t->sps_size+t->pps_size+63)&~63;
    sps_pps=memalign(64,sps_pps_capacity);
    rgb=memalign(64,decode_stride*rgb_height*4);
    if(!mpeg_mem||!ddr||!au||!sps_pps||!rgb){result=-45;utubbu_log("video alloc",result);goto finish;}
    memset(sps_pps,0,sps_pps_capacity);
    memcpy(sps_pps,t->sps,t->sps_size);memcpy((unsigned char*)sps_pps+pps_offset,t->pps,t->pps_size);
    result=sceMpegCreateUtubbu(&mpeg,mpeg_mem,mem_size,&ring,512,mpeg_mode,(int)ddr);utubbu_log("sceMpegCreate new",result);if(result<0)goto finish;
    au_mem=(unsigned char*)ddr+0x10000;memset(au,0xff,64);result=sceMpegInitAu(&mpeg,au_mem,au);utubbu_log("sceMpegInitAu",result);if(result<0)goto delete_mpeg;
    if(t->width<=480&&t->height<=272){
        avc_mode.iUnk0=-1;avc_mode.iPixelFormat=PSP_DISPLAY_PIXEL_FORMAT_8888;
        result=sceMpegAvcDecodeMode(&mpeg,&avc_mode);utubbu_log("sceMpegAvcDecodeMode",result);if(result<0)goto delete_mpeg;
    }else utubbu_log("sceMpegAvcDecodeMode skipped",mpeg_mode);
    memset(rgb,0,decode_stride*rgb_height*4);
    sceKernelDcacheWritebackInvalidateRange(sps_pps,sps_pps_capacity);
    video_gu_init();
    /* Audio may initialize now; its hardware output becomes the master clock. */
    p->video_ready=1;
    for(i=0;!p->stop;i++){
        Mp4AvcNal nal;uint64_t target;
        int failure_stage=0;
        while(p->paused&&!p->stop)sceKernelDelayThread(10000);
        if(p->stop)break;
        if(seen_seek!=p->seek_generation){
            unsigned int generation=p->seek_generation;
            int requested=ensure_time_available(p,0,t,p->seek_target_ms);
            i=sync_sample_for(t,requested);seen_seek=generation;seek_reset=1;
            waiting_for_idr=0;consecutive_decode_errors=0;
        }
        p->video_index=i;
        unsigned int sample_capacity;int read_result,epoch,actual_idr,ready=ensure_sample(p,0,t,(uint32_t)i);
        if(ready<=0){result=ready<0?-46:0;break;}
        sample_capacity=(t->sizes[i]+63)&~63U;free(sample);sample=memalign(64,sample_capacity);if(!sample){result=-41;break;}
        memset(sample,0,sample_capacity);
        read_result=read_at_slot(p,0,t->offsets[i],sample,t->sizes[i]);
        if(read_result<0){utubbu_log("video read code",read_result);utubbu_log("video read offset",(int)t->offsets[i]);result=-42;break;}
        epoch=sample_decoder_epoch(sample,t->sizes[i],t->nal_prefix);
        actual_idr=epoch==1;
        if(waiting_for_idr&&!actual_idr)continue;
        if(waiting_for_idr){waiting_for_idr=0;consecutive_decode_errors=0;utubbu_log("video resume idr",i);}
        nal.sps=sps_pps;nal.sps_size=t->sps_size;nal.pps=(unsigned char*)sps_pps+pps_offset;nal.pps_size=t->pps_size;
        nal.nal_prefix=t->nal_prefix;nal.nal=sample;nal.nal_size=t->sizes[i];
        /* Mode 3 injects SPS/PPS and starts the firmware decoder.  Repeating
           it at every later IDR flushes pictures still pending for display
           (notably B-frame GOPs) and makes the following AU fail with
           0x80628002.  The stream keeps the same parameter sets throughout,
           so every AU after the first must remain in mode 0. */
        nal.mode=(i&&!seek_reset)?0:3;
        if(actual_idr&&i==0)utubbu_log("video idr",i);
        target=t->times[i];
        while(!p->stop&&!p->paused&&seen_seek==p->seek_generation&&!p->audio_ready&&i>0)sceKernelDelayThread(1000);
        while(!p->stop&&!p->paused&&seen_seek==p->seek_generation&&p->audio_ready&&target>(uint64_t)p->audio_time_ms+10)
            sceKernelDelayThread(1000);
        while(p->paused&&!p->stop&&seen_seek==p->seek_generation)sceKernelDelayThread(10000);
        if(seen_seek!=p->seek_generation){i--;continue;}
        if(i==30||i==300||i==600){
            utubbu_log("video schedule frame",i);
            utubbu_log("video schedule ms",(int)t->times[i]);
            utubbu_log("audio master ms",(int)p->audio_time_ms);
        }
        sceKernelDcacheWritebackInvalidateRange(sample,sample_capacity);
        sceKernelWaitSema(p->codec_mutex,1,NULL);
        failure_stage=1;
        result=sceMpegGetAvcNalAu(&mpeg,&nal,au);
        seek_reset=0;
        i=p->video_index;
        /* The VSH MPEG implementation used by current CFW exposes decoded
           YUV through Detail2. Passing our own VRAM surface array here makes
           it reject the first AU with 0x80628001 on real hardware. */
        /* Il parametro di sceMpegAvcDecode resta 512 anche in mode 5.
           Soltanto BaseCsc usa 768 per una superficie RGB larga oltre 480. */
        if(result>=0){
            failure_stage=2;
            result=sceMpegAvcDecode(&mpeg,au,512,NULL,&pics);
            i=p->video_index;
        }
        if(result>=0){
            int detail_status,csc_status=0;
            Mp4AvcDetail *detail=NULL;
            /* Decode, Detail2 e CSC formano una sola transazione Media Engine.
               AAC non deve entrare tra queste chiamate, soprattutto in mode 5. */
            failure_stage=3;
            detail_status=sceMpegAvcDecodeDetail2(&mpeg,&detail);
            i=p->video_index;
            if(detail_status<0)result=-48;
            else if(pics>0&&(!detail||!detail->info||!detail->yuv))result=-48;
            else if(pics>0&&
                (!p->audio_ready||t->times[p->video_index]+80>=p->audio_time_ms)){
                Mp4AvcYuv *yuv=detail->yuv;
                Mp4AvcCsc csc;
                csc.height=(detail->info->height+15)>>4;csc.width=(detail->info->width+15)>>4;
                csc.mode0=0;csc.mode1=0;memcpy(csc.b,yuv->b,sizeof(csc.b));
                failure_stage=4;
                csc_status=sceMpegBaseCscAvc(rgb,0,decode_stride,&csc);
                i=p->video_index;
                if(csc_status<0)result=-49;
                /* CSC has finished using the Media Engine.  Do not keep AAC
                   locked out while GU waits for the next display refresh. */
                if(result>=0){
                    sceKernelSignalSema(p->codec_mutex,1);
                    show_frame(rgb,t->width,t->height);
                    i=p->video_index;
                    sceKernelWaitSema(p->codec_mutex,1,NULL);
                }
            }else if(pics>0){
                p->video_dropped++;
                if(p->video_dropped==1||!(p->video_dropped%120))
                    utubbu_log("video late drops",p->video_dropped);
            }
            if(i==0&&pics>0)utubbu_log("sceMpegBaseCscAvc",csc_status);
        }
        sceKernelSignalSema(p->codec_mutex,1);
        if(i==0)utubbu_log("sceMpegAvcDecode",result);
        if(result<0){
            utubbu_log("decode frame",i);utubbu_log("decode error",result);
            utubbu_log("decode failure stage",failure_stage);
            utubbu_log("decode sample bytes",(int)t->sizes[i]);
            utubbu_log("decode nal epoch",epoch);
            utubbu_log("decode nal mode",nal.mode);
            utubbu_log("decode pictures",pics);
            if((unsigned int)result==0x80628002U){
                /* Un recovery-point YouTube puo produrre un errore transitorio
                   senza rendere inutilizzabile il GOP. Continua ad alimentare
                   il decoder; salta fino all'IDR solo dopo una vera sequenza
                   di errori, evitando fermo-immagine di diversi secondi. */
                consecutive_decode_errors++;
                /* An IDR rejection at startup makes the picture appear stuck
                   even when later AUs might recover.  Let the caller switch
                   to the PSP compatibility stream immediately. */
                if(i<120){
                    utubbu_log("video compatibility retry",i);
                    result=-71;break;
                }
                if(consecutive_decode_errors>=8){waiting_for_idr=1;utubbu_log("video wait for idr",i);}
                else utubbu_log("video transient drop",consecutive_decode_errors);
                result=0;continue;
            }
            result=i<240?-71:-43;break;
        }
        consecutive_decode_errors=0;
        {SceCtrlData pad;sceCtrlPeekBufferPositive(&pad,1);if(pad.Buttons&PSP_CTRL_CIRCLE)p->stop=1;}
    }
    sceGuTerm();
delete_mpeg:sceMpegDelete(&mpeg);
finish:sceMpegFinish();
done:utubbu_log("video late total",p->video_dropped);utubbu_log("video result",result);free(sample);free(au);free(ddr);free(mpeg_mem);free(sps_pps);free(rgb);if(result&& !p->stop)p->error=result;p->video_done=1;
    sceKernelExitDeleteThread(0);return 0;
}

static int audio_thread(SceSize args, void *argp)
{
    Player*p=&player;Mp4Track*t=p->audio_file.audio;unsigned long codec[65] __attribute__((aligned(64)));
    short pcm[4][2048] __attribute__((aligned(64)));
    void*sample=NULL;int channel=-1,i,result=-50,pcm_logged=0,edram=0,src_reserved=0;
    unsigned int seen_seek=0;
    int he_aac=(t->aac_object_type==5||t->aac_object_type==29);int pcm_frames=1024;
    (void)args;(void)argp;memset(codec,0,sizeof(codec));utubbu_log("audio cache build",2);
    while(!p->stop&&!p->error&&!p->video_ready)sceKernelDelayThread(1000);
    if(p->stop||p->error)goto done;
    result=sceAudiocodecCheckNeedMem(codec,PSP_CODEC_AAC);utubbu_log("aac need mem",result);if(result<0)goto done;
    utubbu_log("aac mem size",(int)codec[4]);
    result=sceAudiocodecGetEDRAM(codec,PSP_CODEC_AAC);utubbu_log("aac get edram",result);if(result<0)goto done;edram=1;
    utubbu_log("aac object",t->aac_object_type);utubbu_log("aac output rate",t->sample_rate);utubbu_log("aac core rate",t->aac_core_rate);
    /* Il firmware vuole la frequenza dichiarata dal contenitore anche quando
       restituisce soltanto i 1024 campioni del core HE-AAC. */
    codec[10]=t->aac_core_rate;result=sceAudiocodecInit(codec,PSP_CODEC_AAC);utubbu_log("aac init",result);if(result<0)goto done;
    if(he_aac){channel=sceAudioSRCChReserve(pcm_frames,t->aac_core_rate,2);src_reserved=(channel>=0);utubbu_log("audio src channel",channel);}
    else{channel=sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL,pcm_frames,PSP_AUDIO_FORMAT_STEREO);utubbu_log("audio channel",channel);}
    if(channel<0)goto done;
    p->audio_time_ms=0;p->audio_ready=1;
    for(i=0;!p->stop;i++){
        while(p->paused&&!p->stop)sceKernelDelayThread(10000);
        if(p->stop)break;
        if(seen_seek!=p->seek_generation){
            unsigned int generation=p->seek_generation;
            i=ensure_time_available(p,1,t,p->seek_target_ms);
            seen_seek=generation;p->audio_time_ms=t->times[i];
        }
        p->audio_index=i;
        int read_result,ready=ensure_sample(p,1,t,(uint32_t)i);
        short *current=pcm[i&3];
        if(ready<=0){result=ready<0?-54:0;break;}
        unsigned int sample_capacity=(t->sizes[i]+63)&~63U;
        free(sample);sample=memalign(64,sample_capacity);if(!sample){result=-51;break;}
        memset(sample,0,sample_capacity);
        read_result=read_at_slot(p,1,t->offsets[i],sample,t->sizes[i]);
        if(read_result<0){utubbu_log("audio read code",read_result);utubbu_log("audio read offset",(int)t->offsets[i]);result=-52;break;}
        int decode_result;
        memset(current,0,4096);
        codec[6]=(unsigned long)sample;codec[8]=(unsigned long)current;codec[7]=t->sizes[i];codec[9]=4096;
        /* Il codec AAC gira fuori dalla CPU principale.  Senza questa
           sincronizzazione puo leggere vecchi byte AAC e la CPU puo leggere
           vecchi campioni PCM: su hardware il risultato e audio robotico. */
        sceKernelDcacheWritebackInvalidateRange(sample,sample_capacity);
        sceKernelDcacheWritebackInvalidateRange(current,4096);
        sceKernelWaitSema(p->codec_mutex,1,NULL);
        decode_result=sceAudiocodecDecode(codec,PSP_CODEC_AAC);
        i=p->audio_index;
        sceKernelSignalSema(p->codec_mutex,1);
        sceKernelDcacheInvalidateRange(current,4096);
        if(i==0)utubbu_log("aac decode",decode_result);
        if(decode_result<0){
            p->audio_decode_errors++;
            if(p->audio_decode_errors<=4)utubbu_log("aac decode error",decode_result);
            /* A transient Media Engine refusal is recoverable once the video
               transaction has released the shared codec. */
            sceKernelDelayThread(1000);
            codec[6]=(unsigned long)sample;codec[8]=(unsigned long)current;
            codec[7]=t->sizes[i];codec[9]=4096;
            sceKernelDcacheWritebackInvalidateRange(sample,sample_capacity);
            sceKernelDcacheWritebackInvalidateRange(current,4096);
            sceKernelWaitSema(p->codec_mutex,1,NULL);
            decode_result=sceAudiocodecDecode(codec,PSP_CODEC_AAC);
            i=p->audio_index;
            sceKernelSignalSema(p->codec_mutex,1);
            sceKernelDcacheInvalidateRange(current,4096);
            if(decode_result>=0)utubbu_log("aac retry recovered",p->audio_decode_errors);
        }
        if(decode_result<0)memset(current,0,pcm_frames*4);
        if(!pcm_logged&&i<32){int peak=0,k;for(k=0;k<pcm_frames*2;k++){int v=current[k];if(v<0)v=-v;if(v>peak)peak=v;}if(peak||i==31){utubbu_log("aac pcm frame",i);utubbu_log("aac pcm peak",peak);pcm_logged=1;}}
        while(p->paused&&!p->stop&&seen_seek==p->seek_generation)sceKernelDelayThread(10000);
        if(seen_seek!=p->seek_generation){i--;continue;}
        /* sceAudioOutputBlocking puo restituire mentre il DMA sta ancora
           leggendo il buffer appena consegnato. Alternare due superfici evita
           che la decodifica successiva corrompa l'audio in riproduzione. */
        if(src_reserved)sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX,current);
        else sceAudioOutputBlocking(channel,PSP_AUDIO_VOLUME_MAX,current);
        i=p->audio_index;
        p->audio_time_ms=i+1<(int)t->sample_count?t->times[i+1]:
            t->times[i]+(uint32_t)((uint64_t)pcm_frames*1000/t->aac_core_rate);
    }
    if(src_reserved)sceAudioSRCChRelease();else if(channel>=0)sceAudioChRelease(channel);
done:utubbu_log("audio decode errors",p->audio_decode_errors);utubbu_log("audio result",result);free(sample);if(edram)sceAudiocodecReleaseEDRAM(codec);p->audio_done=1;sceKernelExitDeleteThread(0);return 0;
}

static int utubbu_mp4_play_internal(const char *video_path,const char *audio_path,int software_video)
{
    static int av_module_ready=0;
    int result,i,wait_result;SceUInt timeout;
    utubbu_log("player start",0);
    memset(&player,0,sizeof(player));player.software_video=software_video;player.fd[0]=-1;player.fd[1]=-1;player.audio_thread=-1;player.video_thread=-1;player.codec_mutex=-1;
    if(strchr(video_path,':')) result=snprintf(player.path[0],sizeof(player.path[0]),"%s",video_path);
    else result=snprintf(player.path[0],sizeof(player.path[0]),"ms0:/PSP/GAME/UTUBBU/%s",video_path);
    if(result<0||result>=(int)sizeof(player.path[0]))return -68;
    if(strchr(audio_path,':')) result=snprintf(player.path[1],sizeof(player.path[1]),"%s",audio_path);
    else result=snprintf(player.path[1],sizeof(player.path[1]),"ms0:/PSP/GAME/UTUBBU/%s",audio_path);
    if(result<0||result>=(int)sizeof(player.path[1]))return -68;
    player.io_mutex=sceKernelCreateSema("utubbu_io",0,1,1,NULL);
    if(player.io_mutex<0)return -69;
    player.codec_mutex=sceKernelCreateSema("utubbu_codec",0,1,1,NULL);
    if(player.codec_mutex<0){sceKernelDeleteSema(player.io_mutex);return -67;}
    result=parse_mp4(&player,&player.file,0,HANDLER_VIDEO);utubbu_log("parse video mp4",result);if(result<0)goto done;
    result=parse_mp4(&player,&player.audio_file,1,HANDLER_AUDIO);utubbu_log("parse audio mp4",result);if(result<0)goto done;
    utubbu_log("video track id",(int)player.file.video->track_id);
    utubbu_log("audio track id",(int)player.audio_file.audio->track_id);
    utubbu_log("video timescale",(int)player.file.video->timescale);
    utubbu_log("audio timescale",(int)player.audio_file.audio->timescale);
    utubbu_log("video sample count",(int)player.file.video->sample_count);
    if(player.file.video->sample_count>30)utubbu_log("video time frame30",(int)player.file.video->times[30]);
    if(player.file.video->sample_count>300)utubbu_log("video time frame300",(int)player.file.video->times[300]);
    result=ensure_sample(&player,0,player.file.video,0);utubbu_log("first video fragment",result);if(result<=0){result=-65;goto done;}
    result=ensure_sample(&player,1,player.audio_file.audio,0);utubbu_log("first audio fragment",result);if(result<=0){result=-66;goto done;}
    if(player.fd[0]>=0){sceIoClose(player.fd[0]);player.fd[0]=-1;player.fd_size[0]=0;}
    if(player.fd[1]>=0){sceIoClose(player.fd[1]);player.fd[1]=-1;player.fd_size[1]=0;}
    wait_initial_buffer(&player);
    if(!av_module_ready){
        result=sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);
        utubbu_log("load avcodec module",result);if(result<0){result=-60;goto done;}
        av_module_ready=1;
    }
    utubbu_log("video sps profile",player.file.video->sps_size>1?player.file.video->sps[1]:-1);
    utubbu_log("video sps constraints",player.file.video->sps_size>2?player.file.video->sps[2]:-1);
    utubbu_log("video sps level",player.file.video->sps_size>3?player.file.video->sps[3]:-1);
    if(!software_video){
        result=utubbu_media_engine_start(player.file.video->width,player.file.video->height,
            player.file.video->sps_size>1?player.file.video->sps[1]:0);
        utubbu_log("media engine start",result);if(result<0)goto done;
    }else utubbu_log("media engine software bypass",0);
    player.audio_thread=sceKernelCreateThread("utubbu_audio",audio_thread,0x16,0x10000,PSP_THREAD_ATTR_USER,NULL);
    player.video_thread=sceKernelCreateThread("utubbu_video",software_video?video_thread_software:video_thread_hardware,0x17,0x50000,PSP_THREAD_ATTR_USER,NULL);
    utubbu_log("audio thread id",player.audio_thread);utubbu_log("video thread id",player.video_thread);
    if(player.audio_thread<0||player.video_thread<0){result=-61;goto done;}
    sceKernelStartThread(player.audio_thread,0,NULL);sceKernelStartThread(player.video_thread,0,NULL);
    {SceCtrlData pad;sceCtrlPeekBufferPositive(&pad,1);player.control_buttons=pad.Buttons;}
    while(!player.stop&&!player.error&&(!player.audio_done||!player.video_done)){
        SceCtrlData pad;unsigned int pressed,current,target,duration=0;
        sceCtrlPeekBufferPositive(&pad,1);pressed=pad.Buttons&~player.control_buttons;
        player.control_buttons=pad.Buttons;
        if(pressed&PSP_CTRL_CIRCLE)player.stop=1;
        if(pressed&PSP_CTRL_CROSS)player.paused=!player.paused;
        if(pressed&(PSP_CTRL_LEFT|PSP_CTRL_RIGHT)){
            current=player.audio_ready?player.audio_time_ms:
                player.file.video->times[player.video_index];
            if(player.file.video->sample_count)
                duration=player.file.video->times[player.file.video->sample_count-1];
            if(pressed&PSP_CTRL_LEFT)target=current>10000?current-10000:0;
            else target=current+10000;
            if(!player.fragmented[0]&&duration&&target>duration)target=duration;
            player.seek_target_ms=target;
            player.seek_generation++;
        }
        sceKernelDelayThread(20000);
    }
    player.stop=1;
    if(!player.audio_done){
        timeout=3000000;wait_result=sceKernelWaitThreadEnd(player.audio_thread,&timeout);
        utubbu_log("audio stop wait",wait_result);
        if(wait_result<0)sceKernelTerminateDeleteThread(player.audio_thread);
    }
    if(!player.video_done){
        timeout=3000000;wait_result=sceKernelWaitThreadEnd(player.video_thread,&timeout);
        utubbu_log("video stop wait",wait_result);
        if(wait_result<0)sceKernelTerminateDeleteThread(player.video_thread);
    }
    result=player.error;
done:utubbu_log("player result",result);if(player.fd[0]>=0)sceIoClose(player.fd[0]);if(player.fd[1]>=0)sceIoClose(player.fd[1]);if(player.io_mutex>=0)sceKernelDeleteSema(player.io_mutex);if(player.codec_mutex>=0)sceKernelDeleteSema(player.codec_mutex);for(i=0;i<player.file.count;i++)free_track(&player.file.tracks[i]);for(i=0;i<player.audio_file.count;i++)free_track(&player.audio_file.tracks[i]);return result;
}

int utubbu_mp4_play(const char *video_path,const char *audio_path)
{ return utubbu_mp4_play_internal(video_path,audio_path,0); }

int utubbu_mp4_play_software(const char *video_path,const char *audio_path)
{ return utubbu_mp4_play_internal(video_path,audio_path,1); }
