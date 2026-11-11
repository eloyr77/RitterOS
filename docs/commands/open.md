# open: open files and folders with their apps (needs windowd)

```
open FILE...
```

Opens each FILE as a double click in `owl` would, with the window system
running (`windowd &`); the apps start on their own and `open` returns at
once. It came in step 41g-2 (macOS's `open` is the model).

- An app's bundle (42b: a folder `NAME.program` holding its program `NAME`)
  is started: `open: DIR started`.
- Any other folder opens in `owl`.
- A file opens in the program your file types give its ending (42a):
  `~/.config/filetypes`, `ENDING = PROGRAM` lines (in small or capital
  letters), made with these when missing: pictures (`.ppm`, `.bmp`,
  `.png`, `.jpg`, `.jpeg`) in `imageview`, videos (`.rvid`) in `reel`, web
  pages (`.html`, `.htm`) in `ibrowser`, sounds (`.wav`) in `tune`, text
  (`.txt`, `.md`, `.c`, `.h`) in `edit`. Change or add lines to choose
  others (an ending missing there is looked up in `/system/config/
  filetypes`, an administrator's, if there is one). An app (in
  `/system/<arch>/apps`) opens in its window, a command-line program (in
  `bin`, as `tune` and `edit`) in a terminal window.
- Any other program (a file with an `x` bit) runs in a terminal window.
- Anything else opens in the editor in a terminal window (`term edit`).

## Exit status and messages

For each FILE a line says where it went: `open: FILE in imageview`,
`open: FILE in tune, in a terminal`, `open: FILE run in a terminal`,
`open: FILE in edit, in a terminal`, `open: DIR in owl`, `open: DIR started`.

- `0`: every FILE was opened.
- `1`: one or more could not be, the reason on standard error: `open:
  FILE: No such file or directory`; `open: FILE: cannot start its app:
  REASON`. The others are opened all the same.
- `2`: no FILE: `usage: open FILE...`.

## Examples

```
ritteros:/$ windowd &
ritteros:/$ open /home/root/photo.jpg /home/root
open: /home/root/photo.jpg in imageview
open: /home/root in owl
```

## See also

[`owl`](owl.md), [`windowd`](windowd.md), [`imageview`](imageview.md),
[`reel`](reel.md), [`tune`](tune.md), [`edit`](edit.md)
