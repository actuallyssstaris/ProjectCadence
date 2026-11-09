#include "replay.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REPLAY_DIR "/data/cadence/replays"
#define MAGIC "O5R1"

static void path_for(int64_t when, char *out, size_t n)
{
    snprintf(out, n, REPLAY_DIR "/%lld.o5r", (long long)when);
}

void replay_reset(replay *r, const char *osu_path, int mods)
{
    snprintf(r->osu, sizeof(r->osu), "%s", osu_path);
    r->mods = mods;
    r->count = 0;
}

void replay_push(replay *r, double t, float x, float y, int keys)
{
    if (r->count == r->cap) {
        int cap = r->cap ? r->cap * 2 : 8192;
        replay_frame *grown = realloc(r->frames, sizeof(replay_frame) * (size_t)cap);
        if (!grown)
            return;
        r->frames = grown;
        r->cap = cap;
    }
    r->frames[r->count++] = (replay_frame){t, x, y, keys};
}

/* layout: "O5R1", mods, frame count, path length, path, frames as they are in memory */
bool replay_save(const replay *r, int64_t when)
{
    mkdir(REPLAY_DIR, 0777);
    char path[128];
    path_for(when, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    int32_t head[3] = {r->mods, r->count, (int32_t)strlen(r->osu)};
    bool ok = fwrite(MAGIC, 1, 4, f) == 4 && fwrite(head, sizeof(head), 1, f) == 1 &&
              fwrite(r->osu, 1, (size_t)head[2], f) == (size_t)head[2] &&
              fwrite(r->frames, sizeof(replay_frame), (size_t)r->count, f) == (size_t)r->count;
    fclose(f);
    if (!ok)
        remove(path);
    LOG("replay: %s, %d frames%s", path, r->count, ok ? "" : " (write failed)");
    return ok;
}

bool replay_load(replay *r, int64_t when)
{
    char path[128], magic[4];
    path_for(when, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    int32_t head[3];
    bool ok = fread(magic, 1, 4, f) == 4 && !memcmp(magic, MAGIC, 4) && fread(head, sizeof(head), 1, f) == 1 && head[1] >= 0 &&
              head[2] > 0 && head[2] < (int32_t)sizeof(r->osu);
    if (ok) {
        ok = fread(r->osu, 1, (size_t)head[2], f) == (size_t)head[2];
        r->osu[ok ? head[2] : 0] = 0;
        r->mods = head[0];
    }
    if (ok && head[1] > r->cap) {
        replay_frame *grown = realloc(r->frames, sizeof(replay_frame) * (size_t)head[1]);
        ok = grown != NULL;
        if (grown) {
            r->frames = grown;
            r->cap = head[1];
        }
    }
    if (ok) {
        r->count = head[1];
        ok = fread(r->frames, sizeof(replay_frame), (size_t)r->count, f) == (size_t)r->count;
    }
    fclose(f);
    if (!ok)
        r->count = 0;
    return ok;
}

bool replay_exists(int64_t when)
{
    char path[128];
    path_for(when, path, sizeof(path));
    return access(path, F_OK) == 0;
}

void replay_free(replay *r)
{
    free(r->frames);
    r->frames = NULL;
    r->count = r->cap = 0;
}
