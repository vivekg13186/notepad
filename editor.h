#ifndef EDITOR_H
#define EDITOR_H

#include "gap_buffer.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    MODE_NORMAL,
    MODE_INSERT,
    MODE_VISUAL,
    MODE_COMMAND,   /* typing : command */
    MODE_SEARCH     /* typing / search */
} EditorMode;

/* ---- Undo / redo ---- */
typedef enum { UNDO_INSERT, UNDO_DELETE } UndoKind;

typedef struct {
    UndoKind kind;
    size_t   pos;
    char    *text;
    size_t   len;
    size_t   cursor_before;
} UndoOp;

typedef struct {
    UndoOp *ops;
    int     n;        /* total ops */
    int     cap;
    int     cur;      /* index of next op to redo (ops[0..cur) applied) */
} UndoStack;

typedef struct {
    GapBuffer   gb;
    EditorMode  mode;
    size_t      cursor;        /* logical position */
    size_t      sel_anchor;    /* selection start position */
    bool        has_selection;

    size_t      viewport_line; /* first visible line */
    int         viewport_lines;
    int         viewport_cols;
    float       viewport_x;    /* horizontal scroll in pixels */

    char        filename[512];
    bool        dirty;

    /* command-line / search buffer */
    char        cmdline[512];
    size_t      cmdlen;

    /* status message */
    char        status[512];
    double      status_until;  /* GetTime() expiry */

    /* clipboard */
    char       *clipboard;
    size_t      clipboard_len;

    /* last search string */
    char        last_search[256];

    /* requested quit */
    bool        quit;

    /* undo / redo */
    UndoStack   undo;
    bool        suppress_undo;   /* set during undo/redo to avoid recursion */
} Editor;

void   ed_init(Editor *ed);
void   ed_free(Editor *ed);
bool   ed_load(Editor *ed, const char *path);
bool   ed_save(Editor *ed);

/* line / column helpers */
size_t ed_line_count(Editor *ed);
size_t ed_line_start(Editor *ed, size_t line);     /* logical pos */
size_t ed_line_length(Editor *ed, size_t line);    /* not counting \n */
void   ed_cursor_line_col(Editor *ed, size_t *line, size_t *col);
size_t ed_pos_from_line_col(Editor *ed, size_t line, size_t col);

/* cursor movement */
void   ed_move_left(Editor *ed);
void   ed_move_right(Editor *ed);
void   ed_move_up(Editor *ed);
void   ed_move_down(Editor *ed);
void   ed_move_line_start(Editor *ed);
void   ed_move_line_end(Editor *ed);
void   ed_move_doc_start(Editor *ed);
void   ed_move_doc_end(Editor *ed);
void   ed_goto_line(Editor *ed, size_t line); /* 1-based */

/* editing */
void   ed_insert_char(Editor *ed, char c);
void   ed_insert_newline(Editor *ed);
void   ed_backspace(Editor *ed);
void   ed_delete_char(Editor *ed); /* delete at cursor */
void   ed_insert_str_at(Editor *ed, size_t pos, const char *s, size_t len);
void   ed_delete_at(Editor *ed, size_t pos, size_t len);

/* selection */
void   ed_start_selection(Editor *ed);
void   ed_clear_selection(Editor *ed);
void   ed_get_selection(Editor *ed, size_t *start, size_t *end);
void   ed_copy_selection(Editor *ed);
void   ed_cut_selection(Editor *ed);
void   ed_paste(Editor *ed);
void   ed_delete_selection(Editor *ed);

/* UTF-8 codepoint navigation over the gap buffer */
size_t ed_utf8_prev(Editor *ed, size_t pos);
size_t ed_utf8_next(Editor *ed, size_t pos);

/* search */
bool   ed_search_forward(Editor *ed, const char *needle, size_t from);

/* status helpers */
void   ed_set_status(Editor *ed, const char *msg);

/* undo / redo */
void   ed_undo(Editor *ed);
void   ed_redo(Editor *ed);

#endif
