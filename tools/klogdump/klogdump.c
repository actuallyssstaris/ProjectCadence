/* Payload: keep a copy of the kernel message buffer in /data/cadence/klog.txt for
 * three minutes. Reads kern.msgbuf (what dmesg reads), so it works while
 * something else holds /dev/klog. */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

int sceKernelSendNotificationRequest(int, void *, size_t, int);

static void notify(const char *msg)
{
    struct {
        char pad[0x2d];
        char msg[3075];
    } req;
    memset(&req, 0, sizeof(req));
    strncpy(req.msg, msg, sizeof(req.msg) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

int main(void)
{
    size_t cap = 0;
    if (sysctlbyname("kern.msgbuf", NULL, &cap, NULL, 0) != 0 || cap == 0) {
        notify("klogdump: no kern.msgbuf");
        return 1;
    }
    cap += 4096;
    char *buf = malloc(cap);
    if (!buf)
        return 1;

    notify("klogdump: recording 3 min");
    time_t end = time(NULL) + 180;
    while (time(NULL) < end) {
        size_t n = cap;
        if (sysctlbyname("kern.msgbuf", buf, &n, NULL, 0) == 0) {
            int out = open("/data/cadence/klog.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (out >= 0) {
                write(out, buf, n);
                close(out);
            }
        }
        sleep(1);
    }
    free(buf);
    notify("klogdump: done");
    return 0;
}
