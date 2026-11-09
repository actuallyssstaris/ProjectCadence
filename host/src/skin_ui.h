#ifndef OSU5_SKIN_UI_H
#define OSU5_SKIN_UI_H

#include "input.h"
#include <stdbool.h>

/* load the given skin id ("builtin" for the drawn look) for images and sounds */
void skin_apply(const char *id);

/* the skin picker; first_run turns it into the welcome screen */
void skin_ui_open(bool first_run);
bool skin_ui_frame(const pad_state *pad); /* true once it should close */
void skin_ui_render(void);

#endif
