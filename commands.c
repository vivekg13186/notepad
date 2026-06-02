#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "commands.h"
#include "theme.h"
#include "config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

/* defined in main.c */
extern Theme  g_theme;
extern Config g_config;
extern bool   g_help_open;
bool load_grammar(const char *path);
void auto_select_grammar(const char *filename);
void open_theme_picker(void);
void open_format_picker(void);
bool apply_theme_by_name(const char *name);

/* Active syntax (defined in main.c) so we can look the current format up
   in the schemas table to find its line_comment prefix. */
extern bool   g_syntax_loaded;
#include "syntax.h"
extern Syntax g_syntax;

/* Multi-buffer service (defined in main.c) */
extern Editor g_buffers[];
extern int    g_buf_count;
extern int    g_buf_cur;
int  new_buffer(void);
void close_buffer(int idx);
void switch_to_buffer_idx(int idx);
int  find_buffer_by_filename(const char *path);
void open_buffer_picker(void);

static void set_status(Editor *ed, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ed->status, sizeof ed->status, fmt, ap);
    va_end(ap);
    ed->status_until = 0.0;
}

/* Expand a leading ~ to $HOME. Other paths are copied verbatim. */
static void expand_path(char *dst, size_t dst_sz, const char *src) {
    if (src[0] == '~' && (src[1] == '/' || src[1] == 0)) {
        const char *home = getenv("HOME");
        if (home && *home) {
            snprintf(dst, dst_sz, "%s%s", home, src + 1);
            return;
        }
    }
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = 0;
}

/* Buffer-wide plain-text find/replace.  Used by :replace. */
int cmd_substitute(Editor *ed, const char *pattern, const char *replacement, bool global, bool all_lines) {
    char *full = gb_to_cstr(&ed->gb);
    if (!full) return 0;
    size_t plen = strlen(pattern);
    size_t rlen = strlen(replacement);
    if (plen == 0) { free(full); return 0; }

    size_t doc_len = strlen(full);
    size_t r_start = 0, r_end = doc_len;
    if (!all_lines) {
        size_t ln, col;
        ed_cursor_line_col(ed, &ln, &col);
        r_start = ed_line_start(ed, ln);
        r_end = r_start + ed_line_length(ed, ln);
    }

    size_t cap = doc_len + 64;
    char *out = (char *)malloc(cap);
    size_t olen = 0;
    int replaced = 0;

    if (olen + r_start + 1 >= cap) { cap = (olen + r_start + 1) * 2; out = (char *)realloc(out, cap); }
    memcpy(out + olen, full, r_start);
    olen += r_start;

    size_t i = r_start;
    bool did_one = false;
    while (i < r_end) {
        if ((global || !did_one) && i + plen <= r_end && memcmp(full + i, pattern, plen) == 0) {
            if (olen + rlen + 1 >= cap) { cap = (olen + rlen + 1) * 2; out = (char *)realloc(out, cap); }
            memcpy(out + olen, replacement, rlen);
            olen += rlen;
            i += plen;
            replaced++;
            did_one = true;
        } else {
            if (olen + 2 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
            out[olen++] = full[i++];
        }
    }
    if (olen + (doc_len - r_end) + 1 >= cap) {
        cap = (olen + (doc_len - r_end) + 1) * 2;
        out = (char *)realloc(out, cap);
    }
    memcpy(out + olen, full + r_end, doc_len - r_end);
    olen += doc_len - r_end;
    out[olen] = 0;

    gb_free(&ed->gb);
    gb_init(&ed->gb, olen + 64);
    gb_insert_str(&ed->gb, 0, out, olen);
    if (ed->cursor > olen) ed->cursor = olen;
    ed->dirty = true;
    free(out);
    free(full);
    return replaced;
}

static void trim(char *s) {
    char *p = s; while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t L = strlen(s);
    while (L > 0 && isspace((unsigned char)s[L - 1])) s[--L] = 0;
}

/* Read either a "quoted token" or a bare word into `out`; returns pointer
   to the next char after the token. */
static const char *read_token(const char *p, char *out, size_t out_sz) {
    while (*p == ' ' || *p == '\t') p++;
    size_t n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (n + 1 < out_sz) out[n++] = *p;
            p++;
        }
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') {
            if (n + 1 < out_sz) out[n++] = *p;
            p++;
        }
    }
    out[n] = 0;
    return p;
}

/* Common popen-and-trim helper used by the open / save dialogs. */
static bool run_dialog(const char *cmd, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return false;
    out[0] = 0;
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    char *got = fgets(out, (int)out_sz, p);
    pclose(p);
    if (!got) return false;
    size_t L = strlen(out);
    while (L > 0 && (out[L - 1] == '\n' || out[L - 1] == '\r')) out[--L] = 0;
    return out[0] != 0;
}

/* Open the OS's native "choose file" dialog and write the absolute path
   into out. Returns false if the user cancelled or the helper failed.
   - macOS: osascript (AppleScript `choose file`)
   - Linux: zenity, falling back to kdialog */
static bool pick_file(char *out, size_t out_sz) {
#if defined(__APPLE__)
    const char *cmd =
        "osascript "
        "-e 'try' "
        "-e 'POSIX path of (choose file with prompt \"Open file in notepad\")' "
        "-e 'on error' "
        "-e 'return \"\"' "
        "-e 'end try' 2>/dev/null";
#elif defined(__linux__)
    const char *cmd =
        "zenity --file-selection --title='Open file in notepad' 2>/dev/null "
        "|| kdialog --getopenfilename 2>/dev/null";
#else
    (void)out; (void)out_sz;
    return false;
#endif
    return run_dialog(cmd, out, out_sz);
}

/* Native "save as" dialog — user picks a destination filename. */
static bool pick_save_file(char *out, size_t out_sz, const char *suggested) {
#if defined(__APPLE__)
    char cmd[1024];
    if (suggested && *suggested) {
        snprintf(cmd, sizeof cmd,
            "osascript "
            "-e 'try' "
            "-e 'POSIX path of (choose file name with prompt \"Save as\" "
            "default name \"%s\")' "
            "-e 'on error' "
            "-e 'return \"\"' "
            "-e 'end try' 2>/dev/null",
            suggested);
    } else {
        snprintf(cmd, sizeof cmd,
            "osascript "
            "-e 'try' "
            "-e 'POSIX path of (choose file name with prompt \"Save as\")' "
            "-e 'on error' "
            "-e 'return \"\"' "
            "-e 'end try' 2>/dev/null");
    }
#elif defined(__linux__)
    char cmd[1024];
    if (suggested && *suggested) {
        snprintf(cmd, sizeof cmd,
            "zenity --file-selection --save --confirm-overwrite "
            "--filename='%s' --title='Save as' 2>/dev/null "
            "|| kdialog --getsavefilename '%s' 2>/dev/null",
            suggested, suggested);
    } else {
        snprintf(cmd, sizeof cmd,
            "zenity --file-selection --save --confirm-overwrite "
            "--title='Save as' 2>/dev/null "
            "|| kdialog --getsavefilename 2>/dev/null");
    }
#else
    (void)out; (void)out_sz; (void)suggested;
    return false;
#endif
    return run_dialog(cmd, out, out_sz);
}

static bool do_save_as(Editor *ed, const char *arg) {
    char path[512];
    expand_path(path, sizeof path, arg);
    strncpy(ed->filename, path, sizeof ed->filename - 1);
    ed->filename[sizeof ed->filename - 1] = 0;
    return ed_save(ed);
}

/* ---- comment-line toggle ----
   Returns the configured line comment for the active grammar, falling back
   to "// " when no schema matches. */
static const char *active_line_comment(void) {
    if (g_syntax_loaded && g_syntax.name) {
        const SchemaEntry *s = config_find_schema_by_id(&g_config, g_syntax.name);
        if (s && s->line_comment[0]) return s->line_comment;
    }
    return "// ";
}

static bool line_starts_with(Editor *ed, size_t line_no, const char *prefix, size_t plen) {
    size_t ls = ed_line_start(ed, line_no);
    size_t llen = ed_line_length(ed, line_no);
    if (llen < plen) return false;
    for (size_t i = 0; i < plen; i++)
        if (gb_char_at(&ed->gb, ls + i) != prefix[i]) return false;
    return true;
}

void toggle_comment_lines(Editor *ed) {
    const char *prefix = active_line_comment();
    size_t plen = strlen(prefix);
    if (plen == 0) return;

    /* determine the inclusive line range */
    size_t l1, l2;
    if (ed->has_selection) {
        size_t s, e; ed_get_selection(ed, &s, &e);
        size_t save = ed->cursor, ln, col;
        ed->cursor = s; ed_cursor_line_col(ed, &ln, &col); l1 = ln;
        ed->cursor = e ? e - 1 : e; ed_cursor_line_col(ed, &ln, &col); l2 = ln;
        ed->cursor = save;
        if (l2 < l1) { size_t t = l1; l1 = l2; l2 = t; }
    } else {
        size_t col; ed_cursor_line_col(ed, &l1, &col); l2 = l1;
    }

    /* are ALL lines already commented? if so we uncomment, else we comment */
    bool all_commented = true;
    for (size_t L = l1; L <= l2; L++) {
        if (!line_starts_with(ed, L, prefix, plen)) { all_commented = false; break; }
    }

    /* iterate bottom-up so removing/inserting doesn't shift later line starts */
    for (size_t L = l2 + 1; L > l1; L--) {
        size_t line = L - 1;
        size_t ls = ed_line_start(ed, line);
        if (all_commented) ed_delete_at(ed, ls, plen);
        else               ed_insert_str_at(ed, ls, prefix, plen);
    }
}

bool cmd_execute(Editor *ed, const char *line_in) {
    char line[512];
    strncpy(line, line_in, sizeof line - 1);
    line[sizeof line - 1] = 0;
    trim(line);

    if (line[0] == 0) return true;

    /* numeric goto: ":42" */
    bool numeric = true;
    for (char *p = line; *p; p++) if (!isdigit((unsigned char)*p)) { numeric = false; break; }
    if (numeric) {
        size_t L = (size_t)atol(line);
        ed_goto_line(ed, L);
        set_status(ed, "line %zu", L);
        return true;
    }

    /* ---- quit ---- */
    if (strcmp(line, "q") == 0) {
        if (ed->dirty) { set_status(ed, "unsaved changes (use :q! to force)"); return true; }
        ed->quit = true;
        return false;
    }
    if (strcmp(line, "q!") == 0) { ed->quit = true; return false; }

    /* ---- save ---- :w saves in place when the buffer has a real filename,
       otherwise it falls through to the native save-as dialog. */
    if (strcmp(line, "w") == 0 || strcmp(line, "wq") == 0) {
        bool quit_after = (strcmp(line, "wq") == 0);
        bool untitled   = (ed->filename[0] == 0
                           || strcmp(ed->filename, "[No Name]") == 0);

        if (untitled) {
            char picked[1024];
            if (!pick_save_file(picked, sizeof picked, "")) {
                set_status(ed, "save cancelled");
                return true;
            }
            if (!do_save_as(ed, picked)) {
                set_status(ed, "save failed: %s", picked);
                return true;
            }
            set_status(ed, "saved %s", ed->filename);
        } else {
            if (!ed_save(ed)) {
                set_status(ed, "save failed: %s", ed->filename);
                return true;
            }
            set_status(ed, "saved %s", ed->filename);
        }
        if (quit_after) { ed->quit = true; return false; }
        return true;
    }
    if (strncmp(line, "w ", 2) == 0) {
        if (do_save_as(ed, line + 2)) set_status(ed, "saved %s", ed->filename);
        else                          set_status(ed, "save failed: %s", ed->filename);
        return true;
    }

    /* ---- open ---- */
    if (strcmp(line, "o") == 0) {
        char picked[1024];
        if (!pick_file(picked, sizeof picked)) {
            set_status(ed, "open cancelled");
            return true;
        }
        /* Already open? Just switch. */
        int existing = find_buffer_by_filename(picked);
        if (existing >= 0) {
            switch_to_buffer_idx(existing);
            set_status(&g_buffers[g_buf_cur], "switched to %s", picked);
            return true;
        }
        /* Reuse current buffer if it's untitled+empty; otherwise add one. */
        Editor *target = ed;
        if (ed->dirty || (ed->filename[0] && strcmp(ed->filename, "[No Name]"))) {
            int idx = new_buffer();
            if (idx < 0) { set_status(ed, "too many buffers"); return true; }
            switch_to_buffer_idx(idx);
            target = &g_buffers[idx];
        }
        if (ed_load(target, picked)) {
            auto_select_grammar(target->filename);
            set_status(target, "opened %s", target->filename);
        } else {
            set_status(target, "open failed: %s", picked);
        }
        return true;
    }
    if (strncmp(line, "o ", 2) == 0) {
        char path[512];
        expand_path(path, sizeof path, line + 2);
        int existing = find_buffer_by_filename(path);
        if (existing >= 0) {
            switch_to_buffer_idx(existing);
            set_status(&g_buffers[g_buf_cur], "switched to %s", path);
            return true;
        }
        Editor *target = ed;
        if (ed->dirty || (ed->filename[0] && strcmp(ed->filename, "[No Name]"))) {
            int idx = new_buffer();
            if (idx < 0) { set_status(ed, "too many buffers"); return true; }
            switch_to_buffer_idx(idx);
            target = &g_buffers[idx];
        }
        if (ed_load(target, path)) {
            auto_select_grammar(target->filename);
            set_status(target, "opened %s", target->filename);
        } else {
            set_status(target, "open failed: %s", path);
        }
        return true;
    }

    /* ---- buffer switching ---- */
    if (strcmp(line, "bn") == 0 || strcmp(line, "next") == 0) {
        if (g_buf_count > 1) {
            switch_to_buffer_idx((g_buf_cur + 1) % g_buf_count);
            set_status(&g_buffers[g_buf_cur], "buf %d/%d %s",
                       g_buf_cur + 1, g_buf_count,
                       g_buffers[g_buf_cur].filename);
        } else {
            set_status(ed, "only one buffer");
        }
        return true;
    }
    if (strcmp(line, "bp") == 0 || strcmp(line, "prev") == 0) {
        if (g_buf_count > 1) {
            switch_to_buffer_idx((g_buf_cur - 1 + g_buf_count) % g_buf_count);
            set_status(&g_buffers[g_buf_cur], "buf %d/%d %s",
                       g_buf_cur + 1, g_buf_count,
                       g_buffers[g_buf_cur].filename);
        } else {
            set_status(ed, "only one buffer");
        }
        return true;
    }
    if (strcmp(line, "bd") == 0 || strcmp(line, "close") == 0) {
        if (ed->dirty) {
            set_status(ed, "unsaved changes (use :bd! to force)");
            return true;
        }
        close_buffer(g_buf_cur);
        return true;
    }
    if (strcmp(line, "bd!") == 0) { close_buffer(g_buf_cur); return true; }
    if (strncmp(line, "b ", 2) == 0) {
        int n = atoi(line + 2);                     /* 1-based */
        if (n >= 1 && n <= g_buf_count) {
            switch_to_buffer_idx(n - 1);
        } else {
            set_status(ed, "no buffer %d (have %d)", n, g_buf_count);
        }
        return true;
    }
    if (strcmp(line, "ls") == 0 || strcmp(line, "buffers") == 0 ||
        strcmp(line, "t")  == 0 || strcmp(line, "tabs")    == 0) {
        open_buffer_picker();
        return true;
    }

    if (strncmp(line, "goto ", 5) == 0) {
        ed_goto_line(ed, (size_t)atol(line + 5));
        return true;
    }

    /* ---- find ---- */
    if (strncmp(line, "find ", 5) == 0) {
        const char *needle = line + 5;
        while (*needle == ' ') needle++;
        if (!*needle) { set_status(ed, "usage: :find <text>"); return true; }
        strncpy(ed->last_search, needle, sizeof ed->last_search - 1);
        ed->last_search[sizeof ed->last_search - 1] = 0;
        if (ed_search_forward(ed, needle, ed->cursor + 1))
            set_status(ed, "found '%s'", needle);
        else
            set_status(ed, "not found: %s", needle);
        return true;
    }

    /* ---- replace-all <find> <replace>   (every occurrence in the buffer) ---- */
    if (strncmp(line, "replace-all ", 12) == 0) {
        char a[256], b[256];
        const char *p = line + 12;
        p = read_token(p, a, sizeof a);
        p = read_token(p, b, sizeof b);
        if (!*a) { set_status(ed, "usage: :replace-all <find> <replace>"); return true; }
        int n = cmd_substitute(ed, a, b, /*global=*/true, /*all_lines=*/true);
        set_status(ed, "%d replacements", n);
        return true;
    }

    /* ---- replace <find> <replace>   (first occurrence in the buffer) ---- */
    if (strncmp(line, "replace ", 8) == 0) {
        char a[256], b[256];
        const char *p = line + 8;
        p = read_token(p, a, sizeof a);
        p = read_token(p, b, sizeof b);
        if (!*a) { set_status(ed, "usage: :replace <find> <replace>"); return true; }
        int n = cmd_substitute(ed, a, b, /*global=*/false, /*all_lines=*/true);
        set_status(ed, n ? "replaced 1" : "no match");
        return true;
    }

    /* ---- theme picker (bare :theme or :themes) ---- */
    if (strcmp(line, "theme") == 0 || strcmp(line, "themes") == 0) {
        open_theme_picker();
        return true;
    }

    /* ---- theme by name (:theme <name|path>) ---- */
    if (strncmp(line, "theme ", 6) == 0) {
        const char *arg = line + 6;
        while (*arg == ' ') arg++;
        if (!*arg) { set_status(ed, "usage: :theme <name|path>"); return true; }
        if (apply_theme_by_name(arg)) set_status(ed, "theme: %s", arg);
        else                          set_status(ed, "theme not found: %s", arg);
        return true;
    }

    /* ---- format picker (bare :format or :formats) ---- */
    if (strcmp(line, "format") == 0 || strcmp(line, "formats") == 0) {
        open_format_picker();
        return true;
    }

    /* ---- format by id (:format <schema>) ---- */
    if (strncmp(line, "format ", 7) == 0) {
        const char *arg = line + 7;
        while (*arg == ' ') arg++;
        if (!*arg) { set_status(ed, "usage: :format <schema>"); return true; }
        const SchemaEntry *s = config_find_schema_by_id(&g_config, arg);
        if (!s || !s->grammar[0]) {
            set_status(ed, "no schema matched: %s", arg);
            return true;
        }
        if (load_grammar(s->grammar))
            set_status(ed, "format: %s", s->name[0] ? s->name : s->id);
        else
            set_status(ed, "grammar file missing: %s", s->grammar);
        return true;
    }

    /* ---- undo / redo ---- */
    if (strcmp(line, "undo") == 0) { ed_undo(ed); set_status(ed, "undo"); return true; }
    if (strcmp(line, "redo") == 0) { ed_redo(ed); set_status(ed, "redo"); return true; }

    /* ---- comment / uncomment ---- */
    if (strcmp(line, "comment") == 0 || strcmp(line, "c") == 0) {
        toggle_comment_lines(ed);
        set_status(ed, "toggled comment");
        return true;
    }

    /* ---- help ---- */
    if (strcmp(line, "help") == 0 || strcmp(line, "h") == 0) {
        g_help_open = true;
        return true;
    }

    set_status(ed, "unknown command: %s", line);
    return true;
}
