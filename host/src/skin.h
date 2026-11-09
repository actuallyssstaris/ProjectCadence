#ifndef SKIN_H
#define SKIN_H

#include "../include/osu5.h"
#include <stdbool.h>

void skin_load(const char *dir);
void skin_play(const osu5_sound *ev, float master);
void skin_section(bool pass, float master);

#endif
