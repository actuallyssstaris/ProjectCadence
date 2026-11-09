#ifndef OSU5_REPLAY_H
#define OSU5_REPLAY_H

#include <stdbool.h>
#include <stdint.h>

/* Every osu5_update call of a play: feeding the same calls back gives the same
 * judgements, so that's all a replay needs. Kept in /data/cadence/replays/<when>.o5r,
 * <when> being the score's timestamp. */

typedef struct {
    double t;
    float x, y;
    int32_t keys;
} replay_frame;

typedef struct {
    char osu[768];
    int mods;
    replay_frame *frames;
    int count, cap;
} replay;

void replay_reset(replay *r, const char *osu_path, int mods);
void replay_push(replay *r, double t, float x, float y, int keys);
bool replay_save(const replay *r, int64_t when);
bool replay_load(replay *r, int64_t when);
bool replay_exists(int64_t when);
void replay_free(replay *r);

#endif
