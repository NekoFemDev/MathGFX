#include "mgfx.h"
#include "mux.h"
#include "video.h"
#include "portable.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *f)
{
    fprintf(f,
        "MathGFX %s — software procedural renderer\n"
        "\n"
        "usage:\n"
        "  mathgfx render <scene.mgf> [out.png] [--width N] [--height N] [--threads N]\n"
        "  mathgfx check <scene.mgf> [more.mgf ...]\n"
        "  mathgfx video <scene.mgf> <out.avi|out.mp4> [--fps N] [--quality N] [--crf N]\n"
        "                  [--width N] [--height N]\n"
        "  mathgfx mux -o <out.avi|out.mp4> [--fps N] [--quality N] [--crf N] <frame.png> ...\n"
        "  mathgfx --version\n"
        "\n"
        "If the scene calls save_frame, those files are written and the\n"
        "positional out.png is ignored. Otherwise out.png (default: out.png)\n"
        "receives the final framebuffer.\n",
        MGFX_VERSION);
}

static int parse_dim(const char *s, int *out)
{
    char *end = NULL;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || !end || *end || v <= 0 || v > MGFX_MAX_DIM)
        return -1;
    *out = (int)v;
    return 0;
}

static int parse_range(const char *s, int lo, int hi, int *out)
{
    char *end = NULL;
    long v;
    if (!s || !*s)
        return -1;
    v = strtol(s, &end, 10);
    if (!end || *end || v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

static int cmd_render(int argc, char **argv)
{
    const char *scene = NULL;
    const char *out_path = NULL;
    int width = 0;
    int height = 0;
    int threads = 0;
    int i;
    Program prog;
    RenderResult res;
    char err[600];
    double t0, t1;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--width") == 0 || strcmp(argv[i], "-w") == 0) {
            if (++i >= argc || parse_dim(argv[i], &width) != 0) {
                fprintf(stderr, "mathgfx: bad --width\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--height") == 0 || strcmp(argv[i], "-h") == 0) {
            if (++i >= argc || parse_dim(argv[i], &height) != 0) {
                fprintf(stderr, "mathgfx: bad --height\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--threads") == 0 || strcmp(argv[i], "-j") == 0) {
            char *end = NULL;
            long v;
            if (++i >= argc) {
                fprintf(stderr, "mathgfx: bad --threads\n");
                return 2;
            }
            errno = 0;
            v = strtol(argv[i], &end, 10);
            if (errno || !end || *end || v < 1 || v > MGFX_MAX_THREADS) {
                fprintf(stderr, "mathgfx: bad --threads (1..%d)\n", MGFX_MAX_THREADS);
                return 2;
            }
            threads = (int)v;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "mathgfx: unknown option '%s'\n", argv[i]);
            return 2;
        } else if (!scene) {
            scene = argv[i];
        } else if (!out_path) {
            out_path = argv[i];
        } else {
            fprintf(stderr, "mathgfx: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }

    if (!scene) {
        usage(stderr);
        return 2;
    }
    if (!out_path)
        out_path = "out.png";

    if (mgfx_compile_file(scene, &prog, err, sizeof(err)) != 0) {
        fprintf(stderr, "mathgfx: %s\n", err);
        return 1;
    }

    if (threads == 0) {
        int n = mgfx_cpu_count();
        if (n < 1)
            n = 1;
        if (n > MGFX_MAX_THREADS)
            n = MGFX_MAX_THREADS;
        threads = n;
    }
    if (!prog.has_yloop || prog.has_save_frame)
        threads = 1;

    t0 = mgfx_now_sec();
    if (mgfx_render(&prog, width, height, threads, &res, err, sizeof(err)) != 0) {
        fprintf(stderr, "mathgfx: %s\n", err);
        program_free(&prog);
        return 1;
    }
    t1 = mgfx_now_sec();

    if (res.saved_any) {
        printf("wrote %d frame(s) from %s (%.3fs, %d thread(s))\n",
               res.frames, scene, t1 - t0, threads);
    } else {
        if (!mgfx_write_png(out_path, res.width, res.height, res.pixels)) {
            fprintf(stderr, "mathgfx: cannot write '%s'\n", out_path);
            mgfx_render_free(&res);
            program_free(&prog);
            return 1;
        }
        printf("rendered %dx%d -> %s (%.3fs, %d thread(s))\n",
               res.width, res.height, out_path, t1 - t0, threads);
    }

    mgfx_render_free(&res);
    program_free(&prog);
    return 0;
}

static int cmd_check(int argc, char **argv)
{
    int i;
    int bad = 0;
    if (argc < 3) {
        usage(stderr);
        return 2;
    }
    for (i = 2; i < argc; i++) {
        Program prog;
        char err[600];
        if (mgfx_compile_file(argv[i], &prog, err, sizeof(err)) != 0) {
            fprintf(stderr, "mathgfx: %s\n", err);
            bad = 1;
            continue;
        }
        printf("ok: %s (%dx%d, %d instructions, %d string(s))\n",
               argv[i], prog.canvas_w, prog.canvas_h, prog.ncode, prog.nstrings);
        program_free(&prog);
    }
    return bad ? 1 : 0;
}

typedef struct {
    VideoWriter *writer;
    int w;
    int h;
    char err[512];
} VideoCtx;

static int video_sink(void *user, int w, int h, const unsigned char *rgb)
{
    VideoCtx *c = (VideoCtx *)user;
    if (w != c->w || h != c->h) {
        snprintf(c->err, sizeof(c->err), "video: frame size changed");
        return -1;
    }
    return video_writer_add(c->writer, rgb, c->err, sizeof(c->err));
}

static int cmd_video(int argc, char **argv)
{
    const char *scene = NULL;
    const char *out_path = NULL;
    int width = 0;
    int height = 0;
    int fps = 25;
    int quality = 90;
    int crf = 20;
    int i;
    Program prog;
    RenderResult res;
    VideoCtx ctx;
    char err[600];
    double t0, t1;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            if (parse_range(argv[++i], 1, 240, &fps) != 0)
                goto bad;
        } else if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc) {
            if (parse_range(argv[++i], 1, 100, &quality) != 0)
                goto bad;
        } else if (strcmp(argv[i], "--crf") == 0 && i + 1 < argc) {
            if (parse_range(argv[++i], 0, 51, &crf) != 0)
                goto bad;
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            if (parse_dim(argv[++i], &width) != 0)
                goto bad;
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            if (parse_dim(argv[++i], &height) != 0)
                goto bad;
        } else if (argv[i][0] == '-') {
            goto bad;
        } else if (!scene) {
            scene = argv[i];
        } else if (!out_path) {
            out_path = argv[i];
        } else {
            goto bad;
        }
    }
    if (!scene || !out_path) {
bad:
        usage(stderr);
        return 2;
    }

    if (mgfx_compile_file(scene, &prog, err, sizeof(err)) != 0) {
        fprintf(stderr, "mathgfx: %s\n", err);
        return 1;
    }
    if (!prog.has_save_frame) {
        fprintf(stderr, "mathgfx: %s has no save_frame (use time_loop + save_frame)\n",
                scene);
        program_free(&prog);
        return 1;
    }

    if (width <= 0)
        width = prog.canvas_w;
    if (height <= 0)
        height = prog.canvas_h;
    if (mgfx_ensure_parent_dir(out_path) != 0) {
        fprintf(stderr, "mathgfx: cannot create directory for '%s'\n", out_path);
        program_free(&prog);
        return 1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.w = width;
    ctx.h = height;
    ctx.writer = video_writer_open(out_path, width, height, fps, quality, crf,
                                   err, sizeof(err));
    if (!ctx.writer) {
        fprintf(stderr, "mathgfx: %s\n", err);
        program_free(&prog);
        return 1;
    }

    t0 = mgfx_now_sec();
    if (mgfx_render_sink(&prog, width, height, 1, video_sink, &ctx,
                         &res, err, sizeof(err)) != 0) {
        fprintf(stderr, "mathgfx: %s\n", ctx.err[0] ? ctx.err : err);
        video_writer_abort(ctx.writer);
        program_free(&prog);
        return 1;
    }
    t1 = mgfx_now_sec();

    if (res.frames == 0) {
        fprintf(stderr, "mathgfx: scene produced no frames\n");
        video_writer_abort(ctx.writer);
        mgfx_render_free(&res);
        program_free(&prog);
        return 1;
    }
    if (video_writer_close(ctx.writer, err, sizeof(err)) != 0) {
        fprintf(stderr, "mathgfx: %s\n", err);
        mgfx_render_free(&res);
        program_free(&prog);
        return 1;
    }

    printf("wrote %d frames, %dx%d @ %d fps -> %s (%.3fs)\n",
           res.frames, width, height, fps, out_path, t1 - t0);
    mgfx_render_free(&res);
    program_free(&prog);
    return 0;
}

static int cmd_mux(int argc, char **argv)
{
    const char *out_path = NULL;
    int fps = 25;
    int quality = 90;
    int crf = 20;
    int i;
    int nfiles = 0;
    int frames = 0;
    char **files;
    char err[600];

    files = (char **)calloc((size_t)argc, sizeof(*files));
    if (!files) {
        fprintf(stderr, "mathgfx: out of memory\n");
        return 1;
    }

    for (i = 2; i < argc; i++) {
        if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--out") == 0) &&
            i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            if (parse_range(argv[++i], 1, 240, &fps) != 0)
                goto bad;
        } else if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc) {
            if (parse_range(argv[++i], 1, 100, &quality) != 0)
                goto bad;
        } else if (strcmp(argv[i], "--crf") == 0 && i + 1 < argc) {
            if (parse_range(argv[++i], 0, 51, &crf) != 0)
                goto bad;
        } else if (argv[i][0] == '-') {
            goto bad;
        } else {
            files[nfiles++] = argv[i];
        }
    }
    if (!out_path || nfiles == 0) {
bad:
        free(files);
        usage(stderr);
        return 2;
    }

    if (mgfx_ensure_parent_dir(out_path) != 0) {
        fprintf(stderr, "mathgfx: cannot create directory for '%s'\n", out_path);
        free(files);
        return 1;
    }
    if (mgfx_mux_pngs(out_path, fps, quality, crf, files, nfiles, &frames,
                      err, sizeof(err)) != 0) {
        fprintf(stderr, "mathgfx: %s\n", err);
        free(files);
        return 1;
    }
    printf("muxed %d frames @ %d fps -> %s\n", frames, fps, out_path);
    free(files);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        printf("mathgfx %s\n", MGFX_VERSION);
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(argv[1], "render") == 0)
        return cmd_render(argc, argv);
    if (strcmp(argv[1], "check") == 0)
        return cmd_check(argc, argv);
    if (strcmp(argv[1], "video") == 0)
        return cmd_video(argc, argv);
    if (strcmp(argv[1], "mux") == 0)
        return cmd_mux(argc, argv);

    fprintf(stderr, "mathgfx: unknown command '%s'\n", argv[1]);
    usage(stderr);
    return 2;
}
