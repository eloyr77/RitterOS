# term: a terminal in a window (FONT of /system/fonts); with a command, runs it (needs windowd)

```
term [-f FONT] [CMD [ARG...]]
```

A terminal in a window of the window system: it runs the shell on a
pseudo-terminal, shows its output as a grid of characters and sends your
keys to it, so the whole command line works inside the window system.
`term` is a windowed app (in `/system/<arch>/apps`) and needs `windowd`
running. It came in step 15g, as the first graphical program.

The window starts with 80 columns and 25 rows; resizing it changes the
number of rows and columns, and the shell and programs are told the new
size. A small VT100 interpreter handles the cursor moves and erasing that
the line editor and programs such as `edit` use; colors are not kept yet
(light gray on black). The last 1000 lines that left the top are kept:
the mouse wheel (3 lines a step) or the scroll bar at the right shows
them, and a key or new output brings the view back to the bottom.

Dragging across the text selects it (shown inverted, the scrollback
too), a double click selects a word; **Ctrl+Shift+C** copies the
selection to the window system's clipboard (each line without its
trailing blanks) and **Ctrl+Shift+V** types the clipboard into the
program, line breaks as Enter. Plain Ctrl+C stays the interrupt.

Without CMD, `term` runs the shell (title `Terminal`) and the window
closes when the shell ends. With CMD it runs that instead (title: CMD's
name), which is how `owl` opens programs: a bare name is looked up in
`/system/<arch>/bin`, then in `/system/<arch>/apps` (42b: inside an app bundle, `NAME.program/NAME`); a name with a `/` is
run as it is. The window then stays open after CMD ends, with a line
`[NAME exited (CODE) - close this window]`, so its output can be read.

## Options

| Option | Meaning |
|---|---|
| `-f FONT` | the font for the text, a face of `/system/fonts` by its file's name without `.bdf` (`9x15`, the default; `10x20`, `6x13B`, ...); rows and columns are sized to its cell. Only as the first word |

## Exit status and messages

- `0`: the window was closed, or the shell ended.
- `1`: `term: cannot open a window (is windowd running?)`, `term: cannot
  open a window: the screen is another user's`, `term: no pseudo-terminal:
  WHY`, `term: cannot start NAME: WHY` (CMD not found: `No such file or
  directory`), `term: no reader thread`.

Other lines, on the console that started it: `term: started, font 9x15`,
and `term: no font NAME in /system/fonts; using 9x15` for an unknown FONT.

## Examples

```
ritteros:/$ term
ritteros:/$ term -f 10x20
ritteros:/$ term edit notes.txt
ritteros:/$ term -f 6x13 tail -f /system/logs/kernel.log
```

## See also

[`windowd`](windowd.md), [`owl`](owl.md), [`sh`](sh.md), [`edit`](edit.md),
[`clear`](clear.md)
