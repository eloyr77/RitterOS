# wsend: send keyboard and pointer input to a headless windowd (for tests)

```
wsend click|right|move X Y | drag X1 Y1 X2 Y2 | type TEXT | key|down|up NAME | wait [TITLE]
```

Sends input to the headless window server (`windowd headless WxH`, named
by `WINDOW_SOCKET`) as if from a mouse and a keyboard: it focuses, raises,
opens menus and reaches the window under the pointer or in front exactly
as the machine's devices do. Each event is answered before the next, so
when `wsend` returns everything it sent has been handled (a `wshot` after
it sees the result). It came in step 41h-3, for testing programs without
a screen. A `windowd` on the screen refuses it: its input is the
machine's keyboard and mouse.

- `move X Y`: the pointer to (X, Y), in screen pixels.
- `click X Y`: the pointer there, the left button pressed and let go;
  `right X Y` the same with the right button. Two clicks in a row within
  the double-click time are a double click.
- `drag X1 Y1 X2 Y2`: the left button pressed at the first point, the
  pointer moved to the second in steps of at most 16 pixels, let go there
  (a window's title bar moves the window; dragging in a program is that
  program's drag).
- `type TEXT`: each byte of TEXT as a key's byte (UTF-8 too): what
  programs read as typed text (no physical keys: a game reading keys by
  their place sees none).
- `key NAME`: a key as the keyboard sends one: the physical key pressed,
  its bytes, the key let go.
- `down NAME`, `up NAME` (41h-5): the key pressed (with its bytes) and
  held until its `up`: walking in a game, a held Ctrl or Shift. Each is
  its own `wsend`, so a script holds a key over other commands:
  `wsend down ctrl; wsend key a; wsend up ctrl`.

- `wait` (41h-6): until the windowd at `$WINDOW_SOCKET` answers (just
  started: instead of a `sleep`); `wait TITLE`: until a window of that
  title is open (a program's window, ready for input: the title the
  program gives it, as its title bar shows; imageview's is the file's
  name). Each looks every
  100 ms for at most 10 seconds. It is answered by any windowd, the
  screen's too.

Key names: `a`-`z`, `0`-`9`, `space`, `enter`, `tab`, `escape`,
`backspace`, `up`, `down`, `left`, `right`, `home`, `end`, `pageup`,
`pagedown`, `insert`, `delete`, `ctrl`, `shift`, `alt` (the left ones),
`f1`-`f12`. A key's bytes are those of the key alone (Ctrl, Shift, Alt
and the F keys have none): a held `ctrl` does not turn `a` into a control
byte, but a program reading physical keys (`WINDOW_KEY_DOWN`) sees Ctrl
held as a keyboard's.

## Exit status and messages

- `0`: every event was handled.
- `1`: `wsend: no window server at /run/windowd: REASON` (or at
  `$WINDOW_SOCKET`), `wsend: refused: only a headless windowd takes
  input`, `wsend: the window server stopped answering`.
- `1` from `wait`: `wsend: no window server at PATH (waited 10 s)`, `wsend:
  no window "TITLE" at PATH (waited 10 s)`.
- `2`: bad arguments: `usage: ...`, or `wsend: no key NAME (a-z, 0-9, ...)`.

## Examples

```
$ export WINDOW_SOCKET=/tmp/w1
$ windowd headless 800x600 & wsend wait
$ ./myapp & wsend wait myapp
$ wsend click 200 150; wsend type hello; wsend key enter
$ wshot /tmp/after.png
```

## See also

[`windowd`](windowd.md), [`wshot`](wshot.md)
