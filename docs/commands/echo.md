# echo: print the words

```
echo [WORD]...
```

Prints its words separated by one space each, then a line break. Without
words it prints only the line break (an empty line). With `>` the line goes
into a file: `echo hello > a.txt` makes (or replaces) `a.txt` with the
line `hello`, and `>>` adds it to the end.

`echo` is a program, not a shell built-in (the shell keeps built-ins for
what a program cannot do). It takes no options: `-n`, `-e` and every other
word starting with `-` are printed as they are, and backslashes are not
interpreted (`echo 'a\tb'` prints `a\tb`). Use [`printf`](printf.md) for
output without a line break or with escapes.

The shell expands the words before `echo` sees them: variables (`$HOME`),
command substitutions (`$(pwd)`), and patterns with `*` (`echo *.txt` lists
the matching names). Quoted words are passed as they are, spaces included.

## Exit status and messages

- `0`: always.

## Examples

```
ritteros:/$ echo hello world
hello world
ritteros:/$ echo "two   spaces kept" $HOME
two   spaces kept /home/root
ritteros:/$ echo -n no option
-n no option
ritteros:/$ echo first line > /home/root/notes.txt
ritteros:/$ echo second line >> /home/root/notes.txt
ritteros:/$ cat /home/root/notes.txt
first line
second line
```

## See also

[`printf`](printf.md), [`cat`](cat.md), [`sh`](sh.md) (`>` and `>>`,
expansions), [`tee`](tee.md)
