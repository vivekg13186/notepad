#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <libgen.h>

#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif

#define CONFIG_FILE "notepad.config.json"

void config_defaults(Config *c) {
    memset(c, 0, sizeof *c);
    strcpy(c->theme,   "dark");
    strcpy(c->grammar, "c.tmLanguage.json");
    c->font_size   = 18;
    c->line_height = 0;        /* derive from font_size */
    c->tab_width   = 4;
    c->window_w    = 1100;
    c->window_h    = 720;
    c->gutter_w    = 60;
    c->show_status = true;
}

/* ---- exe dir resolution ---- */

bool config_exe_dir(char *out, size_t out_sz) {
    char path[4096];
    ssize_t n = -1;

#if defined(__APPLE__)
    uint32_t sz = sizeof path;
    if (_NSGetExecutablePath(path, &sz) == 0) {
        n = (ssize_t)strlen(path);
    }
#elif defined(__linux__)
    n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n > 0) path[n] = 0;
#endif

    if (n <= 0) return false;
    /* strip filename */
    char *slash = strrchr(path, '/');
    if (!slash) return false;
    *slash = 0;
    if (strlen(path) + 1 >= out_sz) return false;
    strcpy(out, path);
    return true;
}

/* ---- tiny JSON parser (object of primitives only) ---- */

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
        char ch = *j->p++;
        if (ch == '\\' && j->p < j->end) {
            char e = *j->p++;
            switch (e) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case '"': ch = '"';  break;
                case '\\': ch = '\\'; break;
                default: ch = e; break;
            }
        }
        if (len + 1 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
        out[len++] = ch;
    }
    if (j->p < j->end) j->p++;
    out[len] = 0;
    return out;
}

static bool jp_number(JP *j, double *out) {
    jp_ws(j);
    char buf[64]; size_t n = 0;
    if (j->p < j->end && (*j->p == '-' || *j->p == '+')) buf[n++] = *j->p++;
    while (j->p < j->end && (isdigit((unsigned char)*j->p) || *j->p == '.' || *j->p == 'e' || *j->p == 'E' || *j->p == '+' || *j->p == '-')) {
        if (n + 1 >= sizeof buf) return false;
        buf[n++] = *j->p++;
    }
    if (n == 0) return false;
    buf[n] = 0;
    *out = strtod(buf, NULL);
    return true;
}

static bool jp_bool(JP *j, bool *out) {
    jp_ws(j);
    if (j->p + 4 <= j->end && memcmp(j->p, "true", 4) == 0) { *out = true; j->p += 4; return true; }
    if (j->p + 5 <= j->end && memcmp(j->p, "false", 5) == 0) { *out = false; j->p += 5; return true; }
    return false;
}

/* Skip an unknown value (string, number, object, array, bool, null) */
static void jp_skip_value(JP *j) {
    jp_ws(j);
    if (j->p >= j->end) return;
    char c = *j->p;
    if (c == '"') { free(jp_string(j)); return; }
    if (c == '{' || c == '[') {
        char open = c, close = (c == '{') ? '}' : ']';
        int depth = 0;
        bool in_str = false;
        while (j->p < j->end) {
            char x = *j->p++;
            if (in_str) {
                if (x == '\\' && j->p < j->end) j->p++;
                else if (x == '"') in_str = false;
            } else {
                if (x == '"') in_str = true;
                else if (x == open) depth++;
                else if (x == close) { depth--; if (depth == 0) return; }
            }
        }
        return;
    }
    while (j->p < j->end && *j->p != ',' && *j->p != ']' && *j->p != '}') j->p++;
}

static void copy_str_field(char *dst, size_t dst_sz, const char *src) {
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = 0;
}

/* ---- parse themes object: { "name": "path", ... } ---- */
static void parse_themes_obj(Config *c, JP *j) {
    if (!jp_eat(j, '{')) return;
    while (j->p < j->end) {
        jp_ws(j);
        if (j->p < j->end && *j->p == '}') { j->p++; return; }
        char *k = jp_string(j);
        if (!k) return;
        if (!jp_eat(j, ':')) { free(k); return; }
        jp_ws(j);
        char *v = jp_string(j);
        if (v && c->n_themes < CFG_MAX_THEMES) {
            ThemeEntry *te = &c->themes[c->n_themes++];
            copy_str_field(te->name, sizeof te->name, k);
            copy_str_field(te->path, sizeof te->path, v);
        }
        free(v); free(k);
        jp_ws(j);
        if (j->p < j->end && *j->p == ',') j->p++;
    }
}

/* ---- parse string array: ["x","y"] into entries; returns count used ---- */
static int parse_str_array(JP *j, char dst[][16], int max_n) {
    if (!jp_eat(j, '[')) return 0;
    int n = 0;
    while (j->p < j->end) {
        jp_ws(j);
        if (j->p < j->end && *j->p == ']') { j->p++; return n; }
        char *v = jp_string(j);
        if (v) {
            if (n < max_n) {
                strncpy(dst[n], v, 15);
                dst[n][15] = 0;
                n++;
            }
            free(v);
        } else {
            /* skip primitive */
            while (j->p < j->end && *j->p != ',' && *j->p != ']') j->p++;
        }
        jp_ws(j);
        if (j->p < j->end && *j->p == ',') j->p++;
    }
    return n;
}

/* ---- parse one schema entry object ---- */
static void parse_schema_entry(SchemaEntry *out, JP *j) {
    if (!jp_eat(j, '{')) return;
    while (j->p < j->end) {
        jp_ws(j);
        if (j->p < j->end && *j->p == '}') { j->p++; return; }
        char *k = jp_string(j);
        if (!k) return;
        if (!jp_eat(j, ':')) { free(k); return; }
        jp_ws(j);
        if (*j->p == '"') {
            char *v = jp_string(j);
            if (v) {
                if      (!strcmp(k, "name"))         copy_str_field(out->name,         sizeof out->name,         v);
                else if (!strcmp(k, "grammar"))      copy_str_field(out->grammar,      sizeof out->grammar,      v);
                else if (!strcmp(k, "line_comment")) copy_str_field(out->line_comment, sizeof out->line_comment, v);
                free(v);
            }
        } else if (*j->p == '[') {
            if (!strcmp(k, "file_extensions")) {
                out->n_extensions = parse_str_array(j, out->extensions, CFG_MAX_EXTS);
            } else {
                jp_skip_value(j);
            }
        } else {
            jp_skip_value(j);
        }
        free(k);
        jp_ws(j);
        if (j->p < j->end && *j->p == ',') j->p++;
    }
}

/* ---- snippets:  { "Python": { "for": "for ${...}", ... }, ... } ---- */
static void parse_snippets_obj(Config *c, JP *j) {
    if (!jp_eat(j, '{')) return;
    while (j->p < j->end) {
        jp_ws(j);
        if (*j->p == '}') { j->p++; return; }
        char *lang = jp_string(j);
        if (!lang) return;
        if (!jp_eat(j, ':')) { free(lang); return; }
        jp_ws(j);
        if (*j->p != '{') { jp_skip_value(j); free(lang); continue; }
        j->p++;
        while (j->p < j->end) {
            jp_ws(j);
            if (*j->p == '}') { j->p++; break; }
            char *trig = jp_string(j);
            if (!trig) break;
            if (!jp_eat(j, ':')) { free(trig); break; }
            char *body = jp_string(j);
            if (body && c->n_snippets < CFG_MAX_SNIPPETS) {
                SnippetEntry *e = &c->snippets[c->n_snippets++];
                copy_str_field(e->language, sizeof e->language, lang);
                copy_str_field(e->trigger,  sizeof e->trigger,  trig);
                e->body = body;
            } else {
                free(body);
            }
            free(trig);
            jp_ws(j);
            if (*j->p == ',') j->p++;
        }
        free(lang);
        jp_ws(j);
        if (*j->p == ',') j->p++;
    }
}

/* ---- parse $schema object: { "id": { ... }, ... } ---- */
static void parse_schema_map(Config *c, JP *j) {
    if (!jp_eat(j, '{')) return;
    while (j->p < j->end) {
        jp_ws(j);
        if (j->p < j->end && *j->p == '}') { j->p++; return; }
        char *k = jp_string(j);
        if (!k) return;
        if (!jp_eat(j, ':')) { free(k); return; }
        jp_ws(j);
        if (*j->p == '{') {
            if (c->n_schemas < CFG_MAX_SCHEMAS) {
                SchemaEntry *se = &c->schemas[c->n_schemas];
                memset(se, 0, sizeof *se);
                copy_str_field(se->id, sizeof se->id, k);
                parse_schema_entry(se, j);
                c->n_schemas++;
            } else {
                jp_skip_value(j);
            }
        } else {
            jp_skip_value(j);
        }
        free(k);
        jp_ws(j);
        if (j->p < j->end && *j->p == ',') j->p++;
    }
}

bool config_load_file(Config *c, const char *path) {
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
    if (!jp_eat(&j, '{')) { free(buf); return false; }
    while (j.p < j.end) {
        jp_ws(&j);
        if (j.p < j.end && *j.p == '}') { j.p++; break; }
        char *k = jp_string(&j);
        if (!k) break;
        if (!jp_eat(&j, ':')) { free(k); break; }
        jp_ws(&j);

        if (*j.p == '"') {
            char *v = jp_string(&j);
            if (v) {
                if      (!strcmp(k, "theme"))   copy_str_field(c->theme,   sizeof c->theme,   v);
                else if (!strcmp(k, "grammar")) copy_str_field(c->grammar, sizeof c->grammar, v);
                free(v);
            }
        } else if (*j.p == '{') {
            if      (!strcmp(k, "themes"))   parse_themes_obj(c, &j);
            else if (!strcmp(k, "snippets")) parse_snippets_obj(c, &j);
            else if (!strcmp(k, "$schema") || !strcmp(k, "schemas")) parse_schema_map(c, &j);
            else    jp_skip_value(&j);
        } else if (*j.p == 't' || *j.p == 'f') {
            bool bv;
            if (jp_bool(&j, &bv)) {
                if (!strcmp(k, "show_status")) c->show_status = bv;
            }
        } else if (isdigit((unsigned char)*j.p) || *j.p == '-' || *j.p == '+') {
            double dv;
            if (jp_number(&j, &dv)) {
                int iv = (int)dv;
                if      (!strcmp(k, "font_size"))   c->font_size   = iv;
                else if (!strcmp(k, "line_height")) c->line_height = iv;
                else if (!strcmp(k, "tab_width"))   c->tab_width   = iv;
                else if (!strcmp(k, "window_w"))    c->window_w    = iv;
                else if (!strcmp(k, "window_h"))    c->window_h    = iv;
                else if (!strcmp(k, "gutter_w"))    c->gutter_w    = iv;
            }
        } else {
            jp_skip_value(&j);
        }

        free(k);
        jp_ws(&j);
        if (j.p < j.end && *j.p == ',') j.p++;
    }
    free(buf);
    return true;
}

void config_load_layered(Config *c, const char *argv0) {
    config_defaults(c);

    char exedir[4096] = {0};
    bool have_exedir = config_exe_dir(exedir, sizeof exedir);

    /* Fallback: derive from argv[0] if it contains a slash */
    if (!have_exedir && argv0 && strchr(argv0, '/')) {
        char tmp[4096];
        strncpy(tmp, argv0, sizeof tmp - 1);
        tmp[sizeof tmp - 1] = 0;
        char *d = dirname(tmp);
        if (d && strlen(d) + 1 < sizeof exedir) {
            strcpy(exedir, d);
            have_exedir = true;
        }
    }

    /* 1) defaults next to executable */
    if (have_exedir) {
        char p[4200];
        snprintf(p, sizeof p, "%s/" CONFIG_FILE, exedir);
        config_load_file(c, p);
    }

    /* 2) override from cwd (skip if it's the same file we just loaded) */
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd)) {
        if (!have_exedir || strcmp(cwd, exedir) != 0) {
            config_load_file(c, CONFIG_FILE);
        }
    } else {
        config_load_file(c, CONFIG_FILE);
    }
}

/* ---- lookups ---- */

const ThemeEntry *config_find_theme(const Config *c, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < c->n_themes; i++) {
        if (strcmp(c->themes[i].name, name) == 0) return &c->themes[i];
    }
    return NULL;
}

static int icmp(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

const SchemaEntry *config_find_schema_by_id(const Config *c, const char *id) {
    if (!id) return NULL;
    /* exact match first */
    for (int i = 0; i < c->n_schemas; i++) {
        if (strcmp(c->schemas[i].id, id) == 0) return &c->schemas[i];
        if (strcmp(c->schemas[i].name, id) == 0) return &c->schemas[i];
    }
    /* case-insensitive fallback so :format python / :format C are forgiving */
    for (int i = 0; i < c->n_schemas; i++) {
        if (icmp(c->schemas[i].id, id) == 0)   return &c->schemas[i];
        if (icmp(c->schemas[i].name, id) == 0) return &c->schemas[i];
    }
    return NULL;
}

const SnippetEntry *config_find_snippet(const Config *c, const char *language, const char *trigger) {
    if (!trigger || !language) return NULL;
    for (int i = 0; i < c->n_snippets; i++) {
        const SnippetEntry *e = &c->snippets[i];
        if (strcmp(e->language, language) == 0 && strcmp(e->trigger, trigger) == 0)
            return e;
    }
    return NULL;
}

const SchemaEntry *config_find_schema_by_ext(const Config *c, const char *path_or_ext) {
    if (!path_or_ext) return NULL;
    const char *dot = strrchr(path_or_ext, '.');
    const char *ext = dot ? dot : path_or_ext;   /* include the dot */
    for (int i = 0; i < c->n_schemas; i++) {
        const SchemaEntry *s = &c->schemas[i];
        for (int k = 0; k < s->n_extensions; k++) {
            if (strcmp(s->extensions[k], ext) == 0) return s;
        }
    }
    return NULL;
}
