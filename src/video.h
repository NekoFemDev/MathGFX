#ifndef MGFX_VIDEO_H
#define MGFX_VIDEO_H

#include <stddef.h>

typedef struct VideoWriter VideoWriter;

/* Chooses AVI/MJPEG for .avi, H.264/MP4 for .mp4. */
VideoWriter *video_writer_open(const char *path, int width, int height,
                               int fps, int quality, int crf,
                               char *err, size_t errsz);
int video_writer_add(VideoWriter *w, const unsigned char *rgb,
                     char *err, size_t errsz);
int video_writer_close(VideoWriter *w, char *err, size_t errsz);
void video_writer_abort(VideoWriter *w);

#endif /* MGFX_VIDEO_H */
