/*
 * MathGFX — software procedural renderer.
 *
 * A small ASM-like DSL with double registers, compiled to a flat bytecode
 * and executed by a register/stack VM. No GPU, no graphics API.
 */
#ifndef MGFX_H
#define MGFX_H

#include <stddef.h>

#define MGFX_VERSION "0.1.0"

#define MGFX_MAX_REG     16
#define MGFX_MAX_STACK   1024
#define MGFX_MAX_REPEAT  16
#define MGFX_MAX_BLOCKS  32
#define MGFX_MAX_DIM     16384
#define MGFX_MAX_THREADS 64

/* ---- bytecode ---------------------------------------------------------- */

typedef enum {
    OP_HALT = 0,
    OP_PUSH_NUM,      /* d = value                                  */
    OP_LOAD_REG,      /* a = register index                         */
    OP_LOAD_VAR,      /* a = builtin variable id                    */
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_NEG,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_EQ,
    OP_NE,
    OP_CALL,          /* a = builtin id, b = argc                   */
    OP_STORE_REG,     /* a = register index                         */
    OP_SET_PIXEL,     /* pops x, y, r, g, b                         */
    OP_PIXEL,         /* pops r, g, b (uses current X, Y)           */
    OP_JMP,           /* a = target instruction                     */
    OP_JZ,            /* a = target; pops condition                 */
    OP_REPEAT_INIT,   /* a = slot; pops count                       */
    OP_REPEAT_NEXT,   /* a = slot, b = body target                  */
    OP_Y_INIT,
    OP_Y_NEXT,        /* a = body target                            */
    OP_X_INIT,
    OP_X_NEXT,        /* a = body target                            */
    OP_TIME_INIT,     /* a = target after loop; pops from, to, step */
    OP_TIME_NEXT,     /* a = body target                            */
    OP_SET_TIME,      /* pops time                                  */
    OP_SAVE_FRAME     /* a = string table index                     */
} OpCode;

/* builtin variables */
enum {
    V_X = 0, V_Y, V_U, V_V, V_T, V_W, V_H, V_PI
};

/* builtin functions */
enum {
    BI_SIN = 0, BI_COS, BI_TAN, BI_SQRT, BI_ABS, BI_FLOOR, BI_CEIL,
    BI_FRACT, BI_MIN, BI_MAX, BI_POW, BI_MOD, BI_ATAN2, BI_CLAMP,
    BI_LERP, BI_SMOOTHSTEP, BI_NOISE2, BI_FBM2
};

typedef struct {
    unsigned char op;
    int a;
    int b;
    double d;
} Instr;

typedef struct {
    Instr *code;
    int ncode;
    char **strings;
    int nstrings;
    int canvas_w;
    int canvas_h;
    int has_yloop;        /* scene uses y_loop -> rows are independent */
    int has_save_frame;   /* save_frame -> keep the render single-threaded */
} Program;

void program_init(Program *p);
void program_free(Program *p);

/* Compile a .mgf scene. Returns 0 on success, -1 on error (err filled). */
int mgfx_compile_file(const char *path, Program *out, char *err, size_t errsz);

/* ---- runtime ----------------------------------------------------------- */

typedef struct {
    int width;
    int height;
    int saved_any;             /* scene called save_frame at least once */
    int frames;                /* number of save_frame calls */
    unsigned char *pixels;     /* RGB8, width*height*3, owned */
} RenderResult;

/* Video/frame sink: return 0 on success, -1 to abort the render. */
typedef int (*MgfxFrameSink)(void *user, int width, int height,
                             const unsigned char *rgb);

/*
 * Run a compiled scene. width/height override the canvas (pass 0 to use
 * the program's canvas). save_frame paths are written relative to cwd.
 */
int mgfx_render(const Program *p, int width, int height, int threads,
                RenderResult *out, char *err, size_t errsz);

/* Like mgfx_render, but every save_frame() feeds the sink instead of PNG.
 * The sink is called sequentially, so this path is always single-threaded. */
int mgfx_render_sink(const Program *p, int width, int height, int threads,
                     MgfxFrameSink sink, void *user,
                     RenderResult *out, char *err, size_t errsz);

void mgfx_render_free(RenderResult *r);

/* ---- image ------------------------------------------------------------- */

int mgfx_write_png(const char *path, int w, int h, const unsigned char *rgb);
int mgfx_ensure_parent_dir(const char *path);

/* Downsample RGB8 buffer with box filter (SSAA).
 * src:  src_w x src_h (src_w = dst_w * ssaa, src_h = dst_h * ssaa)
 * dst:  dst_w x dst_h
 * ssaa: supersampling factor (1..4) */
void mgfx_downsample_box(const unsigned char *src, int src_w, int src_h,
                         int ssaa, unsigned char *dst);

#endif /* MGFX_H */
