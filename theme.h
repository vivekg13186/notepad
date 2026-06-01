#ifndef THEME_H
#define THEME_H

#include "raylib.h"
#include <stdbool.h>

/* Syntax palette indices match syntax_scope_to_color() in syntax.c:
   0 default, 1 keyword, 2 string, 3 comment, 4 number,
   5 type, 6 constant, 7 function, 8 operator. */
#define THEME_PALETTE_N 9

typedef struct {
    char  name[64];

    Color bg;
    Color fg;
    Color gutter_bg;
    Color gutter_fg;
    Color gutter_fg_cur;
    Color status_bg;
    Color status_fg;
    Color cursor;
    Color cursor_block;   /* block cursor overlay (NORMAL mode) */
    Color cursor_glyph;   /* glyph color when block cursor is over it */
    Color selection;

    Color palette[THEME_PALETTE_N];
} Theme;

void theme_dark (Theme *t);
void theme_light(Theme *t);

/* Load a theme from a JSON file. Unspecified fields keep their current value,
   so call theme_dark()/theme_light() first to set defaults. */
bool theme_load_file(Theme *t, const char *path);

#endif
