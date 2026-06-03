/*
 * Minimal TextMate-grammar (.tmLanguage.json) loader.
 *
 * Supported subset (extended beyond the original line-by-line `match`):
 *   { "name": "<lang>",
 *     "patterns": [
 *        { "match": "<regex>", "name": "<scope>",
 *          "captures": { "1": { "name": "<scope>" }, ... } },
 *        { "include": "#name" },                           ← resolved against
 *        ...                                                  the repository
 *     ],
 *     "repository": {
 *        "name": { "patterns": [...] }   or  { "match": "...", "name": "..." }
 *     }
 *   }
 *
 * Regex is POSIX extended (regex.h).  PCRE lookaheads (?=...) and
 * lookbehinds (?<=...) are silently stripped at load time — patterns that
 * relied on them for *exclusion* become looser, but they still match the
 * actual content.  Use capture groups + `captures` to highlight just a
 * portion of a match.
 *
 * Not implemented: begin/end pairs, beginCaptures/endCaptures, nested
 * `patterns` inside a begin/end block, full Oniguruma syntax.
 */

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "syntax.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ============================================================ *
 * tiny JSON tokenizer / value parser                            *
 * ============================================================ */

typedef struct { const char *p, *end; } JP;

static void jp_skip_ws(JP *j) {
    while (j->p < j->end && isspace((unsigned char)*j->p)) j->p++;
}
static bool jp_expect(JP *j, char c) {
    jp_skip_ws(j);
    if (j->p < j->end && *j->p == c) { j->p++; return true; }
    return false;
}

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

static void jp_skip_value(JP *j) {
    jp_skip_ws(j);
    if (j->p >= j->end) return;
    char c = *j->p;
    if (c == '"') { free(jp_parse_string(j)); return; }
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

/* Capture the literal JSON text of the next value (no unescape), preserving
   structure.  Used to stash repository entries for later parsing. */
static char *jp_capture_value(JP *j) {
    jp_skip_ws(j);
    const char *s = j->p;
    if (j->p >= j->end) return NULL;
    char c = *j->p;
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
                else if (x == close) { depth--; if (depth == 0) break; }
            }
        }
    } else if (c == '"') {
        free(jp_parse_string(j));
    } else {
        while (j->p < j->end && *j->p != ',' && *j->p != ']' && *j->p != '}') j->p++;
    }
    size_t L = (size_t)(j->p - s);
    char *out = (char *)malloc(L + 1);
    memcpy(out, s, L);
    out[L] = 0;
    return out;
}

/* ============================================================ *
 * Lookaround stripping — POSIX ERE doesn't support (?=...) or  *
 * (?<=...).  Drop them at load time so patterns still compile. *
 * ============================================================ */

static char *strip_lookarounds(const char *src) {
    size_t len = strlen(src);
    char *out = (char *)malloc(len + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; ) {
        bool look = false;
        size_t skip_from = i, skip_after = i;
        if (i + 2 < len && src[i] == '(' && src[i + 1] == '?' &&
            (src[i + 2] == '=' || src[i + 2] == '!')) {
            look = true;
            skip_after = i + 3;
        } else if (i + 3 < len && src[i] == '(' && src[i + 1] == '?' &&
                   src[i + 2] == '<' &&
                   (src[i + 3] == '=' || src[i + 3] == '!')) {
            look = true;
            skip_after = i + 4;
        }
        if (!look) { out[j++] = src[i++]; continue; }
        /* find the matching closing paren */
        int depth = 1;
        size_t k = skip_after;
        while (k < len && depth > 0) {
            if (src[k] == '\\' && k + 1 < len) { k += 2; continue; }
            if (src[k] == '(') depth++;
            else if (src[k] == ')') { depth--; if (depth == 0) { k++; break; } }
            k++;
        }
        i = k;
        (void)skip_from;
    }
    out[j] = 0;
    return out;
}

/* ============================================================ *
 * Captures parsing                                              *
 * ============================================================ */

static void parse_captures_obj(JP *j, char *captures[SYN_MAX_CAPTURES]) {
    if (!jp_expect(j, '{')) return;
    while (j->p < j->end) {
        jp_skip_ws(j);
        if (*j->p == '}') { j->p++; return; }
        char *key = jp_parse_string(j);
        if (!key) return;
        if (!jp_expect(j, ':')) { free(key); return; }
        int idx = atoi(key);
        free(key);
        /* value is an object — find name field */
        jp_skip_ws(j);
        if (*j->p == '{') {
            j->p++;
            while (j->p < j->end) {
                jp_skip_ws(j);
                if (*j->p == '}') { j->p++; break; }
                char *k2 = jp_parse_string(j);
                if (!k2) break;
                if (!jp_expect(j, ':')) { free(k2); break; }
                jp_skip_ws(j);
                if (*j->p == '"') {
                    char *v2 = jp_parse_string(j);
                    if (v2 && strcmp(k2, "name") == 0 &&
                        idx >= 0 && idx < SYN_MAX_CAPTURES) {
                        free(captures[idx]);
                        captures[idx] = v2;
                    } else {
                        free(v2);
                    }
                } else {
                    jp_skip_value(j);
                }
                free(k2);
                jp_skip_ws(j);
                if (*j->p == ',') j->p++;
            }
        } else {
            jp_skip_value(j);
        }
        jp_skip_ws(j);
        if (*j->p == ',') j->p++;
    }
}

/* ============================================================ *
 * Repository — name -> raw JSON text of the repository entry.   *
 * Resolution is lazy: we expand `{"include": "#name"}` when we  *
 * flatten patterns into rules.                                  *
 * ============================================================ */

typedef struct {
    char *name;
    char *json;       /* raw JSON of the entry's value */
} RepoEntry;

typedef struct {
    RepoEntry *items;
    size_t     n, cap;
} Repo;

static void repo_add(Repo *r, char *name, char *json) {
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 8;
        r->items = (RepoEntry *)realloc(r->items, r->cap * sizeof(RepoEntry));
    }
    r->items[r->n].name = name;
    r->items[r->n].json = json;
    r->n++;
}

static const RepoEntry *repo_find(const Repo *r, const char *name) {
    for (size_t i = 0; i < r->n; i++)
        if (strcmp(r->items[i].name, name) == 0) return &r->items[i];
    return NULL;
}

static void repo_free(Repo *r) {
    for (size_t i = 0; i < r->n; i++) {
        free(r->items[i].name);
        free(r->items[i].json);
    }
    free(r->items);
}

/* ============================================================ *
 * Rule list — what we ultimately produce.                       *
 * ============================================================ */

typedef struct {
    SyntaxRule *rules;
    size_t      n, cap;
    /* Memo of include names already expanded at the top level.  Without
       this, mutually-recursive #foo / #bar repository entries blow up
       combinatorially. */
    char      **seen;
    size_t      n_seen, cap_seen;
} RuleList;

#define SYN_MAX_RULES 4000           /* hard cap to keep memory bounded */

static bool rules_seen(RuleList *rl, const char *name) {
    for (size_t i = 0; i < rl->n_seen; i++)
        if (strcmp(rl->seen[i], name) == 0) return true;
    return false;
}

static void rules_mark_seen(RuleList *rl, const char *name) {
    if (rl->n_seen == rl->cap_seen) {
        rl->cap_seen = rl->cap_seen ? rl->cap_seen * 2 : 32;
        rl->seen = (char **)realloc(rl->seen, rl->cap_seen * sizeof(char *));
    }
    rl->seen[rl->n_seen++] = strdup(name);
}

static SyntaxRule *rules_push(RuleList *rl) {
    if (rl->n >= SYN_MAX_RULES) return NULL;
    if (rl->n == rl->cap) {
        rl->cap = rl->cap ? rl->cap * 2 : 32;
        rl->rules = (SyntaxRule *)realloc(rl->rules, rl->cap * sizeof(SyntaxRule));
    }
    SyntaxRule *r = &rl->rules[rl->n++];
    memset(r, 0, sizeof *r);
    return r;
}

/* Parse a single rule object (with `match`, `name`, `captures`, `include`)
   and append to rl.  Recurses through `repository` for includes.  Depth
   limit avoids cycles.  Returns true on any progress (rule appended or
   include expanded), even partially. */
static bool parse_rule_object(JP *j, RuleList *rl, const Repo *repo, int depth);

static void parse_rule_list(JP *j, RuleList *rl, const Repo *repo, int depth) {
    if (!jp_expect(j, '[')) return;
    while (j->p < j->end) {
        jp_skip_ws(j);
        if (*j->p == ']') { j->p++; return; }
        if (*j->p == '{') parse_rule_object(j, rl, repo, depth);
        else              jp_skip_value(j);
        jp_skip_ws(j);
        if (j->p < j->end && *j->p == ',') j->p++;
    }
}

static void inline_include(const char *include_name, RuleList *rl, const Repo *repo, int depth) {
    if (depth > 32 || !include_name) return;
    if (rl->n >= SYN_MAX_RULES) return;
    /* "#name" → look up `name` */
    const char *key = include_name;
    if (*key == '#') key++;
    /* Cycle / re-expansion guard: each repository key is inlined once per
       load.  Without this, A → B → A inside the repository produces a
       cartesian blow-up of rules. */
    if (rules_seen(rl, key)) return;
    rules_mark_seen(rl, key);
    const RepoEntry *e = repo_find(repo, key);
    if (!e) return;
    JP sub = { e->json, e->json + strlen(e->json) };
    jp_skip_ws(&sub);
    if (*sub.p == '{') {
        /* could be a single rule, or an object with `patterns` */
        const char *save_p = sub.p;
        JP probe = sub;
        probe.p++;  /* past { */
        bool is_collection = false;
        while (probe.p < probe.end) {
            jp_skip_ws(&probe);
            if (*probe.p == '}') { probe.p++; break; }
            char *k = jp_parse_string(&probe);
            if (!k) break;
            if (!jp_expect(&probe, ':')) { free(k); break; }
            if (strcmp(k, "patterns") == 0) is_collection = true;
            free(k);
            jp_skip_value(&probe);
            jp_skip_ws(&probe);
            if (*probe.p == ',') probe.p++;
            if (is_collection) break;
        }
        sub.p = save_p;
        if (is_collection) {
            /* re-parse to find patterns */
            sub.p++;
            while (sub.p < sub.end) {
                jp_skip_ws(&sub);
                if (*sub.p == '}') { sub.p++; break; }
                char *k = jp_parse_string(&sub);
                if (!k) break;
                if (!jp_expect(&sub, ':')) { free(k); break; }
                if (strcmp(k, "patterns") == 0) {
                    parse_rule_list(&sub, rl, repo, depth + 1);
                } else {
                    jp_skip_value(&sub);
                }
                free(k);
                jp_skip_ws(&sub);
                if (*sub.p == ',') sub.p++;
            }
        } else {
            parse_rule_object(&sub, rl, repo, depth + 1);
        }
    }
}

static bool parse_rule_object(JP *j, RuleList *rl, const Repo *repo, int depth) {
    if (!jp_expect(j, '{')) return false;
    char  *r_name = NULL;
    char  *r_match = NULL;
    char  *r_include = NULL;
    bool   has_nested = false;
    bool   has_begin = false;
    char  *captures[SYN_MAX_CAPTURES] = {0};

    while (j->p < j->end) {
        jp_skip_ws(j);
        if (*j->p == '}') { j->p++; break; }
        char *key = jp_parse_string(j);
        if (!key) break;
        if (!jp_expect(j, ':')) { free(key); break; }
        jp_skip_ws(j);
        if (*j->p == '"') {
            char *val = jp_parse_string(j);
            if      (!strcmp(key, "name"))    { free(r_name);    r_name    = val; }
            else if (!strcmp(key, "match"))   { free(r_match);   r_match   = val; }
            else if (!strcmp(key, "include")) { free(r_include); r_include = val; }
            else if (!strcmp(key, "begin"))   { has_begin = true; free(val); }
            else free(val);
        } else if (*j->p == '{') {
            if (!strcmp(key, "captures") ||
                !strcmp(key, "beginCaptures") || !strcmp(key, "endCaptures")) {
                parse_captures_obj(j, captures);
            } else {
                jp_skip_value(j);
            }
        } else if (*j->p == '[') {
            if (!strcmp(key, "patterns")) {
                has_nested = true;
                parse_rule_list(j, rl, repo, depth + 1);
            } else {
                jp_skip_value(j);
            }
        } else {
            jp_skip_value(j);
        }
        free(key);
        jp_skip_ws(j);
        if (j->p < j->end && *j->p == ',') j->p++;
    }

    /* dispatch */
    if (r_include) {
        inline_include(r_include, rl, repo, depth);
        free(r_include); free(r_name); free(r_match);
        for (int i = 0; i < SYN_MAX_CAPTURES; i++) free(captures[i]);
        return true;
    }
    if (r_match) {
        SyntaxRule *rule = rules_push(rl);
        if (!rule) {
            /* hit the cap — drop this rule */
            free(r_name); free(r_match);
            for (int i = 0; i < SYN_MAX_CAPTURES; i++) free(captures[i]);
            return true;
        }
        rule->name    = r_name; r_name = NULL;
        char *clean   = strip_lookarounds(r_match);
        rule->pattern = clean;
        free(r_match);
        if (regcomp(&rule->rx, rule->pattern, REG_EXTENDED) == 0)
            rule->compiled = true;
        for (int i = 0; i < SYN_MAX_CAPTURES; i++) {
            rule->captures[i] = captures[i];
            captures[i] = NULL;
        }
        return true;
    }
    /* nested-only rule with `patterns`: already added by parse_rule_list */
    free(r_name); free(r_match);
    for (int i = 0; i < SYN_MAX_CAPTURES; i++) free(captures[i]);
    /* begin/end currently unsupported — silently drop */
    (void)has_nested; (void)has_begin;
    return true;
}

/* ============================================================ *
 * Top-level loader                                              *
 * ============================================================ */

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

    /* First pass: collect repository entries and language name.
       Patterns are parsed in a second pass so includes resolve correctly. */
    Repo repo = {0};
    char *patterns_text = NULL;

    while (j.p < j.end) {
        jp_skip_ws(&j);
        if (*j.p == '}') { j.p++; break; }
        char *key = jp_parse_string(&j);
        if (!key) break;
        if (!jp_expect(&j, ':')) { free(key); break; }
        jp_skip_ws(&j);
        if (!strcmp(key, "name") && *j.p == '"') {
            s->name = jp_parse_string(&j);
        } else if (!strcmp(key, "patterns") && *j.p == '[') {
            patterns_text = jp_capture_value(&j);
        } else if ((!strcmp(key, "repository") || !strcmp(key, "Repository"))
                   && *j.p == '{') {
            j.p++;
            while (j.p < j.end) {
                jp_skip_ws(&j);
                if (*j.p == '}') { j.p++; break; }
                char *rname = jp_parse_string(&j);
                if (!rname) break;
                if (!jp_expect(&j, ':')) { free(rname); break; }
                char *rjson = jp_capture_value(&j);
                if (rjson) repo_add(&repo, rname, rjson);
                else       free(rname);
                jp_skip_ws(&j);
                if (*j.p == ',') j.p++;
            }
        } else {
            jp_skip_value(&j);
        }
        free(key);
        jp_skip_ws(&j);
        if (j.p < j.end && *j.p == ',') j.p++;
    }

    RuleList rl = {0};
    if (patterns_text) {
        JP pj = { patterns_text, patterns_text + strlen(patterns_text) };
        parse_rule_list(&pj, &rl, &repo, 0);
        free(patterns_text);
    }

    s->rules  = rl.rules;
    s->nrules = rl.n;

    for (size_t i = 0; i < rl.n_seen; i++) free(rl.seen[i]);
    free(rl.seen);
    repo_free(&repo);
    free(buf);
    return true;
}

void syntax_free(Syntax *s) {
    if (!s) return;
    free(s->name);
    for (size_t i = 0; i < s->nrules; i++) {
        free(s->rules[i].name);
        free(s->rules[i].pattern);
        for (int k = 0; k < SYN_MAX_CAPTURES; k++) free(s->rules[i].captures[k]);
        if (s->rules[i].compiled) regfree(&s->rules[i].rx);
    }
    free(s->rules);
    memset(s, 0, sizeof *s);
}

/* ============================================================ *
 * Scope → palette                                               *
 * ============================================================ */

unsigned char syntax_scope_to_color(const char *scope) {
    if (!scope) return 0;
    if (strstr(scope, "comment"))  return 3;
    if (strstr(scope, "string"))   return 2;
    if (strstr(scope, "constant.numeric")) return 4;
    if (strstr(scope, "constant")) return 6;
    if (strstr(scope, "keyword"))  return 1;
    if (strstr(scope, "storage.type") || strstr(scope, "support.type") ||
        strstr(scope, "entity.name.type")) return 5;
    if (strstr(scope, "entity.name.function") ||
        strstr(scope, "support.function")) return 7;
    if (strstr(scope, "keyword.operator") || strstr(scope, "punctuation")) return 8;
    return 0;
}

/* ============================================================ *
 * Per-line highlighter — applies match rules with captures      *
 * ============================================================ */

void syntax_highlight_line(const Syntax *s, const char *line, size_t line_len, unsigned char *colors) {
    memset(colors, 0, line_len);
    if (!s) return;

    char *buf = (char *)malloc(line_len + 1);
    memcpy(buf, line, line_len);
    buf[line_len] = 0;

    for (size_t i = 0; i < s->nrules; i++) {
        const SyntaxRule *r = &s->rules[i];
        if (!r->compiled) continue;

        unsigned char base_col = syntax_scope_to_color(r->name);

        /* per-capture colour lookup */
        unsigned char cap_col[SYN_MAX_CAPTURES];
        bool any_cap = false;
        for (int k = 0; k < SYN_MAX_CAPTURES; k++) {
            cap_col[k] = r->captures[k] ? syntax_scope_to_color(r->captures[k]) : 0;
            if (cap_col[k]) any_cap = true;
        }
        /* Skip rules that have no effect at all */
        if (base_col == 0 && !any_cap) continue;

        size_t offset = 0;
        regmatch_t m[SYN_MAX_CAPTURES];
        while (offset < line_len &&
               regexec(&r->rx, buf + offset, SYN_MAX_CAPTURES, m,
                       offset == 0 ? 0 : REG_NOTBOL) == 0) {
            if (m[0].rm_so == m[0].rm_eo) { offset += 1; continue; }
            size_t a = offset + (size_t)m[0].rm_so;
            size_t b = offset + (size_t)m[0].rm_eo;

            if (any_cap) {
                /* paint base over whole match first, then overlay captures */
                if (base_col != 0) {
                    for (size_t k = a; k < b && k < line_len; k++)
                        if (colors[k] == 0) colors[k] = base_col;
                }
                for (int g = 1; g < SYN_MAX_CAPTURES; g++) {
                    if (cap_col[g] == 0)  continue;
                    if (m[g].rm_so < 0)   continue;
                    size_t ga = offset + (size_t)m[g].rm_so;
                    size_t gb = offset + (size_t)m[g].rm_eo;
                    for (size_t k = ga; k < gb && k < line_len; k++)
                        colors[k] = cap_col[g];
                }
            } else {
                for (size_t k = a; k < b && k < line_len; k++)
                    if (colors[k] == 0) colors[k] = base_col;
            }
            offset = b;
        }
    }
    free(buf);
}
