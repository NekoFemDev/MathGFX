#include "internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lexer_init(Lexer *l, const char *src, size_t len)
{
    l->src = src;
    l->len = len;
    l->pos = 0;
    l->line = 1;
    l->col = 1;
    l->err[0] = '\0';
}

static int lx_peek(const Lexer *l, size_t off)
{
    return (l->pos + off < l->len) ? (unsigned char)l->src[l->pos + off] : -1;
}

static void lx_fail(Lexer *l, int line, int col, const char *msg)
{
    snprintf(l->err, sizeof(l->err), "%d:%d: %s", line, col, msg);
}

static Token make_token(TokKind kind, int line, int col)
{
    Token t;
    memset(&t, 0, sizeof(t));
    t.kind = kind;
    t.line = line;
    t.col = col;
    return t;
}

static void lx_advance_char(Lexer *l)
{
    if (l->pos >= l->len)
        return;
    if (l->src[l->pos] == '\n') {
        l->line++;
        l->col = 1;
    } else {
        l->col++;
    }
    l->pos++;
}

Token lexer_next(Lexer *l)
{
    for (;;) {
        int c = lx_peek(l, 0);

        if (c < 0) {
            Token t = make_token(TK_EOF, l->line, l->col);
            return t;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            lx_advance_char(l);
            continue;
        }
        if (c == '\n') {
            Token t = make_token(TK_NL, l->line, l->col);
            lx_advance_char(l);
            return t;
        }
        if (c == ';' || (c == '/' && lx_peek(l, 1) == '/')) {
            while (lx_peek(l, 0) >= 0 && lx_peek(l, 0) != '\n')
                lx_advance_char(l);
            continue;
        }
        break;
    }

    {
        int line = l->line;
        int col = l->col;
        int c = lx_peek(l, 0);

        /* number: digits, optional fraction, optional exponent */
        if (isdigit(c) || (c == '.' && isdigit(lx_peek(l, 1)))) {
            char buf[128];
            size_t n = 0;
            int seen_dot = 0;
            int seen_exp = 0;

            while (n + 1 < sizeof(buf)) {
                int d = lx_peek(l, 0);
                if (isdigit(d)) {
                    buf[n++] = (char)d;
                    lx_advance_char(l);
                } else if (d == '.' && !seen_dot && !seen_exp) {
                    seen_dot = 1;
                    buf[n++] = '.';
                    lx_advance_char(l);
                } else if ((d == 'e' || d == 'E') && !seen_exp) {
                    int d1 = lx_peek(l, 1);
                    int d2 = lx_peek(l, 2);
                    if (!(isdigit(d1) || ((d1 == '+' || d1 == '-') && isdigit(d2))))
                        break;
                    seen_exp = 1;
                    buf[n++] = (char)d;
                    lx_advance_char(l);
                    if (lx_peek(l, 0) == '+' || lx_peek(l, 0) == '-') {
                        buf[n++] = (char)lx_peek(l, 0);
                        lx_advance_char(l);
                    }
                } else {
                    break;
                }
            }
            buf[n] = '\0';
            {
                Token t = make_token(TK_NUM, line, col);
                char *end = NULL;
                t.num = strtod(buf, &end);
                if (end == buf || *end != '\0') {
                    lx_fail(l, line, col, "malformed number");
                    return make_token(TK_ERROR, line, col);
                }
                return t;
            }
        }

        /* identifier */
        if (isalpha(c) || c == '_') {
            char buf[512];
            size_t n = 0;
            while (n + 1 < sizeof(buf)) {
                int d = lx_peek(l, 0);
                if (!(isalnum(d) || d == '_'))
                    break;
                buf[n++] = (char)d;
                lx_advance_char(l);
            }
            if (n + 1 >= sizeof(buf)) {
                lx_fail(l, line, col, "identifier too long");
                return make_token(TK_ERROR, line, col);
            }
            buf[n] = '\0';
            {
                Token t = make_token(TK_IDENT, line, col);
                memcpy(t.str, buf, n + 1);
                return t;
            }
        }

        /* string */
        if (c == '"') {
            Token t = make_token(TK_STR, line, col);
            size_t n = 0;
            lx_advance_char(l); /* opening quote */
            for (;;) {
                int d = lx_peek(l, 0);
                if (d < 0) {
                    lx_fail(l, line, col, "unterminated string");
                    return make_token(TK_ERROR, line, col);
                }
                if (d == '"') {
                    lx_advance_char(l);
                    break;
                }
                if (d == '\n') {
                    lx_fail(l, line, col, "newline in string");
                    return make_token(TK_ERROR, line, col);
                }
                if (d == '\\') {
                    int e = lx_peek(l, 1);
                    switch (e) {
                    case 'n': d = '\n'; break;
                    case 't': d = '\t'; break;
                    case 'r': d = '\r'; break;
                    case '0': d = '\0'; break;
                    case '\\': d = '\\'; break;
                    case '"': d = '"'; break;
                    default:
                        lx_fail(l, line, col, "unknown escape in string");
                        return make_token(TK_ERROR, line, col);
                    }
                    lx_advance_char(l);
                }
                if (n + 1 >= sizeof(t.str)) {
                    lx_fail(l, line, col, "string too long");
                    return make_token(TK_ERROR, line, col);
                }
                t.str[n++] = (char)d;
                lx_advance_char(l);
            }
            t.str[n] = '\0';
            return t;
        }

        lx_advance_char(l);
        switch (c) {
        case '(': return make_token(TK_LP, line, col);
        case ')': return make_token(TK_RP, line, col);
        case ',': return make_token(TK_COMMA, line, col);
        case '+': return make_token(TK_PLUS, line, col);
        case '-': return make_token(TK_MINUS, line, col);
        case '*': return make_token(TK_STAR, line, col);
        case '/': return make_token(TK_SLASH, line, col);
        case '%': return make_token(TK_PERCENT, line, col);
        case '^': return make_token(TK_CARET, line, col);
        case '<':
            if (lx_peek(l, 0) == '=') {
                lx_advance_char(l);
                return make_token(TK_LE, line, col);
            }
            return make_token(TK_LT, line, col);
        case '>':
            if (lx_peek(l, 0) == '=') {
                lx_advance_char(l);
                return make_token(TK_GE, line, col);
            }
            return make_token(TK_GT, line, col);
        case '=':
            if (lx_peek(l, 0) == '=') {
                lx_advance_char(l);
                return make_token(TK_EQ, line, col);
            }
            break;
        case '!':
            if (lx_peek(l, 0) == '=') {
                lx_advance_char(l);
                return make_token(TK_NE, line, col);
            }
            break;
        default:
            break;
        }

        lx_fail(l, line, col, "unexpected character");
        return make_token(TK_ERROR, line, col);
    }
}
