#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <psputility.h>

#include <string.h>

#include "osk.h"

#define OSK_TEXT_LENGTH 64

static unsigned int __attribute__((aligned(16))) gu_list[262144];

static void ascii_to_utf16(unsigned short *out, int out_count, const char *in)
{
    int i;
    for(i=0;i<out_count-1&&in[i];++i)out[i]=(unsigned char)in[i];
    out[i]=0;
}

static void utf16_to_utf8(char *out, int out_count, const unsigned short *in)
{
    int used=0,i;
    for(i=0;in[i]&&used<out_count-1;++i){
        unsigned int c=in[i];
        if(c<0x80)out[used++]=(char)c;
        else if(c<0x800&&used+2<out_count){out[used++]=(char)(0xC0|(c>>6));out[used++]=(char)(0x80|(c&0x3F));}
        else if(used+3<out_count){out[used++]=(char)(0xE0|(c>>12));out[used++]=(char)(0x80|((c>>6)&0x3F));out[used++]=(char)(0x80|(c&0x3F));}
    }
    out[used]=0;
}

static void graphics_start(void)
{
    sceGuInit();
    sceGuStart(GU_DIRECT,gu_list);
    sceGuDrawBuffer(GU_PSM_8888,(void *)0,512);
    sceGuDispBuffer(480,272,(void *)0x88000,512);
    sceGuDepthBuffer((void *)0x110000,512);
    sceGuOffset(2048-240,2048-136);
    sceGuViewport(2048,2048,480,272);
    sceGuScissor(0,0,480,272);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuFinish();sceGuSync(0,0);
    sceDisplayWaitVblankStart();sceGuDisplay(GU_TRUE);
}

int utubbu_osk_edit(char *text, int capacity)
{
    unsigned short description[OSK_TEXT_LENGTH]={0};
    unsigned short input[OSK_TEXT_LENGTH]={0};
    unsigned short output[OSK_TEXT_LENGTH]={0};
    SceUtilityOskData data;
    SceUtilityOskParams params;
    int done=0,status,result;
    if(!text||capacity<2)return -1;
    ascii_to_utf16(description,OSK_TEXT_LENGTH,"Cerca video su UTUBBU");
    ascii_to_utf16(input,OSK_TEXT_LENGTH,text);
    memset(&data,0,sizeof(data));
    data.language=PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
    data.lines=1;data.unk_24=1;data.inputtype=PSP_UTILITY_OSK_INPUTTYPE_ALL;
    data.desc=description;data.intext=input;data.outtext=output;
    data.outtextlength=OSK_TEXT_LENGTH;data.outtextlimit=capacity-1;
    memset(&params,0,sizeof(params));
    params.base.size=sizeof(params);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE,&params.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN,&params.base.buttonSwap);
    params.base.graphicsThread=17;params.base.accessThread=19;
    params.base.fontThread=18;params.base.soundThread=16;
    params.datacount=1;params.data=&data;
    graphics_start();
    result=sceUtilityOskInitStart(&params);
    if(result<0){sceGuTerm();return result;}
    while(!done){
        sceGuStart(GU_DIRECT,gu_list);
        sceGuClearColor(0x00101010);sceGuClear(GU_COLOR_BUFFER_BIT);
        sceGuFinish();sceGuSync(0,0);
        status=sceUtilityOskGetStatus();
        if(status==PSP_UTILITY_DIALOG_VISIBLE)sceUtilityOskUpdate(1);
        else if(status==PSP_UTILITY_DIALOG_QUIT)sceUtilityOskShutdownStart();
        else if(status==PSP_UTILITY_DIALOG_NONE)done=1;
        sceDisplayWaitVblankStart();sceGuSwapBuffers();
    }
    sceGuTerm();
    /* OSK usa double buffering e può lasciare visibile il buffer 0x88000.
       UTUBBU disegna direttamente nel buffer 0: ripristinalo subito. */
    sceDisplaySetMode(0,480,272);
    sceDisplaySetFrameBuf(sceGeEdramGetAddr(),512,PSP_DISPLAY_PIXEL_FORMAT_8888,
        PSP_DISPLAY_SETBUF_IMMEDIATE);
    if(data.result==PSP_UTILITY_OSK_RESULT_CHANGED||data.result==PSP_UTILITY_OSK_RESULT_UNCHANGED){
        utf16_to_utf8(text,capacity,output);
        return 0;
    }
    return 1;
}
