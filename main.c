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

void load_grammar(const char *path) {
    if (!path || !*path) return;
    Syntax fresh = {0};
    if (syntax_load_tmlanguage(&fresh, path)) {
        if (g_syntax_loaded) syntax_free(&g_syntax);
        g_syntax = fresh;
        g_syntax_loaded = true;
    } else {
        syntax_free(&fresh);
    }
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

static float s_char_w = 10.0f;

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

static void render(Editor *ed, const Syntax *syn, Font font) {
    int W = GetScreenWidth();
    int H = GetScreenHeight();
    int status_h  = 20;                            /* lualine bar height */
    int cmdline_h = g_config.font_size + 6;        /* slim row for : prompts / messages */
    int cmdline_y = H - cmdline_h;
    int status_y  = cmdline_y - status_h;
    ed->viewport_lines = status_y / LINE_HEIGHT;

    ClearBackground(g_theme.bg);

    /* Gutter background */
    DrawRectangle(0, 0, GUTTER_W, status_y, g_theme.gutter_bg);

    size_t lc = ed_line_count(ed);
    size_t cur_line, cur_col;
    ed_cursor_line_col(ed, &cur_line, &cur_col);

    /* keep cursor in viewport */
    if (cur_line < ed->viewport_line) ed->viewport_line = cur_line;
    if ((long)cur_line >= (long)ed->viewport_line + ed->viewport_lines - 1)
        ed->viewport_line = cur_line - ed->viewport_lines + 2;

    /* selection range */
    size_t sel_s = 0, sel_e = 0;
    if (ed->has_selection) ed_get_selection(ed, &sel_s, &sel_e);

    /* Draw lines */
    for (int row = 0; row < ed->viewport_lines && (size_t)row + ed->viewport_line < lc; row++) {
        size_t line_no = ed->viewport_line + (size_t)row;
        int y = row * LINE_HEIGHT + 4;

        /* gutter line numbers */
        char buf[32];
        snprintf(buf, sizeof buf, "%4zu", line_no + 1);
        draw_text(font, buf, 4, y, line_no == cur_line ? g_theme.gutter_fg_cur : g_theme.gutter_fg);

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

        /* Walk chars once, computing exact x positions from MeasureTextEx
           so selection rects, glyphs, and cursor all line up perfectly. */
        float base_x = (float)(GUTTER_W + PAD_X);
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
            case 'u': /* no undo */ ed_set_status(ed, "(undo not implemented)"); break;
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

static void handle_insert(Editor *ed) {
    int c = GetCharPressed();
    while (c > 0) {
        if (c >= 32 && c < 127) ed_insert_char(ed, (char)c);
        c = GetCharPressed();
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressedRepeat(KEY_ENTER)) ed_insert_newline(ed);
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) ed_backspace(ed);
    if (IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE))    ed_delete_char(ed);
    if (IsKeyPressed(KEY_TAB))       { for (int k = 0; k < g_config.tab_width; k++) ed_insert_char(ed, ' '); }
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

static void handle_input(Editor *ed) {
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
            if (s && s->grammar[0]) {
                load_grammar(s->grammar);
                char msg[160];
                snprintf(msg, sizeof msg, "format: %s",
                         s->name[0] ? s->name : s->id);
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
    switch (ed->mode) {
        case MODE_NORMAL:  handle_normal(ed); break;
        case MODE_INSERT:  handle_insert(ed); break;
        case MODE_VISUAL:  handle_visual(ed); break;
        case MODE_COMMAND:
        case MODE_SEARCH:  handle_command_or_search(ed); break;
    }
}

/* ---- help dialog ---- */

static const char *HELP_LINES[] = {
    "notepad — commands",
    "",
    ":s                  save",
    ":s <file>           save as (supports ~/ paths)",
    ":sq                 save and quit",
    ":q                  quit (fails if dirty)",
    ":q!                 force quit",
    ":o <file>           open file (supports ~/ paths)",
    ":NN  /  :goto NN    go to line NN",
    ":find <text>        search forward for text",
    ":replace a b        replace first occurrence of a with b",
    ":replace-all a b    replace every occurrence of a with b",
    ":theme              open the themes dropdown",
    ":theme <name>       switch color theme directly",
    ":format             open the formats dropdown",
    ":format <name>      switch syntax grammar directly",
    ":help               show this dialog",
    "",
    "Modes: ESC = NORMAL,  i = INSERT,  v = VISUAL,  / = SEARCH",
    "Normal keys: h j k l  0 $ gg G  i a A I o O  x dd y p  v  n",
    "",
    "Press any key to close",
};

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

    int n_lines = (int)(sizeof HELP_LINES / sizeof HELP_LINES[0]);
    int line_h  = LINE_HEIGHT;
    int box_w   = 640;
    int box_h   = line_h * (n_lines + 2) + 20;
    int box_x   = (W - box_w) / 2;
    int box_y   = (H - box_h) / 2;

    DrawRectangle(box_x, box_y, box_w, box_h, g_theme.status_bg);
    DrawRectangleLines(box_x, box_y, box_w, box_h, g_theme.fg);

    for (int i = 0; i < n_lines; i++) {
        Color c = g_theme.fg;
        if (i == 0)                                  c = g_theme.palette[1]; /* title in keyword color */
        else if (HELP_LINES[i][0] == 0)              continue;
        else if (i == n_lines - 1)                   c = g_theme.palette[3]; /* footer in comment color */
        draw_text(font, HELP_LINES[i],
                  (float)(box_x + 20),
                  (float)(box_y + 14 + i * line_h),
                  c);
    }
}

/* ---------------- main ---------------- */

int main(int argc, char **argv) {
    Editor ed;
    ed_init(&ed);

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

    if (file) ed_load(&ed, file);

    /* Grammar: prefer schema match by file extension, then fallback path. */
    if (file) auto_select_grammar(file);
    if (!g_syntax_loaded && grammar && *grammar) load_grammar(grammar);

    /* MSAA smooths shape/line edges; HighDPI gives Retina-native rendering.
       VSYNC hint avoids tearing.  All must be set before InitWindow. */
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | FLAG_WINDOW_HIGHDPI);
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

    if (g_syntax_loaded) ed_set_status(&ed, "loaded grammar");
    else                 ed_set_status(&ed, "no grammar matched");

    while (!WindowShouldClose() && !ed.quit) {
        handle_input(&ed);
        BeginDrawing();
        render(&ed, g_syntax_loaded ? &g_syntax : NULL, font);
        if (g_picker_open)     render_theme_picker(font);
        if (g_fmt_picker_open) render_format_picker(font);
        if (g_help_open)       render_help(font);
        EndDrawing();
    }

    UnloadFont(font);
    CloseWindow();
    if (g_syntax_loaded) syntax_free(&g_syntax);
    ed_free(&ed);
    return 0;
}
