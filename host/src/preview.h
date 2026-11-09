#ifndef OSU5_PREVIEW_H
#define OSU5_PREVIEW_H

#include <stdbool.h>

/* Song select's music and background, loaded off the main thread so scrolling
 * doesn't hitch. The newest request wins; older ones are dropped. */

void preview_request(const char *audio_path, const char *bg_path, int preview_ms);

/* stop the preview for good (starting a map); its music is never set after this */
void preview_cancel(void);

/* main thread, every select frame: keeps the preview looping */
void preview_tick(void);

/* background pixels ready for the latest request? hands over rgba (stbi buffer,
 * free with stbi_image_free) and the path it came from */
bool preview_take_bg(unsigned char **px, int *w, int *h, char *path, int path_len);

#endif
