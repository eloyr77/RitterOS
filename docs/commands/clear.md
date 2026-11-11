# clear: clear the screen

```
clear
```

Empties the screen and puts the cursor in its top left corner, so the next
prompt is at the top. `clear` only writes the two escape sequences `ESC [ 2
J` (erase the screen) and `ESC [ H` (cursor home): the console understands
them, and so does a terminal on the serial port, an SSH session or `term`.
`clear` has been in RitterOS since step 26. Ctrl-L at the prompt does the
same and shows the prompt again at the top (since step 30).

`clear` takes no options; arguments are ignored.

## Exit status and messages

- `0`: the sequences were written.
- `1`: they could not be written (the output was closed).

`clear` prints no messages.

## Examples

```
ritteros:/$ clear
ritteros:/$ ls; clear
```

## See also

[`sh`](sh.md) (Ctrl-L), [`term`](term.md), [`edit`](edit.md)
