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

void ed_free(Editor *ed) {
    gb_free(&ed->gb);
    free(ed->clipboard);
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

/* ------ movement ------ */

void ed_move_left(Editor *ed)  { if (ed->cursor > 0) ed->cursor--; }
void ed_move_right(Editor *ed) { if (ed->cursor < gb_length(&ed->gb)) ed->cursor++; }

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
    gb_insert_char(&ed->gb, ed->cursor, c);
    ed->cursor++;
    ed->dirty = true;
}

void ed_insert_newline(Editor *ed) { ed_insert_char(ed, '\n'); }

void ed_backspace(Editor *ed) {
    if (ed->has_selection) { ed_delete_selection(ed); return; }
    if (ed->cursor == 0) return;
    gb_delete(&ed->gb, ed->cursor - 1, 1);
    ed->cursor--;
    ed->dirty = true;
}

void ed_delete_char(Editor *ed) {
    if (ed->has_selection) { ed_delete_selection(ed); return; }
    if (ed->cursor >= gb_length(&ed->gb)) return;
    gb_delete(&ed->gb, ed->cursor, 1);
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
    gb_insert_str(&ed->gb, ed->cursor, ed->clipboard, ed->clipboard_len);
    ed->cursor += ed->clipboard_len;
    ed->dirty = true;
}

void ed_delete_selection(Editor *ed) {
    if (!ed->has_selection) return;
    size_t s, e;
    ed_get_selection(ed, &s, &e);
    gb_delete(&ed->gb, s, e - s);
    ed->cursor = s;
    ed->has_selection = false;
    ed->dirty = true;
}

/* ------ search ------ */

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
