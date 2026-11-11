# notify: show a notification on the window system's screen (needs windowd)

```
notify TEXT...
```

Shows TEXT (the words, a space between them) in a box at the top right of
the screen for 5 seconds, over every window, with the window system
running (`windowd &`). `notify` returns at once. It came in step 41g-4.

Up to four notifications show at once, the newest on top; a fifth takes
the oldest's place. The text is wrapped to the box (up to 511 bytes, the
rest cut). Programs with a window send their own, with a title, through
libwindow's `window_notify(win, title, text)`.

## Exit status and messages

- `0`: windowd took the notification.
- `1`: windowd is not running (or the screen is another user's): `notify:
  no window server: REASON`.
- `2`: no TEXT: `usage: notify TEXT...`.

## Examples

```
ritteros:/$ windowd &
ritteros:/$ make > build.log 2>&1; notify make ended with $?
```

## See also

[`windowd`](windowd.md), [`open`](open.md)
