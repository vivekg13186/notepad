#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "terminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

#if defined(__APPLE__)
#  include <util.h>          /* forkpty on macOS */
#elif defined(__linux__)
#  include <pty.h>           /* forkpty on Linux (link with -lutil) */
#else
#  error "no forkpty on this platform"
#endif

/* -------------------------------------------------------------- *
 *  Bytewise append with terminal-control handling                 *
 *                                                                  *
 *  This is not a VT100 — escape sequences are stripped.  We do     *
 *  handle the byte-level effects most shells produce so the        *
 *  buffer reads correctly:                                          *
 *    \b      delete the previous byte (echoed backspace)           *
 *    \r      bare CR — line reset (progress bars)                  *
 *    \r\n    treat as plain newline (CRLF from cooked tty)         *
 * -------------------------------------------------------------- */

/* ed->cursor IS the terminal's write cursor.  Printable bytes overwrite at
   the cursor; \b / \r / CSI sequences move it around. */

static char gb_at(Editor *ed, size_t i) {
    return (i < gb_length(&ed->gb)) ? gb_char_at(&ed->gb, i) : 0;
}

/* Insert or overwrite one byte at ed->cursor in terminal-output style:
   if the cursor is at a non-newline byte, replace it; otherwise insert. */
static void term_putc(Editor *ed, char c) {
    size_t L = gb_length(&ed->gb);
    if (ed->cursor < L && gb_at(ed, ed->cursor) != '\n') {
        gb_delete(&ed->gb, ed->cursor, 1);
        gb_insert_char(&ed->gb, ed->cursor, c);
        ed->cursor++;
    } else {
        gb_insert_char(&ed->gb, ed->cursor, c);
        ed->cursor++;
    }
}

/* Backspace: just move the cursor left (don't erase).  Stops at line start. */
static void term_bs(Editor *ed) {
    if (ed->cursor > 0 && gb_at(ed, ed->cursor - 1) != '\n')
        ed->cursor--;
}

/* CR: move the cursor to the start of the current line. */
static void term_cr_move(Editor *ed) {
    while (ed->cursor > 0 && gb_at(ed, ed->cursor - 1) != '\n')
        ed->cursor--;
}

/* LF: append newline at end of buffer and move cursor there. */
static void term_lf(Editor *ed) {
    size_t L = gb_length(&ed->gb);
    ed->cursor = L;
    gb_insert_char(&ed->gb, L, '\n');
    ed->cursor = L + 1;
}

/* CSI K (n=0): clear from cursor to end of line. */
static void term_clear_to_eol(Editor *ed) {
    size_t i = ed->cursor;
    size_t L = gb_length(&ed->gb);
    while (i < L && gb_at(ed, i) != '\n') i++;
    if (i > ed->cursor) gb_delete(&ed->gb, ed->cursor, i - ed->cursor);
}

/* CSI 1K: clear from start of line to cursor. */
static void term_clear_to_bol(Editor *ed) {
    size_t ls = ed->cursor;
    while (ls > 0 && gb_at(ed, ls - 1) != '\n') ls--;
    if (ed->cursor > ls) {
        gb_delete(&ed->gb, ls, ed->cursor - ls);
        ed->cursor = ls;
    }
}

/* CSI 2K: clear the entire current line (keeps the surrounding newlines). */
static void term_clear_line(Editor *ed) {
    size_t ls = ed->cursor;
    while (ls > 0 && gb_at(ed, ls - 1) != '\n') ls--;
    size_t le = ed->cursor;
    size_t L = gb_length(&ed->gb);
    while (le < L && gb_at(ed, le) != '\n') le++;
    if (le > ls) {
        gb_delete(&ed->gb, ls, le - ls);
        ed->cursor = ls;
    }
}

/* CSI 0J: clear from cursor to end of buffer.  Used by some progress
   reporters and full-screen apps. */
static void term_clear_to_end(Editor *ed) {
    size_t L = gb_length(&ed->gb);
    if (L > ed->cursor) gb_delete(&ed->gb, ed->cursor, L - ed->cursor);
}

/* CSI nD: cursor back n columns (stops at line start). */
static void term_csi_back(Editor *ed, int n) {
    if (n <= 0) n = 1;
    while (n-- > 0) term_bs(ed);
}

/* CSI nC: cursor forward n columns (stops at end of line). */
static void term_csi_forward(Editor *ed, int n) {
    if (n <= 0) n = 1;
    size_t L = gb_length(&ed->gb);
    while (n-- > 0 && ed->cursor < L && gb_at(ed, ed->cursor) != '\n')
        ed->cursor++;
}

static void term_append_filtered(Editor *ed, const char *data, size_t len) {
    bool save_undo = ed->suppress_undo;
    ed->suppress_undo = true;

    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)data[i];

        /* --- ESC sequences --- */
        if (c == 0x1B) {
            i++;
            if (i >= len) break;
            char kind = data[i++];
            if (kind == '[') {
                /* CSI: parse parameters then a final byte */
                char params[32]; int plen = 0;
                while (i < len && (unsigned char)data[i] >= 0x20 &&
                                  (unsigned char)data[i] < 0x40) {
                    if (plen < (int)sizeof params - 1) params[plen++] = data[i];
                    i++;
                }
                params[plen] = 0;
                if (i >= len) break;
                char final_byte = data[i++];
                int n = (plen > 0 && params[0] >= '0' && params[0] <= '9')
                            ? atoi(params) : 0;
                switch (final_byte) {
                    case 'K':                       /* erase in line */
                        if      (n == 0) term_clear_to_eol(ed);
                        else if (n == 1) term_clear_to_bol(ed);
                        else if (n == 2) term_clear_line(ed);
                        break;
                    case 'J':                       /* erase in display */
                        if      (n == 0) term_clear_to_end(ed);
                        /* n==1 / n==2 (full screen) intentionally dropped —
                           we don't have a fixed-screen model, and acting on
                           2J would wipe scrollback.  Most tools that send
                           it (vim/htop/less) are full-screen apps which
                           also need cursor positioning we don't render. */
                        break;
                    case 'D': term_csi_back(ed, n);    break;
                    case 'C': term_csi_forward(ed, n); break;
                    /* A/B (up/down) / H (move-cursor) / m (SGR colours) /
                       ?-private modes: dropped silently. */
                    default: break;
                }
            } else if (kind == ']') {
                /* OSC: until BEL or ESC \ */
                while (i < len && data[i] != 0x07 && data[i] != 0x1B) i++;
                if (i < len) i++;
            } else if (kind == '(' || kind == ')') {
                if (i < len) i++;
            }
            continue;
        }

        /* --- C0 controls --- */
        if (c == 0x08)  { term_bs(ed); i++; continue; }
        if (c == '\r') {
            /* CRLF from cooked tty → treat as plain LF.  Lone CR → carriage
               return (cursor to line start). */
            if (i + 1 < len && data[i + 1] == '\n') {
                term_lf(ed);
                i += 2; continue;
            }
            term_cr_move(ed);
            i++;
            continue;
        }
        if (c == '\n')  { term_lf(ed);     i++; continue; }
        if (c == 0x07)  { i++; continue; }     /* BEL */

        /* --- printable / utf-8 lead+continuation --- */
        if (c == '\t' || (c >= 0x20 && c < 0x7F) || c >= 0x80) {
            term_putc(ed, (char)c);
        }
        i++;
    }
    ed->suppress_undo = save_undo;
    ed->dirty = false;
}

/* -------------------------------------------------------------- *
 *  Public API                                                      *
 * -------------------------------------------------------------- */

bool term_spawn(Editor *ed, const char *shell) {
    int master = -1;
    struct winsize ws = { .ws_row = 40, .ws_col = 120 };
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) return false;
    if (pid == 0) {
        const char *sh = (shell && *shell) ? shell : getenv("SHELL");
        if (!sh || !*sh) sh = "/bin/sh";
        /* Tell the shell we're a basic ANSI terminal.  We strip escape
           sequences in term_append_filtered, but most shells need a non-dumb
           TERM or they suppress their prompt and line editing entirely. */
        setenv("TERM", "xterm", 1);
        execlp(sh, sh, "-i", (char *)NULL);
        _exit(127);
    }
    int flags = fcntl(master, F_GETFL, 0);
    if (flags >= 0) fcntl(master, F_SETFL, flags | O_NONBLOCK);

    ed->pty_fd      = master;
    ed->pty_pid     = (int)pid;
    ed->is_terminal = true;
    snprintf(ed->filename, sizeof ed->filename, "[term %d]", (int)pid);
    return true;
}

void term_close(Editor *ed) {
    if (!ed->is_terminal) return;
    if (ed->pty_fd >= 0) close(ed->pty_fd);
    if (ed->pty_pid > 0) {
        kill(ed->pty_pid, SIGHUP);
        int status;
        waitpid(ed->pty_pid, &status, WNOHANG);
    }
    ed->pty_fd      = -1;
    ed->pty_pid     = 0;
    ed->is_terminal = false;
}

void term_poll(Editor *ed) {
    if (!ed || !ed->is_terminal || ed->pty_fd < 0) return;
    char buf[8192];
    for (;;) {
        ssize_t n = read(ed->pty_fd, buf, sizeof buf);
        if (n > 0) { term_append_filtered(ed, buf, (size_t)n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n == 0) {
            term_append_filtered(ed, "\n[process exited]\n", 19);
            close(ed->pty_fd);
            ed->pty_fd = -1;
            break;
        }
        if (n < 0) break;
    }
}

void term_send(Editor *ed, const char *s, size_t len) {
    if (!ed->is_terminal || ed->pty_fd < 0 || len == 0) return;
    ssize_t n = write(ed->pty_fd, s, len);
    (void)n;
}

void term_send_char(Editor *ed, char c) {
    term_send(ed, &c, 1);
}
