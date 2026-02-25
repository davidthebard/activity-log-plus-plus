#pragma once

#include <stdbool.h>

void audio_init(const char *path);   /* "romfs:/bgm.mp3" */
void audio_tick(void);               /* call once per frame */
void audio_set_enabled(bool enabled);
bool audio_get_enabled(void);
void audio_exit(void);
