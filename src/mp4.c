/*
 * H.264 in MP4: libx264 encoder + a minimal ISO BMFF muxer.
 *
 * One video track, no audio, one chunk, AVCC sample entries (avc1/avcC),
 * stts/stsc/stsz/stco/stss. The whole sample buffer is kept in memory
 * (fine for short procedural clips), then ftyp + mdat + moov is written.
 */
#include "mp4.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <x264.h>

typedef struct {
    unsigned char *p;
    size_t n;
    size_t cap;
    int failed;
} Buf;

typedef struct {
    uint32_t size;
    int key;
} Sample;

struct Mp4Writer {
    char *path;
    int w;
    int h;
    int fps;
    int crf;
    x264_t *enc;
    x264_picture_t pic;
    Buf mdat;
    Sample *samples;
    size_t nsamples;
    size_t sample_cap;
    unsigned char *sps;
    unsigned char *pps;
    int sps_len;
    int pps_len;
    int failed;
    int pic_alloc;
    int64_t input_frames;
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

static int buf_grow(Buf *b, size_t add)
{
    size_t need;
    unsigned char *np;
    if (b->failed)
        return -1;
    if (add > (size_t)-1 - b->n) {
        b->failed = 1;
        return -1;
    }
    need = b->n + add;
    if (need <= b->cap)
        return 0;
    {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < need) {
            if (cap > (size_t)-1 / 2) {
                b->failed = 1;
                return -1;
            }
            cap *= 2;
        }
        np = (unsigned char *)realloc(b->p, cap);
        if (!np) {
            b->failed = 1;
            return -1;
        }
        b->p = np;
        b->cap = cap;
    }
    return 0;
}

static void put_u8(Buf *b, unsigned v)
{
    if (buf_grow(b, 1))
        return;
    b->p[b->n++] = (unsigned char)(v & 255u);
}

static void put_u16(Buf *b, unsigned v)
{
    if (buf_grow(b, 2))
        return;
    b->p[b->n++] = (unsigned char)((v >> 8) & 255u);
    b->p[b->n++] = (unsigned char)(v & 255u);
}

static void put_u32(Buf *b, uint32_t v)
{
    if (buf_grow(b, 4))
        return;
    b->p[b->n++] = (unsigned char)((v >> 24) & 255u);
    b->p[b->n++] = (unsigned char)((v >> 16) & 255u);
    b->p[b->n++] = (unsigned char)((v >> 8) & 255u);
    b->p[b->n++] = (unsigned char)(v & 255u);
}

static void put_data(Buf *b, const void *p, size_t n)
{
    if (buf_grow(b, n))
        return;
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

static void put_fourcc(Buf *b, const char *s)
{
    put_data(b, s, 4);
}

static size_t box_begin(Buf *b, const char *type)
{
    size_t pos = b->n;
    put_u32(b, 0);
    put_fourcc(b, type);
    return pos;
}

static void box_end(Buf *b, size_t pos)
{
    size_t size;
    uint32_t v;
    if (b->failed)
        return;
    size = b->n - pos;
    if (size > 0xFFFFFFFFu) {
        b->failed = 1;
        return;
    }
    v = (uint32_t)size;
    b->p[pos + 0] = (unsigned char)(v >> 24);
    b->p[pos + 1] = (unsigned char)(v >> 16);
    b->p[pos + 2] = (unsigned char)(v >> 8);
    b->p[pos + 3] = (unsigned char)(v);
}

static void put_matrix(Buf *b)
{
    put_u32(b, 0x00010000); put_u32(b, 0); put_u32(b, 0);
    put_u32(b, 0); put_u32(b, 0x00010000); put_u32(b, 0);
    put_u32(b, 0); put_u32(b, 0); put_u32(b, 0x40000000);
}

static unsigned char clamp_u8(int v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (unsigned char)v;
}

/* BT.601 limited range, 4:2:0. Width and height must be even. */
static void rgb_to_i420(const unsigned char *rgb, int w, int h, x264_picture_t *pic)
{
    int x, y;
    for (y = 0; y < h; y++) {
        unsigned char *yp = pic->img.plane[0] + (size_t)y * pic->img.i_stride[0];
        const unsigned char *row = rgb + (size_t)y * w * 3u;
        for (x = 0; x < w; x++) {
            int r = row[x * 3 + 0];
            int g = row[x * 3 + 1];
            int b = row[x * 3 + 2];
            yp[x] = clamp_u8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    for (y = 0; y < h; y += 2) {
        unsigned char *up = pic->img.plane[1] + (size_t)(y / 2) * pic->img.i_stride[1];
        unsigned char *vp = pic->img.plane[2] + (size_t)(y / 2) * pic->img.i_stride[2];
        for (x = 0; x < w; x += 2) {
            int r = 0, g = 0, b = 0;
            int dy, dx;
            for (dy = 0; dy < 2; dy++) {
                const unsigned char *row = rgb + (size_t)(y + dy) * w * 3u;
                for (dx = 0; dx < 2; dx++) {
                    r += row[(x + dx) * 3 + 0];
                    g += row[(x + dx) * 3 + 1];
                    b += row[(x + dx) * 3 + 2];
                }
            }
            r >>= 2;
            g >>= 2;
            b >>= 2;
            up[x / 2] = clamp_u8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            vp[x / 2] = clamp_u8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

static int copy_nal(unsigned char **dst, int *dst_len,
                    const unsigned char *p, int len)
{
    unsigned char *q;
    if (*dst)
        return 0;
    q = (unsigned char *)malloc((size_t)len);
    if (!q)
        return -1;
    memcpy(q, p, (size_t)len);
    *dst = q;
    *dst_len = len;
    return 0;
}

static int process_nals(Mp4Writer *w, x264_nal_t *nals, int count, int add_sample)
{
    Buf s;
    int i;
    int key = 0;
    memset(&s, 0, sizeof(s));

    for (i = 0; i < count; i++) {
        unsigned char *p = nals[i].p_payload;
        int len = nals[i].i_payload;
        int type;

        if (len >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
            p += 4;
            len -= 4;
        } else if (len >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) {
            p += 3;
            len -= 3;
        }
        if (len <= 0)
            continue;
        type = p[0] & 0x1f;

        if (type == 7) {
            if (copy_nal(&w->sps, &w->sps_len, p, len) != 0)
                goto fail;
            continue;
        }
        if (type == 8) {
            if (copy_nal(&w->pps, &w->pps_len, p, len) != 0)
                goto fail;
            continue;
        }
        if (type == 9)          /* access unit delimiter */
            continue;
        if (!add_sample)
            continue;

        put_u32(&s, (uint32_t)len);
        put_data(&s, p, (size_t)len);
        if (type == 5)
            key = 1;
        if (s.failed)
            goto fail;
    }

    if (add_sample && s.n > 0) {
        if (w->nsamples == w->sample_cap) {
            size_t cap = w->sample_cap ? w->sample_cap * 2 : 64;
            Sample *ns = (Sample *)realloc(w->samples, cap * sizeof(*ns));
            if (!ns)
                goto fail;
            w->samples = ns;
            w->sample_cap = cap;
        }
        if (buf_grow(&w->mdat, s.n) != 0)
            goto fail;
        memcpy(w->mdat.p + w->mdat.n, s.p, s.n);
        w->mdat.n += s.n;
        w->samples[w->nsamples].size = (uint32_t)s.n;
        w->samples[w->nsamples].key = key;
        w->nsamples++;
    }

    free(s.p);
    return 0;

fail:
    free(s.p);
    w->failed = 1;
    return -1;
}

static int capture_headers(Mp4Writer *w, x264_nal_t *nals, int count)
{
    return process_nals(w, nals, count, 0);
}

Mp4Writer *mp4_open(const char *path, int width, int height, int fps,
                    int crf, char *err, size_t errsz)
{
    Mp4Writer *w;
    x264_param_t param;
    x264_nal_t *nals = NULL;
    int nnals = 0;

    if (width <= 0 || height <= 0 || width > 65535 || height > 65535 ||
        (width & 1) || (height & 1)) {
        set_err(err, errsz, "mp4: width and height must be even and <= 65535", NULL);
        return NULL;
    }
    if (fps <= 0 || fps > 240) {
        set_err(err, errsz, "mp4: bad fps", NULL);
        return NULL;
    }
    if (crf < 0 || crf > 51) {
        set_err(err, errsz, "mp4: crf must be 0..51", NULL);
        return NULL;
    }

    w = (Mp4Writer *)calloc(1, sizeof(*w));
    if (!w) {
        set_err(err, errsz, "mp4: out of memory", NULL);
        return NULL;
    }
    w->w = width;
    w->h = height;
    w->fps = fps;
    w->crf = crf;
    w->path = strdup(path);
    if (!w->path) {
        set_err(err, errsz, "mp4: out of memory", NULL);
        mp4_abort(w);
        return NULL;
    }

    x264_param_default_preset(&param, "veryfast", "zerolatency");
    if (x264_param_apply_profile(&param, "main") < 0) {
        set_err(err, errsz, "mp4: x264 profile failed", NULL);
        mp4_abort(w);
        return NULL;
    }
    param.i_csp = X264_CSP_I420;
    param.i_width = width;
    param.i_height = height;
    param.i_fps_num = (uint32_t)fps;
    param.i_fps_den = 1;
    param.i_keyint_max = fps * 2;
    param.i_keyint_min = 1;
    param.b_annexb = 1;
    param.b_repeat_headers = 0;
    param.b_aud = 0;
    param.i_log_level = X264_LOG_ERROR;
    param.rc.i_rc_method = X264_RC_CRF;
    param.rc.f_rf_constant = (float)crf;
    param.vui.b_fullrange = 0;
    param.vui.i_colorprim = 5;   /* BT.601 */
    param.vui.i_transfer = 5;
    param.vui.i_colmatrix = 5;
    param.i_threads = 0;

    w->enc = x264_encoder_open(&param);
    if (!w->enc) {
        set_err(err, errsz, "mp4: x264_encoder_open failed", NULL);
        mp4_abort(w);
        return NULL;
    }
    if (x264_picture_alloc(&w->pic, X264_CSP_I420, width, height) < 0) {
        set_err(err, errsz, "mp4: x264_picture_alloc failed", NULL);
        mp4_abort(w);
        return NULL;
    }
    w->pic_alloc = 1;
    if (x264_encoder_headers(w->enc, &nals, &nnals) < 0 ||
        capture_headers(w, nals, nnals) != 0) {
        set_err(err, errsz, "mp4: x264 headers failed", NULL);
        mp4_abort(w);
        return NULL;
    }
    return w;
}

int mp4_add_rgb(Mp4Writer *w, const unsigned char *rgb,
                char *err, size_t errsz)
{
    x264_nal_t *nals = NULL;
    x264_picture_t out;
    int nnals = 0;
    int rc;

    if (!w || w->failed) {
        set_err(err, errsz, "mp4: writer is closed", NULL);
        return -1;
    }

    rgb_to_i420(rgb, w->w, w->h, &w->pic);
    w->pic.i_pts = w->input_frames++;
    memset(&out, 0, sizeof(out));
    rc = x264_encoder_encode(w->enc, &nals, &nnals, &w->pic, &out);
    if (rc < 0) {
        w->failed = 1;
        set_err(err, errsz, "mp4: x264_encoder_encode failed", NULL);
        return -1;
    }
    if (rc > 0 && process_nals(w, nals, nnals, 1) != 0) {
        set_err(err, errsz, "mp4: cannot store encoded frame", NULL);
        return -1;
    }
    return 0;
}

static int build_mp4(Mp4Writer *w, char *err, size_t errsz)
{
    Buf ftyp;
    Buf moov;
    uint32_t duration_ms;
    uint32_t chunk_offset;
    size_t stco_field_pos = 0;
    size_t p, trak, tkhd, mdia, mdhd, hdlr, minf, vmhd, dinf, dref, url;
    size_t stbl, stsd, avc1, avcc, stts, stsc, stsz, stco, stss;
    static const unsigned char handler_name[16] = {
        'V','i','d','e','o','H','a','n','d','l','e','r',0,0,0,0
    };
    size_t i;
    FILE *f = NULL;
    int rc = -1;

    memset(&ftyp, 0, sizeof(ftyp));
    memset(&moov, 0, sizeof(moov));

    if (!w->sps || !w->pps || w->sps_len < 4 || w->pps_len <= 0) {
        set_err(err, errsz, "mp4: missing SPS/PPS", NULL);
        return -1;
    }
    if (w->nsamples == 0) {
        set_err(err, errsz, "mp4: no encoded frames", NULL);
        return -1;
    }
    if (w->sps_len > 65535 || w->pps_len > 65535) {
        set_err(err, errsz, "mp4: SPS/PPS too large", NULL);
        return -1;
    }

    duration_ms = (uint32_t)(((uint64_t)w->nsamples * 1000u) / (uint64_t)w->fps);

    p = box_begin(&ftyp, "ftyp");
    put_fourcc(&ftyp, "isom");
    put_u32(&ftyp, 512);
    put_fourcc(&ftyp, "isom");
    put_fourcc(&ftyp, "iso2");
    put_fourcc(&ftyp, "avc1");
    put_fourcc(&ftyp, "mp41");
    box_end(&ftyp, p);

    p = box_begin(&moov, "moov");

    {
        size_t mv = box_begin(&moov, "mvhd");
        put_u32(&moov, 0);                 /* version + flags */
        put_u32(&moov, 0);                 /* creation */
        put_u32(&moov, 0);                 /* modification */
        put_u32(&moov, 1000);              /* movie timescale */
        put_u32(&moov, duration_ms);
        put_u32(&moov, 0x00010000);        /* rate 1.0 */
        put_u16(&moov, 0x0100);            /* volume 1.0 */
        put_u16(&moov, 0);
        put_u32(&moov, 0);
        put_u32(&moov, 0);
        put_matrix(&moov);
        for (i = 0; i < 6; i++)
            put_u32(&moov, 0);             /* pre_defined[6] */
        put_u32(&moov, 2);                 /* next_track_ID */
        box_end(&moov, mv);
    }

    trak = box_begin(&moov, "trak");

    tkhd = box_begin(&moov, "tkhd");
    put_u32(&moov, 0x00000007);            /* enabled | in movie | in preview */
    put_u32(&moov, 0);
    put_u32(&moov, 0);
    put_u32(&moov, 1);                     /* track_ID */
    put_u32(&moov, 0);
    put_u32(&moov, duration_ms);
    put_u32(&moov, 0);
    put_u32(&moov, 0);
    put_u16(&moov, 0);                     /* layer */
    put_u16(&moov, 0);                     /* alternate_group */
    put_u16(&moov, 0);                     /* volume (video) */
    put_u16(&moov, 0);
    put_matrix(&moov);
    put_u32(&moov, (uint32_t)w->w << 16);
    put_u32(&moov, (uint32_t)w->h << 16);
    box_end(&moov, tkhd);

    mdia = box_begin(&moov, "mdia");

    mdhd = box_begin(&moov, "mdhd");
    put_u32(&moov, 0);
    put_u32(&moov, 0);
    put_u32(&moov, 0);
    put_u32(&moov, (uint32_t)w->fps);      /* media timescale */
    put_u32(&moov, (uint32_t)w->nsamples);
    put_u16(&moov, 0x55C4);                /* 'und' */
    put_u16(&moov, 0);
    box_end(&moov, mdhd);

    hdlr = box_begin(&moov, "hdlr");
    put_u32(&moov, 0);
    put_u32(&moov, 0);
    put_fourcc(&moov, "vide");
    put_u32(&moov, 0);
    put_u32(&moov, 0);
    put_u32(&moov, 0);
    put_data(&moov, handler_name, sizeof(handler_name));
    box_end(&moov, hdlr);

    minf = box_begin(&moov, "minf");

    vmhd = box_begin(&moov, "vmhd");
    put_u32(&moov, 1);                     /* version 0, flags 1 */
    put_u16(&moov, 0);                     /* graphicsmode */
    put_u16(&moov, 0);
    put_u16(&moov, 0);
    put_u16(&moov, 0);                     /* opcolor */
    box_end(&moov, vmhd);

    dinf = box_begin(&moov, "dinf");
    dref = box_begin(&moov, "dref");
    put_u32(&moov, 0);
    put_u32(&moov, 1);
    url = box_begin(&moov, "url ");
    put_u32(&moov, 1);                     /* flags 1: self-contained */
    box_end(&moov, url);
    box_end(&moov, dref);
    box_end(&moov, dinf);

    stbl = box_begin(&moov, "stbl");

    stsd = box_begin(&moov, "stsd");
    put_u32(&moov, 0);
    put_u32(&moov, 1);
    avc1 = box_begin(&moov, "avc1");
    put_u32(&moov, 0);
    put_u16(&moov, 0);
    put_u16(&moov, 1);                     /* data_reference_index */
    put_u16(&moov, 0);
    put_u16(&moov, 0);
    put_u32(&moov, 0);
    put_u32(&moov, 0);
    put_u32(&moov, 0);
    put_u16(&moov, (uint16_t)w->w);
    put_u16(&moov, (uint16_t)w->h);
    put_u32(&moov, 0x00480000);            /* 72 dpi */
    put_u32(&moov, 0x00480000);
    put_u32(&moov, 0);
    put_u16(&moov, 1);                     /* frame_count */
    {
        unsigned char cn[32];
        memset(cn, 0, sizeof(cn));
        cn[0] = 7;
        memcpy(cn + 1, "MathGFX", 7);
        put_data(&moov, cn, sizeof(cn));
    }
    put_u16(&moov, 0x0018);                /* depth */
    put_u16(&moov, 0xFFFF);
    avcc = box_begin(&moov, "avcC");
    put_u8(&moov, 1);
    put_u8(&moov, w->sps[1]);
    put_u8(&moov, w->sps[2]);
    put_u8(&moov, w->sps[3]);
    put_u8(&moov, 0xFF);                   /* lengthSizeMinusOne = 3 */
    put_u8(&moov, 0xE1);                   /* one SPS */
    put_u16(&moov, (uint16_t)w->sps_len);
    put_data(&moov, w->sps, (size_t)w->sps_len);
    put_u8(&moov, 1);
    put_u16(&moov, (uint16_t)w->pps_len);
    put_data(&moov, w->pps, (size_t)w->pps_len);
    box_end(&moov, avcc);
    box_end(&moov, avc1);
    box_end(&moov, stsd);

    stts = box_begin(&moov, "stts");
    put_u32(&moov, 0);
    put_u32(&moov, 1);
    put_u32(&moov, (uint32_t)w->nsamples);
    put_u32(&moov, 1);                     /* one tick per frame */
    box_end(&moov, stts);

    stsc = box_begin(&moov, "stsc");
    put_u32(&moov, 0);
    put_u32(&moov, 1);
    put_u32(&moov, 1);
    put_u32(&moov, (uint32_t)w->nsamples);
    put_u32(&moov, 1);
    box_end(&moov, stsc);

    stsz = box_begin(&moov, "stsz");
    put_u32(&moov, 0);
    put_u32(&moov, 0);
    put_u32(&moov, (uint32_t)w->nsamples);
    for (i = 0; i < w->nsamples; i++)
        put_u32(&moov, w->samples[i].size);
    box_end(&moov, stsz);

    stco = box_begin(&moov, "stco");
    put_u32(&moov, 0);
    put_u32(&moov, 1);
    put_u32(&moov, 0);                    /* patched after moov is sized */
    stco_field_pos = moov.n - 4;
    box_end(&moov, stco);

    {
        size_t nkey = 0;
        for (i = 0; i < w->nsamples; i++)
            if (w->samples[i].key)
                nkey++;
        stss = box_begin(&moov, "stss");
        put_u32(&moov, 0);
        put_u32(&moov, (uint32_t)nkey);
        for (i = 0; i < w->nsamples; i++)
            if (w->samples[i].key)
                put_u32(&moov, (uint32_t)(i + 1));
        box_end(&moov, stss);
    }

    box_end(&moov, stbl);
    box_end(&moov, minf);
    box_end(&moov, mdia);
    box_end(&moov, trak);
    box_end(&moov, p);

    /* fast-start: ftyp + moov + mdat, so players see the index first */
    chunk_offset = (uint32_t)(ftyp.n + moov.n + 8u);
    if (stco_field_pos + 4u > moov.n) {
        set_err(err, errsz, "mp4: bad stco position", NULL);
        goto out;
    }
    moov.p[stco_field_pos + 0] = (unsigned char)(chunk_offset >> 24);
    moov.p[stco_field_pos + 1] = (unsigned char)(chunk_offset >> 16);
    moov.p[stco_field_pos + 2] = (unsigned char)(chunk_offset >> 8);
    moov.p[stco_field_pos + 3] = (unsigned char)(chunk_offset);

    if (ftyp.failed || moov.failed || w->mdat.failed) {
        set_err(err, errsz, "mp4: out of memory while building boxes", NULL);
        goto out;
    }
    if (w->mdat.n > 0xFFFFFFFFu - 8u) {
        set_err(err, errsz, "mp4: mdat too large", NULL);
        goto out;
    }

    f = fopen(w->path, "wb");
    if (!f) {
        set_err(err, errsz, "mp4: cannot open output file", NULL);
        goto out;
    }
    if (fwrite(ftyp.p, 1, ftyp.n, f) != ftyp.n)
        goto io_fail;
    if (fwrite(moov.p, 1, moov.n, f) != moov.n)
        goto io_fail;
    {
        uint32_t msize = (uint32_t)(w->mdat.n + 8u);
        unsigned char h[8];
        h[0] = (unsigned char)(msize >> 24);
        h[1] = (unsigned char)(msize >> 16);
        h[2] = (unsigned char)(msize >> 8);
        h[3] = (unsigned char)(msize);
        h[4] = 'm'; h[5] = 'd'; h[6] = 'a'; h[7] = 't';
        if (fwrite(h, 1, 8, f) != 8)
            goto io_fail;
    }
    if (w->mdat.n && fwrite(w->mdat.p, 1, w->mdat.n, f) != w->mdat.n)
        goto io_fail;
    if (fclose(f) != 0) {
        f = NULL;
        goto io_fail;
    }
    f = NULL;
    rc = 0;
    goto out;

io_fail:
    set_err(err, errsz, "mp4: write failed", NULL);
    if (f)
        fclose(f);
    remove(w->path);

out:
    free(ftyp.p);
    free(moov.p);
    return rc;
}

int mp4_close(Mp4Writer *w, char *err, size_t errsz)
{
    x264_nal_t *nals = NULL;
    x264_picture_t out;
    int nnals = 0;
    int rc;

    if (!w)
        return -1;
    if (w->failed) {
        mp4_abort(w);
        set_err(err, errsz, "mp4: encoder failed", NULL);
        return -1;
    }

    while (x264_encoder_delayed_frames(w->enc) > 0) {
        rc = x264_encoder_encode(w->enc, &nals, &nnals, NULL, &out);
        if (rc < 0) {
            mp4_abort(w);
            set_err(err, errsz, "mp4: flush failed", NULL);
            return -1;
        }
        if (rc > 0 && process_nals(w, nals, nnals, 1) != 0) {
            mp4_abort(w);
            set_err(err, errsz, "mp4: cannot store flushed frame", NULL);
            return -1;
        }
    }

    rc = build_mp4(w, err, errsz);
    free(w->mdat.p);
    free(w->samples);
    free(w->sps);
    free(w->pps);
    free(w->path);
    if (w->pic_alloc)
        x264_picture_clean(&w->pic);
    x264_encoder_close(w->enc);
    free(w);
    return rc;
}

void mp4_abort(Mp4Writer *w)
{
    if (!w)
        return;
    if (w->pic_alloc)
        x264_picture_clean(&w->pic);
    if (w->enc)
        x264_encoder_close(w->enc);
    free(w->mdat.p);
    free(w->samples);
    free(w->sps);
    free(w->pps);
    free(w->path);
    free(w);
}
