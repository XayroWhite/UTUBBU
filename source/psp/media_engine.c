#include <kubridge.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>

#include "media_engine.h"
#include "diagnostic.h"

extern const unsigned char *utubbu_bridge_blob(int *size);
extern int cooleyesMeBootStart(int devkitVersion, int mebooterType);

static int media_engine_ready;
static int media_engine_mode = -1;

static int write_bridge(const char *path)
{
    const unsigned char *data;
    SceUID fd;
    int size;
    int written;
    data = utubbu_bridge_blob(&size);
    fd = sceIoOpen(path, PSP_O_CREAT | PSP_O_TRUNC | PSP_O_WRONLY, 0777);
    if (fd < 0) return -1;
    written = sceIoWrite(fd, data, size);
    sceIoClose(fd);
    return written == size ? 0 : -2;
}

int utubbu_media_engine_start(int width, int height, int avc_profile)
{
    const char *bridge_path = "cooleyesBridge.prx";
    SceUID module;
    int status = 0;
    int mode;
    (void)height;
    if (!media_engine_ready) {
        utubbu_log("me bridge write", 0);
        if (write_bridge(bridge_path) < 0) return -71;
        module = kuKernelLoadModule(bridge_path, 0, NULL);
        utubbu_log("me bridge load", module);
        if (module < 0) return -72;
        module = sceKernelStartModule(module, 0, NULL, &status, NULL);
        utubbu_log("me bridge start", module);
        if (module < 0) return -73;
        sceIoRemove(bridge_path);

        module = kuKernelLoadModule("flash0:/kd/mpeg_vsh.prx", 0, NULL);
        utubbu_log("me mpeg load", module);
        if (module >= 0 && sceKernelStartModule(module, 0, NULL, &status, NULL) < 0)
            return -74;
        media_engine_ready = 1;
    }

    mode = (width > 480 || height > 272) ? 1 : (avc_profile == 0x42 ? 4 : 3);
    utubbu_log("me requested profile", avc_profile);
    utubbu_log("me requested mode", mode);
    if (mode == media_engine_mode) return 0;
    utubbu_log("me boot before", mode);
    status = cooleyesMeBootStart(sceKernelDevkitVersion(), mode);
    utubbu_log("me boot after", status);
    if (status < 0) return -75;
    media_engine_mode = mode;
    return 0;
}
