# wshot: save the window system's screen as a picture (needs windowd)

```
wshot FILE.png
```

Saves the whole screen as `windowd` composed it (the desktop, every
window, the mouse pointer) as a picture, the same user's or root's. It
came in step 41h-2, with the headless window server for testing programs:
with `WINDOW_SOCKET` set it asks that `windowd` (a headless one's), else
the screen's at `/run/windowd`. The file's ending chooses the format:
`.png` (compressed, RGB), `.bmp`, anything else PPM.

## Exit status and messages

- `0`: saved: `wshot: FILE (WIDTHxHEIGHT)`.
- `1`: `wshot: no window server at /run/windowd: REASON` (or at
  `$WINDOW_SOCKET`), `wshot: windowd sent no picture`, or `wshot: FILE:
  REASON` when the file cannot be written.
- `2`: no FILE: `usage: wshot FILE.png`.

## Examples

```
$ export WINDOW_SOCKET=/tmp/w1
$ windowd headless 800x600 &
$ ./myapp &
$ wshot /tmp/myapp.png
wshot: /tmp/myapp.png (800x600)
```

## See also

[`windowd`](windowd.md), [`notify`](notify.md)
