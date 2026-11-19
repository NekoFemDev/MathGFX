/* Bytecode VM: double registers, small expression stack, software framebuffer. */
#include "mgfx.h"
#include "internal.h"

#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const Program *p;
    int w;
    int h;
    unsigned char *fb;

    double regs[MGFX_MAX_REG];
    double st[MGFX_MAX_STACK];
    int sp;

    int x;
    int y;
    int y0;
    int y1;
    int frame;

    double t;
    double t_end;
    double t_step;

    double rep[MGFX_MAX_REPEAT];

    int saved_any;
    int frames;

    MgfxFrameSink sink;
    void *sink_user;

    jmp_buf jb;
    char err[512];
} VM;

static void vm_fail(VM *vm, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->err, sizeof(vm->err), fmt, ap);
    va_end(ap);
    longjmp(vm->jb, 1);
}

static void push(VM *vm, double v)
{
    if (vm->sp >= MGFX_MAX_STACK)
        vm_fail(vm, "expression stack overflow");
    vm->st[vm->sp++] = v;
}

static double popv(VM *vm)
{
    if (vm->sp <= 0)
        vm_fail(vm, "expression stack underflow");
    return vm->st[--vm->sp];
}

static unsigned char to8(double v)
{
    if (!(v > 0.0))
        return 0;
    if (v >= 1.0)
        return 255;
    return (unsigned char)(v * 255.0 + 0.5);
}

static void put_pixel(VM *vm, double dx, double dy, double r, double g, double b)
{
    int ix = (int)floor(dx);
    int iy = (int)floor(dy);
    unsigned char *px;
    if (ix < 0 || iy < 0 || ix >= vm->w || iy >= vm->h)
        return;
    px = vm->fb + ((size_t)iy * (size_t)vm->w + (size_t)ix) * 3u;
    px[0] = to8(r);
    px[1] = to8(g);
    px[2] = to8(b);
}

static double var_value(const VM *vm, int id)
{
    switch (id) {
    case V_X: return (double)vm->x;
    case V_Y: return (double)vm->y;
    case V_U: return ((double)vm->x + 0.5) / (double)vm->w;
    case V_V: return ((double)vm->y + 0.5) / (double)vm->h;
    case V_T: return vm->t;
    case V_W: return (double)vm->w;
    case V_H: return (double)vm->h;
    case V_PI: return 3.14159265358979323846;
    default: return 0.0;
    }
}

static double fade(double t)
{
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

static double hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 0x27d4eb2du ^ (uint32_t)y * 0x165667b1u;
    h ^= h >> 15;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return (double)h / 4294967296.0;
}

static double noise2(double x, double y)
{
    double fx = floor(x);
    double fy = floor(y);
    int ix = (int)fx;
    int iy = (int)fy;
    double tx = x - fx;
    double ty = y - fy;
    double u = fade(tx);
    double v = fade(ty);
    double a = hash2(ix, iy);
    double b = hash2(ix + 1, iy);
    double c = hash2(ix, iy + 1);
    double d = hash2(ix + 1, iy + 1);
    double ab = a + (b - a) * u;
    double cd = c + (d - c) * u;
    return ab + (cd - ab) * v;
}

static double fbm2(double x, double y, double octaves)
{
    int oct = (int)floor(octaves);
    double sum = 0.0;
    double amp = 1.0;
    double norm = 0.0;
    double fx = x;
    double fy = y;
    int i;
    if (oct < 1)
        oct = 1;
    if (oct > 10)
        oct = 10;
    for (i = 0; i < oct; i++) {
        sum += amp * noise2(fx, fy);
        norm += amp;
        amp *= 0.5;
        fx *= 2.0;
        fy *= 2.0;
    }
    return norm > 0.0 ? sum / norm : 0.0;
}

/* Replace the first %[0-9]*d in pat with the frame index. */
static void frame_path(const char *pat, int index, char *out, size_t outsz)
{
    size_t i = 0;
    size_t o = 0;
    int done = 0;

    out[0] = '\0';
    while (pat[i]) {
        if (!done && pat[i] == '%') {
            size_t j = i + 1;
            int width = 0;
            while (pat[j] >= '0' && pat[j] <= '9') {
                width = width * 10 + (pat[j] - '0');
                j++;
            }
            if (pat[j] == 'd') {
                char tmp[64];
                int n;
                if (width <= 0)
                    width = 1;
                n = snprintf(tmp, sizeof(tmp), "%0*d", width, index);
                if (n < 0 || o + (size_t)n + 1 >= outsz)
                    return;
                memcpy(out + o, tmp, (size_t)n);
                o += (size_t)n;
                i = j + 1;
                done = 1;
                continue;
            }
        }
        if (o + 1 >= outsz)
            return;
        out[o++] = pat[i++];
    }
    out[o] = '\0';
}

static void save_frame(VM *vm, int string_index)
{
    const char *pat;
    char path[1024];
    if (string_index < 0 || string_index >= vm->p->nstrings)
        vm_fail(vm, "internal: bad frame path index");
    if (vm->sink) {
        if (vm->sink(vm->sink_user, vm->w, vm->h, vm->fb) != 0)
            vm_fail(vm, "frame sink failed");
        vm->frame++;
        vm->frames++;
        vm->saved_any = 1;
        return;
    }
    pat = vm->p->strings[string_index];
    path[0] = '\0';
    frame_path(pat, vm->frame, path, sizeof(path));
    if (!path[0])
        vm_fail(vm, "frame path is too long");
    if (mgfx_ensure_parent_dir(path) != 0)
        vm_fail(vm, "cannot create directory for '%s'", path);
    if (!mgfx_write_png(path, vm->w, vm->h, vm->fb))
        vm_fail(vm, "cannot write '%s'", path);
    vm->frame++;
    vm->frames++;
    vm->saved_any = 1;
}

static int run_vm(VM *vm)
{
    int pc = 0;
    const Program *p = vm->p;

    for (;;) {
        const Instr *in;
        if (pc < 0 || pc >= p->ncode)
            vm_fail(vm, "bytecode pc out of range (%d)", pc);
        in = &p->code[pc++];

        switch ((OpCode)in->op) {
        case OP_HALT:
            return 0;

        case OP_PUSH_NUM:
            push(vm, in->d);
            break;

        case OP_LOAD_REG:
            if (in->a < 0 || in->a >= MGFX_MAX_REG)
                vm_fail(vm, "bad register %d", in->a);
            push(vm, vm->regs[in->a]);
            break;

        case OP_LOAD_VAR:
            push(vm, var_value(vm, in->a));
            break;

        case OP_ADD: {
            double b = popv(vm), a = popv(vm);
            push(vm, a + b);
            break;
        }
        case OP_SUB: {
            double b = popv(vm), a = popv(vm);
            push(vm, a - b);
            break;
        }
        case OP_MUL: {
            double b = popv(vm), a = popv(vm);
            push(vm, a * b);
            break;
        }
        case OP_DIV: {
            double b = popv(vm), a = popv(vm);
            push(vm, a / b);
            break;
        }
        case OP_MOD: {
            double b = popv(vm), a = popv(vm);
            push(vm, fmod(a, b));
            break;
        }
        case OP_NEG:
            push(vm, -popv(vm));
            break;

        case OP_LT: {
            double b = popv(vm), a = popv(vm);
            push(vm, a < b ? 1.0 : 0.0);
            break;
        }
        case OP_LE: {
            double b = popv(vm), a = popv(vm);
            push(vm, a <= b ? 1.0 : 0.0);
            break;
        }
        case OP_GT: {
            double b = popv(vm), a = popv(vm);
            push(vm, a > b ? 1.0 : 0.0);
            break;
        }
        case OP_GE: {
            double b = popv(vm), a = popv(vm);
            push(vm, a >= b ? 1.0 : 0.0);
            break;
        }
        case OP_EQ: {
            double b = popv(vm), a = popv(vm);
            push(vm, a == b ? 1.0 : 0.0);
            break;
        }
        case OP_NE: {
            double b = popv(vm), a = popv(vm);
            push(vm, a != b ? 1.0 : 0.0);
            break;
        }

        case OP_CALL: {
            int id = in->a;
            int argc = in->b;
            if (argc == 1) {
                double a = popv(vm);
                switch (id) {
                case BI_SIN: push(vm, sin(a)); break;
                case BI_COS: push(vm, cos(a)); break;
                case BI_TAN: push(vm, tan(a)); break;
                case BI_SQRT: push(vm, sqrt(a)); break;
                case BI_ABS: push(vm, fabs(a)); break;
                case BI_FLOOR: push(vm, floor(a)); break;
                case BI_CEIL: push(vm, ceil(a)); break;
                case BI_FRACT: push(vm, a - floor(a)); break;
                default: vm_fail(vm, "bad 1-arg builtin %d", id);
                }
            } else if (argc == 2) {
                double b = popv(vm), a = popv(vm);
                switch (id) {
                case BI_MIN: push(vm, a < b ? a : b); break;
                case BI_MAX: push(vm, a > b ? a : b); break;
                case BI_POW: push(vm, pow(a, b)); break;
                case BI_MOD: push(vm, fmod(a, b)); break;
                case BI_ATAN2: push(vm, atan2(a, b)); break;
                case BI_NOISE2: push(vm, noise2(a, b)); break;
                default: vm_fail(vm, "bad 2-arg builtin %d", id);
                }
            } else if (argc == 3) {
                double c = popv(vm), b = popv(vm), a = popv(vm);
                switch (id) {
                case BI_CLAMP:
                    push(vm, a < b ? b : (a > c ? c : a));
                    break;
                case BI_LERP:
                    push(vm, a + (b - a) * c);
                    break;
                case BI_SMOOTHSTEP: {
                    double t = c;
                    if (t < 0.0)
                        t = 0.0;
                    if (t > 1.0)
                        t = 1.0;
                    push(vm, a + (b - a) * (t * t * (3.0 - 2.0 * t)));
                    break;
                }
                case BI_FBM2:
                    push(vm, fbm2(a, b, c));
                    break;
                default: vm_fail(vm, "bad 3-arg builtin %d", id);
                }
            } else {
                vm_fail(vm, "bad builtin argc %d", argc);
            }
            break;
        }

        case OP_STORE_REG:
            if (in->a < 0 || in->a >= MGFX_MAX_REG)
                vm_fail(vm, "bad register %d", in->a);
            vm->regs[in->a] = popv(vm);
            break;

        case OP_SET_PIXEL: {
            double b = popv(vm);
            double g = popv(vm);
            double r = popv(vm);
            double yy = popv(vm);
            double xx = popv(vm);
            put_pixel(vm, xx, yy, r, g, b);
            break;
        }

        case OP_PIXEL: {
            double b = popv(vm);
            double g = popv(vm);
            double r = popv(vm);
            put_pixel(vm, (double)vm->x, (double)vm->y, r, g, b);
            break;
        }

        case OP_JMP:
            pc = in->a;
            break;

        case OP_JZ: {
            double cond = popv(vm);
            if (cond == 0.0)
                pc = in->a;
            break;
        }

        case OP_REPEAT_INIT: {
            double n = popv(vm);
            if (!(n > 0.0))
                n = 0.0;
            n = floor(n);
            if (n > 10000000.0)
                vm_fail(vm, "repeat count too large (max 10000000)");
            vm->rep[in->a] = n;
            break;
        }

        case OP_REPEAT_NEXT:
            vm->rep[in->a] -= 1.0;
            if (vm->rep[in->a] > 0.0)
                pc = in->b;
            break;

        case OP_Y_INIT:
            vm->y = vm->y0;
            break;

        case OP_Y_NEXT:
            vm->y++;
            if (vm->y < vm->y1)
                pc = in->a;
            break;

        case OP_X_INIT:
            vm->x = 0;
            break;

        case OP_X_NEXT:
            vm->x++;
            if (vm->x < vm->w)
                pc = in->a;
            break;

        case OP_TIME_INIT: {
            double step = popv(vm);
            double tend = popv(vm);
            double from = popv(vm);
            if (!(step > 1e-12))
                vm_fail(vm, "time_loop step must be > 0");
            vm->t = from;
            vm->t_end = tend;
            vm->t_step = step;
            if (!(vm->t < vm->t_end))
                pc = in->a;
            break;
        }

        case OP_TIME_NEXT:
            vm->t += vm->t_step;
            if (vm->t < vm->t_end)
                pc = in->a;
            break;

        case OP_SET_TIME:
            vm->t = popv(vm);
            break;

        case OP_SAVE_FRAME:
            save_frame(vm, in->a);
            break;

        default:
            vm_fail(vm, "unknown opcode %d", (int)in->op);
        }
    }
}

typedef struct {
    const Program *p;
    unsigned char *fb;
    int w;
    int h;
    int y0;
    int y1;
    int rc;
    int saved_any;
    int frames;
    MgfxFrameSink sink;
    void *sink_user;
    char err[512];
} RenderJob;

static void run_job(RenderJob *job)
{
    VM *vm = (VM *)calloc(1, sizeof(VM));
    if (!vm) {
        job->rc = -1;
        snprintf(job->err, sizeof(job->err), "out of memory");
        return;
    }
    vm->p = job->p;
    vm->w = job->w;
    vm->h = job->h;
    vm->fb = job->fb;
    vm->y0 = job->y0;
    vm->y1 = job->y1;
    vm->t = 0.0;
    vm->sink = job->sink;
    vm->sink_user = job->sink_user;

    if (setjmp(vm->jb) == 0) {
        job->rc = run_vm(vm);
        job->saved_any = vm->saved_any;
        job->frames = vm->frames;
    } else {
        job->rc = -1;
        snprintf(job->err, sizeof(job->err), "%s", vm->err);
    }
    free(vm);
}

static void *job_thread(void *arg)
{
    run_job((RenderJob *)arg);
    return NULL;
}

static int render_impl(const Program *p, int width, int height, int threads,
                       MgfxFrameSink sink, void *sink_user,
                       RenderResult *out, char *err, size_t errsz)
{
    unsigned char *fb;
    size_t npix;
    int use_threads;
    int i;
    int y;
    RenderJob *jobs = NULL;
    pthread_t *tids = NULL;

    memset(out, 0, sizeof(*out));

    if (width <= 0)
        width = p->canvas_w;
    if (height <= 0)
        height = p->canvas_h;
    if (width <= 0 || height <= 0 || width > MGFX_MAX_DIM || height > MGFX_MAX_DIM) {
        snprintf(err, errsz, "invalid canvas size %dx%d", width, height);
        return -1;
    }

    if (threads < 1)
        threads = 1;
    if (threads > MGFX_MAX_THREADS)
        threads = MGFX_MAX_THREADS;
    /* Rows are independent only when the scene drives them with y_loop.
     * save_frame writes files and stays single-threaded. */
    if (!p->has_yloop || p->has_save_frame || sink)
        threads = 1;
    if (threads > height)
        threads = height;
    use_threads = threads;

    npix = (size_t)width * (size_t)height * 3u;
    fb = (unsigned char *)calloc(npix, 1);
    if (!fb) {
        snprintf(err, errsz, "out of memory for %dx%d framebuffer", width, height);
        return -1;
    }

    if (use_threads == 1) {
        RenderJob job;
        memset(&job, 0, sizeof(job));
        job.p = p;
        job.fb = fb;
        job.w = width;
        job.h = height;
        job.y0 = 0;
        job.y1 = height;
        job.sink = sink;
        job.sink_user = sink_user;
        run_job(&job);
        if (job.rc != 0) {
            snprintf(err, errsz, "%s", job.err);
            free(fb);
            return -1;
        }
        out->width = width;
        out->height = height;
        out->saved_any = job.saved_any;
        out->frames = job.frames;
        out->pixels = fb;
        return 0;
    }

    jobs = (RenderJob *)calloc((size_t)use_threads, sizeof(*jobs));
    tids = (pthread_t *)calloc((size_t)use_threads, sizeof(*tids));
    if (!jobs || !tids) {
        free(jobs);
        free(tids);
        free(fb);
        snprintf(err, errsz, "out of memory");
        return -1;
    }

    y = 0;
    for (i = 0; i < use_threads; i++) {
        int rows = height / use_threads + (i < height % use_threads ? 1 : 0);
        jobs[i].p = p;
        jobs[i].fb = fb;
        jobs[i].w = width;
        jobs[i].h = height;
        jobs[i].y0 = y;
        jobs[i].y1 = y + rows;
        jobs[i].sink = NULL;
        jobs[i].sink_user = NULL;
        y += rows;
    }

    for (i = 1; i < use_threads; i++) {
        if (pthread_create(&tids[i], NULL, job_thread, &jobs[i]) != 0)
            tids[i] = 0;
    }

    run_job(&jobs[0]);

    for (i = 1; i < use_threads; i++) {
        if (tids[i])
            pthread_join(tids[i], NULL);
        else
            run_job(&jobs[i]);      /* thread creation failed: run inline */
    }

    for (i = 0; i < use_threads; i++) {
        if (jobs[i].rc != 0) {
            snprintf(err, errsz, "%s", jobs[i].err);
            free(jobs);
            free(tids);
            free(fb);
            return -1;
        }
    }

    free(jobs);
    free(tids);
    out->width = width;
    out->height = height;
    out->saved_any = 0;
    out->frames = 0;
    out->pixels = fb;
    return 0;
}

int mgfx_render(const Program *p, int width, int height, int threads,
                RenderResult *out, char *err, size_t errsz)
{
    return render_impl(p, width, height, threads, NULL, NULL, out, err, errsz);
}

int mgfx_render_sink(const Program *p, int width, int height, int threads,
                     MgfxFrameSink sink, void *user,
                     RenderResult *out, char *err, size_t errsz)
{
    if (!sink) {
        snprintf(err, errsz, "mgfx_render_sink: null sink");
        return -1;
    }
    return render_impl(p, width, height, threads, sink, user, out, err, errsz);
}

void mgfx_render_free(RenderResult *r)
{
    if (!r)
        return;
    free(r->pixels);
    memset(r, 0, sizeof(*r));
}
