/* PNG output via stb_image_write + small path helpers. */
#include "stb_image_write.h"

#include "mgfx.h"
#include "portable.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>

static int mkdir_p(char *path)
{
    char *p = path;
#if defined(_WIN32)
    size_t i;
#endif

    if (!path[0])
        return 0;
#if defined(_WIN32)
    for (i = 0; path[i]; i++) {
        if (path[i] == '\\')
            path[i] = '/';
    }
    if (isalpha((unsigned char)p[0]) && p[1] == ':')
        p += 2;
#endif
    if (*p == '/')
        p++;
    for (; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mgfx_mkdir(path) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mgfx_mkdir(path) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int mgfx_ensure_parent_dir(const char *path)
{
    char tmp[1024];
    char *slash;
    size_t n = strlen(path);
#if defined(_WIN32)
    size_t i;
#endif

    if (n + 1 > sizeof(tmp))
        return -1;
    memcpy(tmp, path, n + 1);
#if defined(_WIN32)
    for (i = 0; tmp[i]; i++) {
        if (tmp[i] == '\\')
            tmp[i] = '/';
    }
    if (isalpha((unsigned char)tmp[0]) && tmp[1] == ':' &&
        (tmp[2] == '\0' || (tmp[2] == '/' && tmp[3] == '\0')))
        return 0;
#endif
    slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return 0;
    *slash = '\0';
    return mkdir_p(tmp);
}

int mgfx_write_png(const char *path, int w, int h, const unsigned char *rgb)
{
    if (mgfx_ensure_parent_dir(path) != 0)
        return 0;
    return stbi_write_png(path, w, h, 3, rgb, w * 3) != 0;
}
