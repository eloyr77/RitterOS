# man: the manual: a command's page, a topic (install, manual), or a search

```
man [-k WORD] [NAME]
```

Shows the manual's page for the program NAME: what it does, its usage,
every option, its messages and examples. The pages are on the disk in
`/system/doc` (made from the project's `docs/` folder, step 50j): a page
per program in `/system/doc/commands/`, the install guide and the user's
manual. A NAME that is not a program is looked up as a topic: `man
install` is the install guide, `man manual` the whole user's manual.

The pages are written in Markdown and shown as plain text: headings in
bold, code and examples indented by four spaces, tables in columns, links
as their text. When the output is a terminal and the page is longer than
the screen, `man` shows it a screen at a time:

| Key | Does |
|---|---|
| space (or any other key) | the next screen |
| Enter | one more line |
| `b` | back a screen |
| `q`, Ctrl-C, Ctrl-D | leave |

Piped or redirected (`man ls | grep -- -l`, `man ls > ls.txt`), the whole
page is written at once, without bold.

`man` alone lists every page with its one-line description, as `help`
lists the programs.

## Options

| Option | Meaning |
|---|---|
| `-k WORD` | list the pages whose title (the program's name and description) has WORD, in any case: `man -k disk` |

## Exit status and messages

- `0`: a page or a list was shown.
- `1`: `man: no page for NAME (man -k WORD searches the titles)`, or
  `man: no page has 'WORD' in its title` for `-k`, or `man:
  /system/doc/commands: No such file or directory` on a disk without the
  pages.
- `2`: `usage: man [-k WORD] [NAME]` (a NAME may not contain `/`).

## Examples

```
ritteros:/$ man ls
ritteros:/$ man -k archive
ar           make and read archives of object files (static libraries)
tar          ...
ritteros:/$ man install
ritteros:/$ man [
```

`man [` shows the page of `test`, which `[` is.

## See also

[`help`](sh.md) (the shell's list of programs), [`edit`](edit.md), the
install guide (`man install`)
