#include "gap_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define GAP_MIN 64

void gb_init(GapBuffer *gb, size_t initial_cap) {
    if (initial_cap < GAP_MIN) initial_cap = GAP_MIN;
    gb->data = (char *)malloc(initial_cap);
    gb->cap = initial_cap;
    gb->gap_start = 0;
    gb->gap_end = initial_cap;
}

void gb_free(GapBuffer *gb) {
    free(gb->data);
    gb->data = NULL;
    gb->cap = gb->gap_start = gb->gap_end = 0;
}

size_t gb_length(const GapBuffer *gb) {
    return gb->cap - (gb->gap_end - gb->gap_start);
}

static size_t logical_to_phys(const GapBuffer *gb, size_t i) {
    return (i < gb->gap_start) ? i : i + (gb->gap_end - gb->gap_start);
}

char gb_char_at(const GapBuffer *gb, size_t i) {
    if (i >= gb_length(gb)) return 0;
    return gb->data[logical_to_phys(gb, i)];
}

static void gb_grow(GapBuffer *gb, size_t need) {
    size_t gap = gb->gap_end - gb->gap_start;
    if (gap >= need) return;
    size_t newcap = gb->cap * 2;
    while (newcap - gb->cap + gap < need) newcap *= 2;
    char *nd = (char *)malloc(newcap);
    /* copy before gap */
    memcpy(nd, gb->data, gb->gap_start);
    /* copy after gap */
    size_t tail = gb->cap - gb->gap_end;
    memcpy(nd + newcap - tail, gb->data + gb->gap_end, tail);
    free(gb->data);
    gb->data = nd;
    gb->gap_end = newcap - tail;
    gb->cap = newcap;
}

void gb_move_gap(GapBuffer *gb, size_t i) {
    if (i == gb->gap_start) return;
    if (i < gb->gap_start) {
        size_t n = gb->gap_start - i;
        memmove(gb->data + gb->gap_end - n, gb->data + i, n);
        gb->gap_start -= n;
        gb->gap_end -= n;
    } else {
        size_t n = i - gb->gap_start;
        memmove(gb->data + gb->gap_start, gb->data + gb->gap_end, n);
        gb->gap_start += n;
        gb->gap_end += n;
    }
}

void gb_insert_char(GapBuffer *gb, size_t i, char c) {
    if (i > gb_length(gb)) i = gb_length(gb);
    gb_grow(gb, 1);
    gb_move_gap(gb, i);
    gb->data[gb->gap_start++] = c;
}

void gb_insert_str(GapBuffer *gb, size_t i, const char *s, size_t len) {
    if (i > gb_length(gb)) i = gb_length(gb);
    gb_grow(gb, len);
    gb_move_gap(gb, i);
    memcpy(gb->data + gb->gap_start, s, len);
    gb->gap_start += len;
}

void gb_delete(GapBuffer *gb, size_t i, size_t count) {
    size_t len = gb_length(gb);
    if (i >= len) return;
    if (i + count > len) count = len - i;
    gb_move_gap(gb, i);
    gb->gap_end += count;
}

void gb_copy_range(const GapBuffer *gb, size_t s, size_t e, char *out) {
    if (e > gb_length(gb)) e = gb_length(gb);
    for (size_t i = s; i < e; i++) {
        out[i - s] = gb->data[logical_to_phys(gb, i)];
    }
}

char *gb_to_cstr(const GapBuffer *gb) {
    size_t n = gb_length(gb);
    char *s = (char *)malloc(n + 1);
    gb_copy_range(gb, 0, n, s);
    s[n] = 0;
    return s;
}

bool gb_load_file(GapBuffer *gb, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    /* clear existing content */
    gb->gap_start = 0;
    gb->gap_end = gb->cap;
    char buf[4096];
    size_t pos = 0, r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) {
        gb_insert_str(gb, pos, buf, r);
        pos += r;
    }
    fclose(f);
    return true;
}

bool gb_save_file(const GapBuffer *gb, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    /* write before gap */
    if (gb->gap_start) fwrite(gb->data, 1, gb->gap_start, f);
    /* write after gap */
    size_t tail = gb->cap - gb->gap_end;
    if (tail) fwrite(gb->data + gb->gap_end, 1, tail, f);
    fclose(f);
    return true;
}
