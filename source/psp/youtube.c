#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diagnostic.h"
#include "network.h"
#include "youtube.h"

#define SEARCH_CAPACITY (640 * 1024)
#define PLAYER_CAPACITY (512 * 1024)

static char cached_visitor[768];
static char youtube_response[SEARCH_CAPACITY];
static int extract_visitor(const char *page, char *visitor, int capacity);

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void json_string(const char *src, char *dst, int capacity)
{
    int out = 0;
    while (*src && *src != '"' && out + 1 < capacity) {
        if (*src == '\\') {
            ++src;
            if (*src == 'u' && hex_value(src[1]) >= 0 && hex_value(src[2]) >= 0 &&
                hex_value(src[3]) >= 0 && hex_value(src[4]) >= 0) {
                int code = (hex_value(src[1]) << 12) | (hex_value(src[2]) << 8) |
                    (hex_value(src[3]) << 4) | hex_value(src[4]);
                if (code >= 32 && code <= 126) dst[out++] = (char)code;
                else if (code == 0x2019) dst[out++] = '\'';
                else dst[out++] = ' ';
                src += 5;
                continue;
            }
            if (*src == 'n' || *src == 'r' || *src == 't') dst[out++] = ' ';
            else if (*src) dst[out++] = *src;
            if (*src) ++src;
            continue;
        }
        dst[out++] = (unsigned char)*src < 128 ? *src : ' ';
        ++src;
    }
    dst[out] = '\0';
}

static void json_escape(const char *input, char *output, int capacity)
{
    int out = 0;
    while (*input && out + 1 < capacity) {
        unsigned char c = (unsigned char)*input++;
        if ((c == '"' || c == '\\') && out + 2 < capacity) {
            output[out++] = '\\'; output[out++] = (char)c;
        } else if (c >= 32 && c < 127) output[out++] = (char)c;
    }
    output[out] = '\0';
}

static int already_added(const UtubbuCatalog *catalog, const char *id)
{
    int i;
    for (i = 0; i < catalog->count; ++i)
        if (!strcmp(catalog->items[i].source + 3, id)) return 1;
    return 0;
}

int utubbu_youtube_search(const char *query, UtubbuCatalog *catalog)
{
    char escaped[96];
    char body[512];
    char *response;
    char *at;
    int status;

    utubbu_log_text("yt search enter", query, -1);
    response = youtube_response;
    json_escape(query, escaped, sizeof(escaped));
    snprintf(body, sizeof(body),
        "{\"context\":{\"client\":{\"clientName\":\"ANDROID_VR\","
        "\"clientVersion\":\"1.65.10\",\"deviceMake\":\"Oculus\","
        "\"deviceModel\":\"Quest 3\",\"androidSdkVersion\":32,"
        "\"osName\":\"Android\",\"osVersion\":\"12L\"}},\"query\":\"%s\"}", escaped);
    status = utubbu_youtube_search_request(body, response, SEARCH_CAPACITY);
    utubbu_log("yt search response bytes", status);
    if (status < 0) return status;

    status = extract_visitor(response, cached_visitor, sizeof(cached_visitor));
    utubbu_log("yt search visitor", status);
    if (status < 0)
        cached_visitor[0] = '\0';

    catalog->count = 0;
    at = response;
    while (catalog->count < 12 && (at = strstr(at, "\"compactVideoRenderer\":")) != NULL) {
        char *end = strstr(at + 24, "\"compactVideoRenderer\":");
        char *id_at = strstr(at, "\"videoId\":\"");
        char *title_at = strstr(at, "\"title\":");
        char *text_at = title_at ? strstr(title_at, "\"text\":\"") : NULL;
        char *length_at = strstr(at, "\"lengthText\":");
        char *duration_at = length_at ? strstr(length_at, "\"simpleText\":\"") : NULL;
        char *duration_run = length_at ? strstr(length_at, "\"text\":\"") : NULL;
        char *seconds_at = strstr(at, "\"lengthSeconds\":\"");
        char id[16];
        int id_len;
        if ((end && ((id_at && id_at >= end) || (text_at && text_at >= end))) || !id_at || !text_at) {
            at += 24; continue;
        }
        id_at += 11;
        {
            char *id_end = strchr(id_at, '"');
            if (!id_end || (end && id_end >= end)) { at += 24; continue; }
            id_len = (int)(id_end - id_at);
        }
        if (id_len != 11) { at += 24; continue; }
        memcpy(id, id_at, 11); id[11] = '\0';
        if (!already_added(catalog, id)) {
            UtubbuCatalogItem *item = &catalog->items[catalog->count++];
            json_string(text_at + 8, item->title, sizeof(item->title));
            if(duration_at&&(!end||duration_at<end))json_string(duration_at+14,item->duration,sizeof(item->duration));
            else if(duration_run&&(!end||duration_run<end))json_string(duration_run+8,item->duration,sizeof(item->duration));
            else if(seconds_at&&(!end||seconds_at<end)){
                int seconds=atoi(seconds_at+17);
                if(seconds>=3600)snprintf(item->duration,sizeof(item->duration),"%d:%02d:%02d",seconds/3600,(seconds/60)%60,seconds%60);
                else snprintf(item->duration,sizeof(item->duration),"%d:%02d",seconds/60,seconds%60);
            }else strcpy(item->duration,"--:--");
            utubbu_log_text("search duration",item->duration,-1);
            snprintf(item->source, sizeof(item->source), "yt:%s", id);
            snprintf(item->local_name, sizeof(item->local_name), "yt-%s.mp4", id);
        }
        at = text_at + 8;
    }
    status = catalog->count > 0 ? 0 : -21;
    utubbu_log("yt search parsed", catalog->count);
    utubbu_log("yt search result", status);
    return status;
}

static int extract_visitor(const char *page, char *visitor, int capacity)
{
    const char *at = strstr(page, "\"VISITOR_DATA\":\"");
    if (at) at += 16;
    else {
        at = strstr(page, "\"visitorData\":\"");
        if (at) at += 15;
    }
    if (!at) return -1;
    json_string(at, visitor, capacity);
    return visitor[0] ? 0 : -1;
}

static int format_url(char *response, int itag, char *url, int capacity)
{
    char needle[24]; char *format, *url_at;
    snprintf(needle, sizeof(needle), "\"itag\":%d,", itag);
    format = strstr(response, needle);
    if (!format) return -1;
    url_at = strstr(format, "\"url\":\"");
    if (!url_at || url_at - format > 4096) return -1;
    json_string(url_at + 7, url, capacity);
    return url[0] ? 0 : -1;
}

static int format_number(char *response, int itag, const char *name)
{
    char needle[24],field[32];char *format,*next,*at;
    snprintf(needle,sizeof(needle),"\"itag\":%d,",itag);
    snprintf(field,sizeof(field),"\"%s\":",name);
    format=strstr(response,needle);if(!format)return 0;
    next=strstr(format+strlen(needle),"\"itag\":");
    at=strstr(format,field);
    if(!at||(next&&at>next)||at-format>4096)return 0;
    return atoi(at+strlen(field));
}

int utubbu_youtube_resolve_mode(const char *video_id, char *video_url,
    int video_capacity, char *audio_url, int audio_capacity,
    int compatibility_mode)
{
    char visitor[768];
    char search_body[512];
    char body[1800];
    char *response;
    int status,video_width,video_height,video_itag=133;

    utubbu_log("resolve enter", 0);
    utubbu_log("resolve compatibility mode", compatibility_mode);
    utubbu_log_text("resolve video id", video_id, -1);
    response = youtube_response;
    if (cached_visitor[0]) {
        strncpy(visitor, cached_visitor, sizeof(visitor) - 1);
        visitor[sizeof(visitor) - 1] = '\0';
    } else {
        snprintf(search_body, sizeof(search_body),
            "{\"context\":{\"client\":{\"clientName\":\"ANDROID_VR\","
            "\"clientVersion\":\"1.65.10\",\"deviceMake\":\"Oculus\","
            "\"deviceModel\":\"Quest 3\",\"androidSdkVersion\":32,"
            "\"osName\":\"Android\",\"osVersion\":\"12L\"}},\"query\":\"%s\"}", video_id);
        status = utubbu_youtube_search_request(search_body, response, PLAYER_CAPACITY);
        utubbu_log("resolve visitor request", status);
        if (status < 0 || extract_visitor(response, visitor, sizeof(visitor)) < 0) {
            utubbu_log("resolve visitor failed", status < 0 ? status : -31);
            return status < 0 ? status : -31;
        }
        strncpy(cached_visitor, visitor, sizeof(cached_visitor) - 1);
        cached_visitor[sizeof(cached_visitor) - 1] = '\0';
    }
    snprintf(body, sizeof(body),
        "{\"context\":{\"client\":{\"clientName\":\"ANDROID_VR\","
        "\"clientVersion\":\"1.65.10\",\"deviceMake\":\"Oculus\","
        "\"deviceModel\":\"Quest 3\",\"androidSdkVersion\":32,"
        "\"userAgent\":\"com.google.android.apps.youtube.vr.oculus/1.65.10 "
        "(Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip\","
        "\"osName\":\"Android\",\"osVersion\":\"12L\",\"visitorData\":\"%s\"}},"
        "\"videoId\":\"%s\",\"playbackContext\":{\"contentPlaybackContext\":"
        "{\"html5Preference\":\"HTML5_PREF_WANTS\"}},\"contentCheckOk\":true,"
        "\"racyCheckOk\":true}", visitor, video_id);
    utubbu_log("player request before", 0);
    status = utubbu_youtube_player_request(visitor, body, response, PLAYER_CAPACITY);
    utubbu_log("player request after", status);
    if (status < 0) return status;

    video_url[0] = '\0'; audio_url[0] = '\0';
    /* The combined stream avoids a second TLS/download pipeline and is the
       configuration used by the AM playback baseline. */
    if(!compatibility_mode){
        status=format_url(response,18,video_url,video_capacity);
        video_width=format_number(response,18,"width");
        video_height=format_number(response,18,"height");
        utubbu_log("resolve video itag18",status);
        utubbu_log("resolve itag18 width",video_width);
        utubbu_log("resolve itag18 height",video_height);
        if(status==0&&video_width>0&&video_height>0&&video_width<=720&&video_height<=480){
            if(strlen(video_url)>=(size_t)audio_capacity)return -33;
            strcpy(audio_url,video_url);
            utubbu_log("resolve selected itag",18);
            utubbu_log("resolve complete",0);
            return 0;
        }
    }
    video_url[0]='\0';
    /* Balanced mode: 426x240 landscape is visibly better than 144p while it
       still avoids the expensive 640x360 mode-5 path. */
    video_itag=compatibility_mode?160:133;
    status=format_url(response,video_itag,video_url,video_capacity);
    utubbu_log("resolve compatibility video",status);
    if(status<0){
        video_itag=160;
        status=format_url(response,video_itag,video_url,video_capacity);
        utubbu_log("resolve fallback itag160",status);
        if(status<0)return -32;
    }
    video_width=format_number(response,video_itag,"width");
    video_height=format_number(response,video_itag,"height");
    if(video_height>272){
        video_itag=160;
        status=format_url(response,video_itag,video_url,video_capacity);
        utubbu_log("resolve portrait itag160",status);
        if(status<0)return -32;
        video_width=format_number(response,video_itag,"width");
        video_height=format_number(response,video_itag,"height");
    }
    utubbu_log("resolve video width",video_width);
    utubbu_log("resolve video height",video_height);
    utubbu_log("resolve selected itag",video_itag);
    status = format_url(response, 140, audio_url, audio_capacity);
    utubbu_log("resolve audio itag140", status);
    if (status < 0) return -33;
    utubbu_log("resolve complete", 0);
    return 0;
}

int utubbu_youtube_resolve(const char *video_id, char *video_url,
    int video_capacity, char *audio_url, int audio_capacity)
{
    return utubbu_youtube_resolve_mode(video_id,video_url,video_capacity,
        audio_url,audio_capacity,0);
}
