#ifndef COMMANDS_H
#define COMMANDS_H

#include "editor.h"

/* Execute a vim-style command line (the text typed after ':').
   Returns true if the editor should remain running. */
bool cmd_execute(Editor *ed, const char *line);

/* Perform a :s/find/replace/[g] operation across the buffer (global=true)
   or current line (global=false). Returns number of replacements. */
int  cmd_substitute(Editor *ed, const char *pattern, const char *replacement, bool global, bool all_lines);

#endif
