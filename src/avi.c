/*
 * Minimal MJPEG AVI muxer.
 *
 * Container layout:
 *   RIFF 'AVI '
 *     LIST 'hdrl'
 *       'avih'  main header
 *       LIST 'strl'
 *         'strh' video stream header (vids/MJPG)
 *         'strf' BITMAPINFOHEADER (24-bit MJPG)
 *     LIST 'movi'
 *       '00dc' JPEG frame chunks
 *     'idx1' index
 *
 * Every frame is encoded by stb_image_write's JPEG encoder. No ffmpeg, no
 * external libraries beyond stb and libc.
 */
#include "avi.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image_write.h"

typedef struct {
    uint32_t offset;   /* relative to the 'movi' FourCC */
    uint32_t size;     /* unpadded JPEG size */
} AviIndex;

struct AviWriter {
    FILE *f;
    char *path;
    int w;
    int h;
    int fps;
    int quality;
    int failed;
    int opened;
    long riff_size_pos;
    long hdrl_size_pos;
    long strl_size_pos;
    long avih_total_pos;
    long avih_maxbytes_pos;
    long avih_suggest_pos;
    long strh_len_pos;
    long strh_suggest_pos;
    long movi_size_pos;
    long movi_fourcc_pos;
    AviIndex *idx;
    size_t nframes;
    size_t idx_cap;
    uint32_t max_frame;
};

static void set_err(char *err, size_t errsz, const char *fmt, const char *arg)
{
    if (!err || errsz == 0)
        return;
    if (arg)
        snprintf(err, errsz, fmt, arg);
    else
        snprintf(err, errsz, "%s", fmt);
}

static int w_bytes(AviWriter *a, const void *p, size_t n)
{
    if (a->failed)
        return -1;
    if (n && fwrite(p, 1, n, a->f) != n) {
        a->failed = 1;
        return -1;
    }
    return 0;
}

static int w_u32(AviWriter *a, uint32_t v)
{
    unsigned char b[4];
    b[0] = (unsigned char)(v & 255u);
    b[1] = (unsigned char)((v >> 8) & 255u);
    b[2] = (unsigned char)((v >> 16) & 255u);
    b[3] = (unsigned char)((v >> 24) & 255u);
    return w_bytes(a, b, 4);
}

static int w_u16(AviWriter *a, uint16_t v)
{
    unsigned char b[2];
    b[0] = (unsigned char)(v & 255u);
    b[1] = (unsigned char)((v >> 8) & 255u);
    return w_bytes(a, b, 2);
}

static int patch_u32(AviWriter *a, long pos, uint32_t v)
{
    long cur;
    if (a->failed)
        return -1;
    cur = ftell(a->f);
    if (cur < 0 || fseek(a->f, pos, SEEK_SET) != 0) {
        a->failed = 1;
        return -1;
    }
    if (w_u32(a, v) != 0)
        return -1;
    if (fseek(a->f, cur, SEEK_SET) != 0) {
        a->failed = 1;
        return -1;
    }
    return 0;
}

typedef struct {
    unsigned char *p;
    size_t n;
    size_t cap;
    int failed;
} JpegBuf;

static void jpeg_cb(void *ctx, void *data, int size)
{
    JpegBuf *b = (JpegBuf *)ctx;
    if (b->failed || size < 0)
        return;
    if (b->n + (size_t)size > b->cap) {
        size_t cap = b->cap ? b->cap : 65536u;
        unsigned char *np;
        while (cap < b->n + (size_t)size)
            cap *= 2;
        np = (unsigned char *)realloc(b->p, cap);
        if (!np) {
            b->failed = 1;
            return;
        }
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->n, data, (size_t)size);
    b->n += (size_t)size;
}

void avi_abort(AviWriter *a)
{
    if (!a)
        return;
    if (a->f)
        fclose(a->f);
    if (a->path) {
        if (a->opened)
            remove(a->path);
        free(a->path);
    }
    free(a->idx);
    free(a);
}

AviWriter *avi_open(const char *path, int width, int height, int fps,
                    int quality, char *err, size_t errsz)
{
    AviWriter *a;
    long hdrl_start, strl_start, here;

    if (width <= 0 || height <= 0 || width > 65535 || height > 65535) {
        set_err(err, errsz, "avi: bad frame size", NULL);
        return NULL;
    }
    if (fps <= 0 || fps > 1000000) {
        set_err(err, errsz, "avi: bad fps", NULL);
        return NULL;
    }
    if (quality < 1 || quality > 100) {
        set_err(err, errsz, "avi: quality must be 1..100", NULL);
        return NULL;
    }

    a = (AviWriter *)calloc(1, sizeof(*a));
    if (!a) {
        set_err(err, errsz, "avi: out of memory", NULL);
        return NULL;
    }
    a->w = width;
    a->h = height;
    a->fps = fps;
    a->quality = quality;
    a->path = strdup(path);
    if (!a->path) {
        set_err(err, errsz, "avi: out of memory", NULL);
        avi_abort(a);
        return NULL;
    }
    a->f = fopen(path, "wb+");
    if (a->f)
        a->opened = 1;
    if (!a->f) {
        set_err(err, errsz, errno ? strerror(errno) : "avi: cannot open", NULL);
        avi_abort(a);
        return NULL;
    }

    if (w_bytes(a, "RIFF", 4) ||
        (a->riff_size_pos = ftell(a->f)) < 0 || w_u32(a, 0) ||
        w_bytes(a, "AVI ", 4))
        goto fail;

    hdrl_start = ftell(a->f);
    if (hdrl_start < 0 || w_bytes(a, "LIST", 4) ||
        (a->hdrl_size_pos = ftell(a->f)) < 0 || w_u32(a, 0) ||
        w_bytes(a, "hdrl", 4))
        goto fail;

    if (w_bytes(a, "avih", 4) || w_u32(a, 56) ||
        w_u32(a, (uint32_t)(1000000 / fps)) ||
        (a->avih_maxbytes_pos = ftell(a->f)) < 0 || w_u32(a, 0) ||
        w_u32(a, 0) ||
        w_u32(a, 0x10u) ||                   /* AVIF_HASINDEX */
        ((a->avih_total_pos = ftell(a->f)) < 0) || w_u32(a, 0) ||
        w_u32(a, 0) ||
        w_u32(a, 1) ||                       /* one stream */
        ((a->avih_suggest_pos = ftell(a->f)) < 0) || w_u32(a, 0) ||
        w_u32(a, (uint32_t)width) || w_u32(a, (uint32_t)height) ||
        w_u32(a, 0) || w_u32(a, 0) || w_u32(a, 0) || w_u32(a, 0))
        goto fail;

    strl_start = ftell(a->f);
    if (strl_start < 0 || w_bytes(a, "LIST", 4) ||
        (a->strl_size_pos = ftell(a->f)) < 0 || w_u32(a, 0) ||
        w_bytes(a, "strl", 4))
        goto fail;

    if (w_bytes(a, "strh", 4) || w_u32(a, 56) ||
        w_bytes(a, "vids", 4) || w_bytes(a, "MJPG", 4) ||
        w_u32(a, 0) ||
        w_u16(a, 0) || w_u16(a, 0) ||
        w_u32(a, 0) ||                       /* initial frames */
        w_u32(a, 1) || w_u32(a, (uint32_t)fps) || w_u32(a, 0) ||
        ((a->strh_len_pos = ftell(a->f)) < 0) || w_u32(a, 0) ||
        ((a->strh_suggest_pos = ftell(a->f)) < 0) || w_u32(a, 0) ||
        w_u32(a, 0xFFFFFFFFu) ||             /* default quality */
        w_u32(a, 0) ||                       /* sample size */
        w_u16(a, 0) || w_u16(a, 0) ||
        w_u16(a, (uint16_t)width) || w_u16(a, (uint16_t)height))
        goto fail;

    if (w_bytes(a, "strf", 4) || w_u32(a, 40) ||
        w_u32(a, 40) ||
        w_u32(a, (uint32_t)width) ||
        w_u32(a, (uint32_t)(-(int32_t)height)) || /* top-down */
        w_u16(a, 1) || w_u16(a, 24) ||
        w_bytes(a, "MJPG", 4) ||
        w_u32(a, 0) || w_u32(a, 0) || w_u32(a, 0) ||
        w_u32(a, 0) || w_u32(a, 0))
        goto fail;

    here = ftell(a->f);
    if (here < 0 ||
        patch_u32(a, a->hdrl_size_pos, (uint32_t)(here - (hdrl_start + 8))) ||
        patch_u32(a, a->strl_size_pos, (uint32_t)(here - (strl_start + 8))))
        goto fail;

    if (w_bytes(a, "LIST", 4) ||
        (a->movi_size_pos = ftell(a->f)) < 0 || w_u32(a, 0) ||
        w_bytes(a, "movi", 4) ||
        (a->movi_fourcc_pos = ftell(a->f) - 4) < 0)
        goto fail;

    set_err(err, errsz, "", NULL);
    return a;

fail:
    set_err(err, errsz, "avi: cannot write header", NULL);
    avi_abort(a);
    return NULL;
}

int avi_add_rgb(AviWriter *a, const unsigned char *rgb,
                char *err, size_t errsz)
{
    JpegBuf b;
    long chunk_pos;

    if (!a || a->failed) {
        set_err(err, errsz, "avi: writer is closed", NULL);
        return -1;
    }

    memset(&b, 0, sizeof(b));
    if (!stbi_write_jpg_to_func(jpeg_cb, &b, a->w, a->h, 3, rgb, a->quality) ||
        b.failed || b.n == 0) {
        free(b.p);
        set_err(err, errsz, "avi: JPEG encode failed", NULL);
        return -1;
    }
    if (b.n > 0xFFFFFFFFu) {
        free(b.p);
        set_err(err, errsz, "avi: frame too large", NULL);
        return -1;
    }

    chunk_pos = ftell(a->f);
    if (chunk_pos < 0 ||
        w_bytes(a, "00dc", 4) ||
        w_u32(a, (uint32_t)b.n) ||
        w_bytes(a, b.p, b.n) ||
        ((b.n & 1u) && w_bytes(a, "\0", 1)))
        goto fail;

    if (a->nframes == a->idx_cap) {
        size_t cap = a->idx_cap ? a->idx_cap * 2 : 64;
        AviIndex *ni = (AviIndex *)realloc(a->idx, cap * sizeof(*ni));
        if (!ni)
            goto fail;
        a->idx = ni;
        a->idx_cap = cap;
    }

    if ((uint64_t)(chunk_pos - a->movi_fourcc_pos) > 0xFFFFFFFFu)
        goto fail;
    a->idx[a->nframes].offset = (uint32_t)(chunk_pos - a->movi_fourcc_pos);
    a->idx[a->nframes].size = (uint32_t)b.n;
    a->nframes++;
    if ((uint32_t)b.n > a->max_frame)
        a->max_frame = (uint32_t)b.n;

    free(b.p);
    return 0;

fail:
    free(b.p);
    a->failed = 1;
    set_err(err, errsz, "avi: cannot write frame", NULL);
    return -1;
}

int avi_close(AviWriter *a, char *err, size_t errsz)
{
    long movi_end, end;
    uint32_t i;
    int rc = 0;

    if (!a)
        return -1;
    if (a->failed)
        rc = -1;

    movi_end = ftell(a->f);
    if (movi_end < 0) {
        rc = -1;
    } else if ((uint64_t)(movi_end - a->movi_fourcc_pos) > 0xFFFFFFFFu) {
        rc = -1;
    } else if (patch_u32(a, a->movi_size_pos,
                         (uint32_t)(movi_end - a->movi_fourcc_pos))) {
        rc = -1;
    }

    if (rc == 0) {
        if (w_bytes(a, "idx1", 4) ||
            w_u32(a, (uint32_t)(a->nframes * 16u)))
            rc = -1;
        for (i = 0; rc == 0 && i < a->nframes; i++) {
            if (w_bytes(a, "00dc", 4) ||
                w_u32(a, 0x10u) ||                /* AVIIF_KEYFRAME */
                w_u32(a, a->idx[i].offset) ||
                w_u32(a, a->idx[i].size))
                rc = -1;
        }
    }

    end = ftell(a->f);
    if (rc == 0 && (end < 0 || (uint64_t)(end - 8) > 0xFFFFFFFFu)) {
        rc = -1;
    } else if (rc == 0 &&
               (patch_u32(a, a->riff_size_pos, (uint32_t)(end - 8)) ||
                patch_u32(a, a->avih_total_pos, (uint32_t)a->nframes) ||
                patch_u32(a, a->strh_len_pos, (uint32_t)a->nframes) ||
                patch_u32(a, a->avih_suggest_pos, a->max_frame) ||
                patch_u32(a, a->strh_suggest_pos, a->max_frame) ||
                patch_u32(a, a->avih_maxbytes_pos,
                          a->max_frame > 0xFFFFFFFFu / (uint32_t)a->fps
                              ? 0xFFFFFFFFu
                              : a->max_frame * (uint32_t)a->fps))) {
        rc = -1;
    }

    if (a->failed)
        rc = -1;
    if (fflush(a->f) != 0)
        rc = -1;
    if (fclose(a->f) != 0)
        rc = -1;
    a->f = NULL;

    if (rc != 0) {
        set_err(err, errsz, "avi: cannot finalize file", NULL);
        remove(a->path);
    } else {
        set_err(err, errsz, "", NULL);
    }

    free(a->idx);
    free(a->path);
    free(a);
    return rc;
}
