/* WSOLA time stretch, for DT/HT that keep the pitch. Windows of the input are
 * laid down at a fixed output hop; each one is taken from near where the tempo
 * says it should come from, nudged to the spot that lines up best with how the
 * previous window carried on, so the waveforms join without clicks. */
#include "audio.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define WIN 1024     /* ~21 ms */
#define HOP (WIN / 2)
#define SEEK 384     /* how far a window may move to line up */
#define STEP 4       /* candidate positions tried */
#define LOOK 8       /* frames compared inside each, plenty for lining up */

static inline int mono(const int16_t *d, long i) { return d[i * 2] + d[i * 2 + 1]; }

/* how well the input at c matches the input at want, over the overlap */
static double match(const int16_t *d, long want, long c)
{
    int64_t corr = 0, energy = 1;
    for (int i = 0; i < HOP; i += LOOK) {
        int a = mono(d, want + i), b = mono(d, c + i);
        corr += (int64_t)a * b;
        energy += (int64_t)b * b;
    }
    return corr / sqrt((double)energy);
}

bool audio_stretch(const sound_buf *in, double rate, sound_buf *out)
{
    out->data = NULL;
    out->frames = 0;
    long nin = in->frames;
    if (!in->data || nin < WIN * 4 || rate <= 0.1)
        return false;

    long nout = (long)(nin / rate);
    float *acc = calloc((size_t)(nout + WIN) * 2, sizeof(float));
    int16_t *res = malloc((size_t)nout * 2 * sizeof(int16_t));
    if (!acc || !res) {
        free(acc);
        free(res);
        return false;
    }

    /* periodic Hann: at half-window hops the windows add up to exactly 1 */
    static float win[WIN];
    for (int i = 0; i < WIN; i++)
        win[i] = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * i / WIN);

    const int16_t *d = in->data;
    long last = nin - WIN - 1, prev = -1;

    for (long opos = 0; opos < nout; opos += HOP) {
        long pick = (long)(opos * rate);
        if (pick > last)
            pick = last;

        if (prev >= 0) {
            long want = prev + HOP; /* where the previous window would have carried on */
            if (want <= last) {
                double best = -1e300;
                long lo = pick - SEEK < 0 ? 0 : pick - SEEK, hi = pick + SEEK > last ? last : pick + SEEK;
                long at = pick;
                for (long c = lo; c <= hi; c += STEP) {
                    double m = match(d, want, c);
                    if (m > best) {
                        best = m;
                        at = c;
                    }
                }
                pick = at;
            }
        }

        float *o = acc + opos * 2;
        const int16_t *s = d + pick * 2;
        for (int i = 0; i < WIN; i++) {
            o[i * 2] += win[i] * s[i * 2];
            o[i * 2 + 1] += win[i] * s[i * 2 + 1];
        }
        prev = pick;
    }

    for (long i = 0; i < nout * 2; i++) {
        float v = acc[i];
        res[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    free(acc);
    out->data = res;
    out->frames = (uint32_t)nout;
    return true;
}
