/* glibc names the .NET 10.0.x runtime pack wants that SharpProspero's compat layer
 * doesn't cover yet. All of them come from /proc and cgroup probing, which has
 * nothing to read on the console, so failing quietly is the right answer. */
#include <stddef.h>

void *__dso_handle __attribute__((visibility("hidden"))) = &__dso_handle;

void *calloc(size_t n, size_t size);
void free(void *p);

/* SharpProspero imports the stdio globals from libc.prx, but the libc.prx we ship
 * (PS5_Vulkan's) only has them as _Stdin/_Stdout/_Stderr, and one unresolved
 * import is enough for the loader to refuse the whole module (0x80020016).
 * Nothing outside the runtime's fatal paths writes to them, so NULL will do. */
void *__stdinp, *__stdoutp, *__stderrp;

/* same story on the kernel side: libkernel.prx exports neither of these */
char *getcwd(char *buf, size_t size)
{
    static const char app[] = "/app0";
    if (!buf || size < sizeof(app))
        return NULL;
    for (size_t i = 0; i < sizeof(app); i++)
        buf[i] = app[i];
    return buf;
}

void syslog(int priority, const char *fmt, ...)
{
    (void)priority;
    (void)fmt;
}

int fgetc(void *stream)
{
    (void)stream;
    return -1;
}

int __isoc99_fscanf(void *stream, const char *fmt, ...)
{
    (void)stream;
    (void)fmt;
    return -1;
}

void *__sched_cpualloc(size_t count)
{
    return calloc((count + 63) / 64, sizeof(unsigned long));
}

void __sched_cpufree(void *set)
{
    free(set);
}

/* sceKernelLoadStartModule calls these. The runtime itself comes up from
 * DT_INIT/init_array before module_start; if it didn't, say so through the
 * start result instead of crashing on the first call. */
static int ctors_ran;

__attribute__((constructor)) static void mark_ctors(void)
{
    ctors_ran = 1;
}

int module_start(size_t argc, const void *argv)
{
    (void)argc;
    (void)argv;
    return ctors_ran ? 0 : 0x5e1f;
}

int module_stop(size_t argc, const void *argv)
{
    (void)argc;
    (void)argv;
    return 0;
}
