# edit: edit a text file (Ctrl-S saves, Ctrl-Q quits)

```
edit FILE
```

Opens FILE in a full-screen editor on the terminal (the console, a
terminal window, or an SSH session with a terminal). A FILE that does not
exist yet is made when you save; the message line then says `New file.`.
`edit` has been in RitterOS since step 22b.

The text fills the screen. Below it, a bar in reverse video shows ` edit:
FILE`, `CRLF` for a file with Windows' line breaks (below), `(modified)`
once you changed something, and on the right the cursor's place: `line
12/40, column 7`. The last line shows the keys (`Ctrl-S save   Ctrl-Q
quit`), or a message after an action.

The text is UTF-8: the cursor moves by whole characters. A Tab is shown as
spaces to the next multiple of 8 columns; other control characters as `?`.
Long lines scroll sideways and long files up and down, so the cursor is
always on the screen. Files up to 1 MiB.

## Keys

| Key | Does |
|---|---|
| letters, digits, Tab | insert at the cursor |
| Enter | break the line |
| Backspace (or Ctrl-H) | erase the character before the cursor (at a line's start: join with the line above) |
| Delete | erase the character at the cursor |
| arrow keys | move by one character or line (up and down keep the column) |
| Home, End | the start or end of the line |
| Page Up, Page Down | a screen up or down |
| **Ctrl-S** | save |
| **Ctrl-Q** | quit; with unsaved changes it asks first: Ctrl-Q again quits without saving, Ctrl-S saves |
| Ctrl-C | does not end the editor: it says `Ctrl-Q quits.` |

Saving writes the whole file at once and adds a line break at the end if
the last line has none. If it fails, the file stays as it was on the disk,
and the text stays in the editor, so you can try again (or save after
fixing the reason).

## CR LF line breaks

Files with Windows' line breaks, CR LF (as many files made on Windows, and
the PlutoSDR's `config.txt`), are edited as any other since step 50i: the
CRs are not shown, the bar says `CRLF` after the name, the first message
says `55 lines, 1067 bytes, CR LF line breaks.`, and saving writes every
line break as CR LF again (`Saved: 55 lines, 1067 bytes, CR LF line
breaks.`), so the file keeps its form. A file is taken as one when its
first line ends in CR LF; a CR elsewhere, not before a line break, stays
and is shown as `?`.

## Exit status and messages

- `0`: the editor was quit (Ctrl-Q), or its input ended.
- `1`: the file cannot be edited: `edit: FILE: Is a directory`, `edit:
  FILE: Permission denied` (it cannot be read), `edit: FILE: too large (at
  most 1 MiB)`; `edit: the screen is too small` (fewer than 3 rows or 20
  columns); `edit: the terminal: ...` (no terminal to switch to raw input).
- `2`: not exactly one FILE: `usage: edit FILE`.

Messages on the last line:

- `12 lines, 340 bytes.`: the file was read (with `, CR LF line breaks`
  for such a file); `New file.` for a FILE not there yet.
- `Saved: 3 lines, 20 bytes.`: saved.
- `Not saved: Permission denied.` (or another reason): not saved.
- `Unsaved changes: Ctrl-Q again quits without saving, Ctrl-S saves.`
- `The text is full (at most 1 MiB).`: what was typed did not fit.

## Examples

```
ritteros:/$ edit /home/root/notes.txt
ritteros:/$ edit /volumes/PLUTO/config.txt
ritteros:/$ edit /system
edit: /system: Is a directory
```

In `make run` on a Mac, the editor draws for RitterOS's screen: use the
QEMU window, or make the Terminal window at least as large (139x48
characters at QEMU's 1280x800).

## See also

[`cat`](cat.md), [`sed`](sed.md) (changes without a screen), [`term`](term.md),
[`owl`](owl.md) (opens data files in `edit`)
