#ifndef GAP_BUFFER_H
#define GAP_BUFFER_H

#include <stddef.h>
#include <stdbool.h>

typedef struct {
    char  *data;
    size_t cap;
    size_t gap_start;   /* gap occupies [gap_start, gap_end) */
    size_t gap_end;
} GapBuffer;

void   gb_init(GapBuffer *gb, size_t initial_cap);
void   gb_free(GapBuffer *gb);
size_t gb_length(const GapBuffer *gb);
char   gb_char_at(const GapBuffer *gb, size_t logical_index);
void   gb_move_gap(GapBuffer *gb, size_t logical_index);
void   gb_insert_char(GapBuffer *gb, size_t logical_index, char c);
void   gb_insert_str(GapBuffer *gb, size_t logical_index, const char *s, size_t len);
void   gb_delete(GapBuffer *gb, size_t logical_index, size_t count);
/* copy [start,end) into out (must hold end-start bytes) */
void   gb_copy_range(const GapBuffer *gb, size_t start, size_t end, char *out);
/* load and save from/to file */
bool   gb_load_file(GapBuffer *gb, const char *path);
bool   gb_save_file(const GapBuffer *gb, const char *path);
/* serialize the entire buffer to a newly malloc'd null-terminated string */
char  *gb_to_cstr(const GapBuffer *gb);

#endif
