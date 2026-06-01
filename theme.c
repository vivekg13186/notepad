#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- hex color: "#rgb", "#rrggbb", "#rrggbbaa" ---- */

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_color(const char *s, Color *out) {
    if (!s || *s != '#') return false;
    const char *p = s + 1;
    size_t n = strlen(p);
    int v[8] = {0};
    if (n == 3 || n == 4 || n == 6 || n == 8) {
        for (size_t i = 0; i < n; i++) {
            int h = hex_nibble(p[i]);
            if (h < 0) return false;
            v[i] = h;
        }
    } else return false;
    if (n == 3) {
        out->r = (unsigned char)((v[0] << 4) | v[0]);
        out->g = (unsigned char)((v[1] << 4) | v[1]);
        out->b = (unsigned char)((v[2] << 4) | v[2]);
        out->a = 255;
    } else if (n == 4) {
        out->r = (unsigned char)((v[0] << 4) | v[0]);
        out->g = (unsigned char)((v[1] << 4) | v[1]);
        out->b = (unsigned char)((v[2] << 4) | v[2]);
        out->a = (unsigned char)((v[3] << 4) | v[3]);
    } else if (n == 6) {
        out->r = (unsigned char)((v[0] << 4) | v[1]);
        out->g = (unsigned char)((v[2] << 4) | v[3]);
        out->b = (unsigned char)((v[4] << 4) | v[5]);
        out->a = 255;
    } else { /* 8 */
        out->r = (unsigned char)((v[0] << 4) | v[1]);
        out->g = (unsigned char)((v[2] << 4) | v[3]);
        out->b = (unsigned char)((v[4] << 4) | v[5]);
        out->a = (unsigned char)((v[6] << 4) | v[7]);
    }
    return true;
}

/* ---- minimal JSON parsing (object of strings + nested object for "syntax") ---- */

typedef struct { const char *p, *end; } JP;
static void jp_ws(JP *j) { while (j->p < j->end && isspace((unsigned char)*j->p)) j->p++; }
static bool jp_eat(JP *j, char c) { jp_ws(j); if (j->p < j->end && *j->p == c) { j->p++; return true; } return false; }

static char *jp_string(JP *j) {
    jp_ws(j);
    if (j->p >= j->end || *j->p != '"') return NULL;
    j->p++;
    size_t cap = 32, len = 0;
    char *out = (char *)malloc(cap);
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\' && j->p < j->end) {
            char e = *j->p++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '"': c = '"';  break;
                case '\\': c = '\\'; break;
                default: c = e; break;
            }
        }
        if (len + 1 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
        out[len++] = c;
    }
    if (j->p < j->end) j->p++;
    out[len] = 0;
    return out;
}

/* Apply one "key": value pair to the theme. Returns true if we consumed
   a value cleanly. */
static bool apply_pair(Theme *t, const char *key, JP *j);

/* Walk an object, applying pairs */
static void parse_object(Theme *t, JP *j) {
    if (!jp_eat(j, '{')) return;
    while (j->p < j->end) {
        jp_ws(j);
        if (j->p < j->end && *j->p == '}') { j->p++; return; }
        char *k = jp_string(j);
        if (!k) return;
        jp_ws(j);
        if (!jp_eat(j, ':')) { free(k); return; }
        apply_pair(t, k, j);
        free(k);
        jp_ws(j);
        if (j->p < j->end && *j->p == ',') j->p++;
    }
}

/* Map syntax keys to palette indices */
static int palette_index(const char *k) {
    if (!strcmp(k, "default"))  return 0;
    if (!strcmp(k, "keyword"))  return 1;
    if (!strcmp(k, "string"))   return 2;
    if (!strcmp(k, "comment"))  return 3;
    if (!strcmp(k, "number"))   return 4;
    if (!strcmp(k, "type"))     return 5;
    if (!strcmp(k, "constant")) return 6;
    if (!strcmp(k, "function")) return 7;
    if (!strcmp(k, "operator")) return 8;
    return -1;
}

static void parse_syntax_obj(Theme *t, JP *j) {
    if (!jp_eat(j, '{')) return;
    while (j->p < j->end) {
        jp_ws(j);
        if (j->p < j->end && *j->p == '}') { j->p++; return; }
        char *k = jp_string(j);
        if (!k) return;
        jp_ws(j);
        if (!jp_eat(j, ':')) { free(k); return; }
        char *v = jp_string(j);
        if (v) {
            int idx = palette_index(k);
            Color c;
            if (idx >= 0 && parse_color(v, &c)) t->palette[idx] = c;
        }
        free(v);
        free(k);
        jp_ws(j);
        if (j->p < j->end && *j->p == ',') j->p++;
    }
}

static bool apply_pair(Theme *t, const char *key, JP *j) {
    jp_ws(j);
    if (j->p >= j->end) return false;

    if (strcmp(key, "syntax") == 0) { parse_syntax_obj(t, j); return true; }
    if (strcmp(key, "name") == 0) {
        char *v = jp_string(j);
        if (v) { strncpy(t->name, v, sizeof t->name - 1); t->name[sizeof t->name - 1] = 0; free(v); }
        return true;
    }

    char *v = jp_string(j);
    if (!v) return false;
    Color c;
    bool ok = parse_color(v, &c);
    free(v);
    if (!ok) return false;

    if      (!strcmp(key, "bg"))             t->bg = c;
    else if (!strcmp(key, "fg"))             t->fg = c;
    else if (!strcmp(key, "gutter_bg"))      t->gutter_bg = c;
    else if (!strcmp(key, "gutter_fg"))      t->gutter_fg = c;
    else if (!strcmp(key, "gutter_fg_cur"))  t->gutter_fg_cur = c;
    else if (!strcmp(key, "status_bg"))      t->status_bg = c;
    else if (!strcmp(key, "status_fg"))      t->status_fg = c;
    else if (!strcmp(key, "cursor"))         t->cursor = c;
    else if (!strcmp(key, "cursor_block"))   t->cursor_block = c;
    else if (!strcmp(key, "cursor_glyph"))   t->cursor_glyph = c;
    else if (!strcmp(key, "selection"))      t->selection = c;
    /* default field's palette[0] is also written via "fg" for convenience */
    if (!strcmp(key, "fg")) t->palette[0] = c;
    return true;
}

/* ---- built-in themes ---- */

void theme_dark(Theme *t) {
    memset(t, 0, sizeof *t);
    strcpy(t->name, "Dark");
    t->bg            = (Color){ 30,  34,  42, 255};
    t->fg            = (Color){220, 220, 220, 255};
    t->gutter_bg     = (Color){ 24,  27,  33, 255};
    t->gutter_fg     = (Color){ 90, 100, 115, 255};
    t->gutter_fg_cur = (Color){220, 220, 220, 255};
    t->status_bg     = (Color){ 40,  46,  56, 255};
    t->status_fg     = (Color){220, 220, 220, 255};
    t->cursor        = (Color){240, 240, 240, 255};
    t->cursor_block  = (Color){240, 240, 240,  80};
    t->cursor_glyph  = (Color){ 30,  34,  42, 255};
    t->selection     = (Color){ 60,  90, 130, 200};
    t->palette[0]    = (Color){220, 220, 220, 255}; /* default */
    t->palette[1]    = (Color){198, 120, 221, 255}; /* keyword */
    t->palette[2]    = (Color){152, 195, 121, 255}; /* string  */
    t->palette[3]    = (Color){ 92,  99, 112, 255}; /* comment */
    t->palette[4]    = (Color){209, 154, 102, 255}; /* number  */
    t->palette[5]    = (Color){229, 192, 123, 255}; /* type    */
    t->palette[6]    = (Color){229, 192, 123, 255}; /* const   */
    t->palette[7]    = (Color){ 97, 175, 239, 255}; /* func    */
    t->palette[8]    = (Color){171, 178, 191, 255}; /* op      */
}

void theme_light(Theme *t) {
    memset(t, 0, sizeof *t);
    strcpy(t->name, "Light");
    t->bg            = (Color){250, 250, 250, 255};
    t->fg            = (Color){ 40,  44,  52, 255};
    t->gutter_bg     = (Color){240, 240, 240, 255};
    t->gutter_fg     = (Color){160, 165, 170, 255};
    t->gutter_fg_cur = (Color){ 40,  44,  52, 255};
    t->status_bg     = (Color){230, 230, 230, 255};
    t->status_fg     = (Color){ 40,  44,  52, 255};
    t->cursor        = (Color){ 40,  44,  52, 255};
    t->cursor_block  = (Color){ 40,  44,  52,  80};
    t->cursor_glyph  = (Color){250, 250, 250, 255};
    t->selection     = (Color){180, 200, 240, 180};
    t->palette[0]    = (Color){ 40,  44,  52, 255}; /* default */
    t->palette[1]    = (Color){166,  38, 164, 255}; /* keyword */
    t->palette[2]    = (Color){ 80, 161,  79, 255}; /* string  */
    t->palette[3]    = (Color){160, 160, 160, 255}; /* comment */
    t->palette[4]    = (Color){200, 100,  20, 255}; /* number  */
    t->palette[5]    = (Color){180, 130,  20, 255}; /* type    */
    t->palette[6]    = (Color){180, 130,  20, 255}; /* const   */
    t->palette[7]    = (Color){ 64, 120, 192, 255}; /* func    */
    t->palette[8]    = (Color){ 90,  90,  90, 255}; /* op      */
}

bool theme_load_file(Theme *t, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    char *buf = (char *)malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = 0;
    fclose(f);
    JP j = { buf, buf + sz };
    parse_object(t, &j);
    free(buf);
    return true;
}
