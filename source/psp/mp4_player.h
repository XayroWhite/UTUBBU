#ifndef UTUBBU_MP4_PLAYER_H
#define UTUBBU_MP4_PLAYER_H

int utubbu_mp4_play(const char *video_path, const char *audio_path);
int utubbu_mp4_play_software(const char *video_path, const char *audio_path);
void utubbu_mp4_request_stop(void);

#endif
