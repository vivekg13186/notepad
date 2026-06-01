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
void load_grammar(const char *path);
void auto_select_grammar(const char *filename);
void open_theme_picker(void);
void open_format_picker(void);
bool apply_theme_by_name(const char *name);

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

static bool do_save_as(Editor *ed, const char *arg) {
    char path[512];
    expand_path(path, sizeof path, arg);
    strncpy(ed->filename, path, sizeof ed->filename - 1);
    ed->filename[sizeof ed->filename - 1] = 0;
    return ed_save(ed);
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

    /* ---- save ---- */
    if (strcmp(line, "w") == 0) {
        if (ed_save(ed)) set_status(ed, "saved %s", ed->filename);
        else             set_status(ed, "save failed (no filename?)");
        return true;
    }
    if (strncmp(line, "w ", 2) == 0) {
        if (do_save_as(ed, line + 2)) set_status(ed, "saved %s", ed->filename);
        else                          set_status(ed, "save failed: %s", ed->filename);
        return true;
    }
    if (strcmp(line, "wq") == 0) {
        if (ed_save(ed)) { ed->quit = true; return false; }
        set_status(ed, "save failed");
        return true;
    }

    /* ---- open ---- */
    if (strncmp(line, "o ", 2) == 0) {
        char path[512];
        expand_path(path, sizeof path, line + 2);
        if (ed_load(ed, path)) {
            auto_select_grammar(ed->filename);
            set_status(ed, "opened %s", ed->filename);
        } else {
            set_status(ed, "open failed: %s", path);
        }
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
        if (s && s->grammar[0]) {
            load_grammar(s->grammar);
            set_status(ed, "format: %s (%s)", s->name[0] ? s->name : s->id, s->grammar);
        } else {
            set_status(ed, "no schema: %s", arg);
        }
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
