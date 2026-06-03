# Notepad

A lightweight, keyboard-centric desktop text editor inspired by the efficiency of NeoVim, designed for speed, simplicity, and a minimal resource footprint.

## Features

* **Lightweight & Fast**

  * Built with C and Raylib for excellent performance, low memory usage, and a modern native UI.

* **Keyboard-First Workflow**

  * NeoVim-inspired shortcuts and navigation.
  * Type `:help` to view all available commands and keyboard shortcuts.

* **Syntax Highlighting**

  * Supports TextMate grammars (`tmLanguage.json`) for language syntax highlighting.
  * Compatible with existing VS Code grammar definitions, allowing easy reuse of syntax files.

* **Customizable Themes**

  * Theme system powered by JSON configuration files.
  * Easily create, modify, and share editor themes without recompiling the application.

* **User-Defined Snippets**

  * Snippet definitions stored in simple JSON files.
  * Add, edit, or remove snippets by modifying configuration files directly.

* **Small Footprint, Rich UI**

  * Native desktop application with minimal dependencies.
  * Responsive interface powered by Raylib while maintaining a compact executable size.

## Screens

![Alt text](screenshot/main.png "Editor")
![Alt text](screenshot/help.png "Help")

## Build

Install raylib first:

```
brew install raylib            # macOS
sudo apt install libraylib-dev # Debian/Ubuntu
```

Then:

```
make
./notepad main.c c.tmLanguage.json themes/dark.json
```

`argv[3]` is optional; if omitted, `themes/dark.json` is loaded when present,
otherwise the built-in dark palette is used. Switch at runtime with
`:colorscheme dark`, `:colorscheme light`, `:colorscheme solarized-dark`, or
`:colorscheme path/to/file.json`.

## Features

- Gap buffer for fast inserts/deletes
- Modal editing: NORMAL / INSERT / VISUAL / COMMAND / SEARCH
- Line numbers gutter, status bar with `filename  line:col`
- Selection (visual mode) with copy/cut/paste
- Syntax highlighting from a TextMate grammar JSON (subset: `match` rules)
- Ex commands

## Keys

NORMAL mode:

```
h j k l    move
0 $        line start / end
gg G       document start / end
i a A I    enter INSERT (at, after, end of line, line start)
o O        open line below / above
x          delete char
dd         delete current line
yy / y     yank current line
p          paste
v          enter VISUAL
:          enter COMMAND
/          search
n          repeat last search
```

INSERT mode: type text. ESC to return to NORMAL.

VISUAL mode: move to extend selection; `y` yank, `d`/`x` cut, `p` paste, ESC cancel.

Commands:

```
:w              save; opens the native save-as dialog if the buffer is untitled
:w <file>       save as (supports ~/ paths, e.g. :w ~/Desktop/hello.js)
:wq             save and quit (prompts via dialog if untitled)
:q              quit (fails if dirty)
:q!             force quit
:o              open a native file picker dialog (macOS osascript / Linux zenity)
:o <file>       open file directly (supports ~/ paths)
:42             goto line 42
:goto 42        goto line 42
:find <text>        search forward for text
:replace a b        replace the first occurrence of a with b
:replace-all a b    replace every occurrence of a with b across the buffer
:theme              open a dropdown of every theme (live preview; Enter applies, Esc cancels)
:themes             alias for :theme
:theme <name>       switch color theme directly (themes-map key | dark | light | path.json)
:format             open a dropdown of every schema (Enter applies, Esc cancels)
:formats            alias for :format
:format <name>      switch syntax grammar directly
:help           open the cheat-sheet dialog (any key to close)
```

## Configuration

On startup `notepad` reads `notepad.config.json` in two passes:

1. The file next to the executable provides defaults.
2. A file with the same name in the current working directory overrides those defaults.

CLI args still override the config; theme / grammar paths are resolved
relative to the cwd.

```json
{
  "theme":       "darks",
  "grammar":     "c.tmLanguage.json",
  "font_size":   18,
  "line_height": 22,
  "tab_width":   4,
  "window_w":    1100,
  "window_h":    720,
  "gutter_w":    60,

  "themes": {
    "darks":          "./themes/dark.json",
    "light":          "./themes/light.json",
    "solarized-dark": "./themes/solarized-dark.json"
  },

  "$schema": {
    "**.c": {
      "file_extensions": [".c", ".h"],
      "name":            "C",
      "grammar":         "c.tmLanguage.json"
    }
  }
}
```

`theme` accepts a key from the `themes` map, the built-in names `"dark"` /
`"light"`, a basename in `themes/`, or a direct path.

`themes` is a named map of color profiles. `:colorscheme <key>` first
looks the key up here, then falls back to built-ins / `themes/<key>.json` /
literal paths.

`$schema` is a map of named filetype schemas. Each entry has:

- `file_extensions` — array of extensions (with the dot, e.g. `".c"`)
- `name` — display name
- `grammar` — path to the TextMate grammar JSON

When you open a file (CLI arg or `:e file`), notepad picks the schema
whose `file_extensions` contains the file's extension and loads its grammar.
You can also force one with `:setft <id>` where `<id>` is the schema's key
(or its `name`).

## Color profiles

A profile is a JSON object with hex colors (`#rgb`, `#rrggbb`, or `#rrggbbaa`).
All keys are optional; unspecified ones inherit from the built-in dark theme.

```json
{
  "name": "Dark",
  "bg":            "#1e222a",
  "fg":            "#dcdcdc",
  "gutter_bg":     "#181b21",
  "gutter_fg":     "#5a6473",
  "gutter_fg_cur": "#dcdcdc",
  "status_bg":     "#282e38",
  "status_fg":     "#dcdcdc",
  "cursor":        "#f0f0f0",
  "cursor_block":  "#f0f0f050",
  "cursor_glyph":  "#1e222a",
  "selection":     "#3c5a82c8",
  "syntax": {
    "default":  "#dcdcdc",
    "keyword":  "#c678dd",
    "string":   "#98c379",
    "comment":  "#5c6370",
    "number":   "#d19a66",
    "type":     "#e5c07b",
    "constant": "#e5c07b",
    "function": "#61afef",
    "operator": "#abb2bf"
  }
}
```

Drop new themes into `themes/<name>.json` and switch with `:colorscheme <name>`.
Three ship by default: `dark`, `light`, `solarized-dark`.

## Grammar

Three grammars ship in `syntax/`:

- `c.tmLanguage.json`       — C
- `js.tmLanguage.json`      — JavaScript / TypeScript (.js, .mjs, .cjs, .jsx, .ts, .tsx)
- `python.tmLanguage.json`  — Python (.py, .pyw, .pyi)

Only the `patterns` array with top-level `match` + `name` keys is read.
Regexes are POSIX-extended (no PCRE lookarounds). Scope names map to a small
palette (`keyword`, `string`, `comment`, `constant.numeric`, `storage.type`,
`support.type`, `entity.name.function`, `keyword.operator`).

Selection happens automatically by file extension via the `$schema` map in
`notepad.config.json`. Force a specific grammar with `:setft <id>`, or override
the default with `argv[2]`.
