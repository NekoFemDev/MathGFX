/* PNG sequence -> MJPEG AVI. stb_image decodes, avi.c muxes. */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "mux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video.h"

int mgfx_mux_pngs(const char *out, int fps, int quality, int crf,
                  char **files, int nfiles, int *frames_out,
                  char *err, size_t errsz)
{
    VideoWriter *vw = NULL;
    int w = 0;
    int h = 0;
    int frames = 0;
    int i;

    if (nfiles <= 0) {
        snprintf(err, errsz, "mux: no input frames");
        return -1;
    }

    for (i = 0; i < nfiles; i++) {
        int iw = 0;
        int ih = 0;
        int n = 0;
        unsigned char *rgb = stbi_load(files[i], &iw, &ih, &n, 3);
        if (!rgb) {
            snprintf(err, errsz, "mux: cannot read '%s'", files[i]);
            goto fail;
        }
        if (!vw) {
            w = iw;
            h = ih;
            vw = video_writer_open(out, w, h, fps, quality, crf, err, errsz);
            if (!vw) {
                stbi_image_free(rgb);
                goto fail;
            }
        } else if (iw != w || ih != h) {
            snprintf(err, errsz, "mux: frame '%s' is %dx%d, expected %dx%d",
                     files[i], iw, ih, w, h);
            stbi_image_free(rgb);
            goto fail;
        }
        if (video_writer_add(vw, rgb, err, errsz) != 0) {
            stbi_image_free(rgb);
            goto fail;
        }
        stbi_image_free(rgb);
        frames++;
    }

    if (!vw) {
        snprintf(err, errsz, "mux: no frames");
        return -1;
    }
    if (video_writer_close(vw, err, errsz) != 0) {
        /* writer already freed the backend and removed the file */
        return -1;
    }
    if (frames_out)
        *frames_out = frames;
    return 0;

fail:
    if (vw)
        video_writer_abort(vw);
    return -1;
}
