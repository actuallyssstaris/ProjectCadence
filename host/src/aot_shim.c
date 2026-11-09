/* Names the .NET runtime (built for linux-x64) asks for that neither the PS5 libc
 * nor SharpProspero's compat object provide. Same story as core/prx/shim.c: they
 * come from /proc and cgroup probing, which has nothing to read here. */
#include <stdlib.h>

int Need_sceLibc; /* compat's libc marker points at this */

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
