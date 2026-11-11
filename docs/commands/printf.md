# printf: print the arguments in a format

```
printf FORMAT [ARGUMENT]...
```

Prints FORMAT with its conversions filled in from the arguments, as POSIX
says (since step 51d). Unlike `echo` it adds no line break of its own:
put `\n` in the format. The format is used again while arguments are left,
so one format prints a whole table; arguments that are missing are empty
(or 0 for a number).

| Conversion | Prints |
|---|---|
| `%s` | a string |
| `%d`, `%i` | a whole number |
| `%u`, `%o`, `%x`, `%X` | a number without a sign, in decimal, octal, hexadecimal |
| `%c` | the argument's first character |
| `%e` `%E` `%f` `%F` `%g` `%G` `%a` `%A` | a number with a fraction |
| `%b` | a string with its escapes (below) turned into characters; `\c` in it stops all output |
| `%%` | a `%` |

Between the `%` and the letter: flags (`-` left-aligned, `+` always a
sign, a space, `#`, `0` padded with zeros), a width and a `.precision`;
`*` takes either from the next argument. Size letters (`l`, `h`, `ll`,
...) are allowed and ignored. A number's argument may be decimal, `0x...`
hexadecimal or `0...` octal; `'A` (or `"A`) is the code of the character A.

Escapes in FORMAT: `\n` `\t` `\\` `\a` `\b` `\f` `\r` `\v` `\"` `\'`,
`\NNN` (up to three octal digits) and `\c` (stop here). In `%b`'s argument
the octal form is `\0NNN`. An unknown escape prints the backslash.

## Exit status and messages

- `0`: printed.
- `1`: an argument was not a number (`printf: X: not a number`; it is used
  as 0, or as far as it was one: `12abc` is 12) or too large (`printf: X:
  out of range`, used as 0), or the format has an unknown conversion
  (`printf: %y: not a conversion`; printing stops there).
- `2`: no FORMAT: `usage: printf FORMAT [ARGUMENT]...`.

## Examples

```
ritteros:/$ printf '%-8s %5d\n' apples 12 pears 3
apples      12
pears        3
ritteros:/$ printf '%x %o %d\n' 255 8 "'A"
ff 10 65
ritteros:/$ printf '%.2f%%\n' 99.5
99.50%
ritteros:/$ printf '%b' 'one\ttwo\n'
one	two
ritteros:/$ printf '%d\n' ten
printf: ten: not a number
0
printf: exit code 1
```

## See also

[`echo`](echo.md), [`awk`](awk.md) (its `printf`), [`sh`](sh.md)
