#ifndef UTUBBU_NETWORK_H
#define UTUBBU_NETWORK_H

int utubbu_network_connect(int profile);
void utubbu_network_shutdown(void);
void utubbu_network_request_cancel(void);
int utubbu_download(const char *url, const char *destination);
int utubbu_stream_start(const char *url, const char *destination);
int utubbu_stream_state(int *downloaded, int *total, int *error);
int utubbu_stream_start_slot(int slot, const char *url, const char *destination);
int utubbu_stream_state_slot(int slot, int *downloaded, int *total, int *error);
void utubbu_stream_stop(void);
int utubbu_http_get_memory(const char *url, char *buffer, int capacity);
int utubbu_youtube_player_request(const char *visitor, const char *body,
    char *buffer, int capacity);
int utubbu_youtube_search_request(const char *body, char *buffer, int capacity);

#endif
