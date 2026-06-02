#include "editor.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void ed_init(Editor *ed) {
    memset(ed, 0, sizeof(*ed));
    gb_init(&ed->gb, 4096);
    ed->mode = MODE_NORMAL;
    ed->viewport_lines = 40;
    ed->viewport_cols  = 120;
    strcpy(ed->filename, "[No Name]");
}

static void undo_clear_redo_tail(UndoStack *u) {
    for (int i = u->cur; i < u->n; i++) free(u->ops[i].text);
    u->n = u->cur;
}

static void undo_free(UndoStack *u) {
    for (int i = 0; i < u->n; i++) free(u->ops[i].text);
    free(u->ops);
    u->ops = NULL; u->n = u->cap = u->cur = 0;
}

/* Append `len` bytes of `data` to op->text. */
static void undo_op_append(UndoOp *op, const char *data, size_t len) {
    op->text = (char *)realloc(op->text, op->len + len + 1);
    memcpy(op->text + op->len, data, len);
    op->len += len;
    op->text[op->len] = 0;
}

static UndoOp *undo_push_new(UndoStack *u, UndoKind kind, size_t pos, size_t cursor_before) {
    undo_clear_redo_tail(u);
    if (u->n == u->cap) {
        u->cap = u->cap ? u->cap * 2 : 64;
        u->ops = (UndoOp *)realloc(u->ops, u->cap * sizeof(UndoOp));
    }
    UndoOp *op = &u->ops[u->n++];
    op->kind = kind;
    op->pos  = pos;
    op->text = NULL;
    op->len  = 0;
    op->cursor_before = cursor_before;
    u->cur = u->n;
    return op;
}

/* Merge with the previous op if it's the same kind and physically adjacent.
   Returns the op the caller should append into. */
static UndoOp *undo_push(UndoStack *u, UndoKind kind, size_t pos, size_t cursor_before) {
    if (u->n > 0 && u->cur == u->n) {
        UndoOp *prev = &u->ops[u->n - 1];
        if (prev->kind == kind) {
            if (kind == UNDO_INSERT && pos == prev->pos + prev->len) return prev;
            /* Backspace pattern: deletion shifts pos backward by removed bytes. */
            if (kind == UNDO_DELETE && pos + 0 == prev->pos)         return prev; /* delete-forward streak */
        }
    }
    return undo_push_new(u, kind, pos, cursor_before);
}

void ed_free(Editor *ed) {
    gb_free(&ed->gb);
    free(ed->clipboard);
    undo_free(&ed->undo);
}

bool ed_load(Editor *ed, const char *path) {
    /* reset buffer */
    gb_free(&ed->gb);
    gb_init(&ed->gb, 4096);
    if (path) {
        strncpy(ed->filename, path, sizeof(ed->filename) - 1);
        ed->filename[sizeof(ed->filename) - 1] = 0;
        bool ok = gb_load_file(&ed->gb, path);
        ed->cursor = 0;
        ed->dirty = false;
        return ok;
    }
    ed->cursor = 0;
    ed->dirty = false;
    return true;
}

bool ed_save(Editor *ed) {
    if (ed->filename[0] == 0 || strcmp(ed->filename, "[No Name]") == 0) return false;
    bool ok = gb_save_file(&ed->gb, ed->filename);
    if (ok) ed->dirty = false;
    return ok;
}

void ed_set_status(Editor *ed, const char *msg) {
    strncpy(ed->status, msg, sizeof(ed->status) - 1);
    ed->status[sizeof(ed->status) - 1] = 0;
    /* duration handled by main using current time */
    ed->status_until = 0.0;
}

/* ------ line/col helpers ------ */

size_t ed_line_count(Editor *ed) {
    size_t n = 1;
    size_t L = gb_length(&ed->gb);
    for (size_t i = 0; i < L; i++) if (gb_char_at(&ed->gb, i) == '\n') n++;
    return n;
}

size_t ed_line_start(Editor *ed, size_t line) {
    if (line == 0) return 0;
    size_t cur = 0;
    size_t L = gb_length(&ed->gb);
    for (size_t i = 0; i < L && cur < line; i++) {
        if (gb_char_at(&ed->gb, i) == '\n') {
            cur++;
            if (cur == line) return i + 1;
        }
    }
    return L;
}

size_t ed_line_length(Editor *ed, size_t line) {
    size_t s = ed_line_start(ed, line);
    size_t L = gb_length(&ed->gb);
    size_t i = s;
    while (i < L && gb_char_at(&ed->gb, i) != '\n') i++;
    return i - s;
}

void ed_cursor_line_col(Editor *ed, size_t *line, size_t *col) {
    size_t ln = 0, lstart = 0;
    size_t pos = ed->cursor;
    if (pos > gb_length(&ed->gb)) pos = gb_length(&ed->gb);
    for (size_t i = 0; i < pos; i++) {
        if (gb_char_at(&ed->gb, i) == '\n') { ln++; lstart = i + 1; }
    }
    *line = ln;
    *col  = pos - lstart;
}

size_t ed_pos_from_line_col(Editor *ed, size_t line, size_t col) {
    size_t s = ed_line_start(ed, line);
    size_t len = ed_line_length(ed, line);
    if (col > len) col = len;
    return s + col;
}

/* ------ UTF-8 codepoint navigation ------ */

static int utf8_lead_bytes(unsigned char c) {
    if ((c & 0x80) == 0)   return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

size_t ed_utf8_prev(Editor *ed, size_t pos) {
    if (pos == 0) return 0;
    pos--;
    while (pos > 0 && (((unsigned char)gb_char_at(&ed->gb, pos)) & 0xC0) == 0x80) pos--;
    return pos;
}

size_t ed_utf8_next(Editor *ed, size_t pos) {
    size_t L = gb_length(&ed->gb);
    if (pos >= L) return L;
    int n = utf8_lead_bytes((unsigned char)gb_char_at(&ed->gb, pos));
    if (pos + (size_t)n > L) return L;
    return pos + (size_t)n;
}

/* ------ movement ------ */

void ed_move_left(Editor *ed)  { ed->cursor = ed_utf8_prev(ed, ed->cursor); }
void ed_move_right(Editor *ed) { ed->cursor = ed_utf8_next(ed, ed->cursor); }

void ed_move_up(Editor *ed) {
    size_t ln, col;
    ed_cursor_line_col(ed, &ln, &col);
    if (ln == 0) return;
    ed->cursor = ed_pos_from_line_col(ed, ln - 1, col);
}

void ed_move_down(Editor *ed) {
    size_t ln, col;
    ed_cursor_line_col(ed, &ln, &col);
    if (ln + 1 >= ed_line_count(ed)) return;
    ed->cursor = ed_pos_from_line_col(ed, ln + 1, col);
}

void ed_move_line_start(Editor *ed) {
    size_t ln, col;
    ed_cursor_line_col(ed, &ln, &col);
    ed->cursor = ed_line_start(ed, ln);
}

void ed_move_line_end(Editor *ed) {
    size_t ln, col;
    ed_cursor_line_col(ed, &ln, &col);
    ed->cursor = ed_line_start(ed, ln) + ed_line_length(ed, ln);
}

void ed_move_doc_start(Editor *ed) { ed->cursor = 0; }
void ed_move_doc_end(Editor *ed)   { ed->cursor = gb_length(&ed->gb); }

void ed_goto_line(Editor *ed, size_t line) {
    if (line == 0) line = 1;
    size_t lc = ed_line_count(ed);
    if (line > lc) line = lc;
    ed->cursor = ed_line_start(ed, line - 1);
}

/* ------ editing ------ */

void ed_insert_char(Editor *ed, char c) {
    if (ed->has_selection) ed_delete_selection(ed);
    if (!ed->suppress_undo) {
        UndoOp *op = undo_push(&ed->undo, UNDO_INSERT, ed->cursor, ed->cursor);
        undo_op_append(op, &c, 1);
    }
    gb_insert_char(&ed->gb, ed->cursor, c);
    ed->cursor++;
    ed->dirty = true;
}

void ed_insert_newline(Editor *ed) { ed_insert_char(ed, '\n'); }

void ed_insert_str_at(Editor *ed, size_t pos, const char *s, size_t len) {
    if (len == 0) return;
    if (!ed->suppress_undo) {
        UndoOp *op = undo_push_new(&ed->undo, UNDO_INSERT, pos, ed->cursor);
        undo_op_append(op, s, len);
    }
    gb_insert_str(&ed->gb, pos, s, len);
    if (ed->cursor >= pos) ed->cursor += len;
    ed->dirty = true;
}

void ed_delete_at(Editor *ed, size_t pos, size_t len) {
    if (len == 0) return;
    if (!ed->suppress_undo) {
        char *removed = (char *)malloc(len);
        gb_copy_range(&ed->gb, pos, pos + len, removed);
        UndoOp *op = undo_push_new(&ed->undo, UNDO_DELETE, pos, ed->cursor);
        undo_op_append(op, removed, len);
        free(removed);
    }
    gb_delete(&ed->gb, pos, len);
    if (ed->cursor > pos + len)      ed->cursor -= len;
    else if (ed->cursor > pos)       ed->cursor  = pos;
    ed->dirty = true;
}

void ed_backspace(Editor *ed) {
    if (ed->has_selection) { ed_delete_selection(ed); return; }
    if (ed->cursor == 0) return;
    size_t prev = ed_utf8_prev(ed, ed->cursor);
    size_t n = ed->cursor - prev;
    if (!ed->suppress_undo) {
        char *removed = (char *)malloc(n);
        gb_copy_range(&ed->gb, prev, prev + n, removed);
        UndoOp *op = undo_push_new(&ed->undo, UNDO_DELETE, prev, ed->cursor);
        undo_op_append(op, removed, n);
        free(removed);
    }
    gb_delete(&ed->gb, prev, n);
    ed->cursor = prev;
    ed->dirty = true;
}

void ed_delete_char(Editor *ed) {
    if (ed->has_selection) { ed_delete_selection(ed); return; }
    if (ed->cursor >= gb_length(&ed->gb)) return;
    size_t next = ed_utf8_next(ed, ed->cursor);
    size_t n = next - ed->cursor;
    if (!ed->suppress_undo) {
        char *removed = (char *)malloc(n);
        gb_copy_range(&ed->gb, ed->cursor, next, removed);
        UndoOp *op = undo_push_new(&ed->undo, UNDO_DELETE, ed->cursor, ed->cursor);
        undo_op_append(op, removed, n);
        free(removed);
    }
    gb_delete(&ed->gb, ed->cursor, n);
    ed->dirty = true;
}

/* ------ selection ------ */

void ed_start_selection(Editor *ed) {
    ed->sel_anchor = ed->cursor;
    ed->has_selection = true;
}

void ed_clear_selection(Editor *ed) { ed->has_selection = false; }

void ed_get_selection(Editor *ed, size_t *start, size_t *end) {
    if (!ed->has_selection) { *start = *end = ed->cursor; return; }
    size_t a = ed->sel_anchor, b = ed->cursor;
    if (a > b) { size_t t = a; a = b; b = t; }
    *start = a; *end = b;
    if (*end < gb_length(&ed->gb)) (*end)++; /* inclusive */
}

void ed_copy_selection(Editor *ed) {
    size_t s, e;
    ed_get_selection(ed, &s, &e);
    if (e <= s) return;
    free(ed->clipboard);
    ed->clipboard_len = e - s;
    ed->clipboard = (char *)malloc(ed->clipboard_len + 1);
    gb_copy_range(&ed->gb, s, e, ed->clipboard);
    ed->clipboard[ed->clipboard_len] = 0;
}

void ed_cut_selection(Editor *ed) {
    if (!ed->has_selection) return;
    ed_copy_selection(ed);
    ed_delete_selection(ed);
}

void ed_paste(Editor *ed) {
    if (!ed->clipboard || ed->clipboard_len == 0) return;
    if (ed->has_selection) ed_delete_selection(ed);
    if (!ed->suppress_undo) {
        UndoOp *op = undo_push_new(&ed->undo, UNDO_INSERT, ed->cursor, ed->cursor);
        undo_op_append(op, ed->clipboard, ed->clipboard_len);
    }
    gb_insert_str(&ed->gb, ed->cursor, ed->clipboard, ed->clipboard_len);
    ed->cursor += ed->clipboard_len;
    ed->dirty = true;
}

void ed_delete_selection(Editor *ed) {
    if (!ed->has_selection) return;
    size_t s, e;
    ed_get_selection(ed, &s, &e);
    if (!ed->suppress_undo && e > s) {
        char *removed = (char *)malloc(e - s);
        gb_copy_range(&ed->gb, s, e, removed);
        UndoOp *op = undo_push_new(&ed->undo, UNDO_DELETE, s, ed->cursor);
        undo_op_append(op, removed, e - s);
        free(removed);
    }
    gb_delete(&ed->gb, s, e - s);
    ed->cursor = s;
    ed->has_selection = false;
    ed->dirty = true;
}

/* ------ search ------ */

/* ------ undo / redo ------ */

void ed_undo(Editor *ed) {
    UndoStack *u = &ed->undo;
    if (u->cur == 0) return;
    u->cur--;
    UndoOp *op = &u->ops[u->cur];
    ed->suppress_undo = true;
    if (op->kind == UNDO_INSERT) {
        /* undo an insertion = remove the inserted bytes */
        gb_delete(&ed->gb, op->pos, op->len);
        ed->cursor = op->cursor_before;
    } else { /* UNDO_DELETE */
        gb_insert_str(&ed->gb, op->pos, op->text, op->len);
        ed->cursor = op->cursor_before;
    }
    ed->suppress_undo = false;
    ed->dirty = true;
    ed->has_selection = false;
}

void ed_redo(Editor *ed) {
    UndoStack *u = &ed->undo;
    if (u->cur >= u->n) return;
    UndoOp *op = &u->ops[u->cur];
    u->cur++;
    ed->suppress_undo = true;
    if (op->kind == UNDO_INSERT) {
        gb_insert_str(&ed->gb, op->pos, op->text, op->len);
        ed->cursor = op->pos + op->len;
    } else { /* UNDO_DELETE */
        gb_delete(&ed->gb, op->pos, op->len);
        ed->cursor = op->pos;
    }
    ed->suppress_undo = false;
    ed->dirty = true;
    ed->has_selection = false;
}

bool ed_search_forward(Editor *ed, const char *needle, size_t from) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return false;
    size_t L = gb_length(&ed->gb);
    if (from >= L) from = 0;
    for (size_t i = from; i + nlen <= L; i++) {
        size_t j = 0;
        while (j < nlen && gb_char_at(&ed->gb, i + j) == needle[j]) j++;
        if (j == nlen) { ed->cursor = i; return true; }
    }
    /* wrap */
    for (size_t i = 0; i + nlen <= from && i + nlen <= L; i++) {
        size_t j = 0;
        while (j < nlen && gb_char_at(&ed->gb, i + j) == needle[j]) j++;
        if (j == nlen) { ed->cursor = i; return true; }
    }
    return false;
}
