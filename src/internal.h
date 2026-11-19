#ifndef MGFX_INTERNAL_H
#define MGFX_INTERNAL_H

#include "mgfx.h"

typedef enum {
    TK_EOF = 0,
    TK_NL,
    TK_IDENT,
    TK_NUM,
    TK_STR,
    TK_LP,
    TK_RP,
    TK_COMMA,
    TK_PLUS,
    TK_MINUS,
    TK_STAR,
    TK_SLASH,
    TK_PERCENT,
    TK_CARET,
    TK_LT,
    TK_LE,
    TK_GT,
    TK_GE,
    TK_EQ,
    TK_NE,
    TK_ERROR
} TokKind;

typedef struct {
    TokKind kind;
    int line;
    int col;
    double num;
    char str[512];
} Token;

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    int line;
    int col;
    char err[256];
} Lexer;

void lexer_init(Lexer *l, const char *src, size_t len);
Token lexer_next(Lexer *l);

#endif /* MGFX_INTERNAL_H */
