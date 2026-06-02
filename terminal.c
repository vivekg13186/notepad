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

static void term_append_byte(Editor *ed, char c) {
    size_t L = gb_length(&ed->gb);
    bool at_end = (ed->cursor == L);
    gb_insert_char(&ed->gb, L, c);
    if (at_end) ed->cursor = gb_length(&ed->gb);
}

static void term_handle_backspace(Editor *ed) {
    size_t L = gb_length(&ed->gb);
    if (L == 0) return;
    bool at_end = (ed->cursor == L);
    gb_delete(&ed->gb, L - 1, 1);
    if (at_end) ed->cursor = gb_length(&ed->gb);
}

static void term_handle_cr(Editor *ed) {
    size_t L = gb_length(&ed->gb);
    size_t j = L;
    while (j > 0 && gb_char_at(&ed->gb, j - 1) != '\n') j--;
    if (j < L) {
        bool at_end = (ed->cursor == L);
        gb_delete(&ed->gb, j, L - j);
        if (at_end) ed->cursor = gb_length(&ed->gb);
    }
}

static void term_append_filtered(Editor *ed, const char *data, size_t len) {
    bool save_undo = ed->suppress_undo;
    ed->suppress_undo = true;

    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)data[i];
        if (c == 0x1B) {
            /* ESC ... — strip the entire control sequence. */
            i++;
            if (i >= len) break;
            char kind = data[i++];
            if (kind == '[') {                    /* CSI: ESC [ params final */
                while (i < len && (unsigned char)data[i] < 0x40) i++;
                if (i < len) i++;
            } else if (kind == ']') {             /* OSC: until BEL or ESC \ */
                while (i < len && data[i] != 0x07 && data[i] != 0x1B) i++;
                if (i < len) i++;
            } else if (kind == '(' || kind == ')') {
                if (i < len) i++;
            }
            continue;
        }
        if (c == 0x08)        { term_handle_backspace(ed); i++; continue; }
        if (c == '\r') {
            /* PTY in cooked mode emits CRLF.  Skip CR when followed by LF
               and let the LF land naturally.  A lone CR triggers line-reset. */
            if (i + 1 < len && data[i + 1] == '\n') { i++; continue; }
            term_handle_cr(ed);
            i++;
            continue;
        }
        if (c == 0x07)        { i++; continue; }     /* BEL */
        if (c == '\n' || c == '\t' ||
            (c >= 0x20 && c < 0x7F) || c >= 0x80) {
            term_append_byte(ed, (char)c);
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
        setenv("TERM", "dumb", 1);
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
