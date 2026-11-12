#ifndef OSU5_OSR_H
#define OSU5_OSR_H

#include "replay.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* osu!'s .osr replay files: ours go out to /data/cadence/exports, theirs come in
 * from /data/cadence/replays/import (copy them there over FTP). */

#define OSR_IMPORT_DIR "/data/cadence/replays/import"
#define OSR_EXPORT_DIR "/data/cadence/exports"

typedef struct {
    int mode;
    char md5[33];
    char player[32];
    int n300, n100, n50, geki, katu, miss;
    int score, max_combo;
    bool perfect;
    int mods;       /* OSU5_MOD_* */
    long long when; /* unix ms */
} osr_info;

/* writes the file and puts its name in out_name; false if it couldn't */
bool osr_export(const osr_info *info, const replay *rp, const char *title, char *out_name, size_t n);

/* reads the header, and the frames too when rp isn't NULL */
bool osr_read(const char *path, osr_info *info, replay *rp, const char *osu_path);

#endif
