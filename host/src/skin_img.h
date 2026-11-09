#ifndef OSU5_SKIN_IMG_H
#define OSU5_SKIN_IMG_H

#include <stdbool.h>

/* tex 0 = the skin doesn't have it; w/h are in SD pixels (an @2x file counts half) */
typedef struct {
    int tex;
    float w, h;
} skin_sprite;

typedef struct {
    bool on;
    skin_sprite circle, overlay, approach, digit[10];
    skin_sprite cursor, ball, follow, reverse, tick, point;
    skin_sprite hit0, hit50, hit100, hit300;
    float overlap; /* HitCircleOverlap, SD pixels */
} skin_images;

extern skin_images skin_img;

/* NULL or a folder without images = the drawn look */
void skin_images_load(const char *dir);

#endif
