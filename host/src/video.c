/* Video backgrounds through the system's AvPlayer, the player library console
 * apps use. It's loaded at run time, so a console where it won't load just keeps
 * the still background. Frames come out as NV12; we turn them into a half size
 * RGBA texture on the CPU, which is plenty for something under a 80% dim. */
#include "video.h"
#include "gfx.h"
#include "log.h"
#include "songs.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/types.h>

int sceSysmoduleLoadModule(uint16_t id);
int sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags, void *opt, int *res);
int sceKernelDlsym(int handle, const char *sym, void **addr);
int sceKernelAllocateMainDirectMemory(size_t len, size_t align, int type, off_t *phys);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, off_t phys, size_t align);
int sceKernelReleaseDirectMemory(off_t start, size_t len);

/* layouts as the library has them (shadPS4's reimplementation spells them out) */
typedef struct {
    void *obj;
    void *(*alloc)(void *obj, uint32_t align, uint32_t size);
    void (*dealloc)(void *obj, void *mem);
    void *(*alloc_tex)(void *obj, uint32_t align, uint32_t size);
    void (*dealloc_tex)(void *obj, void *mem);
} av_mem;

typedef struct {
    void *obj;
    int (*open)(void *obj, const char *name);
    int (*close)(void *obj);
    int (*read)(void *obj, uint8_t *buf, uint64_t pos, uint32_t len);
    uint64_t (*size)(void *obj);
} av_file;

typedef struct {
    void *obj;
    void (*event)(void *obj, int ev, int src, void *data);
} av_events;

typedef struct {
    av_mem mem;
    av_file file;
    av_events events;
    int debug;
    uint32_t priority;
    int32_t buffers;
    bool auto_start;
    uint8_t reserved[3];
    const char *language;
} av_init;

typedef struct {
    uint32_t width, height;
    float aspect;
    uint8_t language[4];
    uint32_t framerate;
    uint32_t crop_left, crop_right, crop_top, crop_bottom;
    uint32_t pitch;
    uint8_t luma_bits, chroma_bits;
    bool full_range;
    uint8_t reserved[37];
} av_video;

typedef struct {
    void *data;
    uint8_t reserved[4];
    uint64_t timestamp;
    union {
        av_video video;
        uint8_t raw[80];
    } d;
} av_frame;

typedef struct {
    uint32_t type; /* 0 video, 1 audio, 2 text */
    uint8_t reserved[4];
    uint8_t details[16];
    uint64_t duration, start;
} av_stream;

static struct {
    void *(*init)(av_init *);
    int (*add_source)(void *, const char *);
    int (*start)(void *);
    int (*stop)(void *);
    int (*close)(void *);
    int (*pause)(void *);
    int (*resume)(void *);
    bool (*video_ex)(void *, av_frame *);
    int (*stream_count)(void *);
    int (*stream_info)(void *, uint32_t, av_stream *);
    int (*disable)(void *, uint32_t);
    uint64_t (*now)(void *);
    int (*jump)(void *, uint64_t);
} av;

static int lib_state; /* 0 not tried, 1 loaded, -1 not available */

static bool load_lib(void)
{
    if (lib_state)
        return lib_state > 0;
    lib_state = -1;
    int sm = sceSysmoduleLoadModule(0x0095); /* the PS4 numbering; harmless if it means nothing here */
    LOG("video: sysmodule 0x%x", (unsigned)sm);
    /* PS5 titles get the .native build; the plain one is the PS4 compat copy */
    static const char *paths[] = {"/system/common/lib/libSceAvPlayer.native.sprx", "libSceAvPlayer.native.sprx",
                                  "/system/common/lib/libSceAvPlayer.sprx", "libSceAvPlayer.sprx"};
    int h = -1;
    for (int i = 0; i < 4 && h <= 0; i++) {
        int res = 0;
        h = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &res);
        LOG("video: load %s -> 0x%x", paths[i], (unsigned)h);
    }
    if (h <= 0)
        return false;

    static const struct { const char *name; void **slot; } syms[] = {
        {"sceAvPlayerInit", (void **)&av.init},
        {"sceAvPlayerAddSource", (void **)&av.add_source},
        {"sceAvPlayerStart", (void **)&av.start},
        {"sceAvPlayerStop", (void **)&av.stop},
        {"sceAvPlayerClose", (void **)&av.close},
        {"sceAvPlayerPause", (void **)&av.pause},
        {"sceAvPlayerResume", (void **)&av.resume},
        {"sceAvPlayerGetVideoDataEx", (void **)&av.video_ex},
        {"sceAvPlayerStreamCount", (void **)&av.stream_count},
        {"sceAvPlayerGetStreamInfo", (void **)&av.stream_info},
        {"sceAvPlayerDisableStream", (void **)&av.disable},
        {"sceAvPlayerCurrentTime", (void **)&av.now},
        {"sceAvPlayerJumpToTime", (void **)&av.jump},
    };
    for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++)
        if (sceKernelDlsym(h, syms[i].name, syms[i].slot) != 0 || !*syms[i].slot) {
            LOG("video: no %s", syms[i].name);
            return false;
        }
    lib_state = 1;
    return true;
}

/* ---- memory the player asks for ---- */

static void *mem_alloc(void *obj, uint32_t align, uint32_t size)
{
    void *p = NULL;
    return posix_memalign(&p, align < 16 ? 16 : align, size) == 0 ? p : NULL;
}

static void mem_free(void *obj, void *p) { free(p); }

/* frame buffers have to be memory the decoder hardware can reach */
#define MAX_TEX_ALLOCS 32
static struct { void *addr; off_t phys; size_t len; } tex_allocs[MAX_TEX_ALLOCS];
static pthread_mutex_t tex_lock = PTHREAD_MUTEX_INITIALIZER;

static void *tex_alloc(void *obj, uint32_t align, uint32_t size)
{
    size_t a = align < 0x10000 ? 0x10000 : align;
    size_t len = (size + 0xFFFF) & ~(size_t)0xFFFF;
    off_t phys = 0;
    if (sceKernelAllocateMainDirectMemory(len, a, 0, &phys) != 0)
        return NULL;
    void *addr = NULL;
    if (sceKernelMapDirectMemory(&addr, len, 0x33, 0, phys, a) != 0) {
        sceKernelReleaseDirectMemory(phys, len);
        return NULL;
    }
    pthread_mutex_lock(&tex_lock);
    for (int i = 0; i < MAX_TEX_ALLOCS; i++)
        if (!tex_allocs[i].addr) {
            tex_allocs[i].addr = addr;
            tex_allocs[i].phys = phys;
            tex_allocs[i].len = len;
            break;
        }
    pthread_mutex_unlock(&tex_lock);
    return addr;
}

static void tex_free(void *obj, void *p)
{
    pthread_mutex_lock(&tex_lock);
    for (int i = 0; i < MAX_TEX_ALLOCS; i++)
        if (tex_allocs[i].addr == p) {
            munmap(p, tex_allocs[i].len);
            sceKernelReleaseDirectMemory(tex_allocs[i].phys, tex_allocs[i].len);
            tex_allocs[i].addr = NULL;
            break;
        }
    pthread_mutex_unlock(&tex_lock);
}

/* ---- one playing video ---- */

static struct {
    void *h;
    volatile bool ready;
    bool streams_set, started, paused;
    double offset;
    uint64_t last_ts;
    int tex, w, h_px;
    unsigned char *rgba;
} v;

static void on_event(void *obj, int ev, int src, void *data)
{
    if (ev == 2) /* ready */
        v.ready = true;
}

bool video_find(const char *dir, const char *osu_path, char *out, size_t n, double *offset)
{
    FILE *f = fopen(osu_path, "r");
    if (!f)
        return false;
    char line[1024];
    bool events = false, found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        if (line[0] == '[') {
            events = !strncmp(line, "[Events]", 8);
            continue;
        }
        if (!events || (strncmp(line, "Video,", 6) && strncmp(line, "1,", 2)))
            continue;
        char *a = strchr(line, ','), *b = a ? strchr(a + 1, ',') : NULL;
        if (!b)
            continue;
        *offset = atof(a + 1);
        char *name = b + 1;
        while (*name == ' ' || *name == '"')
            name++;
        name[strcspn(name, "\"\r\n")] = 0;
        for (char *c = name; *c; c++)
            if (*c == '\\')
                *c = '/';
        const char *ext = strrchr(name, '.');
        /* the player only reads MP4; old maps' .avi / .flv stay still images */
        if (!ext || (strcasecmp(ext, ".mp4") && strcasecmp(ext, ".m4v") && strcasecmp(ext, ".mov"))) {
            LOG("video: %s isn't mp4, skipped", name);
            break;
        }
        found = songs_file(dir, name, out, n);
        LOG("video: map wants %s at %.0f ms%s", name, *offset, found ? "" : ", not in the folder");
    }
    fclose(f);
    return found;
}

bool video_open(const char *path, double offset)
{
    video_close();
    if (!load_lib())
        return false;
    av_init in;
    memset(&in, 0, sizeof(in));
    in.mem.alloc = mem_alloc;
    in.mem.dealloc = mem_free;
    in.mem.alloc_tex = tex_alloc;
    in.mem.dealloc_tex = tex_free;
    in.events.event = on_event;
    in.priority = 700;
    in.buffers = 2;
    in.auto_start = false;
    in.language = "en";
    v.h = av.init(&in);
    if (!v.h) {
        LOG("video: init failed");
        return false;
    }
    int rc = av.add_source(v.h, path);
    if (rc < 0) {
        LOG("video: add source %s: 0x%x", path, (unsigned)rc);
        av.close(v.h);
        v.h = NULL;
        return false;
    }
    v.offset = offset;
    LOG("video: %s from %.0f ms", path, offset);
    return true;
}

static void to_rgba(const av_frame *f)
{
    const av_video *vi = &f->d.video;
    int sw = (int)(vi->width - vi->crop_left - vi->crop_right), sh = (int)(vi->height - vi->crop_top - vi->crop_bottom);
    int w = sw / 2, h = sh / 2;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048)
        return;
    if (!v.rgba || w != v.w || h != v.h_px) {
        if (v.tex)
            gfx_texture_free(v.tex);
        free(v.rgba);
        v.rgba = malloc((size_t)w * h * 4);
        if (!v.rgba)
            return;
        memset(v.rgba, 0, (size_t)w * h * 4);
        v.tex = gfx_texture(w, h, v.rgba);
        v.w = w;
        v.h_px = h;
    }
    uint32_t pitch = vi->pitch ? vi->pitch : vi->width;
    const uint8_t *Y = (const uint8_t *)f->data, *UV = Y + (size_t)pitch * vi->height;
    /* BT.709, video range unless the stream says full */
    bool full = vi->full_range;
    for (int y = 0; y < h; y++) {
        const uint8_t *yr = Y + (size_t)(vi->crop_top + y * 2) * pitch + vi->crop_left;
        const uint8_t *uvr = UV + (size_t)((vi->crop_top / 2) + y) * pitch + (vi->crop_left & ~1u);
        uint8_t *o = v.rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            float yy = yr[x * 2], u = uvr[x * 2] - 128.0f, vv = uvr[x * 2 + 1] - 128.0f;
            if (!full) {
                yy = (yy - 16) * 1.164f;
                u *= 1.138f;
                vv *= 1.138f;
            }
            float r = yy + 1.5748f * vv, g = yy - 0.1873f * u - 0.4681f * vv, b = yy + 1.8556f * u;
            o[x * 4] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
            o[x * 4 + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
            o[x * 4 + 2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
            o[x * 4 + 3] = 255;
        }
    }
    gfx_texture_update(v.tex, w, h, v.rgba);
}

void video_tick(double map_time, bool paused)
{
    if (!v.h || !v.ready)
        return;
    if (!v.streams_set) {
        /* our own audio plays the song; the video's sound would only double it */
        int n = av.stream_count(v.h);
        for (int i = 0; i < n; i++) {
            av_stream s;
            memset(&s, 0, sizeof(s));
            if (av.stream_info(v.h, (uint32_t)i, &s) == 0 && s.type == 1)
                av.disable(v.h, (uint32_t)i);
        }
        v.streams_set = true;
    }
    double vt = map_time - v.offset;
    if (!v.started) {
        if (vt < 0)
            return;
        av.start(v.h);
        if (vt > 500)
            av.jump(v.h, (uint64_t)vt); /* skipped the intro */
        v.started = true;
    }
    if (paused != v.paused) {
        paused ? av.pause(v.h) : av.resume(v.h);
        v.paused = paused;
    }
    if (paused)
        return;
    /* the player keeps its own clock; pull it back if it wanders */
    double drift = (double)av.now(v.h) - vt;
    if (fabs(drift) > 400 && vt > 0)
        av.jump(v.h, (uint64_t)vt);

    av_frame f;
    memset(&f, 0, sizeof(f));
    if (av.video_ex(v.h, &f) && f.data && f.timestamp != v.last_ts) {
        v.last_ts = f.timestamp;
        to_rgba(&f);
    }
}

bool video_draw(float dim)
{
    if (!v.h || !v.tex || !v.started)
        return false;
    float s = fmaxf((float)GFX_W / v.w, (float)GFX_H / v.h_px);
    float w = v.w * s, h = v.h_px * s;
    float lit = 1 - dim;
    gfx_bind(v.tex);
    gfx_push(&(gfx_inst){GFX_W / 2, GFX_H / 2, w / 2, h / 2, 0, gfx_next_z(), gfx_rgba(lit, lit, lit, 1), GK_TEX, {0, 0, 1, 1}});
    gfx_bind(0);
    return true;
}

void video_close(void)
{
    if (v.h) {
        av.stop(v.h);
        av.close(v.h);
    }
    if (v.tex)
        gfx_texture_free(v.tex);
    free(v.rgba);
    memset(&v, 0, sizeof(v));
}
