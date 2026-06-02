#ifndef TERMINAL_H
#define TERMINAL_H

#include "editor.h"
#include <stdbool.h>
#include <stddef.h>

/* Spawn a shell inside `ed` via forkpty.  `shell` may be NULL to use $SHELL. */
bool term_spawn(Editor *ed, const char *shell);

/* Tear down: close the PTY, reap the child. */
void term_close(Editor *ed);

/* Non-blocking read of pending PTY output, filtered (ANSI escapes dropped,
   \b and \r handled).  Appends to the buffer.  Safe to call every frame. */
void term_poll(Editor *ed);

/* Send raw bytes / a single byte to the child shell. */
void term_send(Editor *ed, const char *s, size_t len);
void term_send_char(Editor *ed, char c);

#endif
