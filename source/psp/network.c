#include <sys/select.h>
#include <curl.h>
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <psputility_netmodules.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "network.h"
#include "diagnostic.h"

static int network_ready;
static volatile int cancel_pending;
static volatile int active_resolver=-1;
typedef struct StreamDownload {
    volatile int active;
    volatile int stop;
    volatile int downloaded;
    volatile int total;
    volatile int error;
    int next_progress_log;
    SceUID thread;
    char url[2048];
    char destination[256];
    SceUID output;
} StreamDownload;
static StreamDownload stream_download[2];

static int curl_trace(CURL *curl, curl_infotype type, char *data,
    size_t size, void *user);
static int resolve_for_curl(CURL *curl, const char *url,
    struct curl_slist **resolve_entries);

static int initialize_network_stack(void)
{
    int result;
    result = sceNetInit(0x20000, 0x20, 0x1000, 0x20, 0x1000);
    if (result != 0) return result;
    result = sceNetInetInit();
    if (result != 0) { sceNetTerm(); return result; }
    result = sceNetResolverInit();
    if (result != 0) { sceNetInetTerm(); sceNetTerm(); return result; }
    result = sceNetApctlInit(0x1600, 0x42);
    if (result != 0) {
        sceNetResolverTerm(); sceNetInetTerm(); sceNetTerm();
        return result;
    }
    return 0;
}

static void terminate_network_stack(void)
{
    sceNetApctlTerm();
    sceNetResolverTerm();
    sceNetInetTerm();
    sceNetTerm();
}

int utubbu_network_connect(int profile)
{
    int result;
    int state = 0;
    int tries;
    int reached_joining = 0;
    utubbu_log("network connect enter", profile);
    if (network_ready) { utubbu_log("network already ready", 0); return 0; }
    utubbu_log("net common before", 0);
    result = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    utubbu_log("net common after", result);
    if (result < 0) return -1;
    result = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    utubbu_log("net inet module", result);
    if (result < 0) return -2;
    result = initialize_network_stack();
    utubbu_log("net stack", result);
    if (result != 0) return -3;
    result = sceNetApctlConnect(profile);
    utubbu_log("net connect", result);
    if (result != 0) {
        terminate_network_stack();
        sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
        sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
        return -4;
    }
    for (tries = 0; tries < 300; ++tries) {
        if (sceNetApctlGetState(&state) != 0) return -5;
        if (tries == 0 || (tries % 20) == 0) utubbu_log("network ap state", state);
        if (state >= 2) reached_joining = 1;
        if (state == 4) {
            network_ready = 1;
            curl_global_init(CURL_GLOBAL_DEFAULT);
            utubbu_log("network ready tries", tries);
            return 0;
        }
        if(reached_joining&&state==0&&tries>=20){
            utubbu_log("network association dropped",tries);
            break;
        }
        sceKernelDelayThread(50000);
    }
    sceNetApctlDisconnect();
    terminate_network_stack();
    sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
    sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
    return -6;
}

void utubbu_network_shutdown(void)
{
    utubbu_log("network shutdown enter", network_ready);
    if (!network_ready) return;
    utubbu_stream_stop();
    curl_global_cleanup();
    sceNetApctlDisconnect();
    terminate_network_stack();
    sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
    sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
    network_ready = 0;
    utubbu_log("network shutdown done", 0);
}

static size_t write_stream(void *data, size_t size, size_t count, void *user)
{
    StreamDownload *stream = (StreamDownload *)user;
    size_t bytes = size * count;
    int written;
    if (stream->stop) return 0;
    written = sceIoWrite(stream->output, data, bytes);
    if (written < 0) return 0;
    stream->downloaded += written;
    if (stream->downloaded >= stream->next_progress_log) {
        utubbu_log("stream progress bytes", stream->downloaded);
        /* Persistent logging flushes the Memory Stick.  One checkpoint every
           2 MiB preserves diagnostics without stealing video frame time. */
        stream->next_progress_log += 2 * 1024 * 1024;
    }
    return (size_t)written;
}

static int stream_thread(SceSize args, void *argp)
{
    int slot = (args >= sizeof(int) && argp) ? *(int *)argp : 0;
    StreamDownload *stream = &stream_download[slot];
    CURL *curl;
    CURLcode code = CURLE_FAILED_INIT;
    struct curl_slist *resolve_entries = NULL;
    int attempt;
    long status = 0;
    double length = 0;
    (void)args; (void)argp;
    utubbu_log_text("stream file", stream->destination, strlen(stream->destination));
    stream->output = sceIoOpen(stream->destination,
        PSP_O_CREAT | PSP_O_TRUNC | PSP_O_WRONLY, 0777);
    utubbu_log("stream open", stream->output);
    if (stream->output < 0) { stream->error = -2; goto done; }
    for (attempt = 1; attempt <= 3 && !stream->stop; ++attempt) {
        status = 0; length = 0; resolve_entries = NULL;
        curl = curl_easy_init();
        if (!curl) { code = CURLE_FAILED_INIT; break; }
        utubbu_log("stream connect attempt", attempt);
        if (resolve_for_curl(curl, stream->url, &resolve_entries) < 0) {
            code = CURLE_COULDNT_RESOLVE_HOST;
        } else {
            curl_easy_setopt(curl, CURLOPT_URL, stream->url);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT,
                "Mozilla/5.0 (PlayStation Portable) UTUBBU/0.4");
            curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
            curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
            curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
            curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_trace);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 12L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 128L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 45L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_CAINFO, NULL);
            curl_easy_setopt(curl, CURLOPT_CAPATH, NULL);
            curl_easy_setopt(curl, CURLOPT_CRLFILE, NULL);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_stream);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, stream);
            utubbu_log("stream stack free", sceKernelGetThreadStackFreeSize(0));
            utubbu_log("stream curl before", attempt);
            code = curl_easy_perform(curl);
            utubbu_log("stream curl after", (int)code);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &length);
        }
        curl_easy_cleanup(curl);
        curl_slist_free_all(resolve_entries);
        if (code == CURLE_OK || stream->downloaded > 0 || attempt == 3) break;
        utubbu_log("stream retry no bytes", (int)code);
        sceKernelDelayThread(250000);
    }
    utubbu_log("stream http status", (int)status);
    utubbu_log("stream downloaded", stream->downloaded);
    if (length > 0 && length < 2147483647.0) stream->total = (int)length;
    if (stream->stop) stream->error = -4;
    else if (code != CURLE_OK) stream->error = -(100 + (int)code);
    else if (status < 200 || status >= 300) stream->error = -(int)status;
    utubbu_log("stream final error", stream->error);
done:
    if (stream->output >= 0) { sceIoClose(stream->output); stream->output = -1; }
    stream->active = 0;
    sceKernelExitDeleteThread(0);
    return 0;
}

int utubbu_stream_start_slot(int slot, const char *url, const char *destination)
{
    StreamDownload *stream;
    const char *thread_name;
    if (slot < 0 || slot > 1) return -5;
    stream = &stream_download[slot];
    if (!network_ready || stream->active) return -1;
    memset(stream, 0, sizeof(*stream));
    stream->output = -1;
    stream->next_progress_log = 1;
    if (strlen(url) >= sizeof(stream->url)) return -2;
    strcpy(stream->url, url);
    if (strchr(destination, ':')) {
        if (strlen(destination) >= sizeof(stream->destination)) return -2;
        strcpy(stream->destination, destination);
    } else {
        if (snprintf(stream->destination, sizeof(stream->destination),
            "ms0:/PSP/GAME/UTUBBU/%s", destination) >=
            (int)sizeof(stream->destination)) return -2;
    }
    stream->active = 1;
    thread_name = slot ? "utubbu_audio_stream" : "utubbu_video_stream";
    /* curl/mbedTLS alloca i buffer sullo heap; lo stack reale osservato e
       minimo.  128 KiB consente due stream senza esaurire la RAM PSP. */
    /* Keep HTTPS below AAC (0x16) and MPEG (0x17).  It may use spare CPU but
       must not interrupt a real-time audio/video transaction. */
    stream->thread = sceKernelCreateThread(thread_name, stream_thread,
        0x20, 0x20000, PSP_THREAD_ATTR_USER, NULL);
    if (stream->thread < 0) { stream->active = 0; return -3; }
    if (sceKernelStartThread(stream->thread, sizeof(slot), &slot) < 0) {
        sceKernelDeleteThread(stream->thread);
        stream->active = 0;
        return -4;
    }
    return 0;
}

int utubbu_stream_start(const char *url, const char *destination)
{
    return utubbu_stream_start_slot(0, url, destination);
}

int utubbu_stream_state_slot(int slot, int *downloaded, int *total, int *error)
{
    StreamDownload *stream;
    if (slot < 0 || slot > 1) return 0;
    stream = &stream_download[slot];
    if (downloaded) *downloaded = stream->downloaded;
    if (total) *total = stream->total;
    if (error) *error = stream->error;
    return stream->active;
}

int utubbu_stream_state(int *downloaded, int *total, int *error)
{
    return utubbu_stream_state_slot(0, downloaded, total, error);
}

void utubbu_stream_stop(void)
{
    int slot, tries;
    for (slot = 0; slot < 2; ++slot) stream_download[slot].stop = 1;
    for (slot = 0; slot < 2; ++slot) {
        StreamDownload *stream = &stream_download[slot];
        for (tries = 0; tries < 100 && stream->active; ++tries) sceKernelDelayThread(10000);
        if (stream->active && stream->thread >= 0) {
            sceKernelTerminateDeleteThread(stream->thread);
            stream->active = 0;
        }
    }
}

static size_t write_file(void *data, size_t size, size_t count, void *user)
{
    return fwrite(data, size, count, (FILE *)user);
}

typedef struct MemoryWrite {
    char *buffer;
    int capacity;
    int used;
    int overflow;
} MemoryWrite;

static int resolve_for_curl(CURL *curl, const char *url,
    struct curl_slist **resolve_entries)
{
    const char *start,*end;char host[256],entry[320];
    unsigned char work[1024];struct in_addr address;unsigned char *ip;
    int rid=-1,status,length;
    start=strstr(url,"://");start=start?start+3:url;
    end=start;while(*end&&*end!='/'&&*end!=':')end++;
    length=(int)(end-start);
    if(length<=0||length>=(int)sizeof(host))return -1;
    memcpy(host,start,length);host[length]='\0';
    status=sceNetResolverCreate(&rid,work,sizeof(work));
    utubbu_log("dns resolver create",status);
    if(status<0)return status;
    /* libcurl PSP non puo interrompere gethostbyname. Il resolver nativo
       invece applica davvero timeout e retry, poi CURLOPT_RESOLVE evita una
       seconda risoluzione bloccante dentro curl. */
    if(cancel_pending){sceNetResolverDelete(rid);return -3;}
    active_resolver=rid;
    status=sceNetResolverStartNtoA(rid,host,&address,5,2);
    active_resolver=-1;
    utubbu_log("dns resolver lookup",status);
    sceNetResolverDelete(rid);
    if(status<0)return status;
    ip=(unsigned char*)&address.s_addr;
    snprintf(entry,sizeof(entry),"%s:443:%u.%u.%u.%u",host,
        ip[0],ip[1],ip[2],ip[3]);
    utubbu_log_text("dns resolved",entry,-1);
    *resolve_entries=curl_slist_append(NULL,entry);
    if(!*resolve_entries)return -2;
    curl_easy_setopt(curl,CURLOPT_RESOLVE,*resolve_entries);
    return 0;
}

void utubbu_network_request_cancel(void)
{
    int rid;
    cancel_pending=1;
    rid=active_resolver;
    if(rid>=0)sceNetResolverStop(rid);
}

static int cancel_progress(void *user,double total_down,double now_down,
    double total_up,double now_up)
{
    (void)user;(void)total_down;(void)now_down;(void)total_up;(void)now_up;
    return cancel_pending?1:0;
}

static int curl_trace(CURL *curl, curl_infotype type, char *data,
    size_t size, void *user)
{
    (void)curl;
    (void)user;
    if (type == CURLINFO_TEXT)
        utubbu_log_text("curl", data, (int)size);
    else if (type == CURLINFO_HEADER_OUT)
        utubbu_log("curl header out", (int)size);
    else if (type == CURLINFO_HEADER_IN)
        utubbu_log("curl header in", (int)size);
    return 0;
}

static size_t write_memory(void *data, size_t size, size_t count, void *user)
{
    MemoryWrite *out = (MemoryWrite *)user;
    int bytes = (int)(size * count);
    int available = out->capacity - out->used - 1;
    if (bytes > available) {
        out->overflow = 1;
        return 0;
    }
    memcpy(out->buffer + out->used, data, bytes);
    out->used += bytes;
    out->buffer[out->used] = '\0';
    return size * count;
}

static int memory_request(const char *url, const char *post, struct curl_slist *headers,
    char *buffer, int capacity)
{
    CURL *curl;
    struct curl_slist *resolve_entries = NULL;
    CURLcode code;
    long http_status = 0;
    MemoryWrite output;
    utubbu_log_text("http request", url, -1);
    utubbu_log("http post bytes", post ? strlen(post) : 0);
    if (!network_ready) { utubbu_log("http no network", -1); return -1; }
    output.buffer = buffer; output.capacity = capacity; output.used = 0; output.overflow = 0;
    buffer[0] = '\0';
    utubbu_log("curl init before", 0);
    curl = curl_easy_init();
    if (!curl) return -2;
    utubbu_log("curl init after", 0);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    if(resolve_for_curl(curl,url,&resolve_entries)<0){
        curl_easy_cleanup(curl);return -4;
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/144 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, cancel_progress);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_trace);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, NULL);
    curl_easy_setopt(curl, CURLOPT_CAPATH, NULL);
    curl_easy_setopt(curl, CURLOPT_CRLFILE, NULL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (post) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(post));
    }
    utubbu_log("stack free", sceKernelGetThreadStackFreeSize(0));
    utubbu_log("curl perform before", 0);
    code = curl_easy_perform(curl);
    utubbu_log("curl perform after", (int)code);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    utubbu_log("http status", (int)http_status);
    utubbu_log("http response bytes", output.used);
    utubbu_log("http overflow", output.overflow);
    curl_easy_cleanup(curl);
    curl_slist_free_all(resolve_entries);
    if (output.overflow) return -3;
    if (code != CURLE_OK) return -(100 + (int)code);
    if (http_status < 200 || http_status >= 300) return -(int)http_status;
    return output.used;
}

int utubbu_http_get_memory(const char *url, char *buffer, int capacity)
{
    return memory_request(url, NULL, NULL, buffer, capacity);
}

int utubbu_youtube_player_request(const char *visitor, const char *body,
    char *buffer, int capacity)
{
    struct curl_slist *headers = NULL;
    char visitor_header[820];
    int result;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "X-YouTube-Client-Name: 28");
    headers = curl_slist_append(headers, "X-YouTube-Client-Version: 1.65.10");
    headers = curl_slist_append(headers, "Origin: https://www.youtube.com");
    snprintf(visitor_header, sizeof(visitor_header), "X-Goog-Visitor-Id: %s", visitor);
    headers = curl_slist_append(headers, visitor_header);
    result = memory_request("https://www.youtube.com/youtubei/v1/player?prettyPrint=false",
        body, headers, buffer, capacity);
    curl_slist_free_all(headers);
    return result;
}

int utubbu_youtube_search_request(const char *body, char *buffer, int capacity)
{
    struct curl_slist *headers = NULL;
    int result;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "X-YouTube-Client-Name: 28");
    headers = curl_slist_append(headers, "X-YouTube-Client-Version: 1.65.10");
    headers = curl_slist_append(headers, "Origin: https://www.youtube.com");
    result = memory_request("https://www.youtube.com/youtubei/v1/search?prettyPrint=false",
        body, headers, buffer, capacity);
    curl_slist_free_all(headers);
    return result;
}

int utubbu_download(const char *url, const char *destination)
{
    CURL *curl;
    CURLcode code;
    long status = 0;
    FILE *output;
    if (!network_ready) return -1;
    output = fopen(destination, "wb");
    if (!output) return -2;
    curl = curl_easy_init();
    if (!curl) { fclose(output); return -3; }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "UTUBBU/0.4 (PlayStation Portable)");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 256L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    /* PSP has no maintained system CA store. Catalog files must be public. */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, NULL);
    curl_easy_setopt(curl, CURLOPT_CAPATH, NULL);
    curl_easy_setopt(curl, CURLOPT_CRLFILE, NULL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, output);
    code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    fclose(output);
    if (code != CURLE_OK || (status != 0 && (status < 200 || status >= 300))) {
        remove(destination);
        return code ? -(100 + (int)code) : -(int)status;
    }
    return 0;
}
