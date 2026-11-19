#ifndef MGFX_PORTABLE_H
#define MGFX_PORTABLE_H

/* Small POSIX/Windows shims used by the CPU and Vulkan hosts. */
double mgfx_now_sec(void);
int mgfx_cpu_count(void);
int mgfx_mkdir(const char *path);

#endif /* MGFX_PORTABLE_H */
