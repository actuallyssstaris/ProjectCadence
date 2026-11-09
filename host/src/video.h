#ifndef OSU5_VIDEO_H
#define OSU5_VIDEO_H

#include <stdbool.h>
#include <stddef.h>

/* the map's Video event, if it's a file the player can read */
bool video_find(const char *dir, const char *osu_path, char *out, size_t n, double *offset);

bool video_open(const char *path, double offset_ms);
/* once a frame between frames, with the map clock; uploads new pictures */
void video_tick(double map_time, bool paused);
/* false while there's nothing to show yet */
bool video_draw(float dim);
void video_close(void);

#endif
