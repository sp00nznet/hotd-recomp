/* DirectDraw kept off the real screen (ddraw_window.c). */
#pragma once
#include <stdint.h>

void ddw_init(int headless, const char* record);
/* Hook every IDirectDraw version before the game runs. 0 means the run must
 * not go ahead: a compatibility layer intercepts DirectDraw, or a hook could
 * not be placed. With fullscreen only the check is made. */
int ddw_install(int fullscreen);
/* Watch every monitor's mode from now on; any change ends the process. */
void ddw_guard_start(void);
void ddw_close(void);
long ddw_frames(void);
/* ffmpeg.exe next to the host, else "ffmpeg" (PATH). ff holds MAX_PATH. */
void ddw_ffmpeg(char* ff);
/* A movie is playing: its frames are shown, the game's presents are not. */
void ddw_movie(int on);
void ddw_movie_frame(const uint32_t* bgra, int w, int h);
