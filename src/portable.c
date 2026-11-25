#include "portable.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <time.h>
#  include <unistd.h>
#endif

double mgfx_now_sec(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    static int init;
    LARGE_INTEGER counter;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        init = 1;
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static int cpu_count_from_proc(void)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f)
        return 0;
    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "processor\t:", 11) == 0 ||
            strncmp(line, "processor :", 11) == 0)
            count++;
    }
    fclose(f);
    return count > 0 ? count : 0;
}

static int cpu_count_from_nproc(void)
{
    FILE *f = popen("nproc 2>/dev/null", "r");
    if (!f)
        return 0;
    char buf[32];
    int n = 0;
    if (fgets(buf, sizeof(buf), f))
        n = atoi(buf);
    pclose(f);
    return n > 0 ? n : 0;
}

int mgfx_cpu_count(void)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
#else
    /* Try sysconf first (standard POSIX) */
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0)
        return (int)n;

    /* Fallback 1: /proc/cpuinfo (Linux) */
    n = cpu_count_from_proc();
    if (n > 0)
        return n;

    /* Fallback 2: nproc command (most Linux/BSD) */
    n = cpu_count_from_nproc();
    if (n > 0)
        return n;

    /* Final fallback */
    return 1;
#endif
}

int mgfx_mkdir(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}
