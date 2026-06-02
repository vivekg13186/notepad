/*
 * notepad - a tiny Neovim-style editor in C using raylib.
 *
 * Usage:  ./notepad [file] [grammar.tmLanguage.json]
 *
 *   :w   :q   :wq   :e <file>   :s/a/b/[g]   :%s/a/b/g   :NN   /needle
 *
 * Modes:  ESC -> NORMAL, i/a -> INSERT, v -> VISUAL, : -> COMMAND, / -> SEARCH
 */

#include "raylib.h"
#include "editor.h"
#include "commands.h"
#include "syntax.h"
#include "theme.h"
#include "config.h"
#include "font_data.h"   /* embedded JetBrainsMono-Regular */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

#define PAD_X       8

/* Runtime config; populated from notepad.config.json before InitWindow */
Config g_config;

/* Convenience accessors so render code stays readable */
#define FONT_SIZE   ((float)g_config.font_size)
#define LINE_HEIGHT (g_config.line_height > 0 ? g_config.line_height : g_config.font_size + 4)
#define GUTTER_W    (g_config.gutter_w)

/* Active theme + syntax (mutable so :theme / :format can swap them). */
Theme   g_theme;
Syntax  g_syntax;
bool    g_syntax_loaded = false;
bool    g_help_open = false;

/* ---- Multi-buffer service ----
   Several files can be open at once; commands.c calls into these via extern
   declarations so it can swap the active buffer without knowing the
   internals. Always at least one buffer exists. */
#define MAX_BUFFERS 64
Editor  g_buffers[MAX_BUFFERS];
int     g_buf_count = 0;
int     g_buf_cur   = 0;

Editor *cur_ed(void) { return &g_buffers[g_buf_cur]; }

int new_buffer(void) {
    if (g_buf_count >= MAX_BUFFERS) return -1;
    int idx = g_buf_count++;
    ed_init(&g_buffers[idx]);
    return idx;
}

int find_buffer_by_filename(const char *path) {
    if (!path) return -1;
    for (int i = 0; i < g_buf_count; i++)
        if (strcmp(g_buffers[i].filename, path) == 0) return i;
    return -1;
}

void switch_to_buffer_idx(int idx); /* forward */

void close_buffer(int idx) {
    if (idx < 0 || idx >= g_buf_count) return;
    ed_free(&g_buffers[idx]);
    for (int i = idx; i < g_buf_count - 1; i++)
        g_buffers[i] = g_buffers[i + 1];
    g_buf_count--;
    if (g_buf_count == 0) new_buffer();         /* keep at least one buffer */
    if (g_buf_cur >= g_buf_count) g_buf_cur = g_buf_count - 1;
    if (g_buf_cur < 0) g_buf_cur = 0;
}

/* Generic picker state (used by theme + format pickers). */
#define PICKER_MAX 64

bool  g_picker_open  = false;
int   g_picker_index = 0;
Theme g_picker_saved;
static char g_picker_items[PICKER_MAX][64];
static int  g_picker_count = 0;

/* Format picker has its own state because we don't preview on hover. */
bool g_fmt_picker_open  = false;
int  g_fmt_picker_index = 0;
static char g_fmt_items[PICKER_MAX][64];     /* schema id */
static char g_fmt_labels[PICKER_MAX][80];    /* schema display label */
static int  g_fmt_count = 0;

/* Buffer picker — lists every open file. */
bool g_buf_picker_open  = false;
int  g_buf_picker_index = 0;

bool load_grammar(const char *path) {
    if (!path || !*path) return false;
    Syntax fresh = {0};
    if (syntax_load_tmlanguage(&fresh, path)) {
        if (g_syntax_loaded) syntax_free(&g_syntax);
        g_syntax = fresh;
        g_syntax_loaded = true;
        return true;
    }
    syntax_free(&fresh);
    return false;
}

/* Resolve a theme name and install it into g_theme.  Resolution order:
   themes-map key  →  built-in dark/light  →  themes/<name>.json  →  raw path. */
bool apply_theme_by_name(const char *name) {
    if (!name || !*name) return false;
    const ThemeEntry *te = config_find_theme(&g_config, name);
    if (te && theme_load_file(&g_theme, te->path)) return true;
    if (strcmp(name, "dark")  == 0) { theme_dark(&g_theme);  return true; }
    if (strcmp(name, "light") == 0) { theme_light(&g_theme); return true; }
    char path[512];
    snprintf(path, sizeof path, "themes/%s.json", name);
    if (theme_load_file(&g_theme, path)) return true;
    if (theme_load_file(&g_theme, name)) return true;
    return false;
}

/* Build the format picker list from config.schemas. */
void open_format_picker(void) {
    g_fmt_count = 0;
    for (int i = 0; i < g_config.n_schemas && g_fmt_count < PICKER_MAX; i++) {
        const SchemaEntry *s = &g_config.schemas[i];
        strncpy(g_fmt_items[g_fmt_count], s->id, 63);
        g_fmt_items[g_fmt_count][63] = 0;
        snprintf(g_fmt_labels[g_fmt_count], sizeof g_fmt_labels[0],
                 "%s  (%s)", s->name[0] ? s->name : s->id, s->id);
        g_fmt_count++;
    }
    if (g_fmt_count == 0) return;
    g_fmt_picker_index = 0;
    g_fmt_picker_open  = true;
}

/* Build the picker list (built-ins + every entry in config.themes, deduped),
   snapshot the current theme for ESC-restore, and apply the first item live. */
void open_theme_picker(void) {
    g_picker_count = 0;
    const char *builtins[] = { "dark", "light" };
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++) {
        if (g_picker_count < PICKER_MAX) {
            strncpy(g_picker_items[g_picker_count], builtins[i], 63);
            g_picker_items[g_picker_count++][63] = 0;
        }
    }
    for (int i = 0; i < g_config.n_themes && g_picker_count < PICKER_MAX; i++) {
        const char *n = g_config.themes[i].name;
        bool dup = false;
        for (int j = 0; j < g_picker_count; j++) {
            if (strcmp(g_picker_items[j], n) == 0) { dup = true; break; }
        }
        if (!dup) {
            strncpy(g_picker_items[g_picker_count], n, 63);
            g_picker_items[g_picker_count++][63] = 0;
        }
    }
    if (g_picker_count == 0) return;
    g_picker_saved = g_theme;
    g_picker_index = 0;
    apply_theme_by_name(g_picker_items[0]);
    g_picker_open = true;
}

/* Pick a grammar based on file extension, using the config schemas list. */
void auto_select_grammar(const char *filename) {
    if (!filename) return;
    const SchemaEntry *s = config_find_schema_by_ext(&g_config, filename);
    if (s && s->grammar[0]) load_grammar(s->grammar);
}

/* Make `idx` the active buffer and re-select the grammar for its filename. */
void switch_to_buffer_idx(int idx) {
    if (idx < 0 || idx >= g_buf_count) return;
    g_buf_cur = idx;
    auto_select_grammar(g_buffers[idx].filename);
}

void open_buffer_picker(void) {
    if (g_buf_count == 0) return;
    g_buf_picker_index = g_buf_cur;
    g_buf_picker_open  = true;
}

static float s_char_w = 10.0f;
Font g_font;   /* set once in main() after font load; used by mouse code */

static void draw_text(Font font, const char *s, float x, float y, Color c) {
    DrawTextEx(font, s, (Vector2){x, y}, FONT_SIZE, 0, c);
}

static void measure_char_w(Font font) {
    /* Average over many M's to avoid rounding error on a single glyph */
    const char *probe = "MMMMMMMMMMMMMMMMMMMM";
    Vector2 v = MeasureTextEx(font, probe, FONT_SIZE, 0);
    s_char_w = v.x / 20.0f;
    if (s_char_w <= 0.0f) s_char_w = 10.0f;
}

/* ---- lualine-style status bar helpers ---- */

static Color shade(Color c, int delta) {
    int r = (int)c.r + delta; if (r < 0) r = 0; if (r > 255) r = 255;
    int g = (int)c.g + delta; if (g < 0) g = 0; if (g > 255) g = 255;
    int b = (int)c.b + delta; if (b < 0) b = 0; if (b > 255) b = 255;
    return (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, c.a};
}

/* "B-section" colour: subtly distinct from status_bg in either direction. */
static Color seg_b_bg(Color base) {
    int sum = (int)base.r + (int)base.g + (int)base.b;
    return shade(base, (sum < 384) ? +18 : -18);
}

static void mode_colors(EditorMode m, const Theme *t, Color *bg, Color *fg, const char **label) {
    switch (m) {
        case MODE_INSERT:  *label = " INSERT ";  *bg = t->palette[2]; break;  /* green */
        case MODE_VISUAL:  *label = " VISUAL ";  *bg = t->palette[1]; break;  /* purple */
        case MODE_COMMAND: *label = " COMMAND "; *bg = t->palette[4]; break;  /* orange */
        case MODE_SEARCH:  *label = " SEARCH ";  *bg = t->palette[6]; break;  /* yellow */
        default:           *label = " NORMAL ";  *bg = t->palette[7]; break;  /* blue */
    }
    *fg = t->bg; /* high contrast against bright mode color */
}

static float draw_seg_left(Font font, float x, int y, int h, const char *txt, Color bg, Color fg) {
    float tw = MeasureTextEx(font, txt, FONT_SIZE, 0).x;
    float w  = tw + 6;
    DrawRectangle((int)x, y, (int)(w + 0.5f), h, bg);
    int ty = y + (h - g_config.font_size) / 2 - 1;
    draw_text(font, txt, x + 3, (float)ty, fg);
    return x + w;
}

static float draw_seg_right(Font font, float rx, int y, int h, const char *txt, Color bg, Color fg) {
    float tw = MeasureTextEx(font, txt, FONT_SIZE, 0).x;
    float w  = tw + 6;
    float x  = rx - w;
    DrawRectangle((int)x, y, (int)(w + 0.5f), h, bg);
    int ty = y + (h - g_config.font_size) / 2 - 1;
    draw_text(font, txt, x + 3, (float)ty, fg);
    return x;
}

/* Width in pixels of the first `n` chars of `s`, using the font's actual layout */
static float text_width_n(Font font, const char *s, size_t n) {
    if (n == 0) return 0.0f;
    char small[1024];
    if (n < sizeof small) {
        memcpy(small, s, n);
        small[n] = 0;
        return MeasureTextEx(font, small, FONT_SIZE, 0).x;
    }
    char *buf = (char *)malloc(n + 1);
    memcpy(buf, s, n);
    buf[n] = 0;
    float w = MeasureTextEx(font, buf, FONT_SIZE, 0).x;
    free(buf);
    return w;
}

/* Forward declarations for renderers defined below. */
static int render_tab_bar(Font font);

/* Scan from byte 0 to `end` deciding whether `end` lies inside a slash-star
   block comment.  Tracks line-comment and string state so a slash-slash that
   contains slash-star and a string that contains slash-star don't wrongly
   open a block.  Used to seed per-line state at the top of the viewport. */
static bool block_comment_state_at(Editor *ed, size_t end) {
    bool in_block = false, in_str = false, in_line = false;
    size_t L = gb_length(&ed->gb);
    if (end > L) end = L;
    for (size_t i = 0; i < end; i++) {
        char c = gb_char_at(&ed->gb, i);
        if (in_str) {
            if (c == '\\' && i + 1 < end) { i++; }
            else if (c == '"') { in_str = false; }
            else if (c == '\n') { in_str = false; }
            continue;
        }
        if (in_line) { if (c == '\n') in_line = false; continue; }
        if (in_block) {
            if (c == '*' && i + 1 < end && gb_char_at(&ed->gb, i + 1) == '/') {
                in_block = false; i++;
            }
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '/' && i + 1 < end) {
            char d = gb_char_at(&ed->gb, i + 1);
            if (d == '/') { in_line = true; i++; continue; }
            if (d == '*') { in_block = true; i++; continue; }
        }
    }
    return in_block;
}

/* Apply multi-line slash-star block-comment shading on top of whatever the
   line highlighter already produced.  Returns the updated in_block state
   for the next line. */
static bool apply_block_comment(const char *line, size_t llen, unsigned char *cols, bool in_block) {
    bool in_str = false, in_line = false;
    size_t i = 0;
    if (in_block) {
        size_t close = (size_t)-1;
        for (size_t k = 0; k + 1 < llen; k++) {
            if (line[k] == '*' && line[k + 1] == '/') { close = k + 2; break; }
        }
        if (close != (size_t)-1) {
            for (size_t k = 0; k < close && k < llen; k++) cols[k] = 3;
            i = close;
            in_block = false;
        } else {
            for (size_t k = 0; k < llen; k++) cols[k] = 3;
            return true;
        }
    }
    while (i < llen) {
        char c = line[i];
        if (in_str) {
            if (c == '\\' && i + 1 < llen) i += 2;
            else { if (c == '"') in_str = false; i++; }
            continue;
        }
        if (in_line) { i++; continue; }
        if (c == '"') { in_str = true; i++; continue; }
        if (c == '/' && i + 1 < llen) {
            if (line[i + 1] == '/') { in_line = true; i += 2; continue; }
            if (line[i + 1] == '*') {
                size_t close = (size_t)-1;
                for (size_t k = i + 2; k + 1 < llen; k++) {
                    if (line[k] == '*' && line[k + 1] == '/') { close = k + 2; break; }
                }
                if (close != (size_t)-1) {
                    for (size_t k = i; k < close; k++) cols[k] = 3;
                    i = close;
                } else {
                    for (size_t k = i; k < llen; k++) cols[k] = 3;
                    return true;
                }
                continue;
            }
        }
        i++;
    }
    return false;
}

static void render(Editor *ed, const Syntax *syn, Font font) {
    int W = GetScreenWidth();
    int H = GetScreenHeight();
    int status_h  = 20;                            /* lualine bar height */
    int cmdline_h = g_config.font_size + 6;        /* slim row for : prompts / messages */
    int cmdline_y = H - cmdline_h;
    int status_y  = cmdline_y - status_h;

    ClearBackground(g_theme.bg);

    /* Tab bar (only when multiple buffers); offsets everything else down. */
    int tab_h    = render_tab_bar(font);
    int editor_y = tab_h;
    ed->viewport_lines = (status_y - editor_y) / LINE_HEIGHT;

    /* Horizontal scroll: keep cursor visible by shifting viewport_x in pixels. */
    {
        size_t ln, col;
        ed_cursor_line_col(ed, &ln, &col);
        size_t lstart = ed_line_start(ed, ln);
        size_t llen   = ed_line_length(ed, ln);
        char *lbuf = (char *)malloc(llen + 1);
        gb_copy_range(&ed->gb, lstart, lstart + llen, lbuf);
        lbuf[llen] = 0;
        float cursor_px = text_width_n(font, lbuf, col);
        free(lbuf);
        float avail = (float)(W - GUTTER_W - PAD_X * 2);
        if (cursor_px - ed->viewport_x < 0)
            ed->viewport_x = cursor_px - s_char_w * 4;
        if (cursor_px - ed->viewport_x > avail)
            ed->viewport_x = cursor_px - avail + s_char_w * 4;
        if (ed->viewport_x < 0) ed->viewport_x = 0;
    }

    /* Gutter background */
    DrawRectangle(0, editor_y, GUTTER_W, status_y - editor_y, g_theme.gutter_bg);

    /* Multi-line block comment state at the top of the viewport. */
    size_t vp_byte = ed_line_start(ed, ed->viewport_line);
    bool in_block = block_comment_state_at(ed, vp_byte);

    size_t lc = ed_line_count(ed);
    size_t cur_line, cur_col;
    ed_cursor_line_col(ed, &cur_line, &cur_col);

    /* keep cursor in viewport */
    if (cur_line < ed->viewport_line) ed->viewport_line = cur_line;
    if ((long)cur_line >= (long)ed->viewport_line + ed->viewport_lines - 1)
        ed->viewport_line = cur_line - ed->viewport_lines + 2;

    /* Draw gutter line numbers in their own pass — they sit at x < GUTTER_W
       so they must be rendered before the horizontal-scroll scissor opens. */
    for (int row = 0; row < ed->viewport_lines && (size_t)row + ed->viewport_line < lc; row++) {
        size_t line_no = ed->viewport_line + (size_t)row;
        int y = editor_y + row * LINE_HEIGHT + 4;
        char buf[32];
        snprintf(buf, sizeof buf, "%4zu", line_no + 1);
        draw_text(font, buf, 4, y, line_no == cur_line ? g_theme.gutter_fg_cur : g_theme.gutter_fg);
    }

    /* selection range */
    size_t sel_s = 0, sel_e = 0;
    if (ed->has_selection) ed_get_selection(ed, &sel_s, &sel_e);

    /* Clip text drawing to the editor region so off-screen glyphs don't
       bleed onto the gutter. */
    BeginScissorMode(GUTTER_W, editor_y, W - GUTTER_W, status_y - editor_y);

    /* Draw lines */
    for (int row = 0; row < ed->viewport_lines && (size_t)row + ed->viewport_line < lc; row++) {
        size_t line_no = ed->viewport_line + (size_t)row;
        int y = editor_y + row * LINE_HEIGHT + 4;

        size_t lstart = ed_line_start(ed, line_no);
        size_t llen = ed_line_length(ed, line_no);
        if (llen > 2048) llen = 2048;

        /* extract line and highlight */
        char *line = (char *)malloc(llen + 1);
        gb_copy_range(&ed->gb, lstart, lstart + llen, line);
        line[llen] = 0;
        unsigned char *cols = (unsigned char *)malloc(llen + 1);
        memset(cols, 0, llen + 1);
        if (syn) syntax_highlight_line(syn, line, llen, cols);
        in_block = apply_block_comment(line, llen, cols, in_block);

        /* Walk chars once, computing exact x positions from MeasureTextEx
           so selection rects, glyphs, and cursor all line up perfectly. */
        float base_x = (float)(GUTTER_W + PAD_X) - ed->viewport_x;
        float fy = (float)y;

        /* selection background */
        if (ed->has_selection) {
            for (size_t k = 0; k < llen; k++) {
                size_t p = lstart + k;
                if (p >= sel_s && p < sel_e) {
                    float xs = base_x + text_width_n(font, line, k);
                    float xe = base_x + text_width_n(font, line, k + 1);
                    DrawRectangle((int)xs, y, (int)(xe - xs + 1), LINE_HEIGHT, g_theme.selection);
                }
            }
            size_t p_eol = lstart + llen;
            if (p_eol >= sel_s && p_eol < sel_e) {
                float xs = base_x + text_width_n(font, line, llen);
                DrawRectangle((int)xs, y, (int)(s_char_w / 2), LINE_HEIGHT, g_theme.selection);
            }
        }

        /* draw chars in spans of same color, advancing x by measured width */
        {
            size_t i = 0;
            float  x = base_x;
            while (i < llen) {
                size_t j = i;
                while (j < llen && cols[j] == cols[i]) j++;
                char save = line[j];
                line[j] = 0;
                draw_text(font, line + i, x, fy, g_theme.palette[cols[i]]);
                x += MeasureTextEx(font, line + i, FONT_SIZE, 0).x;
                line[j] = save;
                i = j;
            }
        }

        /* cursor on this row */
        if (line_no == cur_line) {
            float cx = base_x + text_width_n(font, line, cur_col);
            if (ed->mode == MODE_INSERT) {
                DrawRectangle((int)cx, y, 2, LINE_HEIGHT, g_theme.cursor);
            } else {
                /* block cursor */
                float cw = (cur_col < llen)
                    ? text_width_n(font, line + cur_col, 1)
                    : s_char_w;
                DrawRectangle((int)cx, y, (int)(cw + 0.5f), LINE_HEIGHT, g_theme.cursor_block);
                if (cur_col < llen) {
                    char one[2] = {line[cur_col], 0};
                    draw_text(font, one, cx, fy, g_theme.cursor_glyph);
                }
            }
        }

        free(line);
        free(cols);
    }

    EndScissorMode();

    /* ---- lualine-style segmented status bar ---- */
    {
        int sh = status_h;

        /* base fill (covers the gap between left and right segment groups) */
        DrawRectangle(0, status_y, W, sh, g_theme.status_bg);

        const char *mode_label;
        Color mode_bg, mode_fg;
        mode_colors(ed->mode, &g_theme, &mode_bg, &mode_fg, &mode_label);

        Color b_bg = seg_b_bg(g_theme.status_bg);
        Color b_fg = g_theme.fg;

        /* Left side: [MODE] [filename] [format] */
        char file_label[400];
        const char *fname = ed->filename[0] ? ed->filename : "[No Name]";
        const char *slash = strrchr(fname, '/');
        const char *base  = slash ? slash + 1 : fname;
        snprintf(file_label, sizeof file_label, " %s%s ", base, ed->dirty ? " ●" : "");

        const char *fmt_name = (g_syntax_loaded && g_syntax.name) ? g_syntax.name : "plain";
        char fmt_label[80];
        snprintf(fmt_label, sizeof fmt_label, " %s ", fmt_name);

        float x = 0;
        x = draw_seg_left(font, x, status_y, sh, mode_label, mode_bg, mode_fg);
        x = draw_seg_left(font, x, status_y, sh, file_label, b_bg,     b_fg);
        x = draw_seg_left(font, x, status_y, sh, fmt_label,  b_bg,     b_fg);

        /* Right side: [progress] [line:col] [theme-name] */
        size_t total = lc;
        char prog_label[16];
        if (total == 0 || cur_line == 0)            snprintf(prog_label, sizeof prog_label, " Top ");
        else if (cur_line + 1 >= total)             snprintf(prog_label, sizeof prog_label, " Bot ");
        else                                        snprintf(prog_label, sizeof prog_label, " %zu%% ", (cur_line + 1) * 100 / total);

        char loc_label[32];
        snprintf(loc_label, sizeof loc_label, " %zu:%zu ", cur_line + 1, cur_col + 1);

        char theme_label[80];
        snprintf(theme_label, sizeof theme_label, " %s ", g_theme.name[0] ? g_theme.name : "Theme");

        float rx = (float)W;
        rx = draw_seg_right(font, rx, status_y, sh, loc_label,   mode_bg, mode_fg);
        rx = draw_seg_right(font, rx, status_y, sh, prog_label,  b_bg,    b_fg);
        rx = draw_seg_right(font, rx, status_y, sh, theme_label, b_bg,    b_fg);

        (void)x; (void)rx;
    }

    /* dedicated slim cmdline / message row below the status bar */
    DrawRectangle(0, cmdline_y, W, cmdline_h, g_theme.bg);
    int cy = cmdline_y + (cmdline_h - g_config.font_size) / 2 - 1;
    if (ed->mode == MODE_COMMAND || ed->mode == MODE_SEARCH) {
        char cmd[600];
        snprintf(cmd, sizeof cmd, "%c%s",
                 ed->mode == MODE_COMMAND ? ':' : '/', ed->cmdline);
        draw_text(font, cmd, PAD_X, cy, g_theme.fg);
    } else if (ed->status[0]) {
        draw_text(font, ed->status, PAD_X, cy, g_theme.fg);
    }
}

/* ---------------- input ---------------- */

static void handle_normal(Editor *ed) {
    int c = GetCharPressed();
    while (c > 0) {
        switch (c) {
            case 'h': ed_move_left(ed); break;
            case 'l': ed_move_right(ed); break;
            case 'j': ed_move_down(ed); break;
            case 'k': ed_move_up(ed); break;
            case '0': ed_move_line_start(ed); break;
            case '$': ed_move_line_end(ed); break;
            case 'g': /* simple gg */ ed_move_doc_start(ed); break;
            case 'G': ed_move_doc_end(ed); break;
            case 'i': ed->mode = MODE_INSERT; break;
            case 'a': ed_move_right(ed); ed->mode = MODE_INSERT; break;
            case 'A': ed_move_line_end(ed); ed->mode = MODE_INSERT; break;
            case 'I': ed_move_line_start(ed); ed->mode = MODE_INSERT; break;
            case 'o': ed_move_line_end(ed); ed_insert_newline(ed); ed->mode = MODE_INSERT; break;
            case 'O': ed_move_line_start(ed); gb_insert_char(&ed->gb, ed->cursor, '\n'); ed->mode = MODE_INSERT; break;
            case 'x': ed_delete_char(ed); break;
            case 'v': ed_start_selection(ed); ed->mode = MODE_VISUAL; break;
            case 'y': /* yank line */ {
                size_t ln, col; ed_cursor_line_col(ed, &ln, &col);
                size_t s = ed_line_start(ed, ln);
                size_t e = s + ed_line_length(ed, ln);
                free(ed->clipboard);
                ed->clipboard_len = e - s;
                ed->clipboard = (char *)malloc(ed->clipboard_len + 2);
                gb_copy_range(&ed->gb, s, e, ed->clipboard);
                ed->clipboard[ed->clipboard_len++] = '\n';
                ed->clipboard[ed->clipboard_len] = 0;
                ed_set_status(ed, "yanked line");
                break;
            }
            case 'p': ed_paste(ed); break;
            case 'd': /* dd: delete current line */ {
                size_t ln, col; ed_cursor_line_col(ed, &ln, &col);
                size_t s = ed_line_start(ed, ln);
                size_t e = s + ed_line_length(ed, ln);
                if (e < gb_length(&ed->gb)) e++;
                gb_delete(&ed->gb, s, e - s);
                ed->cursor = s;
                ed->dirty = true;
                break;
            }
            case 'u': ed_undo(ed); ed_set_status(ed, "undo"); break;
            case ':': ed->mode = MODE_COMMAND; ed->cmdline[0] = 0; ed->cmdlen = 0; break;
            case '/': ed->mode = MODE_SEARCH;  ed->cmdline[0] = 0; ed->cmdlen = 0; break;
            case 'n': if (ed->last_search[0]) {
                if (!ed_search_forward(ed, ed->last_search, ed->cursor + 1))
                    ed_set_status(ed, "not found");
            } break;
            default: break;
        }
        c = GetCharPressed();
    }
    /* arrow keys */
    if (IsKeyPressed(KEY_LEFT))  ed_move_left(ed);
    if (IsKeyPressed(KEY_RIGHT)) ed_move_right(ed);
    if (IsKeyPressed(KEY_UP))    ed_move_up(ed);
    if (IsKeyPressed(KEY_DOWN))  ed_move_down(ed);
    if (IsKeyPressed(KEY_HOME))  ed_move_line_start(ed);
    if (IsKeyPressed(KEY_END))   ed_move_line_end(ed);
}

/* Encode a Unicode codepoint as UTF-8 into out[0..n).  Returns n. */
static int utf8_encode_cp(int cp, char out[5]) {
    if (cp < 0)        { out[0] = 0; return 0; }
    if (cp < 0x80)     { out[0] = (char)cp;                                    return 1; }
    if (cp < 0x800)    { out[0] = (char)(0xC0 |  (cp >> 6));
                         out[1] = (char)(0x80 |  (cp        & 0x3F));         return 2; }
    if (cp < 0x10000)  { out[0] = (char)(0xE0 |  (cp >> 12));
                         out[1] = (char)(0x80 | ((cp >> 6)  & 0x3F));
                         out[2] = (char)(0x80 |  (cp        & 0x3F));         return 3; }
                       { out[0] = (char)(0xF0 |  (cp >> 18));
                         out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                         out[2] = (char)(0x80 | ((cp >> 6)  & 0x3F));
                         out[3] = (char)(0x80 |  (cp        & 0x3F));         return 4; }
}

static void handle_insert(Editor *ed) {
    int c = GetCharPressed();
    while (c > 0) {
        if (c < 128) {
            if (c >= 32) ed_insert_char(ed, (char)c);
        } else {
            char buf[5];
            int n = utf8_encode_cp(c, buf);
            for (int i = 0; i < n; i++) ed_insert_char(ed, buf[i]);
        }
        c = GetCharPressed();
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressedRepeat(KEY_ENTER)) ed_insert_newline(ed);
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) ed_backspace(ed);
    if (IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE))    ed_delete_char(ed);
    if (IsKeyPressed(KEY_TAB)) {
        /* Snippet expansion: look at the word ending at the cursor and
           check the config snippets map under the active grammar's name. */
        bool expanded = false;
        if (g_syntax_loaded && g_syntax.name && ed->cursor > 0) {
            size_t s = ed->cursor;
            while (s > 0) {
                char c = gb_char_at(&ed->gb, s - 1);
                if (!(isalnum((unsigned char)c) || c == '_' || c == '-' ||
                      c == ':' || c == '!' || c == '?')) break;
                s--;
            }
            size_t wlen = ed->cursor - s;
            if (wlen > 0 && wlen < 31) {
                char trig[32];
                for (size_t i = 0; i < wlen; i++) trig[i] = gb_char_at(&ed->gb, s + i);
                trig[wlen] = 0;
                const SnippetEntry *sn = config_find_snippet(&g_config, g_syntax.name, trig);
                if (sn && sn->body) {
                    ed_delete_at(ed, s, wlen);
                    size_t bodylen = strlen(sn->body);
                    ed_insert_str_at(ed, s, sn->body, bodylen);
                    expanded = true;
                }
            }
        }
        if (!expanded) {
            for (int k = 0; k < g_config.tab_width; k++) ed_insert_char(ed, ' ');
        }
    }
    if (IsKeyPressed(KEY_LEFT))  ed_move_left(ed);
    if (IsKeyPressed(KEY_RIGHT)) ed_move_right(ed);
    if (IsKeyPressed(KEY_UP))    ed_move_up(ed);
    if (IsKeyPressed(KEY_DOWN))  ed_move_down(ed);
    if (IsKeyPressed(KEY_HOME))  ed_move_line_start(ed);
    if (IsKeyPressed(KEY_END))   ed_move_line_end(ed);
    if (IsKeyPressed(KEY_ESCAPE)) ed->mode = MODE_NORMAL;
}

static void handle_visual(Editor *ed) {
    int c = GetCharPressed();
    while (c > 0) {
        switch (c) {
            case 'h': ed_move_left(ed); break;
            case 'l': ed_move_right(ed); break;
            case 'j': ed_move_down(ed); break;
            case 'k': ed_move_up(ed); break;
            case '0': ed_move_line_start(ed); break;
            case '$': ed_move_line_end(ed); break;
            case 'y': ed_copy_selection(ed); ed_clear_selection(ed); ed->mode = MODE_NORMAL; ed_set_status(ed, "yanked"); break;
            case 'd':
            case 'x': ed_cut_selection(ed); ed->mode = MODE_NORMAL; break;
            case 'p': ed_paste(ed); ed->mode = MODE_NORMAL; break;
            default: break;
        }
        c = GetCharPressed();
    }
    if (IsKeyPressed(KEY_LEFT))  ed_move_left(ed);
    if (IsKeyPressed(KEY_RIGHT)) ed_move_right(ed);
    if (IsKeyPressed(KEY_UP))    ed_move_up(ed);
    if (IsKeyPressed(KEY_DOWN))  ed_move_down(ed);
    if (IsKeyPressed(KEY_ESCAPE)) { ed_clear_selection(ed); ed->mode = MODE_NORMAL; }
}

static void handle_command_or_search(Editor *ed) {
    int c = GetCharPressed();
    while (c > 0) {
        if (c >= 32 && c < 127 && ed->cmdlen + 1 < sizeof ed->cmdline) {
            ed->cmdline[ed->cmdlen++] = (char)c;
            ed->cmdline[ed->cmdlen] = 0;
        }
        c = GetCharPressed();
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        if (ed->cmdlen > 0) { ed->cmdline[--ed->cmdlen] = 0; }
        else { ed->mode = MODE_NORMAL; }
    }
    if (IsKeyPressed(KEY_ESCAPE)) { ed->mode = MODE_NORMAL; ed->cmdline[0] = 0; ed->cmdlen = 0; }
    if (IsKeyPressed(KEY_ENTER)) {
        if (ed->mode == MODE_COMMAND) {
            cmd_execute(ed, ed->cmdline);
        } else if (ed->mode == MODE_SEARCH) {
            strncpy(ed->last_search, ed->cmdline, sizeof ed->last_search - 1);
            ed->last_search[sizeof ed->last_search - 1] = 0;
            if (!ed_search_forward(ed, ed->last_search, ed->cursor + 1))
                ed_set_status(ed, "not found");
        }
        ed->mode = MODE_NORMAL;
        ed->cmdline[0] = 0; ed->cmdlen = 0;
    }
}

/* Buffer-word autocomplete: walk the gap buffer, find unique words that
   start with `prefix`, return the longest common prefix of all matches
   (so multiple presses don't cycle — they extend as far as they can). */
static bool is_ident_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static bool autocomplete_at_cursor(Editor *ed) {
    /* word prefix at cursor */
    size_t end = ed->cursor;
    size_t start = end;
    while (start > 0 && is_ident_byte((unsigned char)gb_char_at(&ed->gb, start - 1)))
        start--;
    if (start == end) return false;

    size_t plen = end - start;
    char pref[64];
    if (plen >= sizeof pref) return false;
    for (size_t i = 0; i < plen; i++) pref[i] = gb_char_at(&ed->gb, start + i);
    pref[plen] = 0;

    /* scan buffer for words starting with prefix */
    char matches[32][64];
    int  nmatch = 0;
    size_t L = gb_length(&ed->gb);
    for (size_t i = 0; i + plen <= L && nmatch < 32; i++) {
        if (i == start) continue;
        if (i > 0 && is_ident_byte((unsigned char)gb_char_at(&ed->gb, i - 1))) continue;
        bool ok = true;
        for (size_t k = 0; k < plen && ok; k++)
            if (gb_char_at(&ed->gb, i + k) != pref[k]) ok = false;
        if (!ok) continue;
        size_t j = i + plen;
        char word[64];
        memcpy(word, pref, plen);
        size_t wlen = plen;
        while (j < L && wlen + 1 < sizeof word) {
            unsigned char c = (unsigned char)gb_char_at(&ed->gb, j);
            if (!is_ident_byte(c)) break;
            word[wlen++] = (char)c;
            j++;
        }
        word[wlen] = 0;
        if (wlen == plen) continue;
        bool dup = false;
        for (int m = 0; m < nmatch; m++)
            if (strcmp(matches[m], word) == 0) { dup = true; break; }
        if (!dup) { strncpy(matches[nmatch++], word, sizeof matches[0] - 1); matches[nmatch - 1][sizeof matches[0] - 1] = 0; }
    }

    if (nmatch == 0) return false;

    /* longest common prefix across all matches */
    size_t common = strlen(matches[0]);
    for (int i = 1; i < nmatch; i++) {
        size_t k = 0;
        while (k < common && matches[i][k] == matches[0][k]) k++;
        common = k;
    }
    if (common <= plen) {
        /* prefix already as long as it can get without picking one — insert
           the first match's remainder as a best-effort guess. */
        const char *m = matches[0];
        ed_insert_str_at(ed, ed->cursor, m + plen, strlen(m) - plen);
        return true;
    }
    /* extend to common prefix only */
    ed_insert_str_at(ed, ed->cursor, matches[0] + plen, common - plen);
    return true;
}

/* ---- system clipboard helpers (Ctrl/Cmd C / V / X) ---- */

static bool is_mod_down(void) {
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)
        || IsKeyDown(KEY_LEFT_SUPER)   || IsKeyDown(KEY_RIGHT_SUPER);
}

/* Replace ed->clipboard with `data` (len bytes) and mirror to the OS. */
static void clipboard_store(Editor *ed, const char *data, size_t len) {
    free(ed->clipboard);
    ed->clipboard = (char *)malloc(len + 1);
    memcpy(ed->clipboard, data, len);
    ed->clipboard[len] = 0;
    ed->clipboard_len = len;
    SetClipboardText(ed->clipboard);
}

/* Copy the active selection if any; otherwise copy the current line. */
static void clipboard_copy(Editor *ed) {
    if (ed->has_selection) {
        size_t s, e;
        ed_get_selection(ed, &s, &e);
        if (e <= s) return;
        char *buf = (char *)malloc(e - s + 1);
        gb_copy_range(&ed->gb, s, e, buf);
        buf[e - s] = 0;
        clipboard_store(ed, buf, e - s);
        free(buf);
        ed_set_status(ed, "copied selection");
    } else {
        size_t ln, col;
        ed_cursor_line_col(ed, &ln, &col);
        size_t s = ed_line_start(ed, ln);
        size_t e = s + ed_line_length(ed, ln);
        char *buf = (char *)malloc(e - s + 2);
        gb_copy_range(&ed->gb, s, e, buf);
        buf[e - s] = '\n';
        buf[e - s + 1] = 0;
        clipboard_store(ed, buf, e - s + 1);
        free(buf);
        ed_set_status(ed, "copied line");
    }
}

/* Insert system clipboard at the cursor (or replace selection). */
static void clipboard_paste(Editor *ed) {
    const char *sys = GetClipboardText();
    if (!sys || !*sys) return;
    size_t len = strlen(sys);
    /* mirror into ed->clipboard so subsequent `p` reuses the same payload */
    clipboard_store(ed, sys, len);
    if (ed->has_selection) ed_delete_selection(ed);
    gb_insert_str(&ed->gb, ed->cursor, ed->clipboard, ed->clipboard_len);
    ed->cursor += ed->clipboard_len;
    ed->dirty = true;
    ed_set_status(ed, "pasted");
}

/* Cut = copy + delete (selection or current line). */
static void clipboard_cut(Editor *ed) {
    clipboard_copy(ed);
    if (ed->has_selection) {
        ed_delete_selection(ed);
    } else {
        size_t ln, col;
        ed_cursor_line_col(ed, &ln, &col);
        size_t s = ed_line_start(ed, ln);
        size_t e = s + ed_line_length(ed, ln);
        if (e < gb_length(&ed->gb)) e++;   /* swallow the trailing newline */
        gb_delete(&ed->gb, s, e - s);
        ed->cursor = s;
        ed->dirty = true;
    }
    ed_set_status(ed, "cut");
}

static bool handle_clipboard_shortcuts(Editor *ed) {
    if (!is_mod_down()) return false;
    /* Only meaningful for editor modes — pickers / help / cmdline are skipped
       at the caller. */
    if (IsKeyPressed(KEY_C)) { clipboard_copy(ed);
                               if (ed->mode == MODE_VISUAL) {
                                   ed_clear_selection(ed);
                                   ed->mode = MODE_NORMAL;
                               }
                               return true; }
    if (IsKeyPressed(KEY_X)) { clipboard_cut(ed);
                               if (ed->mode == MODE_VISUAL) ed->mode = MODE_NORMAL;
                               return true; }
    if (IsKeyPressed(KEY_V)) { clipboard_paste(ed);
                               if (ed->mode == MODE_VISUAL) ed->mode = MODE_NORMAL;
                               return true; }
    /* Ctrl+Z = undo,  Ctrl+Shift+Z / Ctrl+R = redo */
    if (IsKeyPressed(KEY_Z)) {
        if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
            ed_redo(ed); ed_set_status(ed, "redo");
        } else {
            ed_undo(ed); ed_set_status(ed, "undo");
        }
        return true;
    }
    if (IsKeyPressed(KEY_R)) { ed_redo(ed); ed_set_status(ed, "redo"); return true; }
    /* Ctrl+/ — toggle comment on current line / selected lines */
    if (IsKeyPressed(KEY_SLASH)) {
        extern void toggle_comment_lines(Editor *);
        toggle_comment_lines(ed);
        ed_set_status(ed, "toggled comment");
        return true;
    }
    /* Ctrl+N — buffer-word autocomplete */
    if (IsKeyPressed(KEY_N)) {
        if (!autocomplete_at_cursor(ed)) ed_set_status(ed, "no completion");
        return true;
    }
    return false;
}

/* Map a screen (x, y) to a byte position in the buffer.  Used by mouse code. */
static size_t pos_at_mouse(Editor *ed, float mx, float my, int editor_y) {
    int row = (int)((my - editor_y) / LINE_HEIGHT);
    if (row < 0) row = 0;
    size_t lc = ed_line_count(ed);
    if (lc == 0) return 0;
    size_t line_no = ed->viewport_line + (size_t)row;
    if (line_no >= lc) line_no = lc - 1;
    size_t lstart = ed_line_start(ed, line_no);
    size_t llen   = ed_line_length(ed, line_no);
    char *line = (char *)malloc(llen + 1);
    gb_copy_range(&ed->gb, lstart, lstart + llen, line);
    line[llen] = 0;
    float base_x = (float)(GUTTER_W + PAD_X) - ed->viewport_x;
    size_t k = 0;
    float x = base_x;
    while (k < llen) {
        unsigned char c = (unsigned char)line[k];
        int cb = (c & 0x80) == 0    ? 1
               : (c & 0xE0) == 0xC0 ? 2
               : (c & 0xF0) == 0xE0 ? 3
               : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (k + (size_t)cb > llen) cb = 1;
        float w = text_width_n(g_font, line + k, (size_t)cb);
        if (mx < x + w * 0.5f) break;
        x += w;
        k += (size_t)cb;
    }
    free(line);
    return lstart + k;
}

/* Click within the tab strip → return the buffer index or -1. */
static int buf_at_mouse_x(float mx) {
    if (g_buf_count <= 1) return -1;
    float x = 0;
    for (int i = 0; i < g_buf_count; i++) {
        const char *fname = g_buffers[i].filename[0]
                            ? g_buffers[i].filename : "[No Name]";
        const char *slash = strrchr(fname, '/');
        const char *base  = slash ? slash + 1 : fname;
        char label[200];
        snprintf(label, sizeof label, "  %s%s  ",
                 base, g_buffers[i].dirty ? " ●" : "");
        float tw = MeasureTextEx(g_font, label, FONT_SIZE, 0).x;
        if (mx >= x && mx < x + tw) return i;
        x += tw;
    }
    return -1;
}

/* Wheel scroll + click/drag.  No-op when a picker / help is open (the modal
   guards in handle_input return before calling this). */
static void handle_mouse(Editor *ed) {
    int H = GetScreenHeight();
    int status_h  = 20;
    int cmdline_h = g_config.font_size + 6;
    int status_y  = H - cmdline_h - status_h;
    int tab_h     = (g_buf_count > 1) ? 24 : 0;

    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        int steps = (int)(wheel * 3.0f);
        if (steps == 0) steps = (wheel > 0) ? 1 : -1;
        size_t lc = ed_line_count(ed);
        if (steps > 0) {
            while (steps-- > 0 && ed->viewport_line > 0) ed->viewport_line--;
        } else {
            while (steps++ < 0 && ed->viewport_line + 1 < lc) ed->viewport_line++;
        }
    }

    Vector2 mp = GetMousePosition();
    static bool dragging = false;
    static size_t mouse_anchor = 0;

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        /* Tab bar click → switch buffer */
        if (tab_h > 0 && mp.y >= 0 && mp.y < tab_h) {
            int idx = buf_at_mouse_x(mp.x);
            if (idx >= 0) switch_to_buffer_idx(idx);
            return;
        }
        /* Editor click */
        if (mp.x > GUTTER_W && mp.y >= tab_h && mp.y < status_y) {
            size_t p = pos_at_mouse(ed, mp.x, mp.y, tab_h);
            ed->cursor = p;
            mouse_anchor = p;
            ed->sel_anchor = p;
            ed->has_selection = true;     /* anchor==cursor → empty, made
                                             real only once the mouse moves */
            dragging = true;
            if (ed->mode == MODE_VISUAL || ed->mode == MODE_COMMAND ||
                ed->mode == MODE_SEARCH)
                ed->mode = MODE_NORMAL;
        }
    } else if (dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (mp.x > GUTTER_W && mp.y >= tab_h && mp.y < status_y) {
            ed->cursor = pos_at_mouse(ed, mp.x, mp.y, tab_h);
        }
    } else if (dragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (ed->cursor == mouse_anchor) ed->has_selection = false;
        dragging = false;
    }
}

static void handle_input(Editor *ed) {
    /* Buffer picker. */
    if (g_buf_picker_open) {
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN))
            g_buf_picker_index = (g_buf_picker_index + 1) % g_buf_count;
        if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP))
            g_buf_picker_index = (g_buf_picker_index - 1 + g_buf_count) % g_buf_count;
        if (IsKeyPressed(KEY_ENTER)) {
            switch_to_buffer_idx(g_buf_picker_index);
            g_buf_picker_open = false;
        }
        if (IsKeyPressed(KEY_ESCAPE)) g_buf_picker_open = false;
        while (GetCharPressed() > 0) {}
        return;
    }

    /* Format picker (no live preview — Enter commits, Esc cancels). */
    if (g_fmt_picker_open) {
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) {
            g_fmt_picker_index = (g_fmt_picker_index + 1) % g_fmt_count;
        }
        if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) {
            g_fmt_picker_index = (g_fmt_picker_index - 1 + g_fmt_count) % g_fmt_count;
        }
        if (IsKeyPressed(KEY_ENTER)) {
            const char *id = g_fmt_items[g_fmt_picker_index];
            const SchemaEntry *s = config_find_schema_by_id(&g_config, id);
            char msg[400];
            if (s && s->grammar[0]) {
                if (load_grammar(s->grammar))
                    snprintf(msg, sizeof msg, "format: %s",
                             s->name[0] ? s->name : s->id);
                else
                    snprintf(msg, sizeof msg, "grammar file missing: %s",
                             s->grammar);
                ed_set_status(ed, msg);
            }
            g_fmt_picker_open = false;
        }
        if (IsKeyPressed(KEY_ESCAPE)) g_fmt_picker_open = false;
        while (GetCharPressed() > 0) {}
        return;
    }

    /* Theme picker is modal too. */
    if (g_picker_open) {
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) {
            g_picker_index = (g_picker_index + 1) % g_picker_count;
            apply_theme_by_name(g_picker_items[g_picker_index]);
        }
        if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) {
            g_picker_index = (g_picker_index - 1 + g_picker_count) % g_picker_count;
            apply_theme_by_name(g_picker_items[g_picker_index]);
        }
        if (IsKeyPressed(KEY_ENTER)) {
            char msg[128];
            snprintf(msg, sizeof msg, "theme: %s", g_picker_items[g_picker_index]);
            ed_set_status(ed, msg);
            g_picker_open = false;
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            g_theme = g_picker_saved;  /* restore */
            g_picker_open = false;
        }
        /* eat any text input while picker is open */
        while (GetCharPressed() > 0) {}
        return;
    }

    /* Help dialog is modal: swallow input until any key closes it. */
    if (g_help_open) {
        if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER) ||
            IsKeyPressed(KEY_SPACE)  || GetCharPressed() > 0) {
            g_help_open = false;
        }
        return;
    }

    /* System clipboard shortcuts — intercept before mode dispatch so the
       same Ctrl+C / Ctrl+V / Ctrl+X work in NORMAL / INSERT / VISUAL.
       Skipped while typing :commands or /search so those keys still type. */
    if (ed->mode != MODE_COMMAND && ed->mode != MODE_SEARCH) {
        if (handle_clipboard_shortcuts(ed)) {
            /* Drain any character event the modifier+key generated so it
               doesn't get re-interpreted by the mode handler below. */
            while (GetCharPressed() > 0) {}
            return;
        }
    }

    /* Mouse: wheel scroll + click/drag. Skipped in COMMAND/SEARCH so the
       prompt keeps focus. */
    if (ed->mode != MODE_COMMAND && ed->mode != MODE_SEARCH) handle_mouse(ed);

    switch (ed->mode) {
        case MODE_NORMAL:  handle_normal(ed); break;
        case MODE_INSERT:  handle_insert(ed); break;
        case MODE_VISUAL:  handle_visual(ed); break;
        case MODE_COMMAND:
        case MODE_SEARCH:  handle_command_or_search(ed); break;
    }
}

/* ---- help dialog ---- */

static const char *HELP_TITLE  = "notepad — commands";
static const char *HELP_FOOTER = "Press any key to close";

static const char *HELP_COL1[] = {
    "Files & Buffers",
    "",
    ":w               save",
    ":w <file>        save as",
    ":wq              save + quit",
    ":q   :q!         quit / force",
    ":new  :n         new buffer",
    ":o               open dialog",
    ":o <file>        open file",
    ":bn   :next      next buffer",
    ":bp   :prev      prev buffer",
    ":b N             switch to N",
    ":bd  :close      close buffer",
    ":bd!             force-close",
    ":t   :ls         buffer list",
    ":!cmd            run shell cmd",
    NULL
};

static const char *HELP_COL2[] = {
    "Editing & Navigation",
    "",
    ":NN  :goto N     go to line",
    ":find <text>     forward search",
    ":replace a b     replace first",
    ":replace-all a b replace every",
    ":comment  :c     toggle comment",
    ":undo  :redo     undo / redo",
    "",
    "Modes  ESC i v / :",
    "Move   h j k l 0 $ gg G",
    "Edit   x dd y p u n",
    "Ctrl+C/V/X   copy/paste/cut",
    "Ctrl+Z/R     undo / redo",
    "Ctrl+N       complete word",
    "Ctrl+/       toggle comment",
    "Tab          expand snippet",
    "Mouse: wheel/click/drag/tab",
    NULL
};

static const char *HELP_COL3[] = {
    "Themes & Formats",
    "",
    ":theme           pick a theme",
    ":theme <name>    set theme",
    ":themes          alias",
    "",
    ":format          pick a format",
    ":format <name>   set grammar",
    ":formats         alias",
    "",
    ":help            this dialog",
    NULL
};

static int help_col_len(const char **col) {
    int n = 0; while (col[n]) n++; return n;
}

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void render_buffer_picker(Font font) {
    int W = GetScreenWidth(), H = GetScreenHeight();
    DrawRectangle(0, 0, W, H, (Color){0, 0, 0, 140});

    int line_h = LINE_HEIGHT;
    int box_w  = 600;
    int rows   = g_buf_count + 3;
    int box_h  = line_h * rows + 24;
    int box_x  = (W - box_w) / 2;
    int box_y  = (H - box_h) / 2;

    DrawRectangle(box_x, box_y, box_w, box_h, g_theme.status_bg);
    DrawRectangleLines(box_x, box_y, box_w, box_h, g_theme.fg);

    draw_text(font, "Buffers",
              (float)(box_x + 16), (float)(box_y + 10), g_theme.palette[1]);

    int y = box_y + 10 + line_h + 6;
    for (int i = 0; i < g_buf_count; i++) {
        if (i == g_buf_picker_index) {
            DrawRectangle(box_x + 8, y - 2, box_w - 16, line_h + 2,
                          (Color){g_theme.palette[7].r, g_theme.palette[7].g,
                                  g_theme.palette[7].b, 60});
        }
        const char *fname = g_buffers[i].filename[0] ? g_buffers[i].filename : "[No Name]";
        char row[480];
        snprintf(row, sizeof row, "%s %d  %s%s",
                 (i == g_buf_picker_index) ? ">" : " ",
                 i + 1, fname,
                 g_buffers[i].dirty ? "  ●" : "");
        draw_text(font, row,
                  (float)(box_x + 16), (float)y,
                  (i == g_buf_picker_index) ? g_theme.palette[7] : g_theme.fg);
        y += line_h;
    }

    draw_text(font, "↑/↓ select   Enter switch   Esc cancel",
              (float)(box_x + 16), (float)(box_y + box_h - line_h - 8),
              g_theme.palette[3]);
}

/* Tab bar drawn above the gutter when more than one buffer is open.
   Returns the bar height (0 when only one buffer is open, so callers can
   use it as a y-offset for everything below). */
static int render_tab_bar(Font font) {
    if (g_buf_count <= 1) return 0;
    int W = GetScreenWidth();
    int h = 24;
    DrawRectangle(0, 0, W, h, g_theme.gutter_bg);

    float x = 0;
    for (int i = 0; i < g_buf_count; i++) {
        const char *base = basename_of(g_buffers[i].filename[0]
                                       ? g_buffers[i].filename : "[No Name]");
        char label[160];
        snprintf(label, sizeof label, "  %s%s  ",
                 base, g_buffers[i].dirty ? " ●" : "");

        float tw = MeasureTextEx(font, label, FONT_SIZE, 0).x;
        Color bg = (i == g_buf_cur) ? g_theme.bg : g_theme.gutter_bg;
        Color fg = (i == g_buf_cur) ? g_theme.fg : g_theme.gutter_fg;
        DrawRectangle((int)x, 0, (int)(tw + 0.5f), h, bg);
        /* underline the active tab in the function color */
        if (i == g_buf_cur)
            DrawRectangle((int)x, h - 2, (int)(tw + 0.5f), 2, g_theme.palette[7]);
        int ty = (h - g_config.font_size) / 2 - 1;
        draw_text(font, label, x, (float)ty, fg);
        x += tw;
    }
    /* divider line at the bottom of the bar */
    DrawRectangle(0, h - 1, W, 1, g_theme.gutter_bg);
    return h;
}

static void render_format_picker(Font font) {
    int W = GetScreenWidth(), H = GetScreenHeight();
    DrawRectangle(0, 0, W, H, (Color){0, 0, 0, 140});

    int line_h = LINE_HEIGHT;
    int box_w  = 420;
    int rows   = g_fmt_count + 3;
    int box_h  = line_h * rows + 24;
    int box_x  = (W - box_w) / 2;
    int box_y  = (H - box_h) / 2;

    DrawRectangle(box_x, box_y, box_w, box_h, g_theme.status_bg);
    DrawRectangleLines(box_x, box_y, box_w, box_h, g_theme.fg);

    draw_text(font, "Formats",
              (float)(box_x + 16), (float)(box_y + 10), g_theme.palette[1]);

    int y = box_y + 10 + line_h + 6;
    for (int i = 0; i < g_fmt_count; i++) {
        if (i == g_fmt_picker_index) {
            DrawRectangle(box_x + 8, y - 2, box_w - 16, line_h + 2,
                          (Color){g_theme.palette[7].r, g_theme.palette[7].g,
                                  g_theme.palette[7].b, 60});
        }
        char buf[128];
        snprintf(buf, sizeof buf, "%s %s",
                 (i == g_fmt_picker_index) ? ">" : " ", g_fmt_labels[i]);
        draw_text(font, buf,
                  (float)(box_x + 16), (float)y,
                  (i == g_fmt_picker_index) ? g_theme.palette[7] : g_theme.fg);
        y += line_h;
    }

    draw_text(font, "↑/↓ select   Enter apply   Esc cancel",
              (float)(box_x + 16), (float)(box_y + box_h - line_h - 8),
              g_theme.palette[3]);
}

static void render_theme_picker(Font font) {
    int W = GetScreenWidth(), H = GetScreenHeight();
    DrawRectangle(0, 0, W, H, (Color){0, 0, 0, 140});

    int line_h = LINE_HEIGHT;
    int box_w  = 360;
    int rows   = g_picker_count + 3;            /* title + spacer + items + footer */
    int box_h  = line_h * rows + 24;
    int box_x  = (W - box_w) / 2;
    int box_y  = (H - box_h) / 2;

    DrawRectangle(box_x, box_y, box_w, box_h, g_theme.status_bg);
    DrawRectangleLines(box_x, box_y, box_w, box_h, g_theme.fg);

    /* title */
    draw_text(font, "Themes",
              (float)(box_x + 16), (float)(box_y + 10), g_theme.palette[1]);

    int y = box_y + 10 + line_h + 6;
    for (int i = 0; i < g_picker_count; i++) {
        if (i == g_picker_index) {
            DrawRectangle(box_x + 8, y - 2, box_w - 16, line_h + 2,
                          (Color){g_theme.palette[7].r, g_theme.palette[7].g,
                                  g_theme.palette[7].b, 60});
        }
        const char *label = g_picker_items[i];
        char buf[96];
        snprintf(buf, sizeof buf, "%s %s",
                 (i == g_picker_index) ? ">" : " ", label);
        draw_text(font, buf,
                  (float)(box_x + 16), (float)y,
                  (i == g_picker_index) ? g_theme.palette[7] : g_theme.fg);
        y += line_h;
    }

    draw_text(font, "↑/↓ select   Enter apply   Esc cancel",
              (float)(box_x + 16), (float)(box_y + box_h - line_h - 8),
              g_theme.palette[3]);
}

static void render_help(Font font) {
    int W = GetScreenWidth(), H = GetScreenHeight();
    /* dim everything */
    DrawRectangle(0, 0, W, H, (Color){0, 0, 0, 160});

    int line_h = LINE_HEIGHT;
    int n1 = help_col_len(HELP_COL1);
    int n2 = help_col_len(HELP_COL2);
    int n3 = help_col_len(HELP_COL3);
    int max_n = n1 > n2 ? n1 : n2;
    if (n3 > max_n) max_n = n3;

    /* layout: title row + blank + columns + blank + footer */
    int col_w = 320;
    int box_w = col_w * 3 + 40;
    int box_h = line_h * (max_n + 4) + 16;
    if (box_w > W - 20) { box_w = W - 20; col_w = (box_w - 40) / 3; }
    if (box_h > H - 20)   box_h = H - 20;
    int box_x = (W - box_w) / 2;
    int box_y = (H - box_h) / 2;

    DrawRectangle(box_x, box_y, box_w, box_h, g_theme.status_bg);
    DrawRectangleLines(box_x, box_y, box_w, box_h, g_theme.fg);

    /* title (keyword color) */
    draw_text(font, HELP_TITLE,
              (float)(box_x + 20), (float)(box_y + 12),
              g_theme.palette[1]);

    /* divider under the title */
    DrawRectangle(box_x + 16, box_y + 12 + line_h + 2,
                  box_w - 32, 1, g_theme.gutter_fg);

    int content_y = box_y + 12 + line_h + 10;
    const char **cols[3]    = { HELP_COL1, HELP_COL2, HELP_COL3 };
    int          counts[3]  = { n1, n2, n3 };

    for (int c = 0; c < 3; c++) {
        int cx = box_x + 20 + c * col_w;
        for (int i = 0; i < counts[c]; i++) {
            const char *txt = cols[c][i];
            if (!txt || !*txt) continue;
            Color col = g_theme.fg;
            if (i == 0) col = g_theme.palette[7];   /* column header in func color */
            draw_text(font, txt,
                      (float)cx, (float)(content_y + i * line_h), col);
        }
    }

    /* footer (comment color) */
    draw_text(font, HELP_FOOTER,
              (float)(box_x + 20),
              (float)(box_y + box_h - line_h - 6),
              g_theme.palette[3]);
}

/* ---------------- main ---------------- */

int main(int argc, char **argv) {
    /* Always start with one empty scratch buffer. */
    new_buffer();

    /* Layered config: defaults < exe-dir notepad.config.json < cwd notepad.config.json. */
    config_load_layered(&g_config, argv[0]);

    /* CLI args override config */
    const char *file    = (argc > 1) ? argv[1] : NULL;
    const char *grammar = (argc > 2) ? argv[2] : g_config.grammar;
    const char *themef  = (argc > 3) ? argv[3] : NULL;

    /* Apply theme from config (built-in name or path), then CLI override. */
    if      (!strcmp(g_config.theme, "dark"))  theme_dark(&g_theme);
    else if (!strcmp(g_config.theme, "light")) theme_light(&g_theme);
    else {
        theme_dark(&g_theme);
        if (!theme_load_file(&g_theme, g_config.theme)) {
            /* try themes/<name>.json */
            char p[512];
            snprintf(p, sizeof p, "themes/%s.json", g_config.theme);
            theme_load_file(&g_theme, p);
        }
    }
    if (themef && !theme_load_file(&g_theme, themef))
        fprintf(stderr, "warning: could not load theme '%s'\n", themef);

    if (file) ed_load(cur_ed(), file);

    /* Grammar: prefer schema match by file extension, then fallback path. */
    if (file) auto_select_grammar(file);
    if (!g_syntax_loaded && grammar && *grammar) load_grammar(grammar);

    /* MSAA smooths shape/line edges; HighDPI gives Retina-native rendering;
       VSYNC hint avoids tearing; RESIZABLE lets the user drag the window
       — the renderer pulls W/H from GetScreenWidth/Height each frame so the
       layout reflows automatically. All must be set before InitWindow. */
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT |
                   FLAG_WINDOW_HIGHDPI | FLAG_WINDOW_RESIZABLE);
    InitWindow(g_config.window_w, g_config.window_h, "notepad");
    SetTargetFPS(60);
    SetExitKey(0);

    /* Load the JetBrainsMono atlas at a fixed BASE size, not at the
       rendering size.  DrawTextEx scales it via bilinear sampling, and the
       oversized atlas (~3-4x our usual font_size) carries enough anti-
       aliased detail to read crisp at any UI size.  Same trick z-console
       uses to stay sharp across font sizes. */
    const int BASE_FONT = 64;
    Font font = LoadFontFromMemory(".ttf",
                                   JetBrainsMono_Regular_ttf,
                                   (int)JetBrainsMono_Regular_ttf_len,
                                   BASE_FONT, NULL, 0);
    if (font.texture.id == 0) font = GetFontDefault();
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    measure_char_w(font);
    g_font = font;

    if (g_syntax_loaded) ed_set_status(cur_ed(), "loaded grammar");
    else                 ed_set_status(cur_ed(), "no grammar matched");

    while (!WindowShouldClose() && !cur_ed()->quit) {
        handle_input(cur_ed());
        BeginDrawing();
        render(cur_ed(), g_syntax_loaded ? &g_syntax : NULL, font);
        if (g_buf_picker_open) render_buffer_picker(font);
        if (g_picker_open)     render_theme_picker(font);
        if (g_fmt_picker_open) render_format_picker(font);
        if (g_help_open)       render_help(font);
        EndDrawing();
    }

    UnloadFont(font);
    CloseWindow();
    if (g_syntax_loaded) syntax_free(&g_syntax);
    for (int i = 0; i < g_buf_count; i++) ed_free(&g_buffers[i]);
    return 0;
}
