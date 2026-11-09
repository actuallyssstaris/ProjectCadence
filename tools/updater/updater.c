/* Payload: finishes a ProjectCadence update. The game can't replace its own files
 * while it runs (and its sandbox won't let it touch them anyway), so it unpacks
 * the new build, writes /data/cadence/update-job.txt and sends this through the
 * ELF loader. We wait for the game to close, copy the new files in, and say so.
 *
 * update-job.txt: staged folder, install folder, the game's pid, the version. */
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define JOB "/data/cadence/update-job.txt"

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

static int copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    if (!in)
        return -1;
    FILE *out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    static char buf[256 * 1024];
    size_t n;
    int ok = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) {
            ok = -1;
            break;
        }
    fclose(in);
    if (fclose(out) != 0)
        ok = -1;
    return ok;
}

static int copy_tree(const char *from, const char *to)
{
    DIR *d = opendir(from);
    if (!d)
        return -1;
    mkdir(to, 0777);
    int bad = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char a[1024], b[1024];
        snprintf(a, sizeof(a), "%s/%s", from, e->d_name);
        snprintf(b, sizeof(b), "%s/%s", to, e->d_name);
        struct stat st;
        if (stat(a, &st) != 0)
            continue;
        if ((S_ISDIR(st.st_mode) ? copy_tree(a, b) : copy_file(a, b)) != 0)
            bad++;
    }
    closedir(d);
    return bad ? -1 : 0;
}

static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        remove_tree(sub);
    }
    closedir(d);
    rmdir(path);
}

static void line(FILE *f, char *out, size_t n)
{
    out[0] = 0;
    if (fgets(out, (int)n, f))
        out[strcspn(out, "\r\n")] = 0;
}

int main(void)
{
    FILE *f = fopen(JOB, "r");
    if (!f) {
        notify("ProjectCadence: no update waiting");
        return 1;
    }
    char from[512], to[512], pid_s[32], version[64];
    line(f, from, sizeof(from));
    line(f, to, sizeof(to));
    line(f, pid_s, sizeof(pid_s));
    line(f, version, sizeof(version));
    fclose(f);
    pid_t pid = (pid_t)atoi(pid_s);

    notify("ProjectCadence: close the game to finish the update");

    /* the game first, then a few seconds for the launcher to let go of the folder */
    for (int i = 0; i < 1800 && pid > 0 && kill(pid, 0) == 0; i++)
        sleep(1);
    sleep(12);

    int ok = -1;
    for (int tries = 0; tries < 30 && ok != 0; tries++) {
        ok = copy_tree(from, to);
        if (ok != 0)
            sleep(2);
    }

    char msg[160];
    if (ok == 0) {
        snprintf(msg, sizeof(msg), "ProjectCadence updated to %s, open it again", version);
        remove_tree(from);
        unlink(JOB);
    } else {
        snprintf(msg, sizeof(msg), "ProjectCadence: the update couldn't be copied in, it's left in %s", from);
    }
    notify(msg);
    return ok == 0 ? 0 : 1;
}
