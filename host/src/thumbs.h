#ifndef OSU5_THUMBS_H
#define OSU5_THUMBS_H

#define THUMB_W 384
#define THUMB_H 216

/* texture for a background image, 0 until it's ready (asking queues it) */
int thumb_get(const char *path);
/* main thread, once a frame: uploads what the worker finished */
void thumb_tick(void);

#endif
