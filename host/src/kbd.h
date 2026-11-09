#ifndef OSU5_KBD_H
#define OSU5_KBD_H

#include "input.h"
#include <stddef.h>

/* On-screen keyboard driven by the pad, typing into a caller's buffer */
typedef struct {
    char *text;
    size_t cap;
    int kx, ky; /* cursor; ky 4 is the wide bottom row */
} kbd;

enum { KBD_TYPING, KBD_DONE, KBD_CANCEL };

int kbd_frame(kbd *k, const pad_state *pad);
void kbd_render(const kbd *k);

#endif
