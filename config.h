#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#define CFG_MAX_THEMES   32
#define CFG_MAX_SCHEMAS  32
#define CFG_MAX_EXTS      8
#define CFG_MAX_SNIPPETS 256

typedef struct {
    char name[64];        /* e.g. "dark", "light" */
    char path[256];       /* e.g. "./themes/dark.json" */
} ThemeEntry;

typedef struct {
    char language[64];    /* schema name to match against, e.g. "C", "Python" */
    char trigger[32];
    char *body;           /* dynamically allocated */
} SnippetEntry;

typedef struct {
    char id[64];          /* schema key ("**.c"); used as :setft argument */
    char name[64];        /* display name ("C") */
    char extensions[CFG_MAX_EXTS][16];
    int  n_extensions;
    char grammar[256];    /* path to tmLanguage.json */
    char line_comment[16];/* e.g. "// " or "# " — used by :comment / Ctrl+/ */
} SchemaEntry;

typedef struct {
    char  theme[256];     /* startup theme: built-in name, themes-map key, or .json path */
    char  grammar[256];   /* fallback grammar path */
    int   font_size;      /* px */
    int   line_height;    /* px, defaults to font_size + 4 if <= 0 */
    int   tab_width;      /* spaces per Tab */
    int   window_w;
    int   window_h;
    int   gutter_w;       /* pixels reserved for line numbers */
    bool  show_status;

    ThemeEntry  themes[CFG_MAX_THEMES];
    int         n_themes;

    SchemaEntry schemas[CFG_MAX_SCHEMAS];
    int         n_schemas;

    SnippetEntry snippets[CFG_MAX_SNIPPETS];
    int          n_snippets;
} Config;

void config_defaults(Config *c);

/* Resolve absolute directory of the running executable.  Returns true on
   success.  Buffer should be PATH_MAX-ish. */
bool config_exe_dir(char *out, size_t out_sz);

/* Load a single notepad.config.json file, applying any present keys onto *c
   (others keep their current values).  Returns true if the file existed and
   parsed; false if it was missing or unparsable. */
bool config_load_file(Config *c, const char *path);

/* Convenience: load exe-dir defaults, then overlay cwd config.
   `argv0` is used only as a fallback for exe path resolution. */
void config_load_layered(Config *c, const char *argv0);

/* Lookup helpers */
const ThemeEntry  *config_find_theme(const Config *c, const char *name);
const SchemaEntry *config_find_schema_by_id(const Config *c, const char *id);
const SchemaEntry *config_find_schema_by_ext(const Config *c, const char *path_or_ext);
const SnippetEntry *config_find_snippet(const Config *c, const char *language, const char *trigger);

#endif
