/* The payload SDK only ships stubs for these, so the prototypes are declared here.
 * Layouts are the ones the PS4 homebrew SDKs have used for years; the PS5 libs kept them. */
#ifndef SCE_H
#define SCE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t priority;
} SceUserServiceInitializeParams;

int sceUserServiceInitialize(SceUserServiceInitializeParams *params);
int sceUserServiceGetInitialUser(int32_t *user);

/* pad */
enum {
    PAD_L3 = 0x2,
    PAD_R3 = 0x4,
    PAD_OPTIONS = 0x8,
    PAD_UP = 0x10,
    PAD_RIGHT = 0x20,
    PAD_DOWN = 0x40,
    PAD_LEFT = 0x80,
    PAD_L2 = 0x100,
    PAD_R2 = 0x200,
    PAD_L1 = 0x400,
    PAD_R1 = 0x800,
    PAD_TRIANGLE = 0x1000,
    PAD_CIRCLE = 0x2000,
    PAD_CROSS = 0x4000,
    PAD_SQUARE = 0x8000,
    PAD_TOUCHPAD = 0x100000,
};

typedef struct {
    uint16_t x, y;
    uint8_t id;
    uint8_t pad[3];
} ScePadTouch;

typedef struct {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;
    uint8_t l2, r2;
    uint8_t pad0[2];
    float quat[4];
    float accel[3];
    float gyro[3];
    uint8_t touch_count;
    uint8_t pad1[3];
    uint32_t pad2;
    ScePadTouch touch[2];
    uint8_t connected;
    uint8_t pad3[3];
    uint64_t timestamp;
    uint8_t rest[64]; /* ext unit data etc, the PS5 lib writes a bit more than the PS4 one */
} ScePadData;

int scePadInit(void);
int scePadOpen(int32_t user, int32_t type, int32_t index, const void *param);
int scePadReadState(int32_t handle, ScePadData *data);
int scePadClose(int32_t handle);

/* audio out */
enum {
    AUDIO_PORT_MAIN = 0,
    AUDIO_FORMAT_S16_STEREO = 1,
};

int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t len, uint32_t freq, uint32_t param);
int sceAudioOutOutput(int32_t handle, const void *ptr);
int sceAudioOutSetVolume(int32_t handle, int32_t flag, int32_t *vol);
int sceAudioOutClose(int32_t handle);

/* notification popup, handy for errors before the renderer is up */
typedef struct {
    char useless1[45];
    char message[3075];
} OrbisNotificationRequest;

int sceKernelSendNotificationRequest(int device, OrbisNotificationRequest *req, size_t size, int blocking);
int sceKernelUsleep(unsigned int usec);

#endif
