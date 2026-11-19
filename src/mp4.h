#ifndef MGFX_MP4_H
#define MGFX_MP4_H

#include <stddef.h>

typedef struct Mp4Writer Mp4Writer;

/* H.264/MP4 writer (libx264 + a minimal ISO BMFF muxer). crf 0..51. */
Mp4Writer *mp4_open(const char *path, int width, int height, int fps,
                    int crf, char *err, size_t errsz);
int mp4_add_rgb(Mp4Writer *w, const unsigned char *rgb,
                char *err, size_t errsz);
int mp4_close(Mp4Writer *w, char *err, size_t errsz);
void mp4_abort(Mp4Writer *w);

#endif /* MGFX_MP4_H */
