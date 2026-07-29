#ifndef UTUBBU_YOUTUBE_H
#define UTUBBU_YOUTUBE_H

#include "catalog.h"

int utubbu_youtube_search(const char *query, UtubbuCatalog *catalog);
int utubbu_youtube_resolve(const char *video_id, char *video_url, int video_capacity,
    char *audio_url, int audio_capacity);
int utubbu_youtube_resolve_mode(const char *video_id, char *video_url,
    int video_capacity, char *audio_url, int audio_capacity,
    int compatibility_mode);

#endif
