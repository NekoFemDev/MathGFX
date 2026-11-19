/* Lexer + parser + one-pass bytecode compiler for the MGF scene language. */
#include "internal.h"

#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    BLK_IF = 0,
    BLK_REPEAT,
    BLK_YLOOP,
    BLK_XLOOP,
    BLK_TIMELOOP
} BlockKind;

typedef struct {
    BlockKind kind;
    int body_pc;
    int slot;
    int breaks[64];
    int nbreaks;
} BlockCtx;

typedef struct {
    Lexer lx;
    Token tok;
    const char *file;
    jmp_buf jb;
    char err[600];

    Instr *code;
    int ncode;
    int cap;

    char **strings;
    int nstrings;
    int capstr;

    BlockCtx blocks[MGFX_MAX_BLOCKS];
    int nblocks;
    int repeat_depth;

    int canvas_w;
    int canvas_h;
    int have_canvas;
} Parser;

typedef struct {
    const char *name;
    int id;
    int arity;
} BuiltinDef;

static const BuiltinDef builtins[] = {
    {"sin", BI_SIN, 1},
    {"cos", BI_COS, 1},
    {"tan", BI_TAN, 1},
    {"sqrt", BI_SQRT, 1},
    {"abs", BI_ABS, 1},
    {"floor", BI_FLOOR, 1},
    {"ceil", BI_CEIL, 1},
    {"fract", BI_FRACT, 1},
    {"min", BI_MIN, 2},
    {"max", BI_MAX, 2},
    {"pow", BI_POW, 2},
    {"mod", BI_MOD, 2},
    {"atan2", BI_ATAN2, 2},
    {"clamp", BI_CLAMP, 3},
    {"lerp", BI_LERP, 3},
    {"smoothstep", BI_SMOOTHSTEP, 3},
    {"noise2", BI_NOISE2, 2},
    {"fbm2", BI_FBM2, 3},
};

static const char *const var_names[] = {
    "X", "Y", "U", "V", "T", "W", "H", "PI"
};

static void perr(Parser *p, const char *fmt, ...)
{
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    snprintf(p->err, sizeof(p->err), "%s:%d:%d: %s",
             p->file, p->tok.line, p->tok.col, msg);
    longjmp(p->jb, 1);
}

static void perr_lex(Parser *p)
{
    snprintf(p->err, sizeof(p->err), "%s: %s", p->file, p->lx.err);
    longjmp(p->jb, 1);
}

static void advance(Parser *p)
{
    p->tok = lexer_next(&p->lx);
    if (p->tok.kind == TK_ERROR)
        perr_lex(p);
}

static void skip_nl(Parser *p)
{
    while (p->tok.kind == TK_NL)
        advance(p);
}

static int is_kw(const Parser *p, const char *s)
{
    return p->tok.kind == TK_IDENT && strcmp(p->tok.str, s) == 0;
}

static void expect_kw(Parser *p, const char *s)
{
    if (!is_kw(p, s))
        perr(p, "expected '%s'", s);
    advance(p);
}

static int emit(Parser *p, unsigned char op, int a, int b, double d)
{
    if (p->ncode >= p->cap) {
        int ncap = p->cap ? p->cap * 2 : 256;
        Instr *n = (Instr *)realloc(p->code, (size_t)ncap * sizeof(Instr));
        if (!n)
            perr(p, "out of memory");
        p->code = n;
        p->cap = ncap;
    }
    {
        Instr *in = &p->code[p->ncode];
        in->op = op;
        in->a = a;
        in->b = b;
        in->d = d;
    }
    return p->ncode++;
}

static void patch_jump(Parser *p, int at, int target)
{
    p->code[at].a = target;
}

static int add_string(Parser *p, const char *s)
{
    if (p->nstrings >= p->capstr) {
        int ncap = p->capstr ? p->capstr * 2 : 8;
        char **n = (char **)realloc(p->strings, (size_t)ncap * sizeof(char *));
        if (!n)
            perr(p, "out of memory");
        p->strings = n;
        p->capstr = ncap;
    }
    p->strings[p->nstrings] = strdup(s);
    if (!p->strings[p->nstrings])
        perr(p, "out of memory");
    return p->nstrings++;
}

static void push_block(Parser *p, BlockKind kind)
{
    BlockCtx *b;
    if (p->nblocks >= MGFX_MAX_BLOCKS)
        perr(p, "blocks nested too deeply (max %d)", MGFX_MAX_BLOCKS);
    b = &p->blocks[p->nblocks++];
    memset(b, 0, sizeof(*b));
    b->kind = kind;
}

static BlockCtx *top_block(Parser *p)
{
    if (p->nblocks <= 0)
        perr(p, "internal: empty block stack");
    return &p->blocks[p->nblocks - 1];
}

static void pop_block(Parser *p)
{
    if (p->nblocks <= 0)
        perr(p, "internal: empty block stack");
    p->nblocks--;
}

static int reg_from_name(const char *s)
{
    int v = 0;
    size_t i;
    if (s[0] != 'r' || !isdigit((unsigned char)s[1]))
        return -1;
    for (i = 1; s[i]; i++) {
        if (!isdigit((unsigned char)s[i]))
            return -1;
        v = v * 10 + (s[i] - '0');
        if (v > 999)
            return -1;
    }
    return (v < MGFX_MAX_REG) ? v : -2; /* -2: looks like a register but out of range */
}

static int parse_reg(Parser *p)
{
    int r;
    if (p->tok.kind != TK_IDENT)
        perr(p, "expected register r0..r15");
    r = reg_from_name(p->tok.str);
    if (r == -2)
        perr(p, "register '%s' out of range (r0..r15)", p->tok.str);
    if (r < 0)
        perr(p, "expected register r0..r15, got '%s'", p->tok.str);
    advance(p);
    return r;
}

static void parse_expr(Parser *p);
static void parse_add(Parser *p);

static void parse_args(Parser *p, int *argc, int want)
{
    if (p->tok.kind != TK_LP)
        perr(p, "expected '(' after function name");
    advance(p);
    *argc = 0;
    if (p->tok.kind == TK_RP) {
        advance(p);
        return;
    }
    for (;;) {
        parse_expr(p);
        (*argc)++;
        if (p->tok.kind == TK_COMMA) {
            advance(p);
            continue;
        }
        break;
    }
    if (p->tok.kind != TK_RP)
        perr(p, "expected ')' after arguments");
    advance(p);
    if (*argc != want)
        perr(p, "function expects %d argument(s), got %d", want, *argc);
}

static void parse_primary(Parser *p)
{
    if (p->tok.kind == TK_NUM) {
        emit(p, OP_PUSH_NUM, 0, 0, p->tok.num);
        advance(p);
        return;
    }
    if (p->tok.kind == TK_LP) {
        advance(p);
        parse_expr(p);
        if (p->tok.kind != TK_RP)
            perr(p, "expected ')'");
        advance(p);
        return;
    }
    if (p->tok.kind == TK_IDENT) {
        int r = reg_from_name(p->tok.str);
        if (r == -2)
            perr(p, "register '%s' out of range (r0..r15)", p->tok.str);
        if (r >= 0) {
            emit(p, OP_LOAD_REG, r, 0, 0.0);
            advance(p);
            return;
        }
        {
            size_t i;
            for (i = 0; i < sizeof(var_names) / sizeof(var_names[0]); i++) {
                if (strcmp(p->tok.str, var_names[i]) == 0) {
                    emit(p, OP_LOAD_VAR, (int)i, 0, 0.0);
                    advance(p);
                    return;
                }
            }
        }
        {
            size_t i;
            for (i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
                if (strcmp(p->tok.str, builtins[i].name) == 0) {
                    int id = builtins[i].id;
                    int arity = builtins[i].arity;
                    int argc = 0;
                    advance(p);
                    parse_args(p, &argc, arity);
                    emit(p, OP_CALL, id, argc, 0.0);
                    return;
                }
            }
        }
        perr(p, "unknown identifier '%s'", p->tok.str);
    }
    perr(p, "expected expression");
}

static void parse_unary(Parser *p);

static void parse_pow(Parser *p)
{
    parse_primary(p);
    if (p->tok.kind == TK_CARET) {
        advance(p);
        parse_unary(p);
        emit(p, OP_CALL, BI_POW, 2, 0.0);
    }
}

static void parse_unary(Parser *p)
{
    if (p->tok.kind == TK_MINUS) {
        advance(p);
        parse_unary(p);
        emit(p, OP_NEG, 0, 0, 0.0);
        return;
    }
    if (p->tok.kind == TK_PLUS) {
        advance(p);
        parse_unary(p);
        return;
    }
    parse_pow(p);
}

static void parse_mul(Parser *p)
{
    parse_unary(p);
    for (;;) {
        TokKind k = p->tok.kind;
        unsigned char op;
        if (k == TK_STAR)
            op = OP_MUL;
        else if (k == TK_SLASH)
            op = OP_DIV;
        else if (k == TK_PERCENT)
            op = OP_MOD;
        else
            break;
        advance(p);
        parse_unary(p);
        emit(p, op, 0, 0, 0.0);
    }
}

static void parse_add(Parser *p)
{
    parse_mul(p);
    for (;;) {
        TokKind k = p->tok.kind;
        unsigned char op;
        if (k == TK_PLUS)
            op = OP_ADD;
        else if (k == TK_MINUS)
            op = OP_SUB;
        else
            break;
        advance(p);
        parse_mul(p);
        emit(p, op, 0, 0, 0.0);
    }
}

static void parse_cmp(Parser *p)
{
    parse_add(p);
    for (;;) {
        TokKind k = p->tok.kind;
        unsigned char op;
        if (k == TK_LT)
            op = OP_LT;
        else if (k == TK_LE)
            op = OP_LE;
        else if (k == TK_GT)
            op = OP_GT;
        else if (k == TK_GE)
            op = OP_GE;
        else if (k == TK_EQ)
            op = OP_EQ;
        else if (k == TK_NE)
            op = OP_NE;
        else
            break;
        advance(p);
        parse_add(p);
        emit(p, op, 0, 0, 0.0);
    }
}

static void parse_expr(Parser *p)
{
    parse_cmp(p);
}

static void opt_comma(Parser *p)
{
    if (p->tok.kind == TK_COMMA)
        advance(p);
}

static void parse_block(Parser *p);

static void stmt_canvas(Parser *p)
{
    int w, h;
    advance(p);
    if (p->have_canvas)
        perr(p, "canvas already declared");
    if (p->tok.kind != TK_NUM)
        perr(p, "canvas width must be a number");
    w = (int)p->tok.num;
    if ((double)w != p->tok.num || w <= 0 || w > MGFX_MAX_DIM)
        perr(p, "canvas width must be an integer in 1..%d", MGFX_MAX_DIM);
    advance(p);
    opt_comma(p);
    if (p->tok.kind != TK_NUM)
        perr(p, "canvas height must be a number");
    h = (int)p->tok.num;
    if ((double)h != p->tok.num || h <= 0 || h > MGFX_MAX_DIM)
        perr(p, "canvas height must be an integer in 1..%d", MGFX_MAX_DIM);
    advance(p);
    p->canvas_w = w;
    p->canvas_h = h;
    p->have_canvas = 1;
}

static void stmt_reg_expr(Parser *p, unsigned char op)
{
    int r;
    advance(p);
    r = parse_reg(p);
    opt_comma(p);
    emit(p, OP_LOAD_REG, r, 0, 0.0);
    parse_expr(p);
    emit(p, op, 0, 0, 0.0);
    emit(p, OP_STORE_REG, r, 0, 0.0);
}

static void stmt_func1(Parser *p, int builtin)
{
    int r;
    advance(p);
    r = parse_reg(p);
    opt_comma(p);
    parse_expr(p);
    emit(p, OP_CALL, builtin, 1, 0.0);
    emit(p, OP_STORE_REG, r, 0, 0.0);
}

static void stmt_set_pixel(Parser *p)
{
    advance(p);
    parse_expr(p);
    opt_comma(p);
    parse_expr(p);
    opt_comma(p);
    parse_expr(p);
    opt_comma(p);
    parse_expr(p);
    opt_comma(p);
    parse_expr(p);
    emit(p, OP_SET_PIXEL, 0, 0, 0.0);
}

static void stmt_pixel(Parser *p)
{
    advance(p);
    parse_expr(p);
    opt_comma(p);
    parse_expr(p);
    opt_comma(p);
    parse_expr(p);
    emit(p, OP_PIXEL, 0, 0, 0.0);
}

static void stmt_save_frame(Parser *p)
{
    int idx;
    advance(p);
    if (p->tok.kind != TK_STR) {
        int mark = p->ncode;
        parse_expr(p);       /* legacy save_frame T, "..." — value ignored */
        p->ncode = mark;
        opt_comma(p);
    }
    if (p->tok.kind != TK_STR)
        perr(p, "save_frame expects a quoted path");
    if (!p->tok.str[0])
        perr(p, "save_frame path is empty");
    idx = add_string(p, p->tok.str);
    advance(p);
    emit(p, OP_SAVE_FRAME, idx, 0, 0.0);
}

static void stmt_if(Parser *p)
{
    int jz;
    int ends[64];
    int nends = 0;

    advance(p);
    parse_expr(p);
    jz = emit(p, OP_JZ, -1, 0, 0.0);
    push_block(p, BLK_IF);

    for (;;) {
        parse_block(p);

        if (is_kw(p, "elif")) {
            int jmp;
            if (nends >= (int)(sizeof(ends) / sizeof(ends[0])))
                perr(p, "too many branches in one if-chain");
            jmp = emit(p, OP_JMP, -1, 0, 0.0);
            ends[nends++] = jmp;
            patch_jump(p, jz, p->ncode);
            advance(p);
            parse_expr(p);
            jz = emit(p, OP_JZ, -1, 0, 0.0);
            continue;
        }

        if (is_kw(p, "else")) {
            int else_line = p->tok.line;
            advance(p);
            /* `else if` on the same line is a chain; `else` + newline + `if`
             * is a plain nested if with its own end. */
            if (is_kw(p, "if") && p->tok.line == else_line) {
                int jmp;
                if (nends >= (int)(sizeof(ends) / sizeof(ends[0])))
                    perr(p, "too many branches in one if-chain");
                jmp = emit(p, OP_JMP, -1, 0, 0.0);
                ends[nends++] = jmp;
                patch_jump(p, jz, p->ncode);
                advance(p);
                parse_expr(p);
                jz = emit(p, OP_JZ, -1, 0, 0.0);
                continue;
            }
            {
                int jmp;
                if (nends >= (int)(sizeof(ends) / sizeof(ends[0])))
                    perr(p, "too many branches in one if-chain");
                jmp = emit(p, OP_JMP, -1, 0, 0.0);
                ends[nends++] = jmp;
                patch_jump(p, jz, p->ncode);
            }
            parse_block(p);
            expect_kw(p, "end");
            {
                int i;
                for (i = 0; i < nends; i++)
                    patch_jump(p, ends[i], p->ncode);
            }
            /* jz already points at the else body; only branch-exit jumps
             * need the end target. */
            pop_block(p);
            return;
        }

        if (is_kw(p, "end")) {
            int i;
            advance(p);
            for (i = 0; i < nends; i++)
                patch_jump(p, ends[i], p->ncode);
            patch_jump(p, jz, p->ncode);
            pop_block(p);
            return;
        }

        perr(p, "expected 'end'");
    }
}

static void stmt_repeat(Parser *p)
{
    BlockCtx *b;
    int slot;
    advance(p);
    parse_expr(p);
    if (p->repeat_depth >= MGFX_MAX_REPEAT)
        perr(p, "repeat loops nested too deeply (max %d)", MGFX_MAX_REPEAT);
    slot = p->repeat_depth++;
    emit(p, OP_REPEAT_INIT, slot, 0, 0.0);
    push_block(p, BLK_REPEAT);
    b = top_block(p);
    b->slot = slot;
    b->body_pc = p->ncode;
    parse_block(p);
    expect_kw(p, "end");
    emit(p, OP_REPEAT_NEXT, slot, b->body_pc, 0.0);
    {
        int i;
        for (i = 0; i < b->nbreaks; i++)
            patch_jump(p, b->breaks[i], p->ncode);
    }
    pop_block(p);
    p->repeat_depth--;
}

static void stmt_break(Parser *p)
{
    int i;
    BlockCtx *loop = NULL;
    advance(p);
    for (i = p->nblocks - 1; i >= 0; i--) {
        if (p->blocks[i].kind == BLK_REPEAT ||
            p->blocks[i].kind == BLK_YLOOP ||
            p->blocks[i].kind == BLK_XLOOP ||
            p->blocks[i].kind == BLK_TIMELOOP) {
            loop = &p->blocks[i];
            break;
        }
    }
    if (!loop)
        perr(p, "'break' outside a loop");
    if (loop->nbreaks >= (int)(sizeof(loop->breaks) / sizeof(loop->breaks[0])))
        perr(p, "too many 'break' statements in one loop");
    loop->breaks[loop->nbreaks++] = emit(p, OP_JMP, -1, 0, 0.0);
}

static void stmt_yloop(Parser *p)
{
    BlockCtx *b;
    advance(p);
    emit(p, OP_Y_INIT, 0, 0, 0.0);
    push_block(p, BLK_YLOOP);
    b = top_block(p);
    b->body_pc = p->ncode;
    parse_block(p);
    expect_kw(p, "end");
    emit(p, OP_Y_NEXT, b->body_pc, 0, 0.0);
    {
        int i;
        for (i = 0; i < b->nbreaks; i++)
            patch_jump(p, b->breaks[i], p->ncode);
    }
    pop_block(p);
}

static void stmt_xloop(Parser *p)
{
    BlockCtx *b;
    advance(p);
    emit(p, OP_X_INIT, 0, 0, 0.0);
    push_block(p, BLK_XLOOP);
    b = top_block(p);
    b->body_pc = p->ncode;
    parse_block(p);
    expect_kw(p, "end");
    emit(p, OP_X_NEXT, b->body_pc, 0, 0.0);
    {
        int i;
        for (i = 0; i < b->nbreaks; i++)
            patch_jump(p, b->breaks[i], p->ncode);
    }
    pop_block(p);
}

static void stmt_time_loop(Parser *p)
{
    BlockCtx *b;
    int init;
    advance(p);
    parse_expr(p);
    opt_comma(p);
    parse_expr(p);
    opt_comma(p);
    parse_expr(p);
    init = emit(p, OP_TIME_INIT, -1, 0, 0.0);
    push_block(p, BLK_TIMELOOP);
    b = top_block(p);
    b->body_pc = p->ncode;
    parse_block(p);
    expect_kw(p, "end");
    emit(p, OP_TIME_NEXT, b->body_pc, 0, 0.0);
    {
        int i;
        for (i = 0; i < b->nbreaks; i++)
            patch_jump(p, b->breaks[i], p->ncode);
    }
    patch_jump(p, init, p->ncode);
    pop_block(p);
}

static void parse_stmt(Parser *p)
{
    const char *s;

    if (p->tok.kind != TK_IDENT)
        perr(p, "expected a statement");
    s = p->tok.str;

    if (strcmp(s, "canvas") == 0) {
        stmt_canvas(p);
    } else if (strcmp(s, "mov") == 0 || strcmp(s, "set") == 0) {
        int r;
        advance(p);
        r = parse_reg(p);
        opt_comma(p);
        parse_expr(p);
        emit(p, OP_STORE_REG, r, 0, 0.0);
    } else if (strcmp(s, "add") == 0) {
        stmt_reg_expr(p, OP_ADD);
    } else if (strcmp(s, "sub") == 0) {
        stmt_reg_expr(p, OP_SUB);
    } else if (strcmp(s, "mul") == 0) {
        stmt_reg_expr(p, OP_MUL);
    } else if (strcmp(s, "div") == 0) {
        stmt_reg_expr(p, OP_DIV);
    } else if (strcmp(s, "mod") == 0) {
        stmt_reg_expr(p, OP_MOD);
    } else if (strcmp(s, "sin") == 0) {
        stmt_func1(p, BI_SIN);
    } else if (strcmp(s, "cos") == 0) {
        stmt_func1(p, BI_COS);
    } else if (strcmp(s, "tan") == 0) {
        stmt_func1(p, BI_TAN);
    } else if (strcmp(s, "sqrt") == 0) {
        stmt_func1(p, BI_SQRT);
    } else if (strcmp(s, "abs") == 0) {
        stmt_func1(p, BI_ABS);
    } else if (strcmp(s, "floor") == 0) {
        stmt_func1(p, BI_FLOOR);
    } else if (strcmp(s, "ceil") == 0) {
        stmt_func1(p, BI_CEIL);
    } else if (strcmp(s, "fract") == 0) {
        stmt_func1(p, BI_FRACT);
    } else if (strcmp(s, "set_pixel") == 0) {
        stmt_set_pixel(p);
    } else if (strcmp(s, "pixel") == 0) {
        stmt_pixel(p);
    } else if (strcmp(s, "save_frame") == 0) {
        stmt_save_frame(p);
    } else if (strcmp(s, "time") == 0) {
        advance(p);
        parse_expr(p);
        emit(p, OP_SET_TIME, 0, 0, 0.0);
    } else if (strcmp(s, "if") == 0) {
        stmt_if(p);
    } else if (strcmp(s, "repeat") == 0) {
        stmt_repeat(p);
    } else if (strcmp(s, "break") == 0) {
        stmt_break(p);
    } else if (strcmp(s, "y_loop") == 0) {
        stmt_yloop(p);
    } else if (strcmp(s, "x_loop") == 0) {
        stmt_xloop(p);
    } else if (strcmp(s, "time_loop") == 0) {
        stmt_time_loop(p);
    } else {
        perr(p, "unknown statement '%s'", s);
    }
}

static void parse_block(Parser *p)
{
    for (;;) {
        skip_nl(p);
        if (p->tok.kind == TK_EOF)
            return;
        if (is_kw(p, "end") || is_kw(p, "else") || is_kw(p, "elif"))
            return;
        parse_stmt(p);
        if (p->tok.kind != TK_NL && p->tok.kind != TK_EOF)
            perr(p, "unexpected token after statement");
    }
}

static void parse_program(Parser *p)
{
    advance(p);
    parse_block(p);
    if (p->tok.kind != TK_EOF)
        perr(p, "unexpected '%s'", p->tok.str);
    if (!p->have_canvas)
        perr(p, "missing 'canvas WIDTH, HEIGHT'");
    emit(p, OP_HALT, 0, 0, 0.0);
}

void program_init(Program *p)
{
    memset(p, 0, sizeof(*p));
}

void program_free(Program *p)
{
    int i;
    if (!p)
        return;
    free(p->code);
    for (i = 0; i < p->nstrings; i++)
        free(p->strings[i]);
    free(p->strings);
    memset(p, 0, sizeof(*p));
}

static void parser_free(Parser *p)
{
    int i;
    free(p->code);
    for (i = 0; i < p->nstrings; i++)
        free(p->strings[i]);
    free(p->strings);
}

int mgfx_compile_file(const char *path, Program *out, char *err, size_t errsz)
{
    FILE *f;
    long sz;
    char *src = NULL;
    Parser *p;
    volatile int rc = -1;

    program_init(out);

    f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errsz, "%s: cannot open file", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        snprintf(err, errsz, "%s: cannot seek", path);
        return -1;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        snprintf(err, errsz, "%s: cannot tell size", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        snprintf(err, errsz, "%s: cannot seek", path);
        return -1;
    }
    src = (char *)malloc((size_t)sz + 1);
    if (!src) {
        fclose(f);
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    if (sz > 0 && fread(src, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(src);
        snprintf(err, errsz, "%s: cannot read file", path);
        return -1;
    }
    fclose(f);
    src[sz] = '\0';

    p = (Parser *)calloc(1, sizeof(Parser));
    if (!p) {
        free(src);
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    p->file = path;
    p->canvas_w = 0;
    p->canvas_h = 0;

    if (setjmp(p->jb) == 0) {
        lexer_init(&p->lx, src, (size_t)sz);
        parse_program(p);
        out->code = p->code;
        out->ncode = p->ncode;
        out->strings = p->strings;
        out->nstrings = p->nstrings;
        out->canvas_w = p->canvas_w;
        out->canvas_h = p->canvas_h;
        out->has_yloop = 0;
        out->has_save_frame = 0;
        {
            int i;
            for (i = 0; i < p->ncode; i++) {
                if (p->code[i].op == OP_Y_INIT)
                    out->has_yloop = 1;
                if (p->code[i].op == OP_SAVE_FRAME)
                    out->has_save_frame = 1;
            }
        }
        p->code = NULL;
        p->ncode = 0;
        p->cap = 0;
        p->strings = NULL;
        p->nstrings = 0;
        p->capstr = 0;
        rc = 0;
    } else {
        snprintf(err, errsz, "%s", p->err);
    }

    parser_free(p);
    free(p);
    free(src);
    return rc;
}
