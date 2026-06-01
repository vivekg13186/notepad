#ifndef SYNTAX_H
#define SYNTAX_H

#include <stdbool.h>
#include <stddef.h>
#include <regex.h>

typedef struct {
    char    *name;      /* TextMate scope name */
    char    *pattern;   /* raw regex source */
    regex_t  rx;        /* compiled */
    bool     compiled;
} SyntaxRule;

typedef struct {
    char       *name;
    SyntaxRule *rules;
    size_t      nrules;
} Syntax;

bool syntax_load_tmlanguage(Syntax *s, const char *path);
void syntax_free(Syntax *s);

/* For a line of text, fill colors[] (length = line_len) with a small palette index.
   0 = default, 1 = keyword, 2 = string, 3 = comment, 4 = number, 5 = type, 6 = constant,
   7 = function, 8 = operator. */
void syntax_highlight_line(const Syntax *s, const char *line, size_t line_len, unsigned char *colors);

unsigned char syntax_scope_to_color(const char *scope);

#endif
