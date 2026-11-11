# cat: show files, or what is typed until Ctrl-D

```
cat [FILE]...
```

Prints the files one after the other on its standard output. Without FILE
it prints what you type until Ctrl-D (or whatever comes down a pipe).
`cat` takes no options: every argument is a FILE.

A file with a zero byte in it (a program, a picture) is binary: shown on
the terminal it would garble the screen, so `cat` names it instead of
printing it -- but only when its standard output is a terminal.
Redirected to a file (`cat a > b`), sent down a pipe or over an ssh
connection, binary passes through unchanged, so `cat` also moves programs
and pictures: `ssh root@ritteros 'cat > /home/root/game' < game` copies a
file in from another machine.

## Exit status and messages

- `0`: every file was printed.
- `1`: a FILE could not be opened or read (`cat: FILE: No such file or
  directory`, `cat: FILE: Permission denied`; the other files are still
  printed), or a file was binary and the output a terminal (`cat: FILE:
  binary file`; it stops at that file's first block of 4096 bytes that has
  a zero byte, so the part before may already be on the screen). Reading
  the typed input, the name in messages is `standard input`.

## Examples

```
ritteros:/$ cat /readme.txt
ritteros:/$ cat /system/logs/kernel.log | grep nvme
ritteros:/$ cat part1.txt part2.txt > whole.txt
ritteros:/$ cat > notes.txt
first line
second line
^D
ritteros:/$ cat /system/x86_64/bin/ls
cat: /system/x86_64/bin/ls: binary file
```

## See also

[`head`](head.md), [`tail`](tail.md), [`tee`](tee.md), [`edit`](edit.md),
[`grep`](grep.md), [`sh`](sh.md) (`>` and `<` redirect a command's output
and input)
