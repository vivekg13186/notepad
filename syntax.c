/*
 * Minimal TextMate-grammar (.tmLanguage.json) loader.
 *
 * Supported subset:
 *   { "name": "...", "patterns": [ { "match": "<regex>", "name": "<scope>" }, ... ] }
 *
 * No begin/end, no nested patterns, no captures.  Only single-line `match`
 * rules.  Regex is POSIX extended (regex.h).
 */

#include "syntax.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ----- tiny JSON parser (object, array, string, ignores numbers/bools) ----- */

typedef struct { const char *p, *end; } JP;

static void jp_skip_ws(JP *j) {
    while (j->p < j->end && isspace((unsigned char)*j->p)) j->p++;
}

static bool jp_expect(JP *j, char c) {
    jp_skip_ws(j);
    if (j->p < j->end && *j->p == c) { j->p++; return true; }
    return false;
}

/* parse a JSON string into a newly-malloc'd C string (unescaped) */
static char *jp_parse_string(JP *j) {
    jp_skip_ws(j);
    if (j->p >= j->end || *j->p != '"') return NULL;
    j->p++;
    size_t cap = 64, len = 0;
    char *out = (char *)malloc(cap);
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\' && j->p < j->end) {
            char esc = *j->p++;
            switch (esc) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '"': c = '"';  break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u': {
                    /* skip 4 hex digits, emit '?' */
                    for (int k = 0; k < 4 && j->p < j->end; k++) j->p++;
                    c = '?';
                    break;
                }
                default: c = esc; break;
            }
        }
        if (len + 1 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
        out[len++] = c;
    }
    if (j->p < j->end && *j->p == '"') j->p++;
    out[len] = 0;
    return out;
}

/* skip an arbitrary value (string, number, true/false/null, array, object) */
static void jp_skip_value(JP *j) {
    jp_skip_ws(j);
    if (j->p >= j->end) return;
    char c = *j->p;
    if (c == '"') { free(jp_parse_string(j)); return; }
    if (c == '{') {
        int depth = 0;
        bool in_str = false;
        while (j->p < j->end) {
            char x = *j->p++;
            if (in_str) {
                if (x == '\\' && j->p < j->end) j->p++;
                else if (x == '"') in_str = false;
            } else {
                if (x == '"') in_str = true;
                else if (x == '{') depth++;
                else if (x == '}') { depth--; if (depth == 0) return; }
            }
        }
        return;
    }
    if (c == '[') {
        int depth = 0;
        bool in_str = false;
        while (j->p < j->end) {
            char x = *j->p++;
            if (in_str) {
                if (x == '\\' && j->p < j->end) j->p++;
                else if (x == '"') in_str = false;
            } else {
                if (x == '"') in_str = true;
                else if (x == '[') depth++;
                else if (x == ']') { depth--; if (depth == 0) return; }
            }
        }
        return;
    }
    /* primitive */
    while (j->p < j->end && *j->p != ',' && *j->p != ']' && *j->p != '}') j->p++;
}

/* parse a single { "name":..., "match":... } object into out (allocated) */
static bool parse_rule_obj(JP *j, SyntaxRule *out) {
    jp_skip_ws(j);
    if (!jp_expect(j, '{')) return false;
    out->name = NULL;
    out->pattern = NULL;
    while (j->p < j->end) {
        jp_skip_ws(j);
        if (*j->p == '}') { j->p++; break; }
        char *key = jp_parse_string(j);
        if (!key) return false;
        jp_skip_ws(j);
        if (!jp_expect(j, ':')) { free(key); return false; }
        jp_skip_ws(j);
        if (*j->p == '"') {
            char *val = jp_parse_string(j);
            if (strcmp(key, "name") == 0) { free(out->name); out->name = val; }
            else if (strcmp(key, "match") == 0) { free(out->pattern); out->pattern = val; }
            else free(val);
        } else {
            jp_skip_value(j);
        }
        free(key);
        jp_skip_ws(j);
        if (j->p < j->end && *j->p == ',') j->p++;
    }
    return true;
}

bool syntax_load_tmlanguage(Syntax *s, const char *path) {
    memset(s, 0, sizeof *s);
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
    if (!jp_expect(&j, '{')) { free(buf); return false; }

    /* walk top-level keys */
    while (j.p < j.end) {
        jp_skip_ws(&j);
        if (*j.p == '}') { j.p++; break; }
        char *key = jp_parse_string(&j);
        if (!key) break;
        jp_skip_ws(&j);
        if (!jp_expect(&j, ':')) { free(key); break; }
        jp_skip_ws(&j);
        if (strcmp(key, "name") == 0 && *j.p == '"') {
            s->name = jp_parse_string(&j);
        } else if (strcmp(key, "patterns") == 0 && *j.p == '[') {
            j.p++;
            size_t cap = 16;
            s->rules = (SyntaxRule *)calloc(cap, sizeof(SyntaxRule));
            s->nrules = 0;
            while (j.p < j.end) {
                jp_skip_ws(&j);
                if (*j.p == ']') { j.p++; break; }
                SyntaxRule r = {0};
                if (parse_rule_obj(&j, &r)) {
                    if (r.pattern) {
                        if (s->nrules == cap) {
                            cap *= 2;
                            s->rules = (SyntaxRule *)realloc(s->rules, cap * sizeof(SyntaxRule));
                        }
                        if (regcomp(&r.rx, r.pattern, REG_EXTENDED) == 0) {
                            r.compiled = true;
                        }
                        s->rules[s->nrules++] = r;
                    } else {
                        free(r.name);
                    }
                }
                jp_skip_ws(&j);
                if (j.p < j.end && *j.p == ',') j.p++;
            }
        } else {
            jp_skip_value(&j);
        }
        free(key);
        jp_skip_ws(&j);
        if (j.p < j.end && *j.p == ',') j.p++;
    }
    free(buf);
    return true;
}

void syntax_free(Syntax *s) {
    if (!s) return;
    free(s->name);
    for (size_t i = 0; i < s->nrules; i++) {
        free(s->rules[i].name);
        free(s->rules[i].pattern);
        if (s->rules[i].compiled) regfree(&s->rules[i].rx);
    }
    free(s->rules);
    memset(s, 0, sizeof *s);
}

unsigned char syntax_scope_to_color(const char *scope) {
    if (!scope) return 0;
    if (strstr(scope, "comment"))  return 3;
    if (strstr(scope, "string"))   return 2;
    if (strstr(scope, "constant.numeric")) return 4;
    if (strstr(scope, "constant")) return 6;
    if (strstr(scope, "keyword"))  return 1;
    if (strstr(scope, "storage.type") || strstr(scope, "support.type") || strstr(scope, "entity.name.type")) return 5;
    if (strstr(scope, "entity.name.function") || strstr(scope, "support.function")) return 7;
    if (strstr(scope, "keyword.operator") || strstr(scope, "punctuation")) return 8;
    return 0;
}

void syntax_highlight_line(const Syntax *s, const char *line, size_t line_len, unsigned char *colors) {
    memset(colors, 0, line_len);
    if (!s) return;
    /* copy into a null-terminated buffer for regexec */
    char *buf = (char *)malloc(line_len + 1);
    memcpy(buf, line, line_len);
    buf[line_len] = 0;

    for (size_t i = 0; i < s->nrules; i++) {
        if (!s->rules[i].compiled) continue;
        unsigned char col = syntax_scope_to_color(s->rules[i].name);
        if (col == 0) continue;
        size_t offset = 0;
        regmatch_t m;
        while (offset < line_len &&
               regexec(&s->rules[i].rx, buf + offset, 1, &m, offset == 0 ? 0 : REG_NOTBOL) == 0) {
            if (m.rm_so == m.rm_eo) { offset += 1; continue; }
            size_t a = offset + (size_t)m.rm_so;
            size_t b = offset + (size_t)m.rm_eo;
            for (size_t k = a; k < b && k < line_len; k++) {
                if (colors[k] == 0) colors[k] = col;
            }
            offset = b;
        }
    }
    free(buf);
}
