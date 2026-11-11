# owl: a graphical file browser (needs windowd)

```
owl [DIR]
```

A file browser in a window of the window system (it needs `windowd`
running: `windowd &`, which starts `owl` by default when it starts). It
lists a folder: directories first, marked with a `/` and in green, then
files with their sizes in bytes, each group sorted by name; a `..` row
leads up (except at `/`). The current path is at the top of the window in
yellow, and a line at the bottom shows what went wrong last, or what Copy
holds. It starts in your home directory, or in DIR if one is given (a DIR
that is no folder gives `/`). The window is 720x420 pixels at first (80
columns, the path line and 27 rows); it can be resized, and a scroll bar
at its right edge shows where the view is when the folder has more rows
(at most 512 entries are listed). `owl` lives in `/system/<arch>/apps`,
the folder of windowed apps. It came in step 17b, its menus in 19c-21.

Names starting with a dot are not listed (41g-6: hidden files, such as
`~/.config`; `ls` shows them). An app is a bundle (42b), a folder
`NAME.program` holding its program `NAME`, its `info` and `icon.png`:
`owl` shows it, in any folder, as one item, its icon left of `NAME`,
among the files. **Show contents** (in the Edit menu and a row's menu)
goes into a bundle instead of running it.

**Opening** (Enter, a double click, or Open in a menu) depends on the
item, by where the file lives:

- a folder is entered;
- an app (a bundle, `NAME.program`) runs its program on its own;
- a file opens in the program its ending has in your file types,
  `~/.config/filetypes` (42a; the same as [`open`](open.md)): a picture
  (`.ppm`, `.bmp`, `.png`, `.jpg`, `.jpeg`) in `imageview`, a video
  (`.rvid`) in `reel`, a web page (`.html`, `.htm`) in `ibrowser`, a sound
  (`.wav`) in `tune` in a terminal window, text (`.txt`, `.md`, `.c`, `.h`)
  in `edit` in a terminal window, until you change them (the endings in
  small or capital letters; a program in `bin` runs in a terminal);
- any other program (a file with an `x` bit) runs in a terminal window
  (`term PATH`) that stays open so its output can be read;
- anything else opens in the editor in a terminal window (`term edit
  PATH`).

**Open with...** (42c, in the File menu and a file's right-click menu)
opens a file in another app than its usual one. It brings up the Open
dialog at `/system/<arch>/apps`, choosing an app instead of a file: the
apps whose `info` lists the file's ending (its `types`) are in the text's
colour and can be chosen, the others are greyed; any folder can be browsed
for apps elsewhere. A double click, Enter, the Open button or the app's
name typed (as it is shown, without `.program`) starts that app with the
file. Your file types are not changed by it.

**Menus.** The menu bar has **File** (New folder, Open, Open with..., Up, Refresh, Quit),
**Edit** (Rename, Delete, Copy, Paste, Show contents) and **Go** (Home, your home
directory; Apps, `/system/<arch>/apps`; Root (/)). A right click on a row
(or Ctrl + click, as on a touchpad) selects it and opens a menu for it:
Open, Open with..., Show contents, Rename, Delete, Copy. New folder and Rename ask for the name in the
path's line (`New folder: NAME_`, `Rename to: NAME_`): type it and press
Enter; Escape cancels, an empty name does nothing. Delete asks `Delete
NAME? (y/n)`: `y` removes a file or an empty folder, any other key keeps
it. Copy remembers the selected file or folder; Paste copies it into the
folder shown, under its own name (an instant copy on the same RitterFS
volume, else byte by byte). Refresh reads the folder again.

**Dragging out.** A file or folder pressed and moved (5 pixels or more,
the button held) is dragged: the pointer carries a small page, and letting
go over another program's window gives that program the entry's full path
(41f-6). What it does with it is its own (`..` is not dragged).

## Keys and the mouse

| Key or action | Does |
|---|---|
| Up, Down | move the selection |
| Enter | open the selected item |
| Backspace | up to the parent folder |
| a click | select the row |
| a double click | open the row |
| a press and a move | drag the row's path to another window |
| a right click, Ctrl + click | select the row and open its menu |
| the wheel | scroll 3 rows a step |
| the scroll bar | press to jump there, drag to move |
| Enter, Escape, Backspace (in a name prompt) | take the name, cancel, erase a character |
| `y` (when Delete asks) | delete; any other key cancels |

Closing the window (its `X`, or File > Quit) ends `owl`.

## Exit status and messages

- `0`: the window was closed.
- `1`: `owl: cannot open a window (is windowd running?)`; `owl: cannot open
  a window: the screen is another user's` (the window system is another
  user's).

It writes what it does to its standard output (the console or the log):
`owl: started`, `owl: at /home/root` (each folder it goes to), `owl: new folder
PATH`, `owl: showing rows 28-54 of 300` (after scrolling). The line at the
bottom of the window shows an error by its text (`Permission denied`,
`Directory not empty`, `File exists`) or `copied PATH` after Copy.

## Examples

```
ritteros:/$ windowd &
ritteros:/$ owl /test &
owl: started
owl: at /test/docs
owl: new folder /test/docs/old
```

## See also

[`windowd`](windowd.md), [`term`](term.md), [`edit`](edit.md),
[`imageview`](imageview.md), [`reel`](reel.md), [`tune`](tune.md),
[`ls`](ls.md), [`cp`](cp.md)
