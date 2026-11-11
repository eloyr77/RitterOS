# windowd: the window server: shows windows on the screen (windowd & starts it)

```
windowd [headless WxH]
```

The window server. It takes the screen over from the text console, keeps
a picture of the whole screen in memory, and shows every program's window
on it with a title bar and border, the desktop behind them and the mouse
pointer on top. Pointer and keyboard go to the window in front. Programs
open windows with the toolkit `libwindow` (`window.h`) and talk to
`windowd` over the local socket `/run/windowd`; at most 8 windows are
open at once. While `windowd` runs, the text console waits; it comes
back when `windowd` ends. `windowd` came in step 15e (its interaction in
15e2).

It starts on demand, not at the boot: `windowd &` at the prompt, or a
line `windowd` in your `~/.config/login` (then the desktop comes up at
each login at the machine, and the prompt is there when you leave it).
When it starts it opens one program, the file browser `owl` by default,
from which you open terminals and other apps; when that program ends the
desktop stays up.

**Using it.** A click chooses and raises a window; dragging its title
bar moves it, dragging an edge or a corner resizes it; the title bar's
`+` maximizes it (again: back to its size and place), `-` minimizes it to
a button at the bottom of the screen, `X` closes it (its program ends).
A window always opens on the screen, made smaller if it does not fit.
Right-clicking the desktop's background opens the desktop menu: every app
bundle `/system/<arch>/apps/NAME.program` (42b), by the name in its `info`
(**Terminal**, **owl**, **Image viewer**...), and **Close window
server**, which gives the screen back to the text console. Ctrl + click is a right-click
everywhere. **Ctrl-Alt-Delete** leaves the window system even when a
program holding the screen is hung (the kernel ends it). `windowd` also
decides what a double click is and keeps the clipboard (up to 64 KiB)
for its user's programs. It shows notifications (a program's
`window_notify`, the shell's [`notify`](notify.md)) in boxes at the top
right for 5 seconds, over every window.

**Who may use it.** Root may start `windowd` anywhere, any other user
only when logged in at the machine's own screen and keyboard (not over
SSH): whoever has the screen gets every key typed at the machine. The
windows on it are its user's and root's only; a program of another user
is refused.

**Settings.** Each key is looked up in the user's `~/.config/windows`
first, then in `/system/config/windows` (optional, for everyone: nothing
writes it). The user's file is made by login, and (since 37e-1b) by
windowd when it is missing, with every key a comment showing its default:
remove the `#` to change one. The keys and their defaults:

```
startup = owl
double_click = 500
click_steady = on
refresh = auto
theme = dark
accent = 5E81AC
```

| Key | Meaning |
|---|---|
| `startup = PROGRAM [ARGUMENTS]` | what opens with the desktop (`owl` when not set; empty: nothing). A bare name is an app of `/system/<arch>/apps`, else a program of `/system/<arch>/bin` |
| `double_click = MS` | two presses within MS milliseconds (100-5000; 500 when not set) and 4 pixels are a double click |
| `click_steady = off` | clicks exactly where the pointer is; on when not set, which holds a touchpad's slow creep around a click |
| `refresh = auto` or `= HZ` | (65e-34b) how many frames a second it draws at most: `auto` (when not set) the screen's own rate from the monitor's EDID, else 60; a number (10-1000) for a monitor whose EDID is missing or wrong |
| `theme = dark` or `light` | (41g-5) the colours of the desktop, frames, menus and the programs' windows (libwidget's, `owl`'s, `term`'s): dark (Nord's polar night) when not set, light (its snow storm); read when windowd and each program start |
| `accent = RRGGBB` | (41g-5) the colour of what is in front and chosen: the title bar of the window in front, menus' chosen items, selections, focus rings, progress; the theme's (`5E81AC`, a blue) when not set |

Since 65e-34b changes are drawn once per frame: a change after a quiet
moment shows at once, and what changes meanwhile (a dragged window, the
pointer, a program's new picture) is drawn together in the next frame, at
most `refresh` frames a second, so a busy screen never falls behind.
Where a graphics driver tells the screen's vertical blank (the J1900's
Intel graphics, 66c-3), the frames start there instead, in step with the
screen, one per refresh at most (unless `refresh` sets a rate of its own).
Where the driver also has screen buffers (66d-3), windowd page-flips: each
frame is copied into the buffer not shown and that one is shown from the
next vertical blank, so the screen never shows a frame half drawn (a
program still drawing into its own window can still show half its new
picture). Closing windowd shows the console's buffer again.

When the screen's mode changes (`displays set`, 66b-5), windowd follows at
once: a back buffer of the new size, every window kept on the new screen,
maximized windows asked to fill it, the frame rate the new mode's (unless
`refresh` sets one), and everything drawn again (`windowd: screen now
1920x1080`). A terminal (`term`) grows to at most 170 columns and 60 rows,
so on a large screen it does not fill it.

When the screen goes off after a while without input (37e: `screen_off`
in `/system/config/power`), the kernel tells windowd (`INPUT_BLANK`):
windowd shows black and draws nothing until the next input brings the
screen back, then draws everything again. The key press or click that
woke the screen does not reach any window.

`kill -USR1 PID` (66c-1) asks windowd where its frames' time went: it
prints, and as root also writes to `/system/logs/windowd.log` (its own
output is the console it hides), `windowd: frames: N, composing X ms,
flush calls Y ms (P Mpx; per frame A ms and B ms; R MB/s flushed)`:
composing is drawing into its copy of the screen in memory, the flush
calls the kernel's copying of that to the screen (when page flipping:
windowd's own copying into the hidden buffer).

**Headless** (41h-1): `windowd headless WxH` takes no screen and no
keyboard or mouse (its input comes from [`wsend`](wsend.md)): it keeps
its WxH picture (64-8192 each way) in memory only, for testing programs (any user may run one, over SSH too), and
starts no `startup` program. It listens where the environment variable
`WINDOW_SOCKET` says, and programs given the same variable open their
windows there (libwindow connects to `$WINDOW_SOCKET`, `/run/windowd` when
it is not set), so it runs beside the screen's own:

```
$ WINDOW_SOCKET=/tmp/w1 windowd headless 800x600 &
windowd: listening on /tmp/w1, screen 800x600 (headless)
$ WINDOW_SOCKET=/tmp/w1 myapp &
$ WINDOW_SOCKET=/tmp/w1 wshot /tmp/myapp.png
```

[`wshot`](wshot.md) (41h-2) saves any windowd's screen as a picture;
[`wsend`](wsend.md) (41h-3) clicks, drags and types into a headless one
(a windowd on the screen refuses it).

Without `headless` it takes no options.

## Exit status and messages

- `0`: **Close window server** was chosen: `windowd: window server
  closed`.
- `1`: it could not start:
  - `windowd: cannot take the screen: Operation not permitted` (a user
    not at the machine), or another reason (no graphics);
  - `windowd: no screen info: WHY`, `windowd: no back buffer`,
    `windowd: cannot listen on /run/windowd: WHY` (or on
    `$WINDOW_SOCKET`).
- `2`: bad arguments: `usage: windowd [headless WxH] (W and H 64-8192)`.

Other lines:

- `windowd: listening on /run/windowd, screen 1280x800` when it is up
  (`windowd: listening on /tmp/w1, screen 800x600 (headless)` headless),
  then `windowd: 60.000 frames a second at most (the default)` (or `(the
  screen's EDID)`, `(the refresh setting)`), or with a driver's vertical
  blank `windowd: drawing at the screen's vertical blank (60.000 Hz)`, or
  with screen buffers `windowd: page flipping between 2 screen buffers
  (60.000 Hz)` (66d-3); `windowd: page flipping stopped (WHY): copying to
  the screen` if a flip fails.
- `windowd: refresh = VALUE: not auto or 10-1000 (frames a second)`.
- `windowd: screen now WxH` after a mode change, or `windowd: no back
  buffer for WxH` (it keeps drawing for the old size).
- `windowd: cannot start PROGRAM: WHY` when `startup`'s program cannot be
  started (the empty desktop waits).
- `windowd: double_click = MS: not 100-5000 (ms); 500 it is`
- `windowd: refused a program of user 1001 (this is user 1000's screen)`
- `windowd: no input thread`, `windowd: no reaper thread`, `windowd: no
  compositor thread`.

## Examples

```
ritteros:/$ windowd &
windowd: listening on /run/windowd, screen 1280x800
windowd: 60.000 frames a second at most (the default)
ritteros:/$ echo 'startup = term' > /system/config/windows
ritteros:/home/anna$ echo windowd > .config/login
```

## See also

[`term`](term.md), [`owl`](owl.md), [`ibrowser`](ibrowser.md),
[`imageview`](imageview.md), [`reel`](reel.md), [`notify`](notify.md), [`wshot`](wshot.md), [`wsend`](wsend.md), [`elf`](elf.md)
