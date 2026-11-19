#ifndef MGFX_AVI_H
#define MGFX_AVI_H

#include <stddef.h>

typedef struct AviWriter AviWriter;

/* MJPEG AVI writer. quality is 1..100. Returns NULL on error. */
AviWriter *avi_open(const char *path, int width, int height, int fps,
                    int quality, char *err, size_t errsz);

/* Encode one RGB8 frame as JPEG and append it to the 'movi' list. */
int avi_add_rgb(AviWriter *w, const unsigned char *rgb,
                char *err, size_t errsz);

/* Write idx1, patch all sizes/counts, close and free. */
int avi_close(AviWriter *w, char *err, size_t errsz);

/* Close and remove a partial file. */
void avi_abort(AviWriter *w);

#endif /* MGFX_AVI_H */
