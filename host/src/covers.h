#ifndef OSU5_COVERS_H
#define OSU5_COVERS_H

/* call once a frame on the render thread, before drawing */
void covers_frame(void);
/* the set's cover texture, or 0 while it's on the way (or has none);
 * asking is what queues the download */
int covers_get(int set_id);

#endif
