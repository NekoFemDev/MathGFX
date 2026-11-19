#include "video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avi.h"
#include "mp4.h"

struct VideoWriter {
    int kind; /* 0 = AVI/MJPEG, 1 = MP4/H.264 */
    void *impl;
};

static int has_suffix(const char *s, const char *suffix)
{
    size_t n = strlen(s);
    size_t m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

VideoWriter *video_writer_open(const char *path, int width, int height,
                               int fps, int quality, int crf,
                               char *err, size_t errsz)
{
    VideoWriter *v = (VideoWriter *)calloc(1, sizeof(*v));
    if (!v) {
        snprintf(err, errsz, "video: out of memory");
        return NULL;
    }
    if (has_suffix(path, ".mp4") || has_suffix(path, ".MP4")) {
        v->kind = 1;
        v->impl = mp4_open(path, width, height, fps, crf, err, errsz);
    } else {
        v->kind = 0;
        v->impl = avi_open(path, width, height, fps, quality, err, errsz);
    }
    if (!v->impl) {
        free(v);
        return NULL;
    }
    return v;
}

int video_writer_add(VideoWriter *w, const unsigned char *rgb,
                     char *err, size_t errsz)
{
    if (!w || !w->impl) {
        snprintf(err, errsz, "video: writer is closed");
        return -1;
    }
    if (w->kind == 1)
        return mp4_add_rgb((Mp4Writer *)w->impl, rgb, err, errsz);
    return avi_add_rgb((AviWriter *)w->impl, rgb, err, errsz);
}

int video_writer_close(VideoWriter *w, char *err, size_t errsz)
{
    int rc;
    if (!w)
        return -1;
    if (w->kind == 1)
        rc = mp4_close((Mp4Writer *)w->impl, err, errsz);
    else
        rc = avi_close((AviWriter *)w->impl, err, errsz);
    free(w);
    return rc;
}

void video_writer_abort(VideoWriter *w)
{
    if (!w)
        return;
    if (w->kind == 1)
        mp4_abort((Mp4Writer *)w->impl);
    else
        avi_abort((AviWriter *)w->impl);
    free(w);
}
