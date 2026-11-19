#ifndef MGFX_MUX_H
#define MGFX_MUX_H

#include <stddef.h>

/* Read PNG frames and write an MJPEG AVI. Returns 0/-1. */
int mgfx_mux_pngs(const char *out, int fps, int quality, int crf,
                  char **files, int nfiles, int *frames_out,
                  char *err, size_t errsz);

#endif /* MGFX_MUX_H */
