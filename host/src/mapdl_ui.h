#ifndef OSU5_MAPDL_UI_H
#define OSU5_MAPDL_UI_H

#include "input.h"
#include <stdbool.h>

void mapdl_ui_open(void);
bool mapdl_ui_frame(const pad_state *pad); /* true = back to song select */
void mapdl_ui_render(void);
/* the plain list instead of the lazer cards */
void mapdl_ui_set_simple(bool on);
/* the mode the listing searches in, following the toolbar */
void mapdl_ui_set_mode(int mode);

#endif
