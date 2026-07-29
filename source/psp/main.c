#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspiofilemgr.h>
#include <pspjpeg.h>
#include <pspkernel.h>
#include <psppower.h>
#include <pspgu.h>
#include <psputility_modules.h>

#include <ctype.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "clock_probe.h"
#include "diagnostic.h"
#include "network.h"
#include "mp4_player.h"
#include "osk.h"
#include "youtube.h"
#include "yvid_player.h"

PSP_MODULE_INFO("UTUBBU", 0, 0, 4);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
/* The UI/network main thread uses little stack.  Keep room for the software
   H.264 worker, which needs a larger private stack on real hardware. */
PSP_MAIN_THREAD_STACK_SIZE_KB(512);
/* Leave kernel/user partition room for downloader, AAC and 512 KiB H.264
   thread stacks.  The previous 1 MiB reserve made video-thread creation fail. */
PSP_HEAP_SIZE_KB(-2048);

static volatile int exit_requested;
extern unsigned char ui_font_texture[];

#define THUMB_COUNT 12
#define THUMB_STRIDE 256
#define THUMB_SOURCE_WIDTH 220
#define THUMB_SOURCE_HEIGHT 100
#define JPEG_CONTEXT_SIZE 512

typedef struct UiThumbnail {
    char id[12];
    int ready;
    unsigned int pixels[THUMB_STRIDE*THUMB_SOURCE_HEIGHT] __attribute__((aligned(16)));
} UiThumbnail;

static UiThumbnail ui_thumbnails[THUMB_COUNT];
static int ui_ready;
static unsigned int ui_gu_list[65536] __attribute__((aligned(16)));

static int exit_callback(int a, int b, void *common)
{
    (void)a; (void)b; (void)common;
    utubbu_log("system exit callback", 0);
    exit_requested = 1;
    utubbu_mp4_request_stop();
    utubbu_network_request_cancel();
    /* The Media Engine can remain inside a firmware call indefinitely.  HOME
       must never wait for that worker: let the PSP kernel tear down the
       process and its modules directly from the registered exit callback. */
    utubbu_log("system exit forced", 0);
    sceKernelExitGame();
    return 0;
}

static int showing_history;

static int callback_thread(SceSize args, void *argp)
{
    int callback;
    (void)args; (void)argp;
    callback = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(callback);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    int thread = sceKernelCreateThread("callback_thread", callback_thread,
        0x11, 0xFA0, PSP_THREAD_ATTR_USER, NULL);
    if (thread >= 0) sceKernelStartThread(thread, 0, NULL);
}

static int exists(const char *path)
{
    SceIoStat stat;
    return sceIoGetstat(path, &stat) >= 0;
}

static int connect_wifi(void)
{
    int status;
    /* La rete usa sceNetApctl direttamente e non apre una utility grafica.
       pspDebugScreenInit qui azzerava inutilmente il framebuffer, lasciando
       lo schermo nero durante tutta la successiva richiesta HTTPS. */
    status=utubbu_network_connect(1);
    if(status){
        utubbu_log("wifi automatic retry",status);
        sceKernelDelayThread(500000);
        status=utubbu_network_connect(1);
        utubbu_log("wifi retry result",status);
    }
    return status;
}

static void read_controls(SceCtrlData *pad)
{
    sceCtrlReadBufferPositive(pad, 1);
    if (pad->Lx < 64) pad->Buttons |= PSP_CTRL_LEFT;
    else if (pad->Lx > 192) pad->Buttons |= PSP_CTRL_RIGHT;
    if (pad->Ly < 64) pad->Buttons |= PSP_CTRL_UP;
    else if (pad->Ly > 192) pad->Buttons |= PSP_CTRL_DOWN;
}

static void wait_buttons_released(void)
{
    SceCtrlData pad;
    do {
        read_controls(&pad);
        /* RemoteJoyLite installs its display hook a few seconds after the
           homebrew starts.  Keep producing vblank events even while the UI is
           idle, otherwise its first capture never fires and PC feed stays
           black although the PSP LCD is correct. */
        sceDisplayWaitVblankStart();
    } while (pad.Buttons);
}

static int build_matches(const UtubbuCatalog *catalog, const char *query, int *matches)
{
    int i, count = 0;
    for (i = 0; i < catalog->count; ++i)
        if (utubbu_catalog_matches(&catalog->items[i], query)) matches[count++] = i;
    return count;
}

#define COLOR_WHITE  0x00FFFFFF
#define COLOR_MUTED  0x00999999
#define COLOR_RED    0x002323FF
#define COLOR_DARK   0x000F0F0F
#define COLOR_CARD   0x00272727
#define COLOR_PANEL  0x001A1A1A
#define COLOR_LINE   0x00383838
#define COLOR_GREEN  0x0068D391
#define COLOR_BLUE   0x00D98A3A
#define COLOR_YELLOW 0x0034C8F4

static unsigned int *screen_vram(void)
{
    return (unsigned int *)((unsigned int)sceGeEdramGetAddr() | 0x40000000);
}

static void fill_rect(int x, int y, int width, int height, unsigned int color)
{
    unsigned int *vram = screen_vram();
    int row;
    int column;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > 480) width = 480 - x;
    if (y + height > 272) height = 272 - y;
    for (row = 0; row < height; ++row)
        for (column = 0; column < width; ++column)
            vram[(y + row) * 512 + x + column] = color;
}

static void shade_rect(int x,int y,int width,int height,int brightness)
{
    unsigned int *vram=screen_vram();int row,column;
    for(row=0;row<height;row++)for(column=0;column<width;column++){
        unsigned int color=vram[(y+row)*512+x+column];
        unsigned int r=(color&255)*brightness/255;
        unsigned int g=((color>>8)&255)*brightness/255;
        unsigned int b=((color>>16)&255)*brightness/255;
        vram[(y+row)*512+x+column]=r|(g<<8)|(b<<16);
    }
}

static void outline_rect(int x,int y,int width,int height,int thickness,unsigned int color)
{
    fill_rect(x,y,width,thickness,color);fill_rect(x,y+height-thickness,width,thickness,color);
    fill_rect(x,y,thickness,height,color);fill_rect(x+width-thickness,y,thickness,height,color);
}

static void fill_round_rect(int x,int y,int width,int height,int radius,unsigned int color)
{
    int row,cut;
    fill_rect(x+radius,y,width-radius*2,height,color);
    for(row=0;row<radius;row++){
        cut=radius-row-1;
        fill_rect(x+cut,y+row,width-cut*2,1,color);
        fill_rect(x+cut,y+height-row-1,width-cut*2,1,color);
    }
    fill_rect(x,y+radius,width,height-radius*2,color);
}

static void outline_round_rect(int x,int y,int width,int height,int radius,unsigned int color,unsigned int inside)
{
    fill_round_rect(x,y,width,height,radius,color);
    fill_round_rect(x+1,y+1,width-2,height-2,radius>1?radius-1:1,inside);
}

static void draw_round_outline(int x,int y,int width,int height,int radius,unsigned int color)
{
    int row,cut;
    fill_rect(x+radius-1,y,width-(radius-1)*2,1,color);
    fill_rect(x+radius-1,y+height-1,width-(radius-1)*2,1,color);
    for(row=1;row<radius;row++){
        cut=radius-row-1;
        fill_rect(x+cut,y+row,1,1,color);
        fill_rect(x+width-cut-1,y+row,1,1,color);
        fill_rect(x+cut,y+height-row-1,1,1,color);
        fill_rect(x+width-cut-1,y+height-row-1,1,1,color);
    }
    fill_rect(x,y+radius,1,height-radius*2,color);
    fill_rect(x+width-1,y+radius,1,height-radius*2,color);
}

static int clean_glyph_bounds(unsigned char c,int *left,int *right)
{
    int sx,sy,min=16,max=-1,cell_x=(c&15)*16,cell_y=(c>>4)*16;
    if(c==' '){*left=0;*right=5;return 1;}
    for(sy=0;sy<16;sy++)for(sx=0;sx<16;sx++)
        if(ui_font_texture[((cell_y+sy)*256+cell_x+sx)*4+3]>12){if(sx<min)min=sx;if(sx>max)max=sx;}
    if(max<min){min=0;max=5;}*left=min;*right=max;return 1;
}

static int clean_text_width(const char *text,int size)
{
    int width=0,left,right,spacing=size>=14?2:1;
    while(text&&*text){clean_glyph_bounds((unsigned char)*text++,&left,&right);width+=(right-left+1)*size/16+spacing;}
    return width?width-spacing:0;
}

static void clean_text_spaced(int x,int y,int size,unsigned int color,const char *text,int max_width,int spacing)
{
    unsigned int *vram=screen_vram();int cursor=x;
    while(text&&*text){
        unsigned char c=(unsigned char)*text++;int left,right,glyph_w,dx,dy,cell_x=(c&15)*16,cell_y=(c>>4)*16;
        clean_glyph_bounds(c,&left,&right);glyph_w=(right-left+1)*size/16;if(glyph_w<1)glyph_w=1;
        if(max_width>0&&cursor+glyph_w>x+max_width)break;
        for(dy=0;dy<size;dy++)for(dx=0;dx<glyph_w;dx++){
            int source_width=right-left+1;
            int sx=left+(glyph_w>1?dx*(source_width-1)/(glyph_w-1):source_width/2);
            int sy=size>1?dy*15/(size-1):0;
            unsigned int a=ui_font_texture[((cell_y+sy)*256+cell_x+sx)*4+3];
            if(a&&cursor+dx>=0&&cursor+dx<480&&y+dy>=0&&y+dy<272){
                unsigned int d=vram[(y+dy)*512+cursor+dx];
                unsigned int r=((color&255)*a+(d&255)*(255-a))/255;
                unsigned int g=(((color>>8)&255)*a+((d>>8)&255)*(255-a))/255;
                unsigned int b=(((color>>16)&255)*a+((d>>16)&255)*(255-a))/255;
                vram[(y+dy)*512+cursor+dx]=r|(g<<8)|(b<<16);
            }
        }
        cursor+=glyph_w+spacing;
    }
}

static void clean_text(int x,int y,int size,unsigned int color,const char *text,int max_width)
{
    clean_text_spaced(x,y,size,color,text,max_width,size>=14?2:1);
}

static void clean_text_centered_clip(int x,int y,int width,int size,unsigned int color,const char *text)
{
    char out[64];int length,w;
    if(!text)return;length=(int)strlen(text);if(length>63)length=63;
    memcpy(out,text,length);out[length]='\0';w=clean_text_width(out,size);
    while(length>3&&w>width){out[--length]='\0';w=clean_text_width(out,size);}
    if(text[length]&&length>=3){out[length-3]='.';out[length-2]='.';out[length-1]='.';w=clean_text_width(out,size);}
    clean_text(x+(width-w)/2,y,size,color,out,width);
}

static void clean_text_left_clip(int x,int y,int width,int size,unsigned int color,const char *text)
{
    char out[64];int length;
    if(!text)return;length=(int)strlen(text);if(length>63)length=63;
    memcpy(out,text,length);out[length]='\0';
    while(length>3&&clean_text_width(out,size)>width)out[--length]='\0';
    if(text[length]&&length>=3){out[length-3]='.';out[length-2]='.';out[length-1]='.';}
    clean_text(x,y,size,color,out,width);
}

static void clean_text_centered_two(int x,int y,int width,int size,unsigned int color,const char *text)
{
    char first[64];int take=0,last_space=-1;
    if(!text)return;while(*text==' ')text++;
    while(text[take]&&take<63){
        first[take]=text[take];first[take+1]='\0';
        if(text[take]==' ')last_space=take;
        if(clean_text_width(first,size)>width)break;
        take++;
    }
    if(text[take]&&last_space>0)take=last_space;
    if(take<1)take=1;memcpy(first,text,take);first[take]='\0';
    clean_text_centered_clip(x,y,width,size,color,first);
    text+=take;while(*text==' ')text++;
    if(*text)clean_text_centered_clip(x,y+size+1,width,size,color,text);
}

static void draw_play(int center_x, int center_y, int size, unsigned int color)
{
    unsigned int *vram = screen_vram();
    int x;
    for (x = 0; x < size; ++x) {
        int half = (size - 1 - x) / 2;
        int y;
        for (y = -half; y <= half; ++y)
            vram[(center_y + y) * 512 + center_x + x] = color;
    }
}

static void draw_circle_button(int center_x,int center_y,int radius,unsigned int color)
{
    unsigned int *vram=screen_vram();int x,y;
    for(y=-radius;y<=radius;y++)for(x=-radius;x<=radius;x++){
        int distance=x*x+y*y;
        if(distance<=radius*radius&&distance>=(radius-2)*(radius-2))
            vram[(center_y+y)*512+center_x+x]=color;
    }
}

static void draw_triangle_button(int center_x,int top_y,int size,unsigned int color)
{
    unsigned int *vram=screen_vram();int row;
    for(row=0;row<size;row++){
        int half=row/2,left=center_x-half,right=center_x+half;
        vram[(top_y+row)*512+left]=color;vram[(top_y+row)*512+right]=color;
        if(row==size-1){int x;for(x=left;x<=right;x++)vram[(top_y+row)*512+x]=color;}
    }
}

static void draw_battery(int x,int y,int percent)
{
    int level;if(percent<0)percent=0;if(percent>100)percent=100;
    outline_rect(x,y,20,10,1,COLOR_MUTED);fill_rect(x+20,y+3,2,4,COLOR_MUTED);
    level=percent*16/100;if(level)fill_rect(x+2,y+2,level,6,percent<20?COLOR_RED:COLOR_GREEN);
}

static int ui_graphics_init(void)
{
    if(ui_ready)return 0;
    sceGuInit();sceGuStart(GU_DIRECT,ui_gu_list);
    sceGuDrawBuffer(GU_PSM_8888,(void*)0,512);sceGuDispBuffer(480,272,(void*)0x88000,512);
    sceGuOffset(2048-240,2048-136);sceGuViewport(2048,2048,480,272);
    sceGuScissor(0,0,480,272);sceGuEnable(GU_SCISSOR_TEST);sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);sceGuBlendFunc(GU_ADD,GU_SRC_ALPHA,GU_ONE_MINUS_SRC_ALPHA,0,0);
    sceGuFinish();sceGuSync(0,0);ui_ready=1;return 0;
}

static void ui_graphics_shutdown(void)
{
    if(!ui_ready)return;
    sceGuTerm();ui_ready=0;
}

static void ui_text(float x,float y,float size,unsigned int color,const char *text)
{
    int row=((int)y-8)/8;(void)size;if(!text||row<0)return;
    pspDebugScreenSetXY((int)x/8,row);pspDebugScreenSetTextColor(color);pspDebugScreenPrintf("%s",text);
}

static void ui_text_px(int x,int y,unsigned int color,const char *text)
{
    if(!text)return;pspDebugScreenSetXY(x/7,y/8);pspDebugScreenSetTextColor(color);pspDebugScreenPrintf("%s",text);
}

static void ui_debug_center_clip(int x,int y,int width,unsigned int color,const char *text)
{
    char out[40];int columns=width/7,length,left;
    if(!text||columns<1)return;if(columns>39)columns=39;
    length=(int)strlen(text);if(length>columns)length=columns;
    memcpy(out,text,length);out[length]='\0';
    if(text[length]&&length>=3){out[length-3]='.';out[length-2]='.';out[length-1]='.';}
    left=x+(width-length*7)/2;ui_text_px(left,y,color,out);
}

static void ui_text_column(float x,float y,float width,float size,unsigned int color,const char *text,int max_chars)
{
    char line[61];int columns=(int)width/8,count=0;(void)size;
    if(columns<1)columns=1;if(columns>60)columns=60;
    while(text&&*text&&count<max_chars){
        int used=0;while(*text&&used<columns&&count<max_chars){line[used++]=*text++;count++;}
        line[used]='\0';ui_text(x,y,1.0f,color,line);y+=8;
    }
}

static void ui_text_centered_lines(int x,int y,int width,unsigned int color,const char *text)
{
    int line,columns=width/8;
    if(!text||columns<1)return;if(columns>59)columns=59;
    for(line=0;line<2&&*text;line++){
        char out[60];int remaining=(int)strlen(text),take=remaining,i;
        while(*text==' ')text++;
        remaining=(int)strlen(text);take=remaining;
        if(take>columns){
            take=columns;
            for(i=columns;i>0;i--)if(text[i]==' '){take=i;break;}
            if(take<1)take=columns;
        }
        while(take>0&&text[take-1]==' ')take--;
        memcpy(out,text,take);out[take]='\0';
        if(line==1&&text[take]&&take>=3){out[take-3]='.';out[take-2]='.';out[take-1]='.';}
        /* pspDebug lavora su celle da 8 pixel: arrotondare alla cella piu'
           vicina evita il costante spostamento a sinistra dovuto al floor. */
        {
            int left=x+(width-take*8)/2;
            left=((left+4)/8)*8;
            ui_text(left,y+line*8,1.0f,color,out);
        }
        text+=take;while(*text==' ')text++;
    }
}

static void ui_text_centered_one(int x,int y,int width,unsigned int color,const char *text)
{
    char out[60];int columns=width/8,take,left;
    if(!text||columns<1)return;if(columns>59)columns=59;
    take=(int)strlen(text);if(take>columns)take=columns;
    memcpy(out,text,take);out[take]='\0';
    if(text[take]&&take>=3){out[take-3]='.';out[take-2]='.';out[take-1]='.';}
    left=x+(width-take*8)/2;left=((left+4)/8)*8;
    ui_text(left,y,1.0f,color,out);
}

static UiThumbnail *find_thumbnail(const char *source)
{
    int i;if(strncmp(source,"yt:",3))return NULL;
    for(i=0;i<THUMB_COUNT;i++)if(ui_thumbnails[i].ready&&!strcmp(ui_thumbnails[i].id,source+3))return &ui_thumbnails[i];
    return NULL;
}

static void ui_draw_thumbnail_vram(UiThumbnail *thumb,int x,int y,int width,int height)
{
    unsigned int *vram=screen_vram();int row,column,crop_height,crop_y;
    if(!thumb||!thumb->ready)return;
    crop_height=THUMB_SOURCE_WIDTH*height/width;
    if(crop_height<1)crop_height=1;
    if(crop_height>THUMB_SOURCE_HEIGHT)crop_height=THUMB_SOURCE_HEIGHT;
    crop_y=(THUMB_SOURCE_HEIGHT-crop_height)/2;
    for(row=0;row<height;row++){
        int source_y=crop_y+row*crop_height/height;
        for(column=0;column<width;column++){
            int source_x=column*THUMB_SOURCE_WIDTH/width;
            /* La cover e' full-bleed, ma conserva gli angoli arrotondati
               della card (raggio 5). */
            if(row<5||row>=height-5){
                int edge_row=row<5?row:height-row-1;
                int cut=4-edge_row;
                if(column<cut||column>=width-cut)continue;
            }
            vram[(y+row)*512+x+column]=thumb->pixels[source_y*THUMB_STRIDE+source_x]&0x00ffffff;
        }
    }
}

static void thumbnail_prepare(const UtubbuCatalog *catalog)
{
    unsigned char *jpeg=NULL;unsigned int *decoded=NULL;int module=-1,jpeg_ready=0,i;
    memset(ui_thumbnails,0,sizeof(ui_thumbnails));
    jpeg=memalign(64,65536);decoded=memalign(64,JPEG_CONTEXT_SIZE*JPEG_CONTEXT_SIZE*4);
    if(!jpeg||!decoded)goto done;
    module=sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);if(module<0)goto done;
    if(sceJpegInitMJpeg()<0)goto done;jpeg_ready=1;
    if(sceJpegCreateMJpeg(JPEG_CONTEXT_SIZE,JPEG_CONTEXT_SIZE)<0)goto done;jpeg_ready=2;
    for(i=0;i<catalog->count&&i<THUMB_COUNT&&!exit_requested;i++){
        const UtubbuCatalogItem *item=&catalog->items[i];char url[96];int bytes,wh,w,h,row,column;
        int crop_x,crop_y,crop_width,crop_height;
        if(strncmp(item->source,"yt:",3))continue;
        snprintf(url,sizeof(url),"https://i.ytimg.com/vi/%s/mqdefault.jpg",item->source+3);
        bytes=utubbu_http_get_memory(url,(char*)jpeg,65536);utubbu_log("thumbnail bytes",bytes);
        if(bytes<=0){
            utubbu_log("thumbnail download retry",i);
            bytes=utubbu_http_get_memory(url,(char*)jpeg,65536);
            utubbu_log("thumbnail retry bytes",bytes);
        }
        if(bytes<=0){
            snprintf(url,sizeof(url),"https://i.ytimg.com/vi/%s/default.jpg",item->source+3);
            bytes=utubbu_http_get_memory(url,(char*)jpeg,65536);
            utubbu_log("thumbnail fallback bytes",bytes);
        }
        if(bytes<=0)continue;
        /* HTTPS fills these buffers through the CPU, while the PSP JPEG
           decoder runs on the Media Engine.  Make both sides see the same
           bytes instead of leaving a successfully decoded but black image in
           the CPU cache. */
        sceKernelDcacheWritebackInvalidateRange(jpeg,bytes);
        sceKernelDcacheWritebackInvalidateRange(decoded,JPEG_CONTEXT_SIZE*JPEG_CONTEXT_SIZE*4);
        wh=sceJpegDecodeMJpeg(jpeg,bytes,decoded,0);
        if(wh<0){utubbu_log("thumbnail decode",wh);continue;}
        w=(wh>>16)&0xffff;h=wh&0xffff;utubbu_log("thumbnail width",w);utubbu_log("thumbnail height",h);
        if(w<=0||h<=0||w>JPEG_CONTEXT_SIZE||h>JPEG_CONTEXT_SIZE)continue;
        /* Discard stale CPU cache lines before reading Media Engine output. */
        sceKernelDcacheInvalidateRange(decoded,JPEG_CONTEXT_SIZE*JPEG_CONTEXT_SIZE*4);
        memset(ui_thumbnails[i].pixels,0,sizeof(ui_thumbnails[i].pixels));
        crop_x=0;crop_y=0;crop_width=w;crop_height=h;
        if(w*THUMB_SOURCE_HEIGHT<h*THUMB_SOURCE_WIDTH){
            crop_height=w*THUMB_SOURCE_HEIGHT/THUMB_SOURCE_WIDTH;
            crop_y=(h-crop_height)/2;
        }else{
            crop_width=h*THUMB_SOURCE_WIDTH/THUMB_SOURCE_HEIGHT;
            crop_x=(w-crop_width)/2;
        }
        for(row=0;row<THUMB_SOURCE_HEIGHT;row++){
            int source_y=crop_y+row*crop_height/THUMB_SOURCE_HEIGHT;
            for(column=0;column<THUMB_SOURCE_WIDTH;column++){
                int source_x=crop_x+column*crop_width/THUMB_SOURCE_WIDTH;
                ui_thumbnails[i].pixels[row*THUMB_STRIDE+column]=decoded[source_y*JPEG_CONTEXT_SIZE+source_x];
            }
        }
        memcpy(ui_thumbnails[i].id,item->source+3,11);ui_thumbnails[i].id[11]='\0';ui_thumbnails[i].ready=1;
        utubbu_log("thumbnail pixel first",(int)ui_thumbnails[i].pixels[0]);
        utubbu_log("thumbnail pixel center",(int)ui_thumbnails[i].pixels[(THUMB_SOURCE_HEIGHT/2)*THUMB_STRIDE+(THUMB_SOURCE_WIDTH/2)]);
        /* The Graphics Engine reads physical memory directly. */
        sceKernelDcacheWritebackInvalidateRange(ui_thumbnails[i].pixels,sizeof(ui_thumbnails[i].pixels));
        if(i<4){
            int preview_x=13+(i%2)*234,preview_y=38+(i/2)*105;
            ui_draw_thumbnail_vram(&ui_thumbnails[i],preview_x,preview_y,220,100);
            sceDisplayWaitVblankStart();
        }
    }
    sceKernelDcacheWritebackInvalidateAll();
done:
    if(jpeg_ready>=2)sceJpegDeleteMJpeg();if(jpeg_ready>=1)sceJpegFinishMJpeg();
    if(module>=0)sceUtilityUnloadModule(PSP_MODULE_AV_AVCODEC);free(decoded);free(jpeg);
}

static void begin_graphics(void)
{
    ui_graphics_init();sceDisplaySetMode(0,480,272);
    sceDisplaySetFrameBuf(sceGeEdramGetAddr(),512,PSP_DISPLAY_PIXEL_FORMAT_8888,PSP_DISPLAY_SETBUF_IMMEDIATE);
    pspDebugScreenSetOffset(0);pspDebugScreenEnableBackColor(0);fill_rect(0,0,480,272,COLOR_DARK);
}

static void render(const UtubbuCatalog *catalog,const int *matches,int match_count,
    int selected,int content_focus,int nav_selected,const char *query,
    UtubbuClockInfo clock,const char *message)
{
    int i,page=(selected/4)*4,visible=match_count-page;char counter[24];
    const unsigned int bg=0x00130d08,card=0x00251b13,field=0x00221912,line=0x00322821;
    const unsigned int white=0x00eeeae5,muted=0x009b9188,red=0x005036ef;
    (void)content_focus;(void)nav_selected;(void)clock;(void)showing_history;
    if(visible>4)visible=4;begin_graphics();fill_rect(0,0,480,272,bg);

    fill_round_rect(13,8,24,18,4,red);draw_play(21,17,9,white);
    clean_text(45,7,20,white,"UTUBBU",100);
    fill_round_rect(151,7,316,20,4,field);outline_round_rect(151,7,316,20,4,line,field);
    clean_text_spaced(161,9,16,red,"CERCA",64,4);
    if(query[0])clean_text_spaced(225,9,16,white,query,232,4);

    for(i=0;i<visible;i++){
        int result=page+i,x=13+(i%2)*234,y=38+(i/2)*105,active=result==selected;
        const UtubbuCatalogItem *item=&catalog->items[matches[result]];UiThumbnail *thumb=find_thumbnail(item->source);
        fill_round_rect(x,y,220,100,5,card);
        fill_round_rect(x,y,220,100,5,0x00201b18);
        if(thumb)ui_draw_thumbnail_vram(thumb,x,y,220,100);else draw_play(x+103,y+50,11,0x006d6660);
        shade_rect(x+1,y+74,218,25,96);
        {
            const char *duration=item->duration[0]?item->duration:"--:--";
            int duration_width=clean_text_width(duration,14),pill_width=duration_width+8;
            int pill_x=x+213-pill_width;
            int title_width=pill_x-x-13;
            clean_text_left_clip(x+7,y+81,title_width,14,0x00000000,item->title);
            clean_text_left_clip(x+7,y+80,title_width,14,white,item->title);
            fill_round_rect(pill_x,y+77,pill_width,18,3,0x000b0908);
            clean_text(pill_x+4,y+79,14,white,duration,duration_width);
        }
        if(active)draw_round_outline(x,y,220,100,5,red);
    }
    fill_rect(0,250,480,22,bg);fill_rect(0,250,480,1,line);
    snprintf(counter,sizeof(counter),"%d/%d",match_count?page/4+1:0,match_count?(match_count+3)/4:0);
    ui_text_px(8,256,muted,counter);
    if(message&&strcmp(message,"Pronto")){
        if(!match_count)clean_text_centered_clip(40,256,400,14,muted,message);
        else ui_debug_center_clip(42,256,278,muted,message);
    }
    if(match_count){
        ui_text_px(329,256,muted,"D-PAD");
        draw_circle_button(375,260,5,0x004060ff);ui_text_px(386,256,white,"APRI");
    }
    draw_triangle_button(428,255,11,0x0070e060);ui_text_px(441,256,white,"CERCA");
}

static void edit_query(char *query,int capacity)
{
    ui_graphics_shutdown();utubbu_osk_edit(query,capacity);
}

static void enter_application_directory(int argc, char **argv)
{
    char directory[256];
    char *slash;
    char *backslash;
    size_t length;

    if (argc <= 0 || !argv || !argv[0]) return;
    length = strlen(argv[0]);
    if (!length || length >= sizeof(directory)) return;
    memcpy(directory, argv[0], length + 1);
    slash = strrchr(directory, '/');
    backslash = strrchr(directory, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    if (!slash) return;
    *slash = '\0';
    length = strlen(directory);
    if (length && directory[length - 1] == ':' && length + 1 < sizeof(directory)) {
        directory[length] = '/';
        directory[length + 1] = '\0';
    }
    if (directory[0]) sceIoChdir(directory);
}

int main(int argc, char **argv)
{
    UtubbuCatalog catalog;
    UtubbuClockInfo clock;
    int matches[UTUBBU_MAX_ITEMS];
    int match_count;
    int selected = 0;
    char query[32] = "";
    char message[64] = "Pronto";
    int dirty = 1;
    int remote_results = 0;
    int content_focus = 1;
    int nav_selected = 0;
    unsigned int previous = 0;

    enter_application_directory(argc, argv);
    if (sceIoChdir("ms0:/PSP/GAME/UTUBBU") < 0)
        enter_application_directory(argc, argv);
    utubbu_log_reset();
    utubbu_log_text("build", "UTUBBU diagnostic 2026-07-22 CE-cover-overlay", -1);
    utubbu_log("app start argc", argc);
    setup_callbacks();
    utubbu_log("callbacks ready", 0);
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    clock = utubbu_detect_clock();
    utubbu_log("clock cpu", clock.cpu_mhz);
    utubbu_log("clock bus", clock.bus_mhz);
    /* The startup catalog is optional and remote search replaces it anyway.
       On the current Memory Stick, opening it at boot intermittently stalls
       before the UI. Start from an empty in-memory catalog instead. */
    memset(&catalog, 0, sizeof(catalog));
    utubbu_log("startup catalog skipped", catalog.count);
    /* Il file diagnostico locale resta sulla Memory Stick ma non deve apparire
       nella Home destinata all'utente. */
    utubbu_log("ui loop ready", 0);

    for (; !exit_requested;) {
        SceCtrlData pad;
        unsigned int pressed;
        match_count = build_matches(&catalog, remote_results ? "" : query, matches);
        if (selected >= match_count) selected = match_count ? match_count - 1 : 0;
        if (dirty) {
            render(&catalog, matches, match_count, selected, content_focus,
                nav_selected, query, clock, message);
            dirty = 0;
        }
        read_controls(&pad);
        pressed = pad.Buttons & ~previous;
        previous = pad.Buttons;
        if (pressed) dirty = 1;

        if ((pressed & PSP_CTRL_LEFT) && selected%2) --selected;
        if ((pressed & PSP_CTRL_RIGHT) && selected%2<1 && selected+1<match_count) ++selected;
        if ((pressed & PSP_CTRL_UP) && selected>=2) selected-=2;
        if ((pressed & PSP_CTRL_DOWN) && selected+2<match_count) selected+=2;
        if (pressed & PSP_CTRL_SQUARE) {
            int history_status=utubbu_catalog_load(&catalog,"history.txt");
            utubbu_log("action history",history_status);showing_history=1;remote_results=1;selected=0;
            query[0]='\0';snprintf(message,sizeof(message),history_status>0?
                "Ultimi %d video visti":"Cronologia vuota",history_status>0?history_status:0);
        }
        if (pressed & PSP_CTRL_TRIANGLE) {
            int status;
            utubbu_log("action search keyboard", 0);
            edit_query(query, sizeof(query));
            utubbu_log_text("search query", query, -1);
            wait_buttons_released();
            selected = 0; previous = 0;
            status = query[0] ? 0 : -1;
            if (!status) {
                showing_history = 0;
                snprintf(message, sizeof(message), "Connessione Wi-Fi: attendere...");
                render(&catalog, matches, match_count, selected, content_focus,
                    nav_selected, query, clock, message);
                status = connect_wifi();
                utubbu_log("search wifi", status);
                if (!status) {
                    snprintf(message, sizeof(message), "Ricerca UTUBBU in corso...");
                    render(&catalog, matches, match_count, selected, content_focus,
                        nav_selected, query, clock, message);
                    status = utubbu_youtube_search(query, &catalog);
                }
                utubbu_log("search youtube", status);
                utubbu_log("search results", catalog.count);
                if (!status) {
                    remote_results = 1;
                    match_count=build_matches(&catalog,"",matches);
                    snprintf(message,sizeof(message),"Caricamento cover UTUBBU...");
                    render(&catalog,matches,match_count,selected,content_focus,
                        nav_selected,query,clock,message);
                    thumbnail_prepare(&catalog);
                    content_focus = 1;
                    nav_selected = 2;
                    snprintf(message, sizeof(message), "Trovati %d video online", catalog.count);
                } else {
                    remote_results = 0;
                    snprintf(message, sizeof(message), "Ricerca online fallita: %d", status);
                }
            }
        }
        if ((pressed & (PSP_CTRL_CROSS|PSP_CTRL_CIRCLE)) && content_focus && match_count) {
            UtubbuCatalogItem *item = &catalog.items[matches[selected]];
            int status = 0;
            int is_youtube = !strncmp(item->source, "yt:", 3);
            int is_mp4 = is_youtube || !strncmp(item->source, "mp4:", 4);
            int combined_stream = 0;
            const char *audio_cache = "yt-audio-cache.m4a";
            char video_url[2048] = "", audio_url[2048] = "";
            utubbu_log("action open index", selected);
            utubbu_log_text("selected title", item->title, -1);
            utubbu_log_text("selected source", item->source, -1);
            utubbu_log_text("video cache", item->local_name, -1);
            if (is_youtube || !exists(item->local_name)) {
                snprintf(message, sizeof(message), "Connessione Wi-Fi...");
                render(&catalog, matches, match_count, selected, content_focus,
                    nav_selected, query, clock, message);
                status = connect_wifi();
                utubbu_log("wifi", status);
                if (!status && is_youtube) {
                    snprintf(message, sizeof(message), "Preparazione video UTUBBU...");
                    render(&catalog, matches, match_count, selected, content_focus,
                        nav_selected, query, clock, message);
                    status = utubbu_youtube_resolve(item->source + 3,
                        video_url, sizeof(video_url), audio_url, sizeof(audio_url));
                    utubbu_log("youtube resolve", status);
                    utubbu_log("video url length", strlen(video_url));
                    utubbu_log("audio url length", strlen(audio_url));
                    combined_stream = !strcmp(video_url, audio_url);
                    utubbu_log("combined stream", combined_stream);
                    if (!status) {
                        status = utubbu_stream_start_slot(0, video_url, item->local_name);
                        utubbu_log("video stream start", status);
                        if (!status && !combined_stream) {
                            status = utubbu_stream_start_slot(1, audio_url, audio_cache);
                            utubbu_log("audio stream start", status);
                        }
                    }
                } else if (!status) status = utubbu_download(item->source, item->local_name);
            }
            if (!status && (is_youtube || exists(item->local_name))) {
                snprintf(message, sizeof(message), "Buffer MP4 / avvio decoder...");
                render(&catalog, matches, match_count, selected, content_focus,
                    nav_selected, query, clock, message);
                ui_graphics_shutdown();
                status = is_mp4 ? utubbu_mp4_play(item->local_name,
                    is_youtube && !combined_stream ? audio_cache : item->local_name) : yvid_play(item->local_name);
                utubbu_log("play returned", status);
                utubbu_stream_stop();
                if(status==-71&&is_youtube&&!exit_requested){
                    int remove_status;
                    utubbu_log("automatic fallback",1);
                    remove_status=sceIoRemove(item->local_name);
                    utubbu_log("fallback video remove",remove_status);
                    if(!combined_stream){
                        remove_status=sceIoRemove(audio_cache);
                        utubbu_log("fallback audio remove",remove_status);
                    }
                    snprintf(message,sizeof(message),"Compatibilita PSP: nuovo formato...");
                    render(&catalog,matches,match_count,selected,content_focus,
                        nav_selected,query,clock,message);
                    status=utubbu_youtube_resolve_mode(item->source+3,
                        video_url,sizeof(video_url),audio_url,sizeof(audio_url),1);
                    utubbu_log("fallback resolve",status);
                    combined_stream=!strcmp(video_url,audio_url);
                    if(!status)status=utubbu_stream_start_slot(0,video_url,item->local_name);
                    if(!status&&!combined_stream)
                        status=utubbu_stream_start_slot(1,audio_url,audio_cache);
                    if(!status){ui_graphics_shutdown();status=utubbu_mp4_play_software(item->local_name,
                        combined_stream?item->local_name:audio_cache);}
                    utubbu_log("fallback play returned",status);
                    utubbu_stream_stop();
                }
                if (is_youtube) {
                    int remove_status = sceIoRemove(item->local_name);
                    utubbu_log("cache remove", remove_status);
                    if (!combined_stream) {
                        remove_status = sceIoRemove(audio_cache);
                        utubbu_log("audio cache remove", remove_status);
                    }
                }
                if (!status && is_youtube) {
                    int history_status = utubbu_history_add(item, "history.txt");
                    utubbu_log("history add", history_status);
                    if (showing_history) utubbu_catalog_load(&catalog, "history.txt");
                }
                snprintf(message, sizeof(message), status ? "Video non valido (%d)" : "Riproduzione terminata", status);
            } else {
                if (is_youtube) {
                    utubbu_stream_stop();
                    sceIoRemove(item->local_name);
                    if (!combined_stream) sceIoRemove(audio_cache);
                }
                snprintf(message, sizeof(message), "Errore rete/download: %d", status);
            }
            wait_buttons_released();
            utubbu_log("open flow complete", status);
            previous = 0;
        }

        if (pressed & PSP_CTRL_SELECT) {
            int status;
            utubbu_log("action refresh", 0);
            showing_history = 0;
            snprintf(message, sizeof(message), "Aggiornamento UTUBBU...");
            render(&catalog, matches, match_count, selected, content_focus,
                nav_selected, query, clock, message);
            status = connect_wifi();
            utubbu_log("refresh wifi", status);
            if (!status) {
                snprintf(message, sizeof(message), "Ricerca UTUBBU in corso...");
                render(&catalog, matches, match_count, selected, content_focus,
                    nav_selected, query, clock, message);
                status = utubbu_youtube_search(query[0] ? query : "video", &catalog);
            }
            utubbu_log("refresh youtube", status);
            utubbu_log("refresh results", catalog.count);
            if (!status) {
                remote_results = 1;
                match_count=build_matches(&catalog,"",matches);
                snprintf(message,sizeof(message),"Caricamento cover UTUBBU...");
                render(&catalog,matches,match_count,selected,content_focus,
                    nav_selected,query,clock,message);
                thumbnail_prepare(&catalog);
                selected = 0;
                content_focus = 1;
                nav_selected = 2;
                snprintf(message, sizeof(message), "Trovati %d video online", catalog.count);
            } else snprintf(message, sizeof(message), "Aggiornamento fallito: %d", status);
            previous = 0;
        }
        sceKernelDelayThread(16000);
    }
    utubbu_log("main loop ended", 0);
    ui_graphics_shutdown();
    utubbu_network_shutdown();
    utubbu_log("network shutdown complete", 0);
    sceKernelExitGame();
    return 0;
}
