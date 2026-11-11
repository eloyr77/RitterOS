# RitterOS manual

How to use RitterOS: logging in, the shell, the files, the disks, the
network and the rest, chapter by chapter. Every program also has a page of
its own with its usage, every option, its messages and examples:
[commands/](commands/README.md) lists them (since 50j). On RitterOS the
same pages are `man NAME` (`man` lists them, `man -k WORD` searches, `man
manual` is this manual); in the shell, `help` lists the programs with a
one-line description each. How to put RitterOS on a stick and install it is
[install.md](install.md) (`man install`).

How RitterOS works inside is in [`README.md`](../README.md); the file
system's on-disk format is in [`ritterfs.md`](ritterfs.md).

Contents:

1. [Starting and logging in](#1-starting-and-logging-in)
2. [The shell](#2-the-shell)
3. [Files and directories](#3-files-and-directories)
4. [The text editor](#4-the-text-editor)
5. [Snapshots, versions and attributes](#5-snapshots-versions-and-attributes)
6. [Users and permissions](#6-users-and-permissions)
7. [Disks and volumes](#7-disks-and-volumes)
8. [The network](#8-the-network)
9. [The system](#9-the-system)
10. [Special directories](#10-special-directories)
11. [The rescue console](#11-the-rescue-console)
12. [The kernel command line](#12-the-kernel-command-line)
13. [Messages](#13-messages)

How to read the usage lines: `WORD` in capitals is something you fill in,
`[...]` is optional, `...` means one or more, `A | B` means one of them.

---

## 1. Starting and logging in

After the boot, the screen (and the serial port, if there is one) shows
`login:`.

**Starting from a USB stick.** Any USB socket works (a laptop often has
two USB controllers; RitterOS uses them all). A USB 3 stick can take a
moment to connect after RitterOS takes over the controller; the boot then
says `system partition: not found yet; waiting for USB disks (at most
10 s)` and goes on as soon as the stick is there (`found after ... ms`).
If it never comes, the boot ends in the rescue console (section 11) with
`login: not started (no file system)`.
A system started from the stick always runs from the stick, also when
another disk holds a copy of a stick image (the same identifiers): the
boot then says `system partition: nvme0n1p2 has the same GPT GUID as
usbdisk0p2 (a copied disk?); using usbdisk0p2`, and the copy is only
mounted as a volume. `osinstall` (section 7) gives the disks it installs
identifiers of their own.

**First start of a new disk.** RitterOS first asks you to choose a
password for `root`, the administrator, and to type it again:

```
RitterOS: first start. Choose a password for root, the administrator.
New password for root:
Again:
The password for root is set.

login:
```

Passwords are never shown while you type them.

**Logging in.** Type the user's name, Enter, then the password, Enter:

```
login: root
Password:
[root logged in]
ritteros:/home/root$
```

Your shell starts in your home directory (`/home/NAME`). A wrong password
or an unknown name both say `Login incorrect.`

**Your own settings.** Each user has a folder of their own settings,
`~/.config` (that is `/home/NAME/.config`), made at their first login at the
machine with a README and the files below, every line a comment (`#`) until
you change it (a file you delete comes back, as comments, at the next
login; since 37e-1b windowd also writes its `windows` when missing). Each
commented line shows the setting's default: remove the `#` to change it. A file there wins over the system's file of the same name in
`/system/config`. Two are read so far:

- `login`: commands run when you log in at the machine's own screen and
  keyboard (not over SSH), one per line, before the first prompt; a line
  starting with `#` is a comment. To make the window system your interface,
  remove the `#` in front of its `windowd` line (`edit .config/login`), or:

  ```
  ritteros:/home/anna$ echo windowd > .config/login
  ```

  From the next login on, the desktop comes up by itself; when you leave
  it, the shell's prompt is there.
- `windows`: the window system's settings, the same keys as
  `/system/config/windows` (`startup = PROGRAM [ARGUMENTS]`: what opens with
  the desktop; `double_click = MS`: how quick a double click is;
  `click_steady = off`: clicks exactly where the pointer is; `text_size =
  PIXELS`: the size of text in programs that use your size, 15 if not set;
  `theme = light`: light windows and menus, dark if not set; `accent =
  RRGGBB`: the colour of what is in front and chosen).

**Logging out.** `exit` (or Ctrl-D on an empty line) ends the shell;
`login:` comes back.

**Rescue console.** Ctrl-D at `login:` ends `login`, and the kernel's
rescue console (`rescue>`) takes over: a few commands for checking the
hardware and the disk when the system cannot start normally (section
10). It asks for no password (so far).

**Switching off.** `poweroff` or `reboot` (root only).

---

## 2. The shell

The shell `sh` shows a prompt with the current directory:

```
ritteros:/home/root$
```

Only when it reads a terminal (since 63c): a script piped into it (`ssh
HOST sh < script.sh`, `cat script | sh`) runs without prompts, without
the `> ` of an unfinished command and without the `exit` at its end.

**Commands.** A line is split into words at spaces. Quotes keep spaces in
a word: `'two words'` or `"two words"`. The first word is a built-in
command or a program. Programs are found in `/system/<arch>/bin`
(`<arch>` is `x86_64` or `aarch64`); a name with a `/` in it is a path,
for example `./myprogram` or `/system/x86_64/test/counter`.

**Several commands.** `;` (outside quotes) separates commands on one
line; they run one after the other: `cd /test; ls`. An empty one
(`;;`) is skipped. `ssh` runs a command line the same way
(`ssh root@host 'uptime; elf'`).

**Pipes** (since 51a). `|` makes one command's output the next one's
input: `ls /test | grep hello`, `cat notes.txt | grep -c todo`. The
commands of a pipeline run at the same time, as one job (Ctrl-C ends them
all); the shell waits for all of them. A built-in command, a function or
a compound command in a pipeline runs in a copy of the shell of its own
(since 56c), so `cd` there changes nothing outside it.

**Success and failure** (since 51a). Every command ends with an exit code:
0 for success, anything else for a failure (`false` always fails). `$?` is
the last one's. `A && B` runs B only if A succeeded, `A || B` only if A
failed; `!` before a command inverts its result:

```
ritteros:/$ grep -q todo notes.txt && echo "something to do"
ritteros:/$ ls /nope || echo "no such directory"
ritteros:/$ false; echo $?
1
```

At the prompt, a line whose last command failed is reported:
`grep: exit code 1` (not a command whose result `&&` or `||` used; nothing
is reported in `sh -c` or over `ssh HOST 'COMMAND'`).

**Variables** (since 22t). `NAME=VALUE` sets a variable of the shell's
own; `$NAME` or `${NAME}` in a word becomes its value (also inside
`"double quotes"`, not inside `'single quotes'`; an unset one becomes
nothing, and a word that was only that is left out). `export NAME=VALUE`
(or `export NAME` for one already set) makes it an environment variable:
every program the shell starts gets it. `export` alone lists them, sorted;
`unset NAME` removes one. `NAME=VALUE command` sets it for that command
only. Unquoted, a value is split into words at spaces, tabs and line
breaks (the characters of `IFS`, since 51b); in double quotes it stays one
word: with `X="a b"`, `ls $X` lists `a` and `b`, `ls "$X"` the one name
`a b`. Logging in sets `HOME`, `USER`, `LOGNAME` and `SHELL`; over SSH
`TERM` too, as the client's terminal is (with a terminal only).

```
ritteros:/$ export GREETING=hello
ritteros:/$ echo "$GREETING there"
hello there
ritteros:/$ GREETING=bye env
...
GREETING=bye
```

**Expansions** (since 51b). Before a command runs, these in its words
become text:

| Form | Becomes |
|---|---|
| `$(COMMAND)` or `` `COMMAND` `` | what COMMAND prints, its last line breaks left out |
| `$((EXPRESSION))` | the value of an integer expression: `+ - * / %`, `<< >>`, comparisons, `&& \|\| !`, `& \| ^ ~`, `? :`, parentheses, `N = 3`, `N += 1` |
| `$?` `$$` `$!` | the last exit code, the shell's process number, the last background job's |
| `$0` `$1`... `$#` `$@` `$*` | the shell's name, its arguments, how many; `"$@"` is each argument as a word of its own |
| `~` `~NAME` | your home, NAME's home |
| `\X` | X itself (`\$HOME` is the text `$HOME`); in double quotes only before `$`, `` ` ``, `"` and `\` |

`$(...)` runs COMMAND in a subshell, a copy of the shell (since 56c a
fork, as on other systems): a `cd` or a variable set inside it does not
change the shell outside; it has the shell's variables, functions and
options, and `$$` there is still the shell's own number. After `X=$(COMMAND)` alone, `$?` is COMMAND's exit
code. `sh -c TEXT NAME ARGS` runs TEXT with `$0` = NAME and `$1`... the
ARGS. An arithmetic error stops the command: `sh: $((1 / 0)): division by
0`.

```
ritteros:/$ echo "today: $(uptime)"
ritteros:/$ N=5; echo $((N * 2 + 1))
11
```

**Control flow** (since 51c). Commands can be grouped and repeated, as
in other shells; a line break works like `;`, and a command not finished
at the end of a line (an `if` without its `fi`, an open quote, a `|` at the
end) goes on in the next, after the prompt `> `:

| Form | Does |
|---|---|
| `if LIST; then LIST; elif LIST; then LIST; else LIST; fi` | the first branch whose condition succeeds |
| `while LIST; do LIST; done`, `until LIST; do LIST; done` | repeat while (until) the condition succeeds |
| `for NAME in WORD...; do LIST; done` | once for each word (without `in`: each argument, `"$@"`) |
| `case WORD in PATTERN \| PATTERN) LIST;; ... esac` | the first branch whose pattern (`*`, `?`, `[...]`) matches; a quoted part of a pattern is plain text, the rest still a pattern (`"$dir"/*`, `'a?'*`: since 68g) |
| `{ LIST; }` | a group: its redirections apply to all of it (`{ a; b; } > file`) |
| `( LIST )` | a subshell: a `cd` or a variable set inside stays inside |
| `NAME() { LIST; }` | a function: called like a command, its words in `$1`...; `return [N]` |
| `break [N]`, `continue [N]` | leave the loop (N levels), or start its next round |

```
ritteros:/$ for f in /test/*.txt; do echo "$f"; done
ritteros:/$ while read line; do echo "> $line"; done < notes.txt
ritteros:/$ ls /test | while read f; do case $f in *.txt) echo $f;; esac; done
ritteros:/$ case "$(id)" in "uid=0("*) echo root;; esac
```

A loop or a built-in in a pipeline (`ls | while read f; ...`), `( ... )`
and `a && b &` run in a subshell (since 56c a fork of the shell, with all
its variables and functions): what they set does not reach this one. A loop reading a file with `< FILE` runs here: its variables
stay set afterwards.

**More built-in commands** (since 51c): `:` (does nothing, succeeds), `.
FILE` (runs FILE's commands in this shell), `eval WORDS` (runs the words as
a command line), `exec COMMAND` (since 55b the shell becomes COMMAND,
which keeps its process number and the line's redirections and
assignments; if COMMAND cannot start, the message and exit code 127 or
126, and a script ends there while the prompt stays; `exec > FILE` alone
sends the shell's output into FILE from then on), `set` (options: `-e` stop at the first failure, `-u` an unset
variable is an error, `-x` show each command before it runs, `-f` no
patterns; `set -- WORDS` new arguments; alone: the variables), `shift [N]`
(drops `$1`), `read [-r] NAME...` (a line of the input into the names, at
`IFS`; it fails at the input's end), `break`, `continue`, `return`.
`umask [-S] [MODE]` (since 52h): the permission bits taken off every file
and folder the shell and the programs it starts make; alone it shows them
(`0022`: a new file gets 0644, a folder 0755), `-S` shows what stays
allowed (`u=rwx,g=rx,o=rx`); MODE is octal (`umask 077`: only you may use
what you make) or like chmod's letters, saying what is allowed
(`umask u=rwx,g=rx,o=`, `umask g-w`). It is passed on to programs and
lasts until the shell ends; `umask: 8: not a mode` refuses a wrong one.
Built-ins take redirections too: `pwd > here.txt`, `read x < file`.

**Scripts** (since 51c). A file of commands is a script: `sh FILE ARGS`
runs it, with `$0` = FILE and the ARGS as `$1`.... A script whose first
line is `#!/system/x86_64/bin/sh` (the program that runs it) can be started
by its name like a program once it may be run (`chmod 755 FILE`). One
without that line runs too (since 52g, as POSIX says): the shell, `env`
and `xargs` hand a runnable file the system will not start to a new `sh`,
unless it is a binary (a program for another processor, or any file with
a zero byte at its start: `Exec format error`). A file without any x
bit is no program, also for root (`Permission denied`). `#` begins a
comment (to the line's end), also on the command line. `~/.config/login`
is read as a script.

```
#!/system/x86_64/bin/sh
# count the lines of each file given
for f; do
    n=0
    while read line; do n=$((n + 1)); done < "$f"
    echo "$f: $n lines"
done
```

**Paths.** `/` is the top of the tree. A path that does not start with `/`
is relative to the current directory. `.` is the current directory, `..`
its parent.

**Names with `*`.** A word with `*` becomes the names that match it,
sorted; `*` stands for any characters within one name (also none), `?`
for one character, `[abc]` for one of a, b and c, `[a-z]` for one in that
range and `[!abc]` for one that is not (since 51b), in any part of a path:

| Word | Becomes |
|---|---|
| `*.txt` | the `.txt` files of the current directory |
| `/home/*/notes` | each user's `notes` |
| `/test/*/` | the directories in `/test` (with their `/`) |

Names starting with `.` match only a pattern that starts with `.`. A word
that matches nothing stays as typed (`nomatch*`). What is in quotes (or
after a `\`) is plain text, the rest of the word still a pattern (since
68h; before, a word with any quote was never expanded): `'*'` and `find /
-name "*.txt"` keep their `*`, while `"$dir"/*.txt` is the `.txt` files
in the folder `$dir`, even one with a space in its name. A command can have at most 4096
words and 128 KiB of them (a program as many; 128 words until 37g-5a-2):
`sh: many/*: too many names (at most 4096 words)`.

**Built-in commands:**

| Command | Does |
|---|---|
| `cd [DIR]` | change the current directory; without DIR: your home |
| `pwd` | print the current directory |
| `help` | list the built-ins and every program with a description |
| `exit` | end the shell (log out) |

**Redirection.** A program's output can go into a file, and its input can
come from one:

| Form | Means |
|---|---|
| `PROGRAM > FILE` | the output replaces FILE's content (FILE is created if missing) |
| `PROGRAM >> FILE` | the output is added at FILE's end |
| `PROGRAM < FILE` | the program reads FILE instead of the keyboard |
| `PROGRAM 2> FILE` | its error messages go into FILE (since 51a) |
| `PROGRAM > FILE 2>&1` | its output and its errors both go into FILE, in order |
| `PROGRAM >&2` | its output goes where errors go |
| `PROGRAM <<WORD` | a here-document (since 63i): the lines after this one, up to a line that is just WORD, are the program's input |
| `PROGRAM <<-WORD` | the same, the lines' leading tabs taken away (for indented scripts) |

The space after `>`, `>>` and `<` is optional; a number right before them
(`2>`) is the descriptor redirected (0 input, 1 output, 2 errors), and
`N>&M` makes N go where M goes. They apply left to right, after a pipe's.
Inside quotes they are plain characters: `echo "a > b"` prints `a > b`.
A here-document's lines are expanded as inside double quotes (`$NAME`,
`$(...)`, `` `...` ``, `$((...))`; `\` keeps `$`, `` ` `` and `\` and joins a
line with the next; a `"` is just a character) unless WORD has quotes
(`<<'END'`, `<<"END"` or `<<\END`: the lines as they are, the way to send a
file's text). Several on one line come one after another; typed at the
prompt, their lines are read after `> `. Any size: a long one is written
to the program as it reads it.

```sh
cat > /home/root/notes.txt <<'END'
the text, $NOT expanded
END
```
Redirection works for programs and built-ins alike (`echo` is a program,
`read X < file` a built-in). A file that several
programs write in turn (`sh -c "make; make test" > log`) gets all of it in
order: a program and the programs it starts share their open files, with
one position (since 51a).

```
ritteros:/$ echo hello > /notes.txt
ritteros:/$ echo more >> /notes.txt
ritteros:/$ cat /notes.txt
hello
more
```

**Jobs.** Each command runs as a job that has the terminal (keyboard and
Ctrl-C) while the shell waits. An `&` at the end runs it in the
background; the shell reports when it ends:

```
ritteros:/$ /system/x86_64/test/counter bg 3 500 &
```

A script or `sh -c` reports no jobs (since 51e, as POSIX's shells): `$!`
is the job's number there.

**Keys at the prompt:**

| Key | Does |
|---|---|
| Up, Down | the earlier commands of this shell, one by one (Down back to what you were typing) |
| Left, Right | move in the line; typing goes in where the cursor is |
| Home, End | to the line's start, to its end |
| Backspace | erase the character before the cursor |
| Delete | erase the character under the cursor |
| Ctrl-U | erase the whole line |
| Ctrl-L | clear the screen; the prompt and what you typed so far stay, at the top |
| Ctrl-C | cancel the line; while a program runs: stop it |
| Ctrl-D | on an empty line: end the shell |
| Enter | run the line |

The shell keeps the last 50 commands while it runs (a command typed
twice in a row once); they are gone when you log out, nothing is
written to the disk. A line longer than the screen is not redrawn
cleanly when the cursor moves through it.

**Messages at the prompt.** The line you type on belongs to the program
in the foreground: its prompt and what you typed. A message from
anything else (the system, an elf, a job in the background) never mixes
into it: it gets a line of its own, and the prompt and what you had
typed so far are shown again below it (also when the message came just
before the program started reading), so you go on typing where you
were. The same holds at `login:`, `Password:` (what was typed stays
hidden) and `rescue>`.

```
ritteros:/$ ls /vol
usb: usbdisk0: Kingston DataTraveler 3.0, 29.3 GiB
ritteros:/$ ls /vol
```

**Characters beyond ASCII.** Text is UTF-8: names like `Grüße.txt` or
`año` work in commands, file names and files. The keyboard is a US
keyboard with the extras of Windows' "US International":

| Type | Gives |
|---|---|
| `'` then a letter | á é í ó ú ý ç (and capitals) |
| `` ` `` then a letter | à è ì ò ù |
| `"` then a letter | ä ë ï ö ü ÿ |
| `^` then a letter | â ê î ô û |
| `~` then a letter | ã õ ñ |
| the accent, then Space | the accent itself: `'`, `"`, `` ` ``, `^`, `~` |
| AltGr (right Alt) + n, ?, 1, s, 5 | ñ, ¿, ¡, ß, € (and more: AltGr + q w e y u i o p a l z c , give ä å é ü ú í ó ö á ø æ © ç) |

So a plain `'` or `"` in front of a vowel needs a Space after it (`'`
Space `a`), as on Windows. The screen shows Latin letters (with Latin
Extended-A), punctuation such as – “ ” …, € and arrows; other characters
appear as a replacement mark.

**The mouse.** A USB mouse or a laptop's built-in PS/2 mouse is found as
`mouse0`, `mouse1`, ... in the order found. There is no window server yet,
so a mouse does nothing on its own; the rescue console's `mouse` command
(section 11) shows that one works — where its pointer is, how far it has
moved and the buttons pressed. The window server that uses it is coming
(the roadmap's "window system").

**Exit codes.** When a program fails it says so and the shell adds its exit
code, for example `ls: exit code 1`.

**Login shell.** `sh -l` (how `login` starts it) runs the commands in
`~/.config/login` before its first prompt.

**One line only.** `sh -c 'LINE'` runs LINE, however long, and ends with
its exit code (`sshd` runs a command given to `ssh` this way). Without a
terminal, `sh` reads its commands line by line from a pipe or a file
(`ssh root@ritteros sh < script`), lines of up to 4,095 bytes; a longer one
is not run at all (`sh: a line longer than 4095 bytes: not run`) and the
next one is. A typed line holds up to 255 characters.

---

## 3. Files and directories

### ls: list directories

The page with every option and message: [`ls`](commands/ls.md) (on RitterOS: `man ls`).

```
ls [-l] [PATH]...
```

Lists the directories (default: the current one): one line per entry, the
size in bytes, or `<dir>` for a directory (its name ends in `/`), or
`<link>` for a symbolic link with what it names (`bin -> /system/x86_64/bin`,
since 57a). A file as PATH is listed alone; a link to a folder lists the
folder. `-l` adds the type and permissions (`l` for a link), the number
of names the file has (hard links, below), the owner and the group
(section 6); a link given as PATH is then shown itself.

```
ritteros:/$ ls /test/docs
     <dir>  notes/
ritteros:/$ ls -l /home
drwx------   1     0     0      <dir>  root/
drwxr-xr-x   1  1000  1000      <dir>  anna/
```

### ln: make links (since 57a)

The page with every option and message: [`ln`](commands/ln.md) (on RitterOS: `man ln`).

```
ln [-s] [-f] TARGET LINK
ln [-s] [-f] TARGET... FOLDER
```

Without `-s`, LINK becomes a second name for the file TARGET (a hard
link): the same file under both names, so a change through one shows
through the other, and the file goes only when its last name is removed
(`ls -l` shows how many it has). Hard links stay on one volume (`ln:
...: Cross-device link`) and are not for folders (`Operation not
permitted`). You must own the file, or be allowed to read and write it.

With `-s`, LINK is a symbolic link: a small file that names TARGET, any
path, kept as written. Opening LINK opens TARGET; a relative TARGET is
taken from LINK's folder (`ln -s ../lib/x /test/a/b/x`), so a moved
folder keeps its inner links. TARGET need not exist (a link to nothing:
`No such file or directory` until it does; writing through it makes it).
`rm LINK` removes the link, never TARGET; `rm -r` does not follow links
into folders. A path may pass through at most 40 links (`Too many levels
of symbolic links`, e.g. two links naming each other). `..` after a link
leaves the folder the link leads to, not the one it is in, as on Unix.
`cd` through a link to a folder ends up in that folder (`pwd` shows it).

Into a FOLDER each link gets TARGET's last name. `-f` removes a LINK
that is there first. Links exist on RitterFS only: on FAT and exFAT
volumes `Operation not permitted`.

```
ritteros:/$ ln -s /test/docs /home/root/docs
ritteros:/$ ls /home/root
    <link>  docs -> /test/docs
ritteros:/$ ln /home/root/notes.txt /home/root/notes-too.txt
```

### readlink: what a link names (since 57a)

The page with every option and message: [`readlink`](commands/readlink.md) (on RitterOS: `man readlink`).

```
readlink [-f] PATH...
```

Prints what the symbolic link PATH names, as it was written (`readlink:
...: Invalid argument` if PATH is no link). `-f` prints the full path
that PATH leads to instead, every link on the way followed (PATH itself
need not be a link).

### cat: show files

The page with every option and message: [`cat`](commands/cat.md) (on RitterOS: `man cat`).

```
cat [FILE]...
```

Prints the files one after the other. Without FILE it prints what you type
until Ctrl-D. A file that contains zero bytes (a program, an image) is
binary; shown on the terminal it would garble the screen, so `cat` names it
instead of printing it -- but only when standard output is the terminal.
Redirected to a file (`cat a > b`) or sent down a pipe or an ssh connection,
binary passes through unchanged, so `cat` moves binaries too.

### env: environment variables

The page with every option and message: [`env`](commands/env.md) (on RitterOS: `man env`).

```
env [-i] [NAME=VALUE]... [COMMAND [ARG]...]
```

Without a command, the environment variables, one `NAME=value` per line;
with one, COMMAND runs with the variables given set (`-i`: with only
those). A command named without a `/` is found as the shell finds it. The
exit code is the command's, 127 if it cannot be started (`env: NAME:
command not found`).

### echo: print words

The page with every option and message: [`echo`](commands/echo.md) (on RitterOS: `man echo`).

```
echo [WORD]...
```

Prints the words separated by spaces, and a line break. With `>` it writes
a file: `echo hello > a.txt`.

### printf: print in a format (since 51d)

The page with every option and message: [`printf`](commands/printf.md) (on RitterOS: `man printf`).

```
printf FORMAT [ARGUMENT]...
```

Prints FORMAT, its conversions filled in from the arguments, as POSIX
says: `%s` (a string), `%d` `%i` `%u` `%o` `%x` `%X` (numbers; `'A` is A's
code), `%c`, `%e` `%f` `%g` `%a` (and capitals), `%b` (a string with
escapes; `\c` stops all output), `%%`; flags `- + space # 0`, a width and a
precision (`*` takes them from the arguments). Escapes: `\n` `\t` `\\` `\a`
`\b` `\f` `\r` `\v` and `\NNN` (octal). The format is used again while
arguments are left; missing ones are empty or 0. No line break of its own:

```
ritteros:/$ printf '%-8s %5d\n' apples 12 pears 3
apples      12
pears        3
```

An argument that is not a number prints `printf: X: not a number` and is
used as 0 (exit code 1).

### test and [: check something (since 51d)

The page with every option and message: [`test`](commands/test.md) (on RitterOS: `man test`).

```
test EXPRESSION
[ EXPRESSION ]
```

Succeeds (exit code 0) when EXPRESSION is true, fails (1) when not, 2 on an
error (`test: x: not an integer`). For `if`, `while`, `&&` and `||`:

| Expression | True when |
|---|---|
| `STRING`, `-n STRING` | it is not empty |
| `-z STRING` | it is empty |
| `A = B`, `A != B` | the strings are equal, not equal (`<` `>`: in byte order, quoted) |
| `A -eq B` `-ne` `-lt` `-le` `-gt` `-ge` | the numbers compare so |
| `-e FILE` `-f` `-d` | it exists, is a file, a directory |
| `-s FILE` | it is not empty |
| `-r FILE` `-w` `-x` | you may read, write, run it |
| `-b FILE` | it is a disk (`/dev`) |
| `-h FILE`, `-L FILE` | it is a symbolic link (since 57a; the others follow links) |
| `-c FILE` `-p` `-S` | it is a character device (`/dev/null`, a terminal), a pipe, a socket |
| `A -nt B`, `A -ot B` | file A is newer, older than B (extras) |
| `-t FD` | the descriptor is a terminal |
| `! E`, `E -a E`, `E -o E`, `( E )` | not, and, or, grouping (`'('` quoted in the shell) |

```
ritteros:/$ if [ -d /test ]; then echo yes; fi
ritteros:/$ [ "$n" -gt 3 ] && echo big
```

### true, false: succeed, fail

The pages with every option and message: [`true`](commands/true.md), [`false`](commands/false.md) (on RitterOS: `man true`, `man false`).

`true` does nothing and succeeds (exit code 0); `false` fails (1).

### sleep: wait (since 51d)

The page with every option and message: [`sleep`](commands/sleep.md) (on RitterOS: `man sleep`).

```
sleep SECONDS...
```

Waits that long: whole seconds, fractions (`sleep 0.5`) and the suffixes
`s` `m` `h` `d` (`sleep 2m`); several are added up.

### date: the date and time (since 51d)

The page with every option and message: [`date`](commands/date.md) (on RitterOS: `man date`).

```
date [-u] [-d @SECONDS] [+FORMAT]
```

Prints the date and time now in the local time (the zone of
`/system/config/time`), or in UTC with `-u`, or that of SECONDS since 1970
(`-d @1700000000`). `+FORMAT` uses `strftime`'s conversions (`%Y-%m-%d
%H:%M:%S`, `%a` the day, `%j` the day of the year, ...; by default `%a %b
%e %H:%M:%S %Z %Y`). It does not set the clock.

### basename, dirname: a path's parts (since 51d)

The pages with every option and message: [`basename`](commands/basename.md), [`dirname`](commands/dirname.md) (on RitterOS: `man basename`, `man dirname`).

```
basename PATH [SUFFIX]
dirname PATH
```

`basename /a/b/c.txt .txt` is `c`, `dirname /a/b/c.txt` is `/a/b`; a name
without a directory has `.` as its dirname.

### head, tail: the first or last lines (since 51e)

The pages with every option and message: [`head`](commands/head.md), [`tail`](commands/tail.md) (on RitterOS: `man head`, `man tail`).

```
head [-n N | -N] [-c N] [FILE]...
tail [-f] [-n [+]N | -N] [-c [+]N] [FILE]...
```

`head` prints the first N lines (10 by default) of each file or of its
input, or with `-c` the first N bytes (an extra); several files each come
under a `==> NAME <==` line. `tail` prints the last N lines or bytes, and
with `+N` everything from the Nth on (`tail -n +2` leaves out the first
line); the traditional `-N` means `-n N` (`tail -20` since 63c, `head -5`
since 63d). `tail -f` then keeps following the file, printing what is added to
it (it looks every second) until Ctrl-C:

```
ritteros:/$ tail -f /system/logs/kernel.log
```

### wc: count lines, words and bytes (since 51e)

The page with every option and message: [`wc`](commands/wc.md) (on RitterOS: `man wc`).

```
wc [-l] [-w] [-c | -m] [FILE]...
```

Prints the lines, words and bytes of each file or of the input, each in 8
columns, with the file's name and, for several files, a `total` line. `-l`
`-w` `-c` choose some of them (`-m`, characters, is the same as `-c`: a
character is a byte).

### sort: lines in order (since 51e)

The page with every option and message: [`sort`](commands/sort.md) (on RitterOS: `man sort`).

```
sort [-bcfnru] [-h] [-V] [-t C] [-k F[.C][,G[.C]][OPTIONS]]... [-o FILE] [FILE]...
```

Prints the lines of the files (or of the input) in byte order. Options:

| Option | What it does |
|---|---|
| `-n` | by the number at the start (`-h`: with K M G T P E, as `du -h` and `df -h` write them elsewhere; an extra) |
| `-V` | as versions: numbers inside the text compared as numbers (`v1.9` before `v1.10`; an extra) |
| `-r` | the other way round |
| `-f` | small and capital letters alike |
| `-b` | blanks before a key do not count |
| `-u` | lines with equal keys only once (the first of them) |
| `-t C` | fields are separated by C (else each field begins with its blanks) |
| `-k F,G` | by fields F to G (`-k 2` from field 2 to the end; `-k 1.2,1.3` characters 2 to 3 of field 1; letters after it: options for this key only, `-k 2,2nr`); several `-k` are tried in turn |
| `-o FILE` | the result into FILE (it may be one of the inputs) |
| `-c` | only check: exit code 1 and `sort: disorder: LINE` if the input is not sorted |

Lines that are equal by their keys come in the order of the whole line.

### uniq: equal lines once (since 51e)

The page with every option and message: [`uniq`](commands/uniq.md) (on RitterOS: `man uniq`).

```
uniq [-c] [-d | -u] [-i] [-f N] [-s N] [INPUT [OUTPUT]]
```

Prints the input with each run of equal lines once (so usually after
`sort`). `-c` puts the run's length before each line, `-d` prints only
lines that repeat, `-u` only those that do not; `-f N` leaves the first N
fields and `-s N` the next N characters out of the comparison, `-i` takes
small and capital letters alike (an extra).

```
ritteros:/$ sort words.txt | uniq -c
```

### cut: parts of each line (since 51e)

The page with every option and message: [`cut`](commands/cut.md) (on RitterOS: `man cut`).

```
cut -b LIST | -c LIST | -f LIST [-d C] [-s] [FILE]...
```

Prints parts of each line: bytes (`-b`), characters (`-c`, the same), or
fields (`-f`) separated by tabs or by `-d C`. LIST is numbers and ranges
counted from 1: `1,3`, `2-4`, `-3` (up to 3), `5-` (from 5 on). Lines
without the separator are printed whole with `-f`, or left out with `-s`.

```
ritteros:/$ cut -d ' ' -f 1,3 data.txt
```

### tr: change characters (since 51e)

The page with every option and message: [`tr`](commands/tr.md) (on RitterOS: `man tr`).

```
tr [-c] [-d] [-s] SET1 [SET2]
```

Copies its input to its output with characters changed: each one of SET1
into the one at the same place in SET2 (SET2's last one again if it is
shorter). `-d` deletes the characters of SET1, `-s` squeezes runs of one
character of the last set into one, `-c` means every character not in
SET1. Sets are characters, ranges (`a-z`), escapes (`\n` `\t` `\\` `\NNN`
in octal), classes (`[:alpha:]` `[:digit:]` `[:upper:]` `[:lower:]`
`[:space:]` `[:blank:]` `[:alnum:]` `[:punct:]` `[:xdigit:]` `[:print:]`
`[:graph:]` `[:cntrl:]`) and repeats (`[x*3]`; `[x*]` in SET2: as many as
SET1 needs). `tr: range z-a in reverse order` refuses a backwards range.

```
ritteros:/$ tr a-z A-Z < notes.txt
ritteros:/$ tr -cs '[:alpha:]' '\n' < notes.txt     (one word a line)
```

### tee: output to files too (since 51e)

The page with every option and message: [`tee`](commands/tee.md) (on RitterOS: `man tee`).

```
tee [-a] [-i] [FILE]...
```

Copies its input to its output and into each FILE (`-a` adds to them
instead of replacing them; `-i` ignores Ctrl-C): `ps | tee ps.txt`.

### xargs: arguments from the input (since 51e)

The page with every option and message: [`xargs`](commands/xargs.md) (on RitterOS: `man xargs`).

```
xargs [-t] [-n N | -L N | -I TEXT] [COMMAND [ARGUMENT]...]
```

Runs COMMAND (`echo` if none is given) with the words of its input added
to the ARGUMENTs, as many at a time as fit in one command (120 words,
3,800 bytes), or N at a time with `-n N`, or the words of N lines with
`-L N`. With `-I TEXT` it runs COMMAND once for each input line, with TEXT
in the arguments replaced by the line. Words are separated by blanks and
line breaks; `'...'`, `"..."` and `\` quote. `-t` prints each command on
the error output before it runs. Empty input runs nothing.

```
ritteros:/$ find /home -name "*.txt" | xargs wc -l
```

Exit code: 0; 123 if a command failed (exit code 1-125), 124 if one ended
with 255 (xargs stops), 125 if one was stopped by a signal, 126 if it could
not be started, 127 if there is no such command (`xargs: NAME: command not
found`); 1 for `xargs: unmatched quote` or `xargs: argument too long`.

### sed: edit a stream (since 58b)

The page with every option and message: [`sed`](commands/sed.md) (on RitterOS: `man sed`).

```
sed [-nE] [-i[SUFFIX]] SCRIPT [FILE]...
sed [-nE] [-i[SUFFIX]] -e SCRIPT... [-f FILE]... [FILE]...
```

Reads the files (or its input) a line at a time, runs SCRIPT on each and
prints the result: POSIX's sed. A command may have an address in front:
a line number, `$` (the last line), `/RE/` (lines that match; `\cREc`
with another delimiter c), or two (`A,B`: from A to B; `A,+N`: A and N
more); `!` after it means the other lines.

| Command | |
|---|---|
| `s/RE/TEXT/FLAGS` | replace: `g` every match, `N` the Nth, `p` print, `w FILE` write; `&` in TEXT is the match, `\1`-`\9` the groups, `\n` a line break |
| `d`, `p`, `q`, `=` | delete the line, print it, stop, print its number |
| `a\ TEXT`, `i\ TEXT`, `c\ TEXT` | add a line after, insert one before, change the line (one-line `a TEXT` too) |
| `y/ABC/XYZ/` | change each A to X, B to Y, C to Z |
| `n`, `N` | the next line instead, or appended after a line break |
| `D`, `P` | delete, print up to the first line break |
| `h`, `H`, `g`, `G`, `x` | the hold space: copy the line there, append to it, copy back, append back, exchange |
| `b LABEL`, `t LABEL`, `: LABEL` | jump (`t`: only if `s` replaced something since the last line) |
| `{ ... }` | a group of commands for one address |
| `l` | show the line with its invisible characters (`\t`, `$` at the end) |
| `r FILE`, `w FILE` | append a file after the line, write the line to a file |

REs are basic (`grep`'s), extended with `-E`; commands are separated by
line breaks or `;`. `-n` prints only what `p` asks for. `-i` changes
each FILE itself (it keeps its permissions and its other names);
`-i.bak` first keeps the old content as `FILE.bak`.

```
ritteros:/$ sed 's/colour/color/g' notes.txt
ritteros:/$ sed -n '/^error/p' /system/logs/kernel.log
ritteros:/$ sed -i '3d' list.txt
```

### awk: scan and process text (since 58c)

The page with every option and message: [`awk`](commands/awk.md) (on RitterOS: `man awk`).

```
awk [-F FS] [-v NAME=VALUE]... 'PROGRAM' [FILE | NAME=VALUE]...
awk [-F FS] [-v NAME=VALUE]... -f PROGFILE... [FILE | NAME=VALUE]...
```

POSIX's awk: a small language for text. PROGRAM is a list of `PATTERN {
ACTION }`: for each line (record) of the files (or the input) that
PATTERN selects, ACTION runs; `BEGIN { }` runs before the first line,
`END { }` after the last. A missing pattern selects every line, a
missing action prints it. Each line is split into fields `$1`, `$2`, ...
(`$0` the whole line, `NF` how many) at blanks, or at FS (`-F:` for
colons, `-F '\t'` for tabs; more than one character: an extended regular
expression).

```
ritteros:/$ awk '{ print $1 }' file.txt                 # the first word of each line
ritteros:/$ awk -F: '$3 >= 1000 { print $1 }' /etc/users
ritteros:/$ awk '{ s += $2 } END { print s, s / NR }' data.txt
ritteros:/$ awk 'length > 72' notes.txt                # the long lines
ritteros:/$ awk '!seen[$0]++' list.txt                 # each line once
```

Values are numbers or strings as they are used; variables need no
declaration; arrays take any subscript (`count[$1]++`, `a[i, j]`, `for (k
in a)`, `delete a[k]`, `if (k in a)`). Patterns: an expression, `/RE/`,
`A, B` (a range). The language has POSIX's operators (`^` `*` `/` `%` `+`
`-`, concatenation, `<` `<=` `==` `!=` `>=` `>`, `~` `!~`, `in`, `&&`
`||` `!`, `?:`, `=` `+=` ..., `++` `--`), `if`/`else`, `while`, `do`,
`for`, `break`, `continue`, `next`, `nextfile`, `exit`, functions
(`function name(a, b,   local) { ... return x }`, arrays passed by
reference), and:

| Built-in | |
|---|---|
| `length(s)`, `substr(s, m, n)`, `index(s, t)` | length (of an array too), part, position |
| `split(s, a, fs)`, `sub(re, t, v)`, `gsub(re, t, v)`, `match(s, re)` | split into a; replace the first or every match (`&`: the match); where it matches (RSTART, RLENGTH) |
| `sprintf(fmt, ...)`, `tolower(s)`, `toupper(s)` | formatting as printf's, case |
| `sin` `cos` `atan2` `exp` `log` `sqrt` `int` `rand` `srand` | numbers |
| `system(cmd)`, `close(name)`, `fflush()` | run a command, close a file or pipe, write out |

`print a, b` writes its items with OFS between and ORS after; `printf
fmt, ...` as C's (`%d %i %o %x %X %u %c %s %e %f %g %%`, widths, `*`).
Either may go to a file (`> "f"`, `>> "f"`) or a command (`| "sort"`).
`getline` reads the next line (`getline var`, `getline < "file"`, `"cmd"
| getline`). The variables NR, NF, FNR, FS, OFS, ORS, RS (`RS = ""`:
paragraphs), FILENAME, SUBSEP, RSTART, RLENGTH, CONVFMT, OFMT, ENVIRON,
ARGC and ARGV are POSIX's. A `NAME=VALUE` among the files sets NAME when
it is reached. Exit status: exit's (0), 2 after an error (`awk: a syntax
error (line 1)`).

### make: build what is out of date (since 59)

The page with every option and message: [`make`](commands/make.md) (on RitterOS: `man make`).

```
make [-f MAKEFILE] [-C DIR] [-n] [-s] [-k] [-i] [-q] [-B] [-t] [-e] [NAME=VALUE]... [TARGET]...
```

Reads `GNUmakefile`, `makefile` or `Makefile` (or MAKEFILE) and makes
each TARGET (the first target in the file if none is named): a target is
remade when it is missing, older than one of its prerequisites, or
`.PHONY`. POSIX's make with what most Makefiles take from GNU make:

```make
CFLAGS = -O2
SRCS := $(wildcard *.c)
OBJS := $(SRCS:.c=.o)

prog: $(OBJS)
	$(CC) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f prog $(OBJS)
```

- **Rules:** `TARGETS: PREREQUISITES`, then recipe lines that start with a
  tab (or `; RECIPE` on the same line); `| ORDER-ONLY` prerequisites;
  pattern rules (`%.o: %.c`), static pattern rules (`$(OBJS): %.o: %.c`),
  suffix rules (`.c.o:`), the built-in rules for C (`$(CC)`, `cc`).
- **Recipes:** each line runs in its own `sh -c`, shown first; `@` in
  front: not shown, `-`: its failure is ignored, `+`: run under `-n`
  too (as a line with `$(MAKE)` is). A failure stops make (`make: ***
  [target] Error 1`, exit code 2) unless `-k` (go on with what does not
  need it) or `-i`.
- **Variables:** `=` (expanded when used), `:=` and `::=` (now), `?=`
  (if unset), `+=`, `!=` (a command's output), `override`, `define ...
  endef`, `export`; `target: NAME = VALUE` for one target's recipe;
  NAME=VALUE on the command line wins over the Makefile's. `$(NAME)`,
  `${NAME}`, `$X`, `$(NAME:.c=.o)`, the automatic `$@ $< $^ $+ $? $* $|`
  and their `D`/`F` forms (`$(@D)`: the target's folder).
- **Functions:** `subst patsubst strip findstring filter filter-out sort
  word wordlist words firstword lastword dir notdir suffix basename
  addsuffix addprefix join wildcard realpath abspath if or and foreach
  call value origin flavor shell eval error warning info`.
- **Directives:** `ifeq`/`ifneq`/`ifdef`/`ifndef`, `else`, `endif`;
  `include`, `-include`.

| Option | |
|---|---|
| `-n` | show what would be run, run nothing |
| `-s` | show no commands |
| `-k` | go on after a failure, with what does not need it |
| `-i` | ignore every failure |
| `-q` | run nothing; exit code 1 if something is out of date |
| `-B` | remake everything |
| `-t` | touch the targets instead of remaking them |
| `-e` | the environment's variables over the Makefile's |
| `-C DIR` | in DIR (`make[1]: Entering directory ...`, unless `--no-print-directory`) |
| `-f FILE` | this Makefile |

One job at a time (`-j` is taken and ignored). Times are a second fine
(a file changed within the same second as its target counts as not
newer).

### cc: compile C programs (since 60)

The page with every option and message: [`cc`](commands/cc.md) (on RitterOS: `man cc`).

```
cc [-c|-S|-E] [-o FILE] [-I DIR] [-isystem DIR] [-D NAME[=VALUE]] [-U NAME] [-L DIR] [-lNAME] [-B DIR] [-v] [-MD|-MMD] [-MP] [-MF FILE] [-MT TARGET] FILE...
```

RitterOS's own C compiler, for the machine it runs on (x86-64 or
AArch64). It takes C11 with the GNU additions the system's own sources
use (`__attribute__`, statement expressions, `typeof`, case ranges, the
`__builtin_` and `__atomic_` functions; since 32d on x86-64 the vector
types and `<immintrin.h>`: SSE to SSE4.1, AVX, AVX2 and FMA intrinsics
with clang's results and calling convention; since 32h in registers, with
`target("avx2")` functions written in VEX), and makes programs like the
system's: position-independent, linked with `libritteros.so.1`.

```sh
cc -o hello hello.c            # compile and link
cc -c part.c                   # part.o
cc -o prog main.c part.o -lwindow
```

Each C file is compiled to assembly (`-S` stops there: FILE.s), assembled
by `as` (`-c` stops there: FILE.o) and everything is linked by `ld` with
the start code (`/system/<arch>/lib/start.o`), the files and `-l`
libraries in their order, `libritteros.so.1` and the static library
`/system/<arch>/lib/libritteros.a` (the SSH, cipher, disk and window code
a program may use without its shared library). `.s` files go to `as`,
`.S` files are preprocessed first; `.o`, `.a` and `.so` files go to `ld`.
Projects made with RitterOS's SDK build on the machine with their own
Makefile and `make` (since 63f: the SDK is in `/system/sdk`, its guide
`/system/sdk/CLAUDE.md`). The headers are in `/system/include` (the C library's and cc's own:
`stddef.h`, `stdint.h`, `stdarg.h`, `stdbool.h`, `limits.h`, `float.h`,
`stdalign.h`, `stdnoreturn.h`, `iso646.h`).

| Option | |
|---|---|
| `-c` | compile and assemble, do not link (FILE.o, or `-o`) |
| `-S` | compile only (FILE.s, or `-o`) |
| `-E` | preprocess only, to the standard output (or `-o`) |
| `-o FILE` | the result's name (`a.out` for a program) |
| `-I DIR`, `-isystem DIR` | look for headers in DIR too (`#include "..."` first in the file's own folder) |
| `-D NAME[=VALUE]`, `-U NAME` | define or undefine a macro |
| `-L DIR`, `-lNAME` | look for libraries in DIR; link `libNAME.so`, `.so.1` or `.a` |
| `-B DIR` | take `as` and `ld` from DIR (not `/system/<arch>/bin`) |
| `-v` | show the commands it runs |
| `-MD`, `-MMD` | also write a make rule of what each object was built from (since 63g): `OBJECT: SOURCE HEADERS...` in OBJECT.d beside the object; `-MMD` leaves out the system's headers (those of `/system/include` and `-isystem` folders) |
| `-MP` | with them: an empty rule for each header too, so a header that is gone does not stop `make` |
| `-MF FILE`, `-MT TARGET` | the rule's file and its target, instead of OBJECT.d and OBJECT |
| `-nostdinc`, `-nostdlib` | without `/system/include`, without the start code and the C library |
| `-fno-pic` | every symbol reached directly, never through the GOT (since 62f: one image whose addresses are all known when linking, as the kernel) |
| `-mgeneral-regs-only` | no floating-point or SIMD register anywhere (since 62f: the kernel's code); floating point is an error |
| `-mcmodel=kernel`, `-mno-red-zone` | taken (since 62f): the code is already right for them |

Since 61 cc keeps values in registers: the registers a call keeps
(x86-64: rbx and r12-r15; AArch64: x20-x28 and d8-d15), given out by
where each value is live, and local variables whose address is never
taken live there too (volatile ones stay in memory). `-O0` turns that
off (every value in its own place on the stack, as before). Since 61b
constants, frame and global addresses and constant offsets go inside the
instructions that use them, and a comparison and the branch on it become
one compare-and-branch. Since 61c constant expressions are worked out
when compiling, results nobody uses are left out, and on x86-64 values
also live in registers a call does not keep (r8, r9, rsi, rdi) where no
call comes in between. Since 61e cc inlines: a call of a small static
function of the same file (or one called only once) becomes that
function's body, unless it keeps buffers on its stack or is marked
`__attribute__((noinline))`. Since 61d frames are smaller: AArch64 values
also live in x2-x7 between calls, locals that live in registers take no
room in the frame, and locals of blocks that never run at the same time
(the arms of a `switch`) share it. `-O`, `-O2`,
`-g`, `-W...`, `-f...` and `-std=...` are taken and ignored. Since 62a
cc has `__int128` (its division and conversions to and from float and
double call the helpers `__divti3` and the like in the C library) and
`_Complex` (`complex.h`, imaginary constants such as `1.0i`, `__real__`
and `__imag__`, `__builtin_complex`); `__builtin_sqrt` and `__builtin_fabs`
of a float or double are instructions. Since 62b per-thread variables
(`__thread`, `_Thread_local`) work as clang compiles them for programs and
libraries (x86-64: `__tls_get_addr`; AArch64: TLS descriptors), and `ld`
lays out their template (PT_TLS). Since 62c GCC's inline assembly:
`asm [volatile] ("template" : outputs : inputs : clobbers)` with the
constraints `r`, `m`, `i`, `n`, x86's `a b c d S D` and `N`, `=`, `+`,
named operands (`%[name]`), the modifiers `%w %k %b %q` (x86) and `%w %x`
(AArch64), `%%` and `%=`, and register variables
(`register long x asm("x0")`). Since 62d `__attribute__((constructor))`
(run when the program or library is loaded), `destructor` (recorded in
`.fini_array`, which RitterOS's loader does not call yet, with clang's
build as well) and `visibility("hidden")`. Since 62f calls through a
function pointer marked `__attribute__((ms_abi))` (x86-64: UEFI's
convention, integer and pointer arguments only), since 62g also functions
defined with it (UEFI's `efi_main`), and attributes inside a declarator
(`void (__attribute__((noreturn)) *fn)(void)`). Not yet: `asm goto`, `switch` on an `__int128`, conversions between `__int128`
and long double; long double constants hold a double's precision.
Messages say where: `FILE:LINE:COLUMN: error: ...`, and cc ends with exit
code 1.

### as: assemble (since 60)

The page with every option and message: [`as`](commands/as.md) (on RitterOS: `man as`).

```
as [-o FILE] FILE
```

The assembler cc uses: GNU assembler syntax (AT&T on x86-64) into an ELF
object (`a.out` if no `-o`). It knows the instructions cc writes and the
C library's own assembly; the directives `.text .data .bss .section
.globl .weak .hidden .type .size .p2align .balign .align .byte .short
.long .quad .zero .ascii .asciz .set`. Short jumps are used where they
reach. Since 62e it also takes what the system's own `.S` files use:
macros (`.macro NAME PARAM[=DEFAULT], ...` to `.endm`, called as `NAME
ARGS`, with `\PARAM`, `\()` and `\@` in the body), repetitions (`.rept
COUNT`, `.irp NAME, VALUES`, `.irpc NAME, CHARS`, each to `.endr`),
numeric labels (`1:`, referred to as `1b`, the last one before, and `1f`,
the next), and the kernel's instructions (x86-64: `cli`, `sti`, `iretq`,
`swapgs`, `lgdt`, `ltr`, `in`/`out`, `fxsave64`, moves to and from segment
and control registers and the like; AArch64: `wfe`, `sev`, `smc`, `tlbi`,
`ic`, `dc`, system registers). `cc` preprocesses a `.S` file for it as
clang does (`__ASSEMBLER__` defined, the source's own spacing).
Since 32c, on x86-64, the vector instructions too: SSE's (to SSE4.1),
each also as AVX (`v` and the name, three operands, `%ymm` registers for
256 bits), AVX2's own (`vpbroadcastb`, `vinserti128`, `vpermq`,
`vpblendvb`, `vzeroupper`, ...), FMA and the compares by predicate name,
all as clang encodes them; not AVX2's gathers, SSE4.2's string
instructions, F16C or AVX-512.
Since 32f, on AArch64, the integer vector (NEON) instructions (`v0.16b`
and lane and list operands; arithmetic, compares, reductions across lanes,
shifts, `dup`, `movi`, `tbl`, permutes, `ld1`-`ld4`/`st1`-`st4`), as clang
encodes them.

### ld: link programs (since 60)

The page with every option and message: [`ld`](commands/ld.md) (on RitterOS: `man ld`).

```
ld [-o FILE] [-L DIR] [-lNAME] [--as-needed] [-dynamic-linker PATH]
   [-static | -shared [-soname NAME] [--version-script FILE]] FILE...
```

The linker cc uses: objects, archives (an archive is searched for any
symbol still undefined, wherever it stands on the line) and shared
libraries into a program like the system's (`a.out` if no `-o`):
position-independent, the loader `/system/<arch>/lib/ld.so`, every
library function bound when the program starts, and with
`--as-needed` only the libraries it uses. The program starts at
`_start` (the start code).

Since 62d it also makes what the rest of the system is built as, with
lld's option names:

| Option | |
|---|---|
| `-static` | a program without the loader or shared libraries (ET_EXEC): every address filled in when linking |
| `-shared` | a shared library; `-soname NAME` is the name programs record for it |
| `--version-script FILE` | the functions a library exports (`{ global: a; b; local: *; };`); the rest stay inside |
| `--no-dynamic-linker` | a position-independent file that loads itself (ld.so) |
| `-T FILE` | `user.ld` and `ldso.ld` are built in; any other file is read as a linker script (since 62f, with `-static`) |
| `-z defs` | accepted: `ld` always refuses what is left undefined, with or without it (a weak symbol becomes 0) |
| `-z norelro` | no read-only part after the relocations |
| `--efi` | a UEFI application (PE32+, since 62g): the objects laid out as lld-link does (ImageBase 0x140000000; .text, .rdata, .data, .reloc), the entry point `efi_main`, a base relocation for every 64-bit address |
| `-Bstatic`, `-Bdynamic`, `-pie`, `-z now`, `-z relro`, `-z nocopyreloc`, `--hash-style=sysv` | taken (what it always does) |

Symbols marked hidden (`__attribute__((visibility("hidden")))` in C,
`.hidden` in assembly) are never exported and are reached without the
GOT.

The linker scripts it reads (62f, what the kernel's `kernel/linker.ld`
uses): `ENTRY(_start)`, `SECTIONS { ... }` with `. = EXPR;`, `NAME =
EXPR;`, output sections `NAME [ADDRESS] [(NOLOAD)] : [ALIGN(N)] { ... }`
holding `*(PATTERN ...)` (also inside `KEEP(...)`), `*(COMMON)`,
assignments and `. += N`, and `/DISCARD/`. Expressions take numbers (with
`K` and `M`), `.`, symbols, `ALIGN(N)`, `ALIGN(EXPR, N)`, `+ - * / % & |
<< >> ~` and parentheses. Each input section goes to the first rule that
names it; an allocated section that no rule names is an error. The result
is a static executable with one segment for each run of sections with the
same permissions, and a symbol table.

### ar: archives of object files (since 63)

The page with every option and message: [`ar`](commands/ar.md) (on RitterOS: `man ar`).

```
ar rcs ARCHIVE FILE...
ar t ARCHIVE
ar x ARCHIVE [MEMBER...]
```

Static libraries such as `libritteros.a` are archives: object files in
one file, with an index of the symbols they define, which `ld` searches
for anything still undefined. `ar rcs` adds each FILE to ARCHIVE (making
it if it is not there), replacing a member of the same name; the index is
always made again (`s`). `ar t` lists the members, `ar x` writes them (or
the MEMBERs named) into the current folder. The format is GNU's, written
as llvm-ar writes it by default: no dates, owners or modes are kept, so
the same objects always make the same archive.

```sh
cc -c part1.c part2.c
ar rcs libparts.a part1.o part2.o
cc -o prog main.c libparts.a
```

### js: run JavaScript (since 27o)

The page with every option and message: [`js`](commands/js.md) (on RitterOS: `man js`).

```
js FILE [ARG...]
js -e CODE [ARG...]
js
```

Runs JavaScript with RitterOS's own engine, the one iBrowser will use: a
script from FILE, the text after `-e`, or, with neither, a prompt `js> `
that runs each line typed (or read from the standard input) and prints its
value unless it is `undefined`. The script's ARGs are the global array
`arguments`. `print(...)` and `console.log(...)` write their arguments,
separated by spaces, as one line on the standard output; `console.error`
and `console.warn` on the standard error.

```sh
js -e 'print([3, 1, 2].sort(), 2 ** 10)'      # 1,2,3 1024
js count.js 10                                # arguments[0] is "10"
```

The language is ECMAScript 2023 but for async generators (and `for
await`), modules, `with`, BigInt and typed arrays: let and
const, arrow functions, classes with fields,
private names, static blocks and `super`, destructuring, spread and rest,
template literals, optional chaining, `??` and the logical assignments,
labeled loops, getters and setters, Symbols and iterators; the standard
objects Object, Function, Array, String, Number, Boolean, Math, JSON,
RegExp (with lookbehind, named groups and the `d`, `s` and `u` flags),
Map, Set, WeakMap, WeakSet, Date, Error and its kinds, Reflect, and since
64r Proxy (all thirteen traps; what a trap answers is checked against its
target as the standard says, so a trap cannot report a frozen property
with another value, and `Proxy.revocable` gives a proxy that can be
switched off); since 27r
generators (`function*`, `yield`, `yield*`), Promises (`then`, `catch`,
`finally`, `all`, `allSettled`, `any`, `race`, `withResolvers`) and `async`
functions with `await`. After the script, `js` runs the jobs it queued
(the Promises' reactions) until none is left, as JavaScriptCore's shell
does. Calling an async generator throws a SyntaxError for now. `eval` runs its
text in the global scope (also when called directly). Case mapping
(`toUpperCase`, `toLowerCase`, regular expressions with `i`) follows the
Unicode Character Database 17.0 for every script: the full mappings
(`"ß".toUpperCase()` is `SS`, `"ﬁ"` gives `FI`), the final sigma, simple
case folding with `iu`; the locale forms (`toLocaleUpperCase`) are the
same (no Turkish or Lithuanian rules). The tables are Unicode's data,
under the Unicode License v3 (`scripts/UNICODE-LICENSE.txt`).

`Intl` (27s) is English only (`en-US`): any other locale is accepted and
formats as English (`resolvedOptions().locale` then says `en` or
`en-US`). It has `NumberFormat` (decimal, percent, currency with symbol,
narrow symbol, code or name, the units the standard lists, compact,
scientific and engineering notation, digit and rounding options, signs,
grouping, `formatToParts` and `formatRange`), `DateTimeFormat` (date and
time styles, the separate fields, 12 or 24 hours, day periods,
`formatToParts`, a simple `formatRange`), `PluralRules` (cardinal and
ordinal), `Collator` (the Unicode collation order for the Latin letters
and punctuation: accents, case and numeric options, `ignorePunctuation`;
other scripts by their letters' order), `RelativeTimeFormat`,
`ListFormat`, `Locale`, `getCanonicalLocales` and `supportedValuesOf`.
`toLocaleString`, `toLocaleDateString`, `toLocaleTimeString` and
`localeCompare` go through them and take the same locales and options.
Time zones: UTC, offsets (`+05:30`, `Etc/GMT+5`), the machine's own (its
local time; named after its offset, e.g. `-05:00`, or `UTC`), and about
fifty named zones of the Americas, Europe, Africa, Asia and Oceania with
today's daylight saving rules (dates before those rules began are shown
by them too); any other name is a RangeError. Not there: `DisplayNames`,
`Segmenter` and `DurationFormat`. `Intl` is compiled the first time a
script uses it (a few milliseconds), so scripts that never do pay nothing.
Its English names and patterns follow Unicode's Common Locale Data
Repository (CLDR), under the Unicode License v3 (`scripts/UNICODE-LICENSE.txt`).

Messages: an uncaught exception prints `js: uncaught NAME: MESSAGE at
FILE:LINE` on the standard error, and `js` ends with exit code 3 (a syntax
error the same way); `js: cannot read FILE` (exit code 1).

### touch: create empty files, or give files the time now (or another)

The page with every option and message: [`touch`](commands/touch.md) (on RitterOS: `man touch`).

```
touch [-acm] [-r REFERENCE | -t [[CC]YY]MMDDhhmm[.SS]] FILE...
```

Creates each FILE empty (not with `-c`); a FILE that exists gets the
current time as its modification time (what `make` goes by), folders too.
Since 63m, as POSIX's touch: `-t` gives a time instead, in the machine's
local time (`touch -t 202001021530.44 f`: 2 January 2020, 15:30:44; YY
69-99 is 19YY, 00-68 20YY; without the year this year's), `-r` the time of
REFERENCE; `-m` sets the modification time, `-a` the access time (both
without either), which RitterOS does not keep, so `-a` alone changes
nothing. The time now needs the right to write the file (or owning it), any
other time owning it (or root): `Operation not permitted`. On FAT a time is
kept in steps of 2 seconds, on exFAT of 10 ms.

### mkdir: create directories

The page with every option and message: [`mkdir`](commands/mkdir.md) (on RitterOS: `man mkdir`).

```
mkdir [-p] DIR...
```

Creates the directories. `-p` also creates missing parents
(`mkdir -p a/b/c`) and accepts directories that exist already.

### rmdir: remove empty directories

The page with every option and message: [`rmdir`](commands/rmdir.md) (on RitterOS: `man rmdir`).

```
rmdir DIR...
```

Only empty directories; otherwise `Directory not empty`.

### rm: remove files

The page with every option and message: [`rm`](commands/rm.md) (on RitterOS: `man rm`).

```
rm [-rf] PATH...
```

Removes files. `-r` (or `-R`) also removes directories with everything in
them. With `-f` (since 63, what makefiles' recipes say) a PATH that is not
there is no error and nothing is printed; `rm -f` with no PATH does
nothing. The options may be written together (`-rf`).
There is no undo, but files stay readable in snapshots taken before
(section 5). A symbolic link is removed itself, never what it names, and
`-r` never follows one into a folder. A file with several names (hard
links) stays until its last name is removed.

### cp: copy

The page with every option and message: [`cp`](commands/cp.md) (on RitterOS: `man cp`).

```
cp [-rp] FROM TO
```

Copies a file, or with `-r` a directory with everything in it. `-p`
(since 63m) also gives the copy, and with `-r` everything in it, its
original's modification time and permissions, and when root copies its
owner and group too (the setuid bit only with the owner, as POSIX's). If TO is a
directory, the copy goes into it under FROM's name; if TO is a file that
exists, its content is replaced (since 59, as POSIX's cp; it keeps its
permissions and other names). Copies to a new name are instant:
the copy shares the data blocks with FROM until one of the two changes, so
copying a large file costs almost no space. FROM may be in a snapshot,
which restores an old version:

```
ritteros:/$ cp /.snapshots/monday/home/root/notes.txt /home/root/notes.txt
```

### tar: archives of files and folders

The page with every option and message: [`tar`](commands/tar.md) (on RitterOS: `man tar`).

```
tar c|x|t [vm] [-f ARCHIVE] [-C DIR] [FILE...]
```

`tar c` puts FILEs (folders with everything in them) into one archive,
`tar x` takes an archive apart (all of it, or the FILEs named), `tar t`
lists it; `v` also names each file (`tv` as `ls -l` does). The archive is
ARCHIVE, or without `-f` (or with `-f -`) the standard input or output, so
a folder goes from one machine to another in one ssh call:

```sh
tar cf - myproject | ssh root@ADDRESS 'tar xf - -C /home/root'     # there
ssh root@ADDRESS 'tar cf - -C /home/root myproject' | tar xf -     # and back
```

`-C DIR` works in DIR (where `c` takes the files from, `x` puts them). The
options may also come as one word without the dash: `tar cvf A.tar dir`.
The format is POSIX's ustar, with pax headers for names too long for it:
what the tar of macOS, Linux and the BSDs make and read (on a Mac set
`COPYFILE_DISABLE=1` before `tar c`, or the archive carries `._NAME` files
of Apple's metadata). Kept are files, folders, symbolic links and their
permission bits (setuid only when root takes it apart); hard links come out
as files of their own. Taken apart, files belong to who takes them apart
and get their times from the archive (since 63m; folders' at the end, as
filling them changed them), or with `m` the time of that moment.
A name loses a leading `/`, and an entry with a `..` part, or one on the
way through a symbolic link, is left out (`tar: NAME: a path that would
leave this folder: left out`, `...: it would go through a symbolic link:
left out`), so an archive never writes outside the folder. No compression
yet (`-z`). Since 63h.

### mv: move or rename

The page with every option and message: [`mv`](commands/mv.md) (on RitterOS: `man mv`).

```
mv FROM TO
```

Moves or renames a file or directory. If TO is a directory, FROM goes into
it under its own name. An existing TO is replaced, as POSIX's `mv`
(68a): a file by a file, an empty folder by a folder; on RitterFS in one
commit, so `mv prog.new prog` updates a program safely. Nothing is
copied; a directory cannot be moved into itself.

### edit: edit a text file

The page with every option and message: [`edit`](commands/edit.md) (on RitterOS: `man edit`).

```
edit FILE
```

Section 4.

### find: search

The page with every option and message: [`find`](commands/find.md) (on RitterOS: `man find`).

```
find [DIR] [-name PATTERN] [-attr KEY[=VALUE]]...
```

Searches DIR (default: the current directory) and everything below it and
prints each path that meets all conditions:

| Condition | Matches |
|---|---|
| `-name PATTERN` | names matching PATTERN: `*` any characters, `?` one character (quote it: `-name '*.txt'`) |
| `-attr KEY` | files with the attribute KEY (section 5) |
| `-attr KEY=VALUE` | files whose attribute KEY is exactly VALUE |

Without conditions it lists everything.

```
ritteros:/$ find /test -name '*.txt'
ritteros:/$ find / -attr project=ritteros
```

### grep: search inside files

The page with every option and message: [`grep`](commands/grep.md) (on RitterOS: `man grep`).

```
grep [-EFcilnqrsvx] [-e PATTERN]... [-f FILE]... [PATTERN] [FILE...]
```

Prints the lines of the files (or of its standard input, `grep x < FILE`)
that match PATTERN. With several files each line starts with its file's
name. PATTERN is a regular expression (quote it, so the shell leaves `*`
alone):

| In PATTERN | Matches |
|---|---|
| a character | itself |
| `.` | any one character |
| `[abc]`, `[a-z]` | one of the set |
| `[^...]` | one character not in the set |
| `[[:alpha:]]`, `[[:digit:]]`, ... | one of a class (`alnum` `blank` `cntrl` `graph` `lower` `print` `punct` `space` `upper` `xdigit` too) |
| `X*` | the item X any number of times, none too |
| `X\{2\}`, `X\{2,5\}`, `X\{2,\}` | X that many times (since 58a) |
| `\(...\)`, `\1`-`\9` | a group, and what the group took, again (since 58a) |
| `^`, `$` | the line's start, its end (first or last in PATTERN) |
| `\X` | X itself (`\.` a dot, `\*` a star) |

With `-E` (since 58a) PATTERN is an extended regular expression, as
`awk` and `egrep` have them: `A|B` (either), `X+` (once or more), `X?`
(once or not), `X{2,5}`, groups in `( )` without backslashes. Both kinds
are POSIX's (the C library's `regex.h`): of the matches that start
first, the longest.

| Option | |
|---|---|
| `-E` | PATTERN is an extended regular expression (since 58a) |
| `-F` | PATTERN is plain text, every character itself (68f) |
| `-e PATTERN`, `-f FILE` | the patterns given so, instead of the first operand: several, a line matching any is selected; FILE has one per line (68f) |
| `-x` | only lines matched whole (68f) |
| `-q` | nothing printed, only the exit code; it ends at the first selected line (68f) |
| `-s` | nothing said of files missing or unreadable (68f) |
| `-i` | upper and lower case alike |
| `-v` | the lines that do not match |
| `-n` | with line numbers |
| `-c` | only how many lines, per file |
| `-l` | only the names of the files with a match |
| `-r` | folders too, everything below them (the current folder without FILE) |

Options combine (`-in`). A file with a NUL byte is binary: `grep: FILE:
binary file matches` instead of its lines. It ends with 0 if a line was
printed (or counted), 1 if none, 2 on an error (a missing file, a pattern
it cannot read).

```
ritteros:/$ grep -n hello /test/text/lorem.txt
ritteros:/$ grep -ri 'error' /system/logs
ritteros:/$ grep -c '^$' notes.txt
```

---

## 4. The text editor

```
edit FILE
```

Opens FILE in a full-screen editor; a FILE that does not exist yet is
created when you save (the message line says `New file.`).

The text fills the screen. Below it, a bar in reverse video shows the
file's name, `(modified)` once you changed something, and the cursor's
line and column. The last line shows the keys, or a message after an
action.

| Key | Does |
|---|---|
| letters, digits, Tab | insert at the cursor |
| Enter | break the line |
| Backspace | erase the character before the cursor (at a line's start: join with the line above) |
| Delete | erase the character at the cursor |
| arrow keys | move by one character or line |
| Home, End | the start or end of the line |
| Page Up, Page Down | a screen up or down |
| **Ctrl-S** | save |
| **Ctrl-Q** | quit; with unsaved changes it asks first: Ctrl-Q again quits without saving, Ctrl-S saves |

Messages: `Saved: 3 lines, 20 bytes.`; `Not saved: Permission denied.`
(or another reason: the file stays as it was on the disk, and your text
stays in the editor). Saving adds a line break at the end if the last line
has none.

Long lines scroll sideways, long files up and down. Files up to 1 MiB.

Files with Windows' line breaks, CR LF (since 50i: as many files made on
Windows, and the PlutoSDR's `config.txt`), are edited as any other: the
CRs are not shown, the bar says `CRLF` after the name, the message says
`55 lines, 1067 bytes, CR LF line breaks.`, and saving writes every line
break as CR LF again, so the file keeps its form. A file is taken as one
when its first line ends in CR LF.

In `make run` on a Mac, the editor draws for RitterOS's screen: use the
QEMU window, or make the Terminal window at least as large (139x48
characters at QEMU's 1280x800).

---

## 5. Snapshots, versions and attributes

RitterFS, the file system, can keep earlier states of the whole disk and
store extra information on files.

### snapshot: keep the current state

The page with every option and message: [`snapshot`](commands/snapshot.md) (on RitterOS: `man snapshot`).

```
snapshot
snapshot create NAME
snapshot delete NAME
```

Without arguments it lists the snapshots with the generation (commit
number) each one shows. `create` keeps the current state of everything
under NAME (1-63 characters, no `/`); it takes no time and no space at
first: only what changes later is stored twice. `delete` removes a snapshot
and reports how many blocks became free. Anyone may list them; `create` and
`delete` are root's.

A snapshot is readable at `/.snapshots/NAME/...` (read only): `ls`, `cat`,
`cd` and `cp` work there, even running an old program.

```
ritteros:/$ snapshot create monday
snapshot 'monday' taken at generation 12
ritteros:/$ ls /.snapshots/monday/home
```

### versions: a file's history

The page with every option and message: [`versions`](commands/versions.md) (on RitterOS: `man versions`).

```
versions PATH
```

The file in every snapshot that has it, oldest first, then as it is now:
each with its size and whether it changed since the version before.

```
ritteros:/$ versions /home/root/notes.txt
/home/root/notes.txt:
  monday (generation 12)              241 bytes  first version
  now                                  39 bytes  changed
```

### attr: attributes

The page with every option and message: [`attr`](commands/attr.md) (on RitterOS: `man attr`).

```
attr PATH                 list PATH's attributes (key=value)
attr PATH KEY             print one value
attr PATH KEY=VALUE       set it
attr PATH -KEY            remove it
```

Attributes are key=value pairs on files and directories (keys up to 63
bytes, values up to 1 KiB), searchable with `find -attr`. In snapshots
they are as they were, and read only.

```
ritteros:/$ attr /home/root/notes.txt project=ritteros
ritteros:/$ attr /home/root/notes.txt
project=ritteros
```

---

## 6. Users and permissions

Every file and directory has an owner, a group and permissions; every
program runs as a user. `root` (user 0) may do everything.

**Permissions** are three groups of three: for the owner, the group, and
everyone else; each `r` (read), `w` (write), `x` (run a program; for a
directory: look up names in it). `ls -l` shows them:

```
-rw-r--r--   1  1000  1000         26  notes.txt
drwx------   1     0     0      <dir>  root/
```

The first character is the type: `-` a file, `d` a directory, `l` a
symbolic link (`ln`, section 3), `b` a disk, `c` a character device
(section 7); the number after the permissions counts the file's names
(hard links). As a number (octal), each group is r=4, w=2, x=1 added up:
`644` is `rw-r--r--`, `755` is `rwxr-xr-x`, `700` is `rwx------`.

**Setuid.** A program with the setuid bit (`4000` added: `4755`, shown as
`rwsr-xr-x`) runs with its owner's rights whoever starts it: a root-owned
one runs as root. That is how `su` and `sudo` work. Only programs on the
system's own disk count (a program on a stick or in a snapshot runs as
whoever starts it), and giving a program to another owner (`chown`) takes
the bit away.

**Sticky folders** (since 57b). A folder with the sticky bit (`1000`
added: `1777`, shown as `rwxrwxrwt`) lets everyone make files in it, but
only a file's owner, the folder's owner or root removes or renames it,
or moves another file over it (`rm: ...: Operation not permitted`
otherwise). `/tmp` is such a folder:
the place for anyone's temporary files.

**The folders of a disk** (since 57b, as on Unix). Besides `/system` (the
system, section 1) and `/home`, every disk has `/bin` and `/usr/bin`
(links to `/system/<arch>/bin`, so scripts from elsewhere starting with
`#!/bin/sh` or `#!/usr/bin/env` run), `/lib` (a link to
`/system/<arch>/lib`), `/etc` (a link to `/system/config`, the settings)
and `/tmp` (`1777`). `osinstall` makes the same on the disk it installs.

### id: who you are

The page with every option and message: [`id`](commands/id.md) (on RitterOS: `man id`).

```
id
```

Prints your user and group: `uid=1000(anna) gid=1000`. In a setuid
program the user whose rights it has comes too: `uid=1000(anna)
euid=0(root) gid=1000` (the real user and the effective one).

### useradd: add a user

The page with every option and message: [`useradd`](commands/useradd.md) (on RitterOS: `man useradd`).

```
useradd NAME
```

Root only. Names: 1-31 of `a-z`, `0-9`, `_` and `-`, not starting with a
digit or `-`. The new user gets the next free user ID from 1000 on, a
group with the same number, and the home directory `/home/NAME`. Set the
password next:

```
ritteros:/$ useradd anna
useradd: anna: user ID 1000, home /home/anna; set a password with: passwd anna
ritteros:/$ passwd anna
```

### passwd: set a password

The page with every option and message: [`passwd`](commands/passwd.md) (on RitterOS: `man passwd`).

```
passwd [NAME]
```

Asks for the new password twice (not shown). Without NAME: your own.
Root only so far. Passwords are stored only as salted hashes
(PBKDF2-HMAC-SHA256, 100,000 rounds) in `/system/config/passwords`,
which only root can read; the users are in `/system/config/users`.

### su: become another user

The page with every option and message: [`su`](commands/su.md) (on RitterOS: `man su`).

```
su [USER]
```

Asks USER's password (root, when no USER is named) and starts their shell
in the current folder; `exit` ends it and you are yourself again. Root is
not asked. A wrong password: `su: wrong password`, after a pause of 2
seconds (guessing stays slow). Every use is written to the log `login`
(control characters in what was typed shown as `?`).

```
ritteros:/home/anna$ su bob
Password:
ritteros:/home/anna$ id
uid=1001(bob) gid=1001
ritteros:/home/anna$ exit
```

### sudo: one command as root

The page with every option and message: [`sudo`](commands/sudo.md) (on RitterOS: `man sudo`).

```
sudo COMMAND [ARGUMENTS...] | -k
```

Runs COMMAND as root after asking for your own password. A correct one
counts for 5 minutes in the same session (68e: the same terminal, or one
SSH connection; each `ssh HOST COMMAND` is a session of its own): a sudo
meanwhile asks nothing, and `sudo -k` forgets it at once. The password is
one line: given on standard input (a script's), the lines after it are
COMMAND's input; no password at all is `sudo: no password given` (68d).
Only the users named in `/system/config/sudo` may, a file root writes:

```
users = anna, bob
nopassword = anna
```

The users also in `nopassword` (41h-7) are never asked: for a program that
deploys without a person, as the SDK's `make deploy` by an agent (whoever
has such a login has root). Anyone else: `sudo: bob may not run commands as root (not in
/system/config/sudo)`, without a password asked. Every new system has the
file from the start (since 63n: a fresh disk, the stick, and every
`osinstall`), with comments and `users =` empty, so only root may until root
adds names; without the file too only root may (and root is never asked).
`osinstall` writes the empty one, never the running system's, whose user
names are not the new system's. A wrong password: `sudo: wrong password`,
after a pause of 2 seconds. sudo ends with the command's exit code. Every
use is written to the log `login` (control characters in the command's name
shown as `?`, so a name cannot add lines of its own).

```
ritteros:/home/anna$ sudo useradd carl
Password for anna:
useradd: carl: user ID 1002, home /home/carl; set a password with: passwd carl
```

`su` and `sudo` are setuid programs (section "Permissions"): root's, with
mode `4755`.

### chmod: change permissions

The page with every option and message: [`chmod`](commands/chmod.md) (on RitterOS: `man chmod`).

```
chmod MODE PATH...
```

MODE in octal, for example `chmod 600 secret.txt`; `4755` also sets the
setuid bit, `1777` the sticky bit (on a folder). Only the owner or root.

### chown: change the owner

The page with every option and message: [`chown`](commands/chown.md) (on RitterOS: `man chown`).

```
chown UID[:GID] PATH...
```

By number: `chown 1000:1000 /home/anna/file`; a number alone (`chown 1000
FILE`) sets the group to the same number. Root only. It takes a program's
setuid bit away.

### login

The page with every option and message: [`login`](commands/login.md) (on RitterOS: `man login`).

The program behind `login:` (section 1). The kernel starts it; you do not
run it yourself. It makes a user's `~/.config` (with a README) at their
login when it is missing, and starts their shell as a login shell, `sh -l`,
which runs `~/.config/login` first (section 1, "Your own settings").

---

## 7. Disks and volumes

### disks: the disks and their partitions

The page with every option and message: [`disks`](commands/disks.md) (on RitterOS: `man disks`).

```
disks                                   all disks and partitions
disks DISK                              one disk in detail
disks init DISK                         a new, empty partition table (GPT)
disks add DISK SIZE|rest [TYPE [NAME]]  a new partition
disks delete PARTITION                  take a partition out of the table
disks rename PARTITION NAME             a partition's name
disks grow [DISK]                       the last partition (RitterFS) to the disk's end
```

`disks grow` (68j) is for a disk image written to a bigger disk, as the
256 MiB stick images are: the GPT is fitted to the whole disk, the last
partition (a mounted RitterFS; on the system disk if DISK is not given)
reaches its end while it stays mounted, and the file system takes the new
room in one commit. It asks nothing, since it only adds free room. The
first start runs it by itself when the system disk's table was made for
a smaller disk.

One line per disk (size, kind, the disk's own name, its partition table)
and one per partition below it (size, number and type, the file system
found on it and its label, where it is mounted):

```
nvme0n1          1.0 GiB  NVMe 'QEMU NVMe Ctrl', GPT, 2 partitions; the system disk
  nvme0n1p1     64.0 MiB  #1 EFI system partition: FAT32 'EFI'
  nvme0n1p2    958.0 MiB  #2 RitterOS: RitterFS 'RitterOS', generation 14, mounted at /
usbdisk1        64.0 MiB  USB 'QEMU QEMU HARDDISK', MBR, 1 partition
  usbdisk1p1    63.0 MiB  #1 type 0x0C (FAT32 (LBA)): FAT32 'STICK'
usbdisk2        16.0 MiB  USB 'QEMU QEMU HARDDISK', no partition table: empty
```

Partition tables: GPT, or MBR (older disks and many sticks); "no partition
table" means a file system on the whole disk, or nothing. Recognized file
systems: RitterFS, FAT12/16/32, exFAT, NTFS, ext2/3/4, APFS; `empty` means
the start of the area is all zeros. Anyone may run `disks`.

Device names: `nvme0n1` (NVMe SSDs), `sata0`, `sata1` (SATA disks),
`usbdisk0`, `usbdisk1` (USB sticks and disks, in the order found; a stick
plugged in again gets its old name). Partitions add their number:
`nvme0n1p2`, `usbdisk0p1`.

A USB disk whose medium is not there yet (a card reader without a card,
or a device that is still preparing it, as a PlutoSDR does for a while
after it starts) keeps its name and is asked again every 2 seconds; its
volumes are mounted when the medium comes:

```
                 mass storage: usbdisk0: no medium yet (sense key 2, code 0x3a); waiting for it
  usbdisk0:      its medium is ready
```

Each disk and partition is also in `/dev` (section 10).

**One disk in detail.** `disks DISK` adds the block size, the blocks
partitions may use, and each partition's first and last block, with the
free areas between them (those of at least 1 MiB):

```
ritteros:/$ disks usbdisk0
usbdisk0        64.0 MiB  USB 'QEMU QEMU HARDDISK', GPT, 1 partition
  blocks of 512 bytes; partitions may use 34-131038
  (free)        16.0 MiB  2048-34815
  usbdisk0p2    46.9 MiB  34816-131038          #2 Microsoft basic data 'Pictures': empty
```

**Changing a partition table** is root's. `disks` shows what it is about
to do and writes only after you type the disk's name; anything else (or
Ctrl-D) leaves the disk as it was:

```
ritteros:/$ disks add usbdisk0 16M
usbdisk0: new partition #1 (usbdisk0p1): 16.0 MiB, blocks 2048-34815, RitterOS
Type the disk's name (usbdisk0) to write the new table: usbdisk0
usbdisk0        64.0 MiB  USB 'QEMU QEMU HARDDISK', GPT, 2 partitions
  ...
```

- `init DISK`: a new, empty GPT. Whatever table (or file system on the
  whole disk) was there is no longer found. Refused while a partition of
  the disk is mounted.
- `add DISK SIZE [TYPE [NAME]]`: a partition of SIZE (`512K`, `100M`,
  `2G`, `1T`: binary units; a plain number is bytes) in the first free
  area large enough, or with `rest` all of the largest free area.
  Partitions start on a 1 MiB boundary. TYPE: `ritteros` (the default),
  `efi` (an EFI system partition), `data` (for FAT and exFAT, as Windows
  and macOS expect), `linux`, `swap`, or a type GUID. NAME (up to 36
  plain characters) defaults to the type's name.
- `delete PARTITION`: its entry goes out of the table. The data stays on
  the disk until something else is written there: `add` of a partition
  with the same start and type finds it again (a RitterFS there is mounted
  again). Refused while it is mounted (`unmount` it first).
- `rename PARTITION NAME`: the name in the GPT (not the file system's
  label).

When the table is written, RitterOS reads it again: `/dev` and `disks`
show the change at once, and a new partition holding a RitterFS is
mounted at `/volumes/<label>`. A GPT is kept twice, at the start and at
the end of the disk, each with checksums; the copy at the end is written
first, so a stick pulled out in the middle still has one whole table (and
`disks` falls back to the copy at the end if the first one is damaged).

**A disk image on a bigger stick.** Copying a RitterOS image to a larger
stick copies its partition table too, made for the image's size: the rest
of the stick lies outside it. The first start of a RitterOS stick grows
its RitterFS to the stick's end by itself (`disks grow`, since 68j). On
another disk, `disks DISK` says so (`the table was made for a smaller
disk (it covers 255.9 MiB of 28.8 GiB)`) and already shows the free space
up to the end; `disks grow DISK` takes it for the last partition's
RitterFS, and any other change, such as `disks add DISK rest data`, fits
the table to the whole disk first.

Only GPT disks can be changed so far; an MBR disk can get a new GPT with
`init` (which also makes it unreadable for very old systems). A new
partition is empty: `format` makes a file system on it.

### format: make a new file system

The page with every option and message: [`format`](commands/format.md) (on RitterOS: `man format`).

```
format DEVICE [LABEL]                  a RitterFS
format -t fat DEVICE [LABEL]           a FAT32, to share with other systems
format -t exfat DEVICE [LABEL]         an exFAT, to share them, files of any size
```

Makes a new, empty RitterFS on a partition (`format usbdisk0p1 Photos`)
or on a whole disk without partitions (`format usbdisk2`), with LABEL as
its name (1-63 plain characters, no `/`; `RitterFS` if not given). It
shows the size and what is about to be lost, and formats only after you
type the device's name:

```
ritteros:/$ format usbdisk0p1 Photos
usbdisk0p1: a new RitterFS 'Photos', 16.0 MiB (4096 blocks of 4 KiB)
Type the device's name (usbdisk0p1) to format it: usbdisk0p1
  ...
usbdisk0p1: formatted, mounted at /volumes/Photos
```

The new file system is mounted at once at `/volumes/<label>`, and again
whenever the disk is plugged in: RitterOS finds every RitterFS by its
content, whatever the partition's type says. Formatting clears the marks
of the file system that was there before (so other systems no longer see
a FAT or ext there either); for a whole disk also its partition table,
both copies. The old data itself is not erased, only no longer found.
Refused while the device is mounted (for a whole disk: while one of its
partitions is). Root only. At least 2 MiB.

**FAT32** (`-t fat`): for a stick or partition that Windows, macOS and
Linux should read and write too. LABEL is at most 11 letters, digits,
spaces or `!#$%&'()-@^_`{}~`, kept in upper case (`share` becomes
`SHARE`); without one the volume has none and is mounted under its
device's name. The cluster size is chosen as Windows does (512 bytes up
to 260 MB, then 4 KiB to 8 GB, 8 KiB to 16 GB, 16 KiB to 32 GB, 32 KiB
above). FAT32 needs at least about 33 MiB and works up to 2 TiB; a file
on it is at most 4 GiB. For other systems to show a partition, give it
the type `data` (`disks add DISK rest data`); `format` warns about a
partition of the type RitterOS.

```
ritteros:/$ format -t fat usbdisk0p3 share
usbdisk0p3: a new FAT32 'SHARE', 27.8 GiB (1823104 clusters of 16.0 KiB)
Type the device's name (usbdisk0p3) to format it: usbdisk0p3
usbdisk0p3: formatted, mounted at /volumes/SHARE
```

**exFAT** (`-t exfat`): what large sticks and memory cards come with, read
and written by Windows, macOS and Linux, without FAT32's limit of 4 GiB
per file. LABEL is at most 11 characters, any but `"*/:<>?\|` (also
beyond ASCII: `"Fotos Señor"`), kept as typed; without one the volume is
mounted under its device's name. The cluster size is chosen as Windows
does: 4 KiB up to 256 MiB, 32 KiB up to 32 GiB, 128 KiB above. At least
1 MiB. The new volume has the full up-case table (which tells every
system that `É` and `é` are the same letter in names). Its capitals are
data of the Unicode Character Database, used under the Unicode License v3:
its copyright and permission notice is in `scripts/UNICODE-LICENSE.txt`.

```
ritteros:/$ format -t exfat usbdisk1 Photos
usbdisk1: a new exFAT 'Photos', 57.6 GiB (472028 clusters of 128.0 KiB)
Type the device's name (usbdisk1) to format it: usbdisk1
usbdisk1: formatted, mounted at /volumes/Photos
```

### Volumes: other disks and sticks

Every RitterFS, FAT or exFAT on another disk or partition (a USB stick, a
second SSD) appears by itself at `/volumes/<label>`, at the boot or when
you plug it in; `ls /volumes` lists them by name. Two with the same label
get `-2`, `-3`, ...: `/volumes/RitterOS`, `/volumes/RitterOS-2`. On a
RitterFS volume everything works as on the system disk: `ls`, `cat`,
`cd`, `edit`, `>`, `mkdir`, `rm`. (`snapshot` works on the system disk
only, so far.) EFI system partitions are left alone, as other systems do
(the system disk's own is at `/boot`); `mount` mounts one by hand.

```
ritteros:/$ ls /volumes
     <dir>  STICK/
ritteros:/$ cp notes.txt /volumes/STICK/
ritteros:/$ mv /volumes/STICK/report.txt /home/root/
```

`cp` and `mv` between volumes copy the data byte by byte (within one
volume they share it instantly); `mv` removes the original once the copy
is complete.

**FAT** (FAT12, FAT16, FAT32: sticks and memory cards from a shop,
cameras) works like RitterFS: `ls`, `cat`, `cd`, `edit`, `>`, `>>`,
`mkdir`, `cp`, `mv`, `rm`. Long file names are shown and written as
Windows and macOS expect them (with an 8.3 alias such as `NEWFOL~1` for
older systems); a name is found whatever its case (`cat
/volumes/STICK/readme.txt` opens `README.TXT`). Names may not contain
`\ / : * ? " < > |` (FAT cannot hold them: `Invalid argument`), and a
file can be at most 4 GiB. FAT has no owners: everything shows as root's,
and everyone may read and write: directories `drwxrwxrwx`, files
`-rw-rw-rw-`, or `-r--r--r--` for a file marked read only (`chmod 444`
marks it, `chmod 644` clears it). Attributes (`attr`) and snapshots are
not supported there. FAT has no protection against a stick pulled out
while writing (other systems have the same problem): `unmount` first.

**exFAT** (large sticks and memory cards, shared with Windows, macOS and
Linux) works like FAT: `ls`, `cat`, `cd`, `edit`, `>`, `>>`, `mkdir`,
`cp`, `mv`, `rm`, `chmod` (the read-only attribute), without FAT's limit
of 4 GiB per file. Names may not contain `\ / : * ? " < > |` (`Invalid
argument`). The volume is marked as being changed until each change is
written out; `unmount` before pulling a stick. Long and Unicode names work,
and a name is found whatever its case, also beyond A-Z (`üNÏCÖDÉ.TXT`
opens `Ünïcödé.txt`), by the table of capitals the volume carries, as
the other systems do. Like FAT it has no owners: everything shows as
root's, `drwxrwxrwx` and `-rw-rw-rw-` (`-r--r--r--` read only). Its
label is kept in the volume's root directory, so `disks` shows it once
the volume is mounted. NTFS is recognized by `disks`, but cannot be
mounted.

A volume whose check at the plug-in finds problems is mounted read only.
Pulling a stick out unmounts it; what was saved is on it (each change is
written at once), but a file still open for writing is lost, so close
programs first, or `unmount` it.

For safety, `rm -r` and `mv` refuse a mounted volume itself (`rm: X: a
mounted volume (unmount it first)`), and `rm -r` stops where another
volume is mounted inside the directory it removes.

### mount: list or mount volumes

The page with every option and message: [`mount`](commands/mount.md) (on RitterOS: `man mount`).

```
mount
mount DEVICE [PATH]
```

Without arguments: what is mounted where, and whether read only:

```
nvme0n1p1    on /boot (FAT32 'EFI', read only)
nvme0n1p2    on / (RitterFS 'RitterOS', read-write)
usbdisk0p2   on /volumes/STICK (RitterFS 'STICK', read-write)
usbdisk1p1   on /volumes/CAMERA (FAT32 'CAMERA', read only)
```

With DEVICE (a partition's name from `disks`, with or without `/dev/`;
RitterFS, FAT or exFAT): mount it at `/volumes/<label>`, or at PATH: a new name in `/volumes`, or
an existing directory anywhere (what it holds is hidden while the volume
is mounted there). Root only. Volumes are mounted by themselves when they
appear; `mount` is for one you unmounted, or another place.

### unmount: unmount volumes

The page with every option and message: [`unmount`](commands/unmount.md) (on RitterOS: `man unmount`).

```
unmount PATH|DEVICE...
```

By where it is mounted (`unmount /volumes/STICK`) or by its device
(`unmount usbdisk0p2`). Refused while a program has a file open there or
its current directory there (`cd` elsewhere first). Afterwards the stick
can be pulled out safely. Root only.

### osinstall: install RitterOS on an internal disk

The page with every option and message: [`osinstall`](commands/osinstall.md) (on RitterOS: `man osinstall`).

```
osinstall
```

Installs RitterOS from the disk it is running from (the stick) on another
disk, such as a PC's SATA SSD or NVMe drive, using the whole disk. It asks
its questions one by one:

```
ritteros:/$ osinstall
osinstall: installs RitterOS (x86_64) from usbdisk0 on another disk, the whole disk.
Disks:
  1  sata0         223.5 GiB  SATA  (the disk's model): GPT, 2 partitions
Which disk (its number or name; Enter to stop)? 1
sata0 (223.5 GiB) gets a new partition table:
  sata0p1  64.0 MiB, EFI system partition, FAT32 'EFI' (the bootloader)
  sata0p2  about 223.4 GiB, RitterOS, RitterFS 'RitterOS' (the system)
Everything on sata0 is lost (now: GPT, 2 partitions).
Type the disk's name (sata0) to install: sata0
osinstall: sata0: a new GPT partition table
osinstall: sata0p1: FAT32 'EFI', 64.0 MiB
osinstall: sata0p2: RitterFS 'RitterOS', 223.4 GiB
osinstall: the bootloader copied (\EFI\BOOT\BOOTX64.EFI, 37888 bytes), command line ""
osinstall: /system/x86_64 copied: the kernel and 49 programs
osinstall: the firmware's boot entry Boot0003 'RitterOS', first in its boot order
RitterOS is installed on sata0. Remove the stick and restart (reboot); the firmware starts it from sata0, and its first start asks for root's password.
```

It offers every disk except the one the system runs from. Enter at the
first question stops it; nothing is written until you type the disk's
name. Volumes of that disk that are mounted are listed and unmounted
first. The new disk gets:

- an EFI system partition of 64 MiB (FAT32, label `EFI`) with the
  bootloader for this machine (`BOOTX64.EFI` or `BOOTAA64.EFI`) at the
  path firmware starts by itself (`\EFI\BOOT\`), so no boot entry is
  needed, and the kernel command line (`\ritteros\cmdline.txt`; of the
  stick's options only `noruntime` and `video` go along);
- a RitterFS `RitterOS` on the rest, with `/system/<arch>` (the kernel
  and the programs, for this machine's architecture only, without the
  test programs) and `/system/config/README`.

Nothing else is copied: no users, passwords, settings, homes or logs. The
installed system starts like a new stick: it asks for root's password
first. Both partitions get new random identifiers (GUIDs), so the
system can tell the new disk from the stick when both are plugged in.

Last, `osinstall` adds an entry `RitterOS` to the firmware's boot menu
(a variable in the firmware's memory, which keeps it across restarts)
and puts it first in the boot order: some firmware starts an internal
disk only through such an entry (the J1900's did). Installing again
reuses the entry. If the firmware cannot keep variables, `osinstall`
says `no firmware boot entry: ...` and the disk may still start through
the fallback path. To start the stick while the entry comes first,
choose the stick in the firmware's boot menu. Root only; the disk needs
at least 256 MiB.

---

## 8. The network

RitterOS finds Intel 8254x network cards (QEMU's `e1000`; `make run`
gives the virtual machine one, on QEMU's own network with the Mac's
internet behind it), Intel 82576 cards (`igb`, also two-port cards:
each port is a card of its own) and Realtek RTL8111/8168 and RTL8125
chips (`rtl8168`; many boards' own port, also sold as Killer E2600 and
E3000; the
boot line names the chip's version, as `Realtek RTL8168H, version
0x541`). It also finds USB network adapters of the CDC-NCM class (the
class modern USB Ethernet and USB-C dongles use; not the older ECM or
RNDIS): such an adapter, plugged in at the boot or later, becomes a card
like the others and reports where it is. A device with several functions
gets each one RitterOS knows: a PlutoSDR set to NCM (Tezuka's
`usb_ethernet_mode` = `ncm`) is a network card (its DHCP server gives
192.168.2.10, the Pluto is 192.168.2.1) and a disk (`/volumes/TEZUKA`)
at once:

```
USB NCM Ethernet on usb0 as eth0, MAC 52:54:00:12:34:59
```

Since 38a there are also virtual cards, `tap0`, `tap1`, ..., made with
`net tap add`: cards whose wire is a program (see "Virtual cards" under
`net` below); since 38b bridges, `br0`, ..., that join cards into one
network (see "Bridges" there).

Since 64m `net` shows two more lines under such a card: what the driver
did (NTBs read, the frames in them and those skipped (too short, too long
or past the NTB's end), the NTBs that came after another in the same
transfer, failed reads, reads that began without an NTB's header; frames
sent, the zero-length packets that ended a transfer of whole packets,
failed sends, frames dropped with a full send queue) and each of its two
USB endpoints (transfers, errors with the last one's USB completion code,
transfers out of time and cancelled, late events of earlier transfers the
controller sent and RitterOS ignored, resets after an error):

```
       usbnet: read 3647 NTB(s) (18462 frames, 0 skipped; 0 after another in one read), 0 read(s) failed, 0 without a header; sent 19563 (0 zero-length end(s)), 0 failed, 0 dropped (queue full), 0 waiting
       usbnet: NTBs read up to 16384 bytes (the adapter offers 16384)
       usbnet: bulk IN 0x81: 3647 transfer(s), 0 error(s) (last code 0), 0 out of time, 0 cancelled, 71 earlier event(s) ignored, 0 reset(s), 0 failed command(s)
```

A read that does not begin with an NTB's header holds the rest of an
NTB whose start went elsewhere; its frames are lost (TCP sends them
again). Since 64n the kernel logs the first five such reads with their
length, the length of the read before and their first 16 bytes (`eth1: a
read of N bytes (after one of M) without an NTB header: ...`), and since
64s a second line with what the read before's NTB header said and how the
USB controller ended both reads (`eth1: the read before: NTB of N bytes,
NDP at N; ended by TRB ...`). The laptop's reads like that (one in about
700 from the PlutoSDR) came from its USB controller and are gone since
64s; the count should stay 0.

The first ten failed transfers of a card also get a line in the kernel's
log (`eth1: a read failed (USB completion code 4)`).

Each card is `eth0`, `eth1`, ... in the
order found. The boot lists them at once, with their links as they are
at that moment:

```
  eth0:          Intel 82576 at 01:00.0 (igb), MAC 00:1b:21:aa:bb:cc, no link
  eth1:          Intel 82576 at 01:00.1 (igb), MAC 00:1b:21:aa:bb:cd, no link
```

A real cable needs a few seconds to agree on a speed, so a link usually
comes after the boot's lines. Whenever a link comes, goes or changes its
speed (also a cable plugged in or pulled later), the system says so:

```
eth0: link up, 1000 Mbit/s
eth0: link down
```

### net: the network cards

The page with every option and message: [`net`](commands/net.md) (on RitterOS: `man net`).

```
net
net up
net names
net forget NAME
net forward [on|off]
net masquerade [ethN ... | off]
net default [CARD... | auto]
net tap add [user NAME]
net tap remove TAP
net bridge add [BRIDGE] PORT...
net bridge remove BRIDGE [PORT...]
```

`net` shows the loopback `lo` first, then each card: its MAC address,
the link and its speed, the card and where it sits, its address, gateway
and DNS server, its IPv6 addresses, what it received and sent; then the
routes, whether packets are forwarded between cards, the neighbours whose
MAC addresses are known (`arp:` IPv4, `ndp:` IPv6) and TCP:

```
lo     the loopback, 127.0.0.1/8
       received 12 packets (1008 bytes), sent 12 packets (1008 bytes), 0 not sent
       IPv6 ::1/128
eth0   52:54:00:12:34:56  link up, 1000 Mbit/s, Intel 82540EM (e1000 at 00:05.0)
       address 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3
       IPv6 fe80::5054:ff:fe12:3456/64 (link-local)
       received 5 frames (456 bytes), sent 8 frames (560 bytes), 0 not sent
route: 127.0.0.0/8 on lo
route: 10.0.2.0/24 on eth0
route: default via 10.0.2.2 on eth0
forwarding: off; 0 packets forwarded, not: 0 without a route, 0 out of time (TTL), 0 without an ARP answer
arp: 10.0.2.2        is 52:55:0a:00:02:02
ndp: fe80::ff:fe00:7102%eth0 is 02:00:00:00:71:02
tcp: 0 connection(s); segments 2120 sent, 1853 received; sent again: 0 after a timeout, 0 fast; 0 kept after a gap, 0 window probes; 0 with scaled windows, 0 with SACK, receive buffers doubled 0 times; 0 holes resent from SACK, send buffers doubled 0 times, 0 ACKs delayed, 0 reset with data unread
udp: 6 of 128 port(s) bound; 0 datagram(s) dropped by a full queue
```

`lo` is the machine itself: everything sent to 127.0.0.1 (any 127.x
address, `localhost`) or to one of the cards' own addresses goes through
it and never leaves the machine. It is always there, also without a
network card (`net` then says `no network cards` after it), so programs
can talk to each other on one machine: `ping localhost`, `ssh localhost`.
"Not sent" counts packets dropped because 128 were already waiting.

**IPv6** (step 64a; roadmap item 29, in steps 64a-64g). IPv6 addresses
are 128 bits, written as eight groups of four hex digits with the longest
run of zero groups shortened to `::` (`fe80:0000:0000:0000:5054:00ff:fe12:3456`
is `fe80::5054:ff:fe12:3456`). Every card gets a **link-local** address
when its link comes up: `fe80::` and the card's 48-bit MAC address made
into 64 bits (its 7th bit flipped, `ff:fe` in the middle: `52:54:00:12:34:56`
gives `fe80::5054:ff:fe12:3456`). It is valid only on that card's own
network, so where an address leaves the card open, the card is written
after a `%`: `ping fe80::ff:fe00:7102%eth0`. Before using it, the card asks the
network whether another machine has it (duplicate address detection:
after a random wait of up to a second, a question from the empty address
`::`, then a second for an answer); `net` shows `being checked for
duplicates` meanwhile, and `used by another machine: not used` if one
answered. `lo` has `::1`, as 127.0.0.1.

IPv6 has no broadcast and no ARP. Instead a machine asks for a neighbour's
MAC address with a *neighbour solicitation* sent to a multicast group made
from the address's last 24 bits (`ff02::1:ff12:3456`), which only the
machines whose addresses end that way are in, and the owner answers with
an *advertisement*; RitterOS keeps the answers for five minutes (`ndp:`
lines in `net`). Such messages must arrive with a hop limit of 255, which
proves nobody forwarded them from another network. Switches that watch
multicast traffic learn which groups a card wants from its MLD reports,
which RitterOS sends before each check and when the network's router asks.
The IPv6 layer itself knows nothing about the cards: it sends and
receives Ethernet frames through every card alike. What a card must do
for it is take the multicast frames of its groups, and how is the card's
own, so the system tells each card's driver which groups it wants (their
MAC addresses, `33:33:` and the group's last 32 bits) whenever they change,
and the driver sets its chip's filter: the e1000 and 82576 their table of
4096 bits, the Realtek chips their 64-bit hash (all ones on the RTL8168F,
which Linux says must take every multicast frame). A card whose driver
has no filter to set is taken to pass every multicast frame, as USB
adapters (their packet filter takes all) and Wi-Fi cards (the access point
sends group frames to every station) do. A filter may let more through
than was asked; the system keeps only its own groups and `ff02::1`.

**Addresses from the router** (step 64b, SLAAC, RFC 4862). With its
link-local address in use, a card asks the routers on its link (a
*router solicitation* to `ff02::2`, at most three times, four seconds
apart). A router answers with an *advertisement* to every machine: the
prefixes on the link, the ones to make addresses in, itself as the
default router, often a DNS server and the link's MTU, each for a
lifetime. The card makes its address from such a prefix and the same 64
bits of its own as in its link-local address, checks it for duplicates
and keeps it as long as the router says (`valid`; after `preferred` it is
kept for conversations already going, not chosen for new ones). An
advertisement that would end an address within two hours is not taken at
its word (RFC 4862's rule against a forged one). `net` shows each part
and the routes (`route6:`); QEMU's network advertises `fec0::/64` with a
DNS server at `fec0::3`. A card with fixed settings (`address6 =
ADDRESS/N`) takes no advertisement.

**DHCPv6** (64g). Some networks give IPv6 addresses by DHCPv6 instead
of (or beside) the router's prefixes: the router's advertisement says
"managed" (addresses from DHCPv6) or "other" (only the DNS server). The
elf `dhclient` follows it for a card on `address6 = auto`, and always asks
for one set to `address6 = dhcp` (`man dhclient`). Until 64e the firewall sees no IPv6 packet:
nothing but ping and neighbour discovery is understood before then.

For the Intel 82576 and the Realtek chips, `net` adds what the chip
itself counted, to tell a frame the chip never got from one the system
never saw:

```
       igb: chip received 710 (good 710, missed 0, no buffer 0, CRC errors 0, queue dropped 0), sent 727
       igb: receive ring head 6 tail 5, send ring head 23 tail 23; STATUS 0x80080483, ...
       igb: interrupts: MSI-X, 676, frames found by the tick instead 2
       igb: IMS 0x00000000, IVAR0 0x00000080, EITR0 0x00000000, GPIE 0x80000011, EIMS 0x00000001; MSI control ...
       rtl8168: chip sent 719 (errors 0, aborted 0, underruns 0), received 703 (errors 0, missed 0, to us 701, broadcasts 2)
       rtl8168: interrupts: MSI, 676, frames found by the tick instead 0
       rtl8168: reset after a send timeout: 0 time(s), 0 frame(s) lost then
       rtl8168: ChipCmd 0x0c, PHYstatus 0x0013, MISC 0x00000000, ...; 0 to send
       rtl8168: ERI send FIFO 0x00020006, receive FIFO 0x00080010; DLLPR 0x00, ..., none waiting 0x00000000
```

Since 38d the Realtek's lines include its PHY's registers: what it can do
(BMSR, and ESTATUS for 1000 Mbit/s), what it advertises to the other end
(the 10/100 modes, then 1000BASE-T) and what the other end offered. The
driver advertises every mode the PHY says it can: before, it kept what the
firmware had left, and the J1900's onboard port linked at 100 Mbit/s with
a gigabit partner (1000BASE-T advertised 0x0000). The Acer's, linked at
1000:

```
       rtl8168: PHY BMSR 0x79ad, ESTATUS 0x2000; advertised 0x01e1, 1000BASE-T 0x0200; partner 0xc5e1, 1000BASE-T 0x3800
```

Under every card (since 38d) a line says how often the system asked for
its link (several times for every packet it sends) and how many of those
reached the card's driver, which is often a read of the chip: a driver is
asked at most once every 10 ms, the answer kept meanwhile. On the J1900
those reads had cost half its sending speed (36.8 MB/s; 75.3 with them
kept). The Acer's eth0 a minute after its start:

```
       its link asked 1238 times by the stack, 761 of them read from the card
```

The cards take frames by interrupt (the Intel cards MSI-X, else MSI;
the Realtek chips MSI): `interrupts` counts them, and
"found by the tick instead" counts frames the timer tick (every 10 ms, a
safety net) saw first; a growing number there means interrupts are lost.
Without MSI the tick does the work, and round trips (`ping`) come in
steps of 10 ms. "Missed" and "no buffer" are frames the chip had no room
for, "queue dropped" those its receive queue threw away; the `rings` line
also shows the receive ring's size, its SRRCTL and the status of the next
descriptor (0 until the chip has written one); the ring
heads move as the chip uses descriptors; "to send" counts frames handed
to the chip and not sent yet. The RTL8168G and later (not the RTL8125)
get an `ERI` line: the sizes of the chip's send and receive FIFOs as the
driver set them (a chip with 0 there sends nothing), some of its power
settings, the send ring's address and the first frame still waiting in
it (its descriptor's first word: bit 31 set means the chip has not taken
it yet). A Realtek chip that holds frames for 5 seconds with its link up
and sends none of them meanwhile is reset and started again (since 64p,
as Linux does after such a send timeout; the frames it held are lost, and
TCP sends them again): the kernel says `eth0: the chip sent nothing for 5
s with N frame(s) waiting: reset`, and `net` counts the resets. Every card's last line, `pci:`, shows the
command and status registers of the bridges above it and of the card (a
card can reach memory only when all of them have bit 2, bus master, set;
status bits 12 and 13 record a refused access).

A wireless card adds what the wireless layer counted, and the MediaTek
MT7921 what its chip reported:

```
       Wi-Fi: joined 02:11:22:33:44:c9 on channel 6, protected (CCMP, in the card)
       Wi-Fi frames: data 1766 in, 1607 out; management 446 in, 3 out; EAPOL 2 in; refused 0 (MIC), 0 (replay), 98 (queue full)
       Wi-Fi: protected frames opened by the card 1535, by the layer 231; 1209 management frames of other senders dropped while joined
       mt7921: 7247 frames sent by the chip, 0 not acknowledged, 0 waiting
       mt7921: send to done: average 8785 us, longest 82420 us; at most 109 in flight of 256; 0 waited for one, 0 in vain
       mt7921: interrupts: MSI, 3270, found by the tick instead 571
```

"In flight" are frames handed to the chip that it has not reported sent
yet; it may hold 256. When it holds all of them, a program's frame waits up
to 20 ms for one ("waited"); one still without a place is not sent ("in
vain", and the card's "not sent" count), as a full card's would be: TCP
sends it again.

"Data" frames carry the network's packets, "management" frames are the
radio's own (beacons, joining), "EAPOL" the WPA handshake's. "Refused"
counts frames thrown away: a wrong check value (MIC: damaged or not ours),
an old packet number (a replay), or a full queue (frames for `wifid` it did
not read, mostly beacons from the scan before joining). While joined, only
the access point's own management frames are kept: the card filters the
other networks' beacons itself (and, the MT7921, other networks' frames),
and management frames of other senders that it still passes -- other
devices' broadcast action frames, about 20 a second on a network with Apple
devices -- are dropped as they arrive and counted on their own line.
"In the card" means the card encrypts and decrypts with the keys itself
(the MT7921 does): the system hands it frames in plain text and gets them
back opened. "Opened by the card" and "by the layer" count the protected
frames each one decrypted; on the MT7921 the card opens everything sent to
the machine, and the layer still opens the access point's broadcasts (the
chip holds their key but does not look it up for them). The
link's speed in the card's line is the most it may go: 54 Mbit/s for
802.11g, 72 or 144 for 802.11n (one or two streams at 20 MHz) when the
access point does 802.11n. The chip's line counts
the frames it reported sent, those the access point did not acknowledge,
and those still waiting for its report; how long a frame took from being
handed to the chip until that report, and how many it held at once (at
most 64); and its interrupts, as for the Ethernet cards ("found by the
tick instead" counts frames the 10 ms tick found while the receiving
thread slept: missed interrupts).

The `tcp:` line counts TCP's connections (with their states, like `1
established, 2 time wait`: a connection closed first waits 2 seconds
for late segments) and segments: how many were sent again because
no acknowledgement came in time ("after a timeout") or because the other
side's duplicate acknowledgements showed one missing ("fast"), how many
that came after a missing one were kept until it arrived, and how often a
closed window (the other side's buffer full) was asked about.

The `udp:` line (since 64j) counts the UDP ports bound (every program's
UDP socket has one, and so has each name lookup while it waits; 128 at
most) and the datagrams dropped because a socket's queue was full: a
socket keeps 128 that its program has not read yet, and drops what comes
after them, as UDP allows. A number above 0 there means a program read
too slowly for what came (dnsd in a burst of lookups lost them before
64j, when a queue kept 16).

`net up` applies the settings in the folder `/system/config/network/` to
the cards (root only): one file per card, named after it (`eth0`,
`eth1`, ...). `login` does it by itself at every start when there is a
network card (not for `lo`, which needs no settings). A card found
without a file gets one that asks the network for its settings (DHCP).
Asking is the elf `dhclient`'s work (section 9): `net up` makes its file
when a card uses DHCP and sees that the elves run; the address comes a
moment later, with `dhclient`'s line:

```
ritteros:/$ net up
net: eth0: /system/config/network/eth0 made (address = dhcp)
net: eth0: DHCP: the elf dhclient asks for its address
net: /system/config/elves/dhclient made: the elf dhclient asks for the addresses
net: /system/config/elves/dnsd made: the elf dnsd answers their questions
net: /system/config/elves/sshd made: the elf sshd lets others log in (port 22)
elvesd: dhclient started (process 4)
elvesd: dnsd started (process 5)
elvesd: sshd started (process 6)
sshd: made a new host key SHA256:Xl3C...
sshd: listening on port 22
dhclient: eth0: 10.0.2.15/24 from 10.0.2.2, gateway 10.0.2.2, DNS 10.0.2.3, lease 86400 s
```

With a card, `net up` also makes the files of the elves `dnsd` (names,
below) and `sshd` (logging in from other machines, section "sshd"), so
both run from the first start; `elf stop sshd` switches the server off
(for the next boots too). A file removed with `elf remove` is made
again by the next `net up`.

The address is lent for the lease's time; `dhclient` asks again at half of
it. When no server answers (`dhclient: eth0: no answer from a DHCP server
yet; asking again every 10 s`) or the card has no link (`dhclient: eth0: no
link: no address`), it keeps asking: every 10 seconds, and at once when
the link comes up. A lease that runs out without a server renewing it
takes the address away (`the lease ended`). (Since 38d) a card that goes
away (a tap or bridge removed) loses its lease, and so does one whose
address was taken from it (it became a bridge's port): when it is back,
`dhclient` asks for it as for a new card. A `dns = ...` in the card's
file wins over the network's. Fixed settings in the card's file
instead:

```
address = 10.0.2.15/24
gateway = 10.0.2.2
dns = 10.0.2.3
```

`address` is the card's address and, after the `/`, how many leading bits
name the network (24: 10.0.2.x). Packets for other networks go to the
`gateway`. `dns` is the server that answers names (with `address = dhcp`
it replaces the network's; `ADDRESS:PORT` asks on another port than 53).
On QEMU's network these are the values DHCP gives. Change the file with
`edit`, then `net up`.

IPv6 has its own keys (64b). Without them a card takes its IPv6
addresses from the network's router, as most networks expect; fixed ones
instead:

```
address6 = 2001:db8:72::1/64
gateway6 = 2001:db8:72::2
dns6 = 2001:db8:72::53
```

`address6 = off` switches IPv6 off on the card, `address6 = auto` is the
default said out loud. `dns6` may also go with `auto`: it replaces the
router's DNS server (`[ADDRESS]:PORT` for another port).

A disk from before step 42d has all of it in one file,
`/system/config/network` (`eth0`'s keys as they are, the other cards' as
`eth1.address` and so on, and `forward`): `net up` moves it into the
folder and `/system/config/routing` and says so.

**Routes.** Each card with an address reaches its own network directly
(`route: 192.168.1.0/24 on eth1`); everything else goes to the gateway of
the first card that has one (`route: default via ...`); 127.x is `lo`.
The most specific route wins (the longest prefix). Programs' packets
leave from the address of the card their route takes. A card without a
link (its cable out, a wireless card not joined) is passed over: its
routes stay listed but carry nothing, and the default route is the first
card's with a gateway and a link. So a laptop with Ethernet and Wi-Fi on
the same network uses the cable while it is in and Wi-Fi when it is out.

**The default route's card** (since 50g) can be chosen when more than one
card has a gateway: `default = wlan0 eth1` in `/system/config/routing`
lists the cards in order of preference, and the first of them with a
link, an address and a gateway carries the default route. When none of
them can (cables out, no address yet), the first other card with a
gateway does, as without the line, so the machine still reaches other
networks. The order follows the links by itself: pull the first card's
cable and the next one takes over, put it back and the first is used
again. `net up` applies the line (without it, there is no preference:
`auto`); `net default eth1 wlan0` and `net default auto` change it at once
(root only), for the time until the next `net up` or start: keep it in
the file. `net default` shows the order and the route now; a listed card
may be one that is not plugged in (a USB adapter used now and then):

```
ritteros:/$ net default
net: the default route: eth1, then any card with a gateway; now via 10.0.3.2 on eth1
ritteros:/$ net default eth0
net: the default route: eth0, then any card with a gateway
ritteros:/$ net default auto
net: the default route: the first card with a gateway (auto)
```

With a preference, `net` adds a line after the routes: `default route:
eth1, then any card with a gateway`. A name that is not a card's (ethN,
wlanN, and since 38b-3 the virtual cards' tapN and bridges' brN) is
refused: `net: bogus: not a card's name (ethN, wlanN, tapN or brN)`.

**Forwarding** makes the machine a router between its networks: a packet
that arrives on one card for an address that is not the machine's own is
sent on to the card its route names, its time to live (TTL) one less. A
packet whose TTL runs out gets an ICMP "time exceeded" back (as
`traceroute` uses it), one without a route "network unreachable". It is
off at the start; `forward = on` in `/system/config/routing` switches it
on at `net up` (without that line, `net up` switches it off), `net
forward on` and `net forward off` switch it at once (root only), `net
forward` shows it. With `/system/config/network/eth0`:

```
address = 192.168.1.1/24
```

`/system/config/network/eth1`:

```
address = 10.0.2.15/24
gateway = 10.0.2.2
```

and `/system/config/routing`:

```
forward = on
```

```
ritteros:/$ net up
net: eth0: 192.168.1.1/24, gateway none, DNS none
net: eth1: 10.0.2.15/24, gateway 10.0.2.2, DNS none
net: forwarding between cards: on
```

The `forwarding:` line counts the packets passed on and those that were
not: without a route, out of time, or for a neighbour that never answered
ARP (a packet waits up to 1.5 seconds for that answer; at most 8 wait).
Packets for 127.x never come in from a network, and broadcasts are not
passed on. A forwarded packet is also checked against the firewall's
`forward` rules (see "The firewall"); with the `forward` default `drop`,
nothing is forwarded until a rule allows it.

**Masquerade (source NAT)** lets a private network reach the internet
through the machine's public address, IPv4 only. The setting marks the
*private* cards (there may be more than one; the public cards are not
listed), as `masquerade = eth0` in `/system/config/routing` (space-separated
for several), applied at `net up`; `net masquerade eth0` and `net
masquerade off` set it at once (root only), `net masquerade` shows it. A
packet forwarded from a private card out a card that is not private has its
source rewritten to that outgoing card's address (and its source port, or
an ICMP echo's identifier, to a fresh one), so several inside hosts share
the one address; the reply is rewritten back and forwarded in. So with two
public uplinks and one private network, only the private one is marked, and
traffic to either uplink is masqueraded while the two public networks still
reach each other unchanged. `net` shows the private cards and how many
packets were rewritten each way. Without masquerade, the machines on both
sides must know their own way back through this one.

**The cards' names** (since 50f) stay with the cards. RitterOS keeps a list
of every card it has found, by MAC address, in
`/system/config/network/names`, and names each card from it at the start
and whenever a card is plugged in, whatever order the cards are found in:
so a card's settings file (`/system/config/network/eth0`) always applies to
the same card. A card not on the list gets the lowest name of its kind
(`ethN`, `wlanN` for Wi-Fi) that no listed card holds, also one that is not
plugged in now, and the elf `netnamesd` adds it to the list within a second,
under a comment saying what it is. A card new to the machine also gets its
settings file there and then (since 50h: `address = dhcp`, the file `net
up` would make; one that is there is never changed), and the elf
`dhclient` is set up if it was not: so a card plugged in for the first
time gets its address by itself, without `net up`. On a machine where
`net up` never ran (no file for `guardd`; since 36d-3) it also sets up
`dnsd`, `sshd` and the firewall's `guardd`, as `net up` would: `login`
runs `net up` only when a card is there at its start, and a card that
comes later (a Wi-Fi card whose firmware takes seconds to start) would
leave the firewall without rules, letting nothing in. `netnamesd` runs on
every machine, also one without a network card: `login` sets it up when
the system starts, before anyone logs in (`login:
/system/config/elves/netnamesd made: the elf netnamesd records new network
cards`), as it does `logd`.

```
# eth0: PlutoSDR (ADALM-PLUTO) (usbnet at usb0 port 1), found 2026-10-26
eth0 = 02:aa:bb:cc:00:02
# eth1: Realtek RTL8168H, version 0x541 (rtl8168 at 03:00.0), found 2026-10-26
eth1 = 02:aa:bb:cc:00:01
```

The list is a settings file: change a name there (with `edit`, as root) to
rename a card; that counts from the next start (a card plugged in later is
named as the file says then). Rename its settings file in
`/system/config/network/` too: the card takes the settings of its new name,
and a card that gets the old name later would take the old ones. At the start the kernel's own lines name the
cards in the order found, and a line says which one got another name:

```
  net:           eth0 (02:aa:bb:cc:00:01) is eth1 now (/system/config/network/names)
```

`net names` shows the list: each card's name, MAC address, whether it is
here, and what it is. `net forget NAME` (root) takes a card that is not
plugged in off the list and removes its settings file: its name is free for
the next new card. A card that is plugged in is refused (`net: eth1 is
plugged in (...): unplug it first`); one unplugged since the start shows as
`unplugged` in `net` and may be forgotten.

```
ritteros:/$ net names
eth0   02:aa:bb:cc:00:02  here     PlutoSDR (ADALM-PLUTO) (usbnet at usb0 port 1), found 2026-10-26
eth1   02:aa:bb:cc:00:01  here     Realtek RTL8168H, version 0x541 (rtl8168 at 03:00.0), found 2026-10-26
eth2   52:54:00:12:34:59  not here QEMU USB NCM Network Device (usbnet at usb0 port 6), found 2026-10-20
ritteros:/$ net forget eth2
net: eth2 (52:54:00:12:34:59) forgotten: the name is free; removed /system/config/network/eth2
```

**Virtual cards** (since 38a) are cards whose wire is a program: a VPN
that carries the frames somewhere, a simulated network, a test. `net tap
add` (root) makes `tap0` (the lowest free number, up to `tap63`); a
program opens `/dev/tap0` and reads one Ethernet frame per `read` (each
frame the system sent through the card) and writes one per `write` (a
frame the card received). For the system it is a card like any other: an
address from `/system/config/network/tap0` at `net up`, routes, the
firewall. Its link is up while the program holds it, down when it closes
it. One program at a time; only root and the card's user (`net tap add
user eloy`) may open it. `net tap remove tap0` takes it away (not while
it is open). As any card's, its settings file is made by `net up` when it
has none (`address = dhcp`; since 38b-3); its MAC address is new each time
it is made. Taps and bridges are named wherever a card is: in the
firewall's rules (`allow in tcp 22 on tap0`) and `net default`, as in
`net masquerade`. Taps and bridges are kept in
`/system/config/network/virtual` (since 38c: `tap0 = user root`, `br0 =
eth0 tap0`): the commands change their own lines, and `net up` makes
them again at every start, before the cards' settings.

```
ritteros:/$ net tap add
tap0: a virtual card for user 0, MAC 2a:19:9f:10:58:69 (/dev/tap0)
net: tap0: a virtual card; /dev/tap0 is root's
```

**Bridges** (since 38b) join cards into one Ethernet network, as a switch
does: `net bridge add eth0 tap0` (root) makes `br0` with those two cards
as its ports. A frame that comes in on a port goes out the port its
destination was last heard on, or out all the others while that is not
known (and always for broadcasts); addresses unheard for 300 seconds are
forgotten. The bridge is the machine's card on that network: its address
goes on `br0` (`/system/config/network/br0`, made with `address = dhcp`
by `net up`), and `br0` has the first port's MAC address, so DHCP gives
it the address that card had. While a card is a port it has no address
of its own (IPv4 or IPv6) and takes every frame on its wire. Wired cards
and taps can be ports, up to 16 a bridge, up to 8 bridges; a Wi-Fi card
cannot (as a client of an access point it may only send frames from its
own MAC address). `net bridge add br0 eth1` adds a port, `net bridge
remove br0 eth1` takes one out and `net bridge remove br0` the whole
bridge; bridges are made again at every start. `net` shows a bridge's ports,
how many addresses it knows and what it passed on, and under each port
`a port of br0`. A port keeps its own settings file, unused while it is a
port: `net up` says `net: eth0: a port of br0: its own settings are not
used`.

There is no spanning tree: never join two bridges (or a bridge and a
switch) by two cables, or broadcasts go round for ever. Frames passed
between ports are not seen by the firewall, which checks only what
reaches `br0` itself.

```
br0    52:54:00:12:34:56  link up, 1000 Mbit/s, bridge (bridge at its ports)
       address 10.0.2.15/24 (DHCP, lease 86400 s), gateway 10.0.2.2, DNS 10.0.2.3
       received 9 frames (1756 bytes), sent 11 frames (1922 bytes), 0 not sent
       IPv6 fe80::5054:ff:fe12:3456/64 (link-local)
       bridge: ports eth0 tap0 tap1; 5 address(es) known; 14 frame(s) passed on, 8 sent to every port, 0 for their own port, 0 come back (a loop)
eth0   52:54:00:12:34:56  link up, 1000 Mbit/s, Intel 82540EM (e1000 at 00:05.0)
       a port of br0: its frames are the bridge's
       received 14 frames (3272 bytes), sent 26 frames (3994 bytes), 0 not sent
```

### ping: is it there?

The page with every option and message: [`ping`](commands/ping.md) (on RitterOS: `man ping`).

```
ping [-c COUNT] HOST
```

Sends an echo request once a second (4 times, or COUNT) and shows how long
each answer took; Ctrl-C stops early:

```
ritteros:/$ ping 10.0.2.2
PING 10.0.2.2: 56 data bytes
64 bytes from 10.0.2.2: seq=1 time=10.54 ms
...
--- 10.0.2.2: 4 sent, 4 received, 0% lost; round trip min/avg/max 9.05/11.62/14.98 ms
```

HOST is an address or a name (`ping example.com`), which the DNS server
turns into an address (`PING example.com (93.184.215.14): ...`). Programs
ask the elf `dnsd` (section 9), which asks the DNS server of the first
card that has one (any kind of card, in the order `net` lists them, a
card with its link up before one without, so an unplugged cable does not
stop names over Wi-Fi) and keeps the answers for their time to live (at most
an hour, 32 names; since 64k also "no such name" and "no record of that
type", for the zone's time, at most five minutes; a restart of `dnsd`
forgets them all); `localhost` is 127.0.0.1 without asking. Since 64q
`dnsd` asks the server many questions at once (up to 64), and a question
asked again while the same one is on its way waits for that answer. `dnsd` answers
other machines too: one that names this machine as its DNS server
(port 53, any of its addresses) gets the same answers and shares the
cache. What may reach it is a question for the firewall (to come). `Name not found`: the DNS server knows no such name; `Timed
out`: it did not answer (twice 2 seconds). `No route to host`: nobody
answered with that address in the local network; `Network is
unreachable`: no address set (`net up`), no link, no gateway for another
network, or no DNS server for a name. Our own address and 127.0.0.1
always answer.

### fetch: get a page or a file over HTTP or HTTPS

The page with every option and message: [`fetch`](commands/fetch.md) (on RitterOS: `man fetch`).

```
fetch [-i] [-o FILE] URL
```

Gets `http://HOST[:PORT]/PATH` or `https://HOST[:PORT]/PATH` and writes
what comes to standard output, or with `-o` to FILE (then it says how much
it saved). `-i` shows the answer's status line and headers first. HOST may
be a name or an address; redirections are followed (up to 5, each one is
reported, also from http:// to https:// and back):

```
ritteros:/$ fetch https://example.com/
<!doctype html>
...
ritteros:/$ fetch -o /page.html https://example.com/
fetch: 200 OK, 1256 bytes saved to /page.html
```

**HTTPS** (since 23e) is HTTP over TLS 1.3 (`libtls.so.1`): the
connection is encrypted, and the server must prove it is HOST. Its
certificate has to name HOST (a `*.` name covers one label), be within its
dates (the machine's clock must be right), and chain to one of the root
certificates in `/system/certs/cacert.pem` -- Mozilla's list of trusted
certificate authorities, as browsers use it (118 roots; one of them, with
a P-521 key, cannot be used here). Servers that speak only TLS 1.2 or
older are refused. Whether a page came whole is HTTP's to say (its
length or its last chunk), as with other HTTP clients.

Messages: `not an http:// or https:// address`; `404 Not Found` (or
another status the server answered with); `the connection ended early (N
of M bytes)`; `too many redirections`; `Name not found`, `Connection
refused`, `Timed out` from the network. From HTTPS, after the host: `the
certificate is not for this host name`; `a certificate is expired or not
yet valid`; `no trusted root certificate signed the chain`; `the server
does not speak TLS 1.3`; `the server refused the handshake` (it found
nothing in common with us); `a record failed its check (bad_record_mac)`
(the data was changed on the way).

### wifi: the wireless link

The page with every option and message: [`wifi`](commands/wifi.md) (on RitterOS: `man wifi`).

```
wifi scan
wifi join SSID [PASSPHRASE [wpa2|wpa3]]
wifi forget SSID
wifi country CC
wifi status
```

A wireless card is `wlan0` (then `wlan1`, ...), an ordinary network card
once it has joined a network: `net up`, DHCP, DNS, routing and the firewall
treat it like any other. What is special -- finding networks, the password,
the keys -- is the elf `wifid`'s (section 9), and `wifi` tells it what to do.

- `wifi scan` lists the networks in range, strongest first: each name, its
  security (`open`, `wpa2` or `wpa3`), channel and -- when the card reports
  it -- signal in dBm (-40 is very strong, -90 barely there). A network sent
  on several channels or by several access points is listed once, at its
  strongest. Cards that scan in their own firmware (the MediaTek MT7921, the
  Intel 8265) do the sweep themselves and only listen while doing it; a scan
  of all their channels takes about 5 seconds.

```
ritteros:/$ wifi scan
wifi: ExampleNet                       wpa2 channel 6   -39 dBm
wifi: Amazon                           wpa2 channel 149 -57 dBm
wifi: xfinitywifi                      open channel 157 -66 dBm
wifi: 3 network(s)
```
- `wifi join SSID` saves a network and sets `wifid` to join it. With no
  passphrase the network is open -- unless it is saved already, then its
  saved settings stay (`wifi: SSID is saved (wpa2); wifid joins it with
  its saved settings`); with one it is WPA2 unless you add `wpa3`.
  The network is remembered in `/system/config/wifi/networks/SSID` (root
  only), so `wifid` rejoins it after a reboot or when the link drops.
- `wifi forget SSID` removes a saved network.
- `wifi country CC` sets the country (a two-letter code, e.g. `US`), saved in
  `/system/config/wifi/radio`. The card's firmware uses it to choose which
  channels and transmit power are allowed where you are; without it the
  world-wide safe default is used. `wifid` applies the setting when it starts.
- `wifi status` shows whether the link is up, the access point and channel,
  the access point's signal (averaged over what it sent),
  and the address.

```
ritteros:/$ wifi join HomeNet s3cret wpa3
wifi: saved HomeNet (wpa3); wifid will join it
ritteros:/$ wifi status
wifi: joined 02:11:22:33:44:55 on channel 6, -48 dBm
wifi: address 192.168.1.40
```

`wifid` reports each step of joining on the console (and in its log):

```
wifid: ExampleNet found on channel 6 (2.4 GHz); joining
wifid: ExampleNet: authenticated
wifid: ExampleNet: associated (AID 2)
wifid: ExampleNet: handshake done
wifid: joined ExampleNet (02:11:22:33:44:c9) on channel 6, wpa2
```

The AID is the number the access point gave the machine. When a step
fails it says which: `authentication failed` (the access point did not
answer or refused), `association failed`, `the card did not take the
association` (the card's firmware refused the join), or `handshake failed`
after a line with the reason: `wifid: handshake: no message 3` is most often
a wrong passphrase (the access point silently drops an answer it cannot
check). `wifid` tries again every few seconds. A network saved without a
passphrase whose beacons say WPA2 or WPA3 is not joined; `wifid` says so
once:

```
wifid: ExampleNet needs a passphrase (wpa2): wifi join ExampleNet PASSPHRASE
```

Once joined, the MT7921's firmware watches the access point's beacons by
itself. When the link drops -- the access point sends the machine away
(deauthentication or disassociation, with the reason code it gave) or, on
the MT7921, its beacons stop -- the kernel says why, the address goes, and
`wifid` joins again; DHCP then gives the address back:

```
wlan0: Wi-Fi link lost: disassociated by the access point, reason 34
wifid: ExampleNet: link lost; joining again
wifid: ExampleNet found on channel 6 (2.4 GHz); joining
wifid: joined ExampleNet (02:11:22:33:44:c9) on channel 6, wpa2
```

The reason codes are the standard's: 3 the access point is leaving (or
restarting), 4 the station was idle too long, 34 frames to the station were
not acknowledged. With WPA3 only a protected deauthentication counts, so a
forged one cannot end the link; that holds for one sent to everyone too
(the access point shutting down): under WPA3 it must carry the access
point's BIP signature (the IGTK, from the handshake), and one without it is
dropped and counted in `net` ("failed BIP").

A hidden network (its beacons carry no name) is joined like any other:
`wifi join NAME ...` saves it, and when `wifid`'s scan does not see it, it
asks for it by name (probe requests; never on the radar channels 52-144,
where only listening is allowed), and a hidden access point answers that.
`wifi scan` does not list hidden networks: they have no name to show.

When the access point announces that it moves to another channel (a channel
switch, e.g. after it detected radar), the machine moves with it at the
announced time instead of losing the link:

```
wlan0: the access point moved to channel 11; following it
```

Roaming: while joined, `wifid` looks at the access point's signal every 10
seconds; below -75 dBm it scans for the same network, and an access point
of it at least 10 dB stronger (a second one in the house, the same name
and security) is joined instead:

```
wifid: ExampleNet: roaming to 02:11:22:33:44:d1 on channel 36 (-52 dBm; the old one -81 dBm)
```

If the card's firmware stops working (the MT7921: commands without an
answer, frames that never come back as sent, or five scans in a row that
end at once having heard nothing; the Intel 8265: an error it reports,
commands without an answer, frames stuck), the driver restarts the
chip -- the reset, the firmware, the setup of the boot -- the link is lost
meanwhile and `wifid` joins again; `net` counts the restarts:

```
  mt7921:        04:00.0: the firmware stopped working (commands without an answer); restarting the chip
wlan0: Wi-Fi link lost: the card's firmware stopped answering; restarting it
  mt7921:        04:00.0: the chip is running again
```

Access points renew the key for their broadcasts now and then (the group
key, every hour or so on many). `wifid` answers that short handshake by
itself and says so; a station that did not would be sent away:

```
wifid: group key renewed (index 2)
```

A renewal message that fails its check is ignored (`wifid: group key message
with a wrong MIC ignored`, `... without a readable key ignored`). Some
access points also renew the station's own key now and then, with the whole
key handshake again; `wifid` runs it while the link stays up:

```
wifid: pairwise key renewed
```

If that handshake fails, `wifid` keeps the keys it has and stays joined
(`wifid: the renewed key handshake failed; the keys stay`).

A network on both bands (often under one name) is joined on 5 GHz when it
is heard there at -75 dBm or better -- less crowded and faster -- else on
the band where it is strongest; `wifid` says which:

```
wifid: ExampleNet found on channel 40 (5 GHz); joining
```

`wifid` asks for 802.11n (HT: 20 MHz, two streams, the short guard
interval) when it joins, and at 5 GHz also for 40 MHz and 802.11ac (VHT:
up to 80 MHz, MCS 0-9); the channel width is the access point's (its
operation elements say which). `net` shows the most the link may go: 144
Mbit/s for 802.11n at 20 MHz, 300 at 40, 400 for 802.11ac at 40 MHz, 867 at
80.

The cards. Two Wi-Fi chips have drivers, each a module that loads only
where its chip is (section 9, `drivers`), both with their maker's firmware
in `/system/firmware/`:

- **MediaTek MT7921** (`mt7921`): its firmware chooses the rates and the
  number of streams itself, encrypts and decrypts (CCMP), and builds the
  bursts.
- **Intel Dual Band Wireless-AC 8265** (`intelwifi`, roadmap item 36):
  its firmware encrypts and decrypts and builds the bursts, but the rates
  are the driver's: it starts with one stream from the access point's
  signal, measures what gets through, and moves up or down, trying two
  streams now and then and keeping them when they carry more (as Linux's
  driver does for this chip). Its firmware chooses the country itself from
  the networks it hears (`country` is saved but not applied); (since 37f)
  `powersave = on` makes it doze as Linux's driver does by default (awake
  again for frames, dozing 100 ms after the last one each way).

Both drivers restart their chip's firmware when it stops working (below),
and both notice when the access point is gone (the firmware says its
beacons stopped; the Intel driver first asks the access point with an
empty frame, and only an unanswered one counts): the link goes down and
`wifid` joins again, about 10 seconds later. Each adds its own lines to
`net` under the card: the MT7921 its counters and airtime, the Intel 8265
`intelwifi:` lines with the firmware's version and country, the channel
and width, the frames sent by queue, the bursts and Block Acks, the rate
it sends at and how often each rate got through, and how often the
access point was lost or the firmware restarted:

```
       intelwifi: firmware 36.c8e8e144.0, country "US"; channel 40 at 80 MHz; associated, AID 13, rate table from the signal -53 dBm; power saving on (keep-alive 25 s)
       intelwifi: rate scaling: at VHT MCS 5 x 2 stream(s); 14 time(s) up, 14 down; two streams tried 1 time(s), kept 1; success (%, MCS 0 up): one stream 0 0 0 0 0 0 0 0 0 25; two 0 0 0 0 97 77 53 0 0 0
       intelwifi: the access point left 0 time(s), lost 0 time(s) (beacons missed: at most 0 in a row); asked 0 time(s), 0 answered, 0 false alarm(s)
       intelwifi: the firmware started again 0 time(s)
```

Measured against a machine on the cable (`netrate`, the tests' program):
the Intel 8265 at 80 MHz about 33 MB/s down and 27-29 MB/s up, the MT7921
at 40 MHz about 29 and 28 MB/s.

Power saving: with `powersave = on` in `/system/config/wifi/radio` (edit
it, next to `country`), the card dozes between beacons once joined and
wakes for the ones that say frames wait for it -- less power, but an idle
machine answers a ping only at the next wake (tens of milliseconds instead
of a few). Off is the default: always awake. `wifid` applies it at the
next join. Measured on the VivoBook (Intel 8265, on battery, idle, the
screen off): 8.92 W with it off, 8.54 W with it on (0.38 W less); pings
from the cable took 1-46 ms (3.6 on average) off, 14-123 ms on.

With 802.11n and WMM the machine and the access point also agree to send in
bursts (A-MPDU: many frames at once, acknowledged together), each way and
per traffic class (TID); the card sends and splits the bursts, and RitterOS
puts frames that arrive out of order back in sequence before it checks them
(a frame missing for 0.1 s is given up on). The kernel says when an
agreement is made:

```
wlan0: sending in bursts to the access point (TID 0, up to 64 frames)
wlan0: receiving in bursts from the access point (TID 0, up to 64 frames)
```

RitterOS supports WPA2-Personal and WPA3-Personal (SAE), with the frames
encrypted (CCMP) and -- for WPA3 -- the management frames protected too. It
is a station only: it joins networks, it is not an access point. The country
that sets the usable channels is `/system/config/wifi/radio` (`country = US`).

### sshd: log in from other machines

The page with every option and message: [`sshd`](commands/sshd.md) (on RitterOS: `man sshd`).

```
sshd [-p PORT]
```

The SSH server (root only), kept running as an elf (section 9): on a
machine with a network card, `net up` makes its elf's file at the first
start, so it runs from then on (`elf stop sshd` switches it off, `elf
start sshd` on again); without one, `elf add sshd sshd` makes the elf,
and `sshd &` starts it once, as a background job. It listens on port 22 (or PORT) and handles every connection
in a process of its own. The first start makes the machine's host key
(Ed25519, kept secret in `/system/config/ssh_host_ed25519_key`) and
shows its fingerprint:

```
ritteros:/$ sshd &
[7]
sshd: made a new host key SHA256:Xl3C...
sshd: listening on port 22
```

The first time a client connects it shows the same fingerprint and
asks whether to trust it: compare the two. Logging in works with a key
listed in `HOME/.ssh/authorized_keys` (Ed25519 keys only, one per line
as OpenSSH writes them: `ssh-ed25519 AAAA... comment`), or with the
user's password. Each login is reported (`sshd: root logged in from
10.0.2.2 (publickey)`).

A client that asks for a terminal (an interactive login, or `ssh -t`)
gets a pseudo-terminal running the user's shell, with echo and line
editing; when the client goes away, its programs end. A client that runs
a command without a terminal (`ssh HOST COMMAND`, as scripts and tools
do) instead gets raw pipes: standard input, output and error pass bytes
through unchanged, so binary and file transfers are clean. Either way the
connection's programs are a session of their own (41h-8: since then also
without a terminal), so what one connection sets for its session, such as
sudo's 5-minute memory, never reaches another. This is how another machine
moves files and programs to and from RitterOS over ssh, with no extra
tool:

```
ssh root@ritteros 'cat > /system/x86_64/bin/game' < game    # copy a program in
ssh root@ritteros 'cat /system/logs/kernel.log' > kernel.log  # copy a file out
ssh root@ritteros 'mem; uptime'                             # run commands, capture the output
```

The command runs under `sh -c`, so `;` and redirections (`<`, `>`, `>>`)
work in it, at any length; its exit status becomes the `ssh` client's. A
script goes in as `ssh root@ritteros sh < script` (section 2, "One line
only").

From the Mac with `make run` (QEMU passes the Mac's port 2222 to the
machine's port 22):

```
ssh -p 2222 root@localhost
```

The connection uses curve25519-sha256 for the key exchange, ssh-ed25519
host keys and chacha20-poly1305@openssh.com encryption, all written for
RitterOS from the RFCs (current OpenSSH has all three). Not there yet:
other algorithms, keys other than Ed25519, a new key exchange during a
long connection (OpenSSH asks for one after about a gigabyte), port
forwarding, file copies (`scp`, `sftp`).

### ssh: log in on another machine

The page with every option and message: [`ssh`](commands/ssh.md) (on RitterOS: `man ssh`).

```
ssh [-p PORT] [USER@]HOST [COMMAND]
```

Logs in on HOST as USER (else as yourself) and runs a shell there, or
COMMAND; the exit code is the remote command's. A host seen for the
first time is shown with its key's fingerprint; type `yes` to connect
and keep the key in `HOME/.ssh/known_hosts`. If a known host shows a
different key later, `ssh` refuses (`WARNING: the host key of HOST is
not the one in ...`): someone may be in between; remove the line if the
key was changed on purpose. With `HOME/.ssh/id_ed25519` (OpenSSH's
format, without a passphrase) `ssh` logs in with that key, else it asks
for the password. During a session every key goes to the other side;
`~.` at the start of a line ends the connection.

```
ritteros:/$ ssh root@10.0.2.2
ssh: 10.0.2.2 is not known yet. Its host key: SHA256:...
ssh: Connect, and keep the key in known_hosts? (yes/no) yes
root@10.0.2.2's password:
```

### cu: a terminal on a serial line (since 50e)

The page with every option and message: [`cu`](commands/cu.md) (on RitterOS: `man cu`).

```
cu DEVICE
cu -l DEVICE
```

Connects your terminal to a serial port: what you type goes to DEVICE
as it is (Ctrl-C too: it reaches the device, not `cu`), what the device
sends comes to your screen. DEVICE is a path or a name in `/dev`:
`cu usbserial0` is `cu /dev/usbserial0` (`-l` is Unix's form and does the
same). USB serial ports (CDC ACM) appear when they are plugged in, also
as one function of a device with several, as the PlutoSDR's console:

```
  usb0 port 1:   USB serial port on usb0 port 1 as usbserial0
ritteros:/$ cu usbserial0
Connected to /dev/usbserial0 (~. leaves).

Welcome to Tezuka
pluto login:
```

The line is 115200 baud, 8 bits, no parity, with DTR and RTS on (a
device's own USB port ignores the speed). Bytes the device sent before
`cu` started are shown first (the port keeps up to 16 KiB).

`~.` at the start of a line (after Enter) leaves (`Disconnected.`); `~~`
sends one `~`. Inside an `ssh` session the `ssh` you started from uses
`~.` itself: type `~~.` (it passes one `~` on); from a Mac or Linux
machine, OpenSSH's `ssh -e none` switches its escape off (RitterOS's own
`ssh` has no `-e`). Unplugging the device ends `cu` with `Disconnected: the
device is gone.` Messages: `cu: PATH: No such file or directory` (no
such port), `Permission denied` (only root and the console's user may use
the ports).

### Sockets: the network for programs

Programs talk over the network through sockets (`ritteros.h`), which
are file descriptors like files: `read` and `write` work on them, `close`
ends them.

A program that wants whole Ethernet frames instead (a VPN, a simulated
network) holds a virtual card's `/dev/tapN` (see `net`, "Virtual cards").

- `ritteros_socket(SOCKET_UDP)`: datagrams. `ritteros_bind(fd, PORT)`
  takes a port (0: any free one from 49152; the port is returned);
  `ritteros_sendto` sends one datagram to an address and port,
  `ritteros_recvfrom` waits for one (with a time limit) and says who sent
  it. After `ritteros_connect` the socket has a default destination for
  `write`. Up to 16 datagrams wait for the program; more are dropped, as
  UDP allows.
- `ritteros_socket(SOCKET_TCP)`: connections, a stream of bytes that
  arrives complete and in order (lost packets are sent again).
  `ritteros_connect(fd, ADDRESS, PORT)` connects; `ritteros_bind` and
  `ritteros_listen` wait for connections, `ritteros_accept` takes the next
  one as a new descriptor. `read` returns 0 at the end of the stream (the
  other side closed it).
- `ritteros_resolve(NAME, &address)`: a name's address (DNS).

Errors: `Connection refused` (nobody listens on that port), `Connection
reset by peer`, `Address already in use` (the port is taken), `Timed out`,
`Not connected`.

**IPv6** (step 64c). TCP and UDP carry IPv6 too, through the same
connections and ports. A socket made with `SOCKET_INET6` added to its kind
(`SOCKET_TCP | SOCKET_INET6`) is an IPv6 socket: it talks to IPv6
machines and to IPv4 ones, whose addresses it sees as `::ffff:a.b.c.d`;
a socket without it stays IPv4 only. The system calls with IPv6 addresses
(`SYS_CONNECT6`, `SYS_SENDTO6`, `SYS_RECVFROM6`, `SYS_ACCEPT6`,
`SYS_SOCKNAME6`) take a `struct sys_inaddr`: the family, the 16-byte
address, the port, and for a link-local address its card (`scope`, the
card's place in `net`'s list + 1). Most programs will reach them through
the BSD sockets' `AF_INET6` (step 64d). A datagram that came in pieces
(IPv6's fragments) is put back together, up to 9,000 bytes, unless its
pieces overlap; RitterOS itself sends none. When a router on the way says
a packet was too big ("packet too big"), the TCP connection sends smaller
segments from then on (`net`'s `tcp:` line counts them: `N smaller (too
big)`).

Through the BSD sockets (step 64d), programs use IPv6 as on other
systems: `socket(AF_INET6, ...)` with `struct sockaddr_in6`
(`netinet/in.h`; `in6addr_any`, `in6addr_loopback`, the `IN6_IS_ADDR_*`
tests), IPv4 peers of such a socket as `::ffff:a.b.c.d` unless
`IPV6_V6ONLY` is set before it is bound. `getaddrinfo` gives a name's
IPv6 addresses (AAAA records, asked of `dnsd`) as well as its IPv4 ones:
IPv6 first when the machine has an IPv6 address beyond the link (RFC
6724; the user's choice), else IPv4 first; on a network without IPv6 it
does not ask for AAAA at all (as macOS's AI_ADDRCONFIG). `fetch`, `ssh`,
iBrowser and HTTPS try each address in that order until one answers,
and take `[ADDRESS]` in URLs and `ssh USER@[ADDRESS]`. `sshd` and `dnsd`
listen on IPv6 and IPv4 at once. `ping` takes names the same way (the
user's: "have ping work as any other app"), an address in its own
family, and `-4`/`-6` to force one.

---

### The firewall: what may come in

Every packet that arrives for the machine on a network card, or leaves
it on one, is checked against the firewall's rules (`lo`, the machine
talking to itself, never is). IPv6's packets too (since 64c, with rules
of their own since 64e): one rules file holds both families' rules. A
rule that names no address (`allow in tcp 22`) counts for both, one with
an IPv4 address for IPv4 only, one with an IPv6 address (`drop in udp
53 from fe80::/10`) for IPv6 only; `icmp` covers ICMPv6, and IPv6's own
messages (neighbour discovery, router advertisements, MLD, its error
messages) always pass, as IPv6 does not work without them. A packet forwarded between two cards (when
routing is on) is checked too, against the `forward` rules. The rules are
in `/system/config/guard`, one per line, read top to bottom: the first
rule that matches a packet decides; if none does, the default of its
direction:

```
in = drop                    # what comes in when no rule matches (drop or allow)
out = allow                  # what goes out when none does
forward = drop               # what is forwarded between cards when none does
allow in icmp                # ping
allow in tcp 22              # sshd
allow in tcp 8000-8010 from 192.168.0.0/24 on eth0
drop in from 10.0.0.0/8
allow forward icmp from 192.168.1.0/24 in eth0 out eth1
```

| Part | Means |
|---|---|
| `allow`, `drop` | let the packet through, or drop it without an answer (the sender waits until it gives up) |
| `limit ... N/minute` | let at most N new connections a minute through, counted per other machine (a machine guessing passwords uses up its own allowance only); the rate is the last word: `limit in tcp 22 10/minute` |
| `in`, `out`, `forward` | arriving for the machine, leaving it, or passing between two cards |
| `tcp`, `udp`, `icmp` | only this protocol (without: all) |
| `PORT`, `PORT-PORT` | coming in, the machine's port; going out or forwarded, the other side's (with `tcp` or `udp`) |
| `from ADDRESS[/N]`, `to ADDRESS[/N]` | only from or to these addresses (`/24`: the first 24 bits must match) |
| `on CARD` | only on this card, `ethN`, `wlanN`, `tapN` or `brN` (`in` and `out` rules; Wi-Fi cards since 50k, virtual cards and bridges since 38b) |
| `in CARD`, `out CARD` | a `forward` rule: only entering on, or leaving on, this card |
| `#` | a comment, to the end of the line |

Forwarding is off until `net forward on` (or `forward = on` in
`/system/config/routing`), and with it on a forwarded packet still needs a
`forward` rule to pass — the `forward` default is `drop`. The answer
traffic of a forwarded connection passes without a second rule (the rules
decide only the first packet of each connection, in either direction).

The answers to the machine's own connections always come in (a web page
it fetched, a DNS answer, the reply to its ping, DHCP's offer), and so do
the further packets of a connection a rule let in: the rules decide only
about the first packet of each connection.

The elf `guardd` loads the rules into the kernel (`net up` makes its file
on a machine with a network card) and loads them again within 2 seconds
whenever the file changes. Its first start makes the file: what comes in
is dropped, what goes out allowed, and ping and the elves that are there
to be reached (`sshd`, `dnsd`) are let in:

```
guardd: /system/config/guard made: in = drop, out = allow; let in: icmp, tcp 22 (sshd), udp 53 (dnsd)
guardd: 3 rule(s) loaded (in = drop, out = allow)
```

A line it does not understand is named, and nothing of the new file is
used (`guardd: /system/config/guard: line 10: 'sideways': in, out or
forward is missing; the rules loaded before stay`). Until `guardd` has loaded the
rules at the boot, nothing comes in. `elf stop guardd` switches the
firewall off: nothing is checked any more (also after the next boots,
until `elf start guardd`); `elf restart guardd` keeps the rules.

```
guard [add RULE | insert N RULE | remove N | in|out|forward drop|allow]
```

`guard` alone shows the rules as the kernel uses them, numbered, with
how many packets each decided (since the rules were last loaded), the
defaults, and the last packets dropped; everyone may look:

```
ritteros:/$ guard
firewall: on; in = drop, out = allow, forward = drop
  1  drop in tcp 7000 from 10.0.2.0/24                         5 packet(s)
  2  allow in icmp                                             0 packet(s)
  3  allow in tcp 22                                           0 packet(s)
     in = drop (no rule matched)                               0 packet(s)
     out = allow (no rule matched)                             0 packet(s)
     forward = drop (no rule matched)                          0 packet(s)
connections: 0 packet(s) of connections already let through (not checked again)
last dropped:
  2026-10-18 21:49:53  in tcp 7000 from 10.0.2.2 to 10.0.2.15 on eth0 (rule 1)
```

Root changes the rules with it; each change goes into
`/system/config/guard` (comments and the other lines stay as they are),
and `guardd` loads it within 2 seconds. A rule is checked before it is
written:

| Command | Does |
|---|---|
| `guard add RULE` | RULE becomes the last rule |
| `guard insert N RULE` | RULE goes before rule N (it is checked first) |
| `guard remove N` | rule N goes |
| `guard in drop`, `guard in allow` | what comes in when no rule matches |
| `guard out drop`, `guard out allow` | what goes out when none does |
| `guard forward drop`, `guard forward allow` | what is forwarded between cards when none does |

```
ritteros:/$ guard add allow in tcp 8080 from 192.168.0.0/24
guard: rule 4: allow in tcp 8080 from 192.168.0.0/24; guardd loads it within 2 s
ritteros:/$ guard add allow sideways
guard: 'sideways': in, out or forward is missing
```

## 9. The system

| Program | Shows or does |
|---|---|
| `clear` | an empty screen, the prompt at the top |
| `ps` | the processes: ID, parent, group, state, name |
| `ldd PROGRAM...` | the shared libraries a program needs, and whether they are all there (below) |
| `kill [-SIGNAL] PID...` | end processes (below) |
| `threads` | the kernel's threads: state, the core each runs on (`cpu`), timer ticks, switches; the processor's clock while it ran, per core |
| `cpus` | the processor's cores: which the kernel started (all, unless `cpus=N` asks for fewer), and their ticks |
| `mem` | free and usable memory, and the page cache (below) |
| `swap` | the swap file `/system/swap`: show, make, turn on or off (below) |
| `uptime` | time since the kernel's timer started |
| `uname [-amnrsv]` | the system's name, release (year.month), the commit it was built from, the machine |
| `pci` | the PCI devices (controllers on the motherboard) |
| `usb` | the USB devices: port, IDs, speed, function, names |
| `cu DEVICE` | a terminal on a serial line, such as a USB device's console (section 8) |
| `poweroff` | switch the machine off (root only) |
| `reboot` | restart it (root only) |
| `hello` | the first RitterOS program: where it is in memory |
| `false` | nothing, unsuccessfully (exit code 1) |
| `sh` | the shell (section 2); `login` starts it for you |
| `elf` | the elves: programs kept running (below) |
| `dhclient` | the DHCP client, an elf: addresses for the cards set to `dhcp` (section 8) |
| `dnsd` | names (DNS) for this machine and others, an elf: answers kept for their time to live (section 8) |
| `logd` | the logs' keeper, an elf: writes `/system/logs/NAME.log` every 10 s (section 10) |
| `guardd` | the firewall's elf: loads `/system/config/guard` into the kernel (section 8) |
| `netnamesd` | the network cards' names, an elf on every machine: adds each new card to `/system/config/network/names` and makes its settings file (DHCP) (section 8, `net names`) |
| `wifid` | the wireless link's elf: scans, joins and holds a Wi-Fi network, runs WPA (section 8) |
| `wifi` | the wireless link: join, forget, scan, status (section 8) |
| `guard` | the firewall's rules: show them, change them (section 8) |
| `windowd` | the window server (below): shows windows on the screen |
| `term` | a terminal in a window; `term CMD...` runs a command, `term -f FONT` uses a font of `/system/fonts` (needs `windowd`) |
| `owl` | a graphical file browser (below; needs `windowd`) |

`kill PID` asks the process numbered PID (as `ps` lists it) to end, with
the signal TERM; `kill -KILL PID` (or `-INT`, `-HUP`, a number 1-31) sends
another. A program ends at the signal unless it ignores it or handles it
(since 22r programs may have handlers: functions that run when the signal
comes, as on Unix; `kill -KILL` cannot be handled). Root may end any process, other users only
their own. A process that is not there is `kill: 999: No such process`
(exit code 1).

```
ritteros:/$ /system/x86_64/test/loop &
[4]
loop: running until Ctrl-C
ritteros:/$ ps
  pid  parent  group  state    name
    1       0      1  running  login
    2       1      2  running  elvesd
    3       1      3  running  sh
    4       3      4  running  loop
    5       3      5  running  ps
ritteros:/$ kill 4
[4] stopped by signal 15
```

Programs for RitterOS can also be written on another computer and run here
over SSH: the source's `sdk/` folder (its `CLAUDE.md`) explains how, for
people and AI agents alike.

The C library covers what programs brought from other systems most often
need (step 22m): stdio's streams (`fopen`, `fprintf`, `fgets`, ...),
`sscanf`, `strtol` and `strtod`, `strdup`, `strcasecmp`, `ctype.h`,
`assert.h`, `inttypes.h`, `system` (it runs `sh -c`) and a small `math.h`
(`sqrt`, `floor`, `fmod`, ...); since 22n also `setjmp.h`, `locale.h`
(the "C" locale) and temporary files (`tmpnam`, `tmpfile`, `mkstemp`), which
are made in the user's own `~/.tmp`; since 22o `time.h`: the clock,
`clock()` (the CPU time a program used, which the kernel counts for each
thread), dates in UTC or local time (the zone of `/system/config/time`)
and `strftime`; since 22p sorting and searching (`qsort`, `bsearch`),
`rand`, `atexit`, `aligned_alloc`, `strtok` and the rest of `stdlib.h` and
`string.h`; since 22q `printf`'s floating point is exact (`%f`, `%e`,
`%g`, `%a`: every digit of the value, rounded as C says), `strtod` is
correctly rounded, and `scanf`/`fscanf` read floating point too; since 22r
signal handlers (`signal`, `sigaction`, `raise`) and `clock_gettime`,
`nanosleep`, `timespec_get`; since 22s all of C99's `math.h` (our own
code, the results the nearest double nearly always); since 22u `fenv.h`:
the rounding direction (`fesetround`) and the exception flags
(`fetestexcept`) of the processor, kept for each thread; since 22v
`wchar.h` and `wctype.h`: wide characters and strings, wide streams
(`fgetwc`, `fputws`, `fwide`, ...) and `wprintf`/`wscanf` in every form,
in the "C" locale (one byte per character; `printf`'s `%ls` and `%lc`
too); since 22w `uchar.h`: UTF-8 always (`mbrtoc32`, `c32rtomb`, the
UTF-16 and `char8_t` forms), for programs that handle Unicode text; since
22x `complex.h`: complex numbers and C99's functions of them (`cexp`,
`clog`, `csqrt`, `cpow`, the trigonometric and hyperbolic functions and
their inverses), and `long double` arithmetic on ARM (at double's
precision, as everywhere here); since 22z the rest of `math.h` (`rint`, `fma`, `remainder`, `erf`,
`tgamma`, `lgamma`, ..., and every long double form); since 22y
`threads.h`: C11's threads
(`thrd_create`, `thrd_join`, `thrd_detach`, ...), mutexes, condition
variables, `call_once` and thread-specific storage, waiting in the kernel
(a blocked thread uses no processor time). The SDK's `API.md` lists every
function.

**Shared libraries.** The system's programs share their libraries: the C
library `libritteros.so.1`, the windows' `libwindow.so.1`,
`libimage.so.1`, `libaudio.so.1`, `libvideo.so.1`, `libdraw.so.1` (2D
drawing), `libwidget.so.1` (controls: buttons, check boxes, layouts), and
`libtls.so.1` (TLS 1.3 connections, OpenBSD's libtls
interface), all in
`/system/<arch>/lib`. When a program starts, `/system/<arch>/lib/ld.so` (the
dynamic loader) maps the ones it needs: one copy of each in memory serves
every program, and replacing a library's file changes what programs started
afterwards get (running ones keep the copy they have). Programs can also
load a library while they run (`dlopen`, for plug-ins). A library missing,
or without a function a program needs, stops that program before it begins,
with exit code 127:

```
ld.so: imageview: libimage.so.1 is not in /system/x86_64/lib/ (No such file or directory)
ld.so: imageview: imageview needs image_load, which none of its libraries has
```

Started by itself, `ld.so` only says what it is. `ldd PROGRAM` shows the
same without running the program: each library it needs (and the ones those
need) and where it is, and every function that none of them has (exit code 1
then). A program is a path or a name, looked up like the shell does:

```
ritteros:/$ ldd imageview
/system/x86_64/apps/imageview.program/imageview:
    libritteros.so.1 => /system/x86_64/lib/libritteros.so.1
    libwindow.so.1 => /system/x86_64/lib/libwindow.so.1
    libimage.so.1 => /system/x86_64/lib/libimage.so.1
ritteros:/$ ldd /system/x86_64/test/ldtest-static
/system/x86_64/test/ldtest-static:
    statically linked: it needs no libraries
```

**The page cache.** Programs are loaded the same way: their code and
read-only data come from the page cache, one copy in memory for every
program running the same file (the shell, `login` and `elvesd` share theirs
with every later copy of themselves), and their writable data is a
program's own only once it writes a page. A file nothing maps any more stays
cached (up to 32 MiB, at most an eighth of the memory; the file unused the
longest goes first), so a program started again, or a library loaded again,
is not read from the disk again. A file rewritten since is read anew. `mem`
shows the cache in its second line: the files in it, their pages, and how
many pages were read from files since the start. It counts the cached files
nothing uses as free memory, and gives them back first, so its count is
exact (so do the rescue console's `mem` and the program `uptime`):

```
ritteros:/$ mem
memory: 4065 MiB free of 4086 MiB (1040746 of 1046188 pages of 4 KiB)
page cache: 6 file(s), 35 page(s) (0 kept unused), 35 page(s) read
swap: off
```

**Swap** (since 35a). Swap is where the kernel keeps pages of programs'
memory while memory is short: the file `/system/swap` on the system's disk.
While free memory is low, the kernel's thread `pageout` (since 35b) writes
pages programs have not used lately there; a program touching one gets it
back (it waits for that read), and a program needing a page when none is
free waits for pageout rather than failing. It is off until root asks:
`swap make 4G` makes the file (writing zeros once, which gives it its
blocks) and turns swap on, `swap on` and `swap off` turn it on and off
(`swap off` reads the pages in it back first), and
both remember it for later starts in `/system/config/swap` (`on = yes`),
which the kernel reads at every start. `swap` alone, and `mem`'s third
line, show it. The file is a *fixed* one: its blocks stay where they are
and only the kernel writes them, in place (the one exception to RitterFS's
copy-on-write), so programs cannot read, change or copy it, and while swap
is on it cannot be removed. Every page in it is encrypted (XTS-AES-256)
with a key made each time swap goes on and kept only in memory: after the
machine is switched off nobody can read what was in it. See
[`swap`](commands/swap.md).

```
ritteros:/$ swap make 4G
swap: /system/swap made: 4096 MiB
swap: on, /system/swap: 4096 MiB (1048576 pages), 0 in use; since the boot 0 pages written, 0 read
```

**Fonts.** The window system's fonts are BDF files in `/system/fonts`, the
public-domain X11 misc-fixed faces: `6x13`, `7x13`, `7x14`, `8x13`, `9x15`,
`9x18` (each also bold, with a `B`: `9x15B`) and `10x20`, named by their
character cell. Programs open one by name; `9x15` is also built into the
window library, the default and the fallback when a font file is missing or
cannot be read. Since 27k also X.Org's Adobe bitmap faces (proportional,
for iBrowser's text): Helvetica (`helvR12`, ...), Times (`timR12`) and
Courier (`courR12`), each regular (`R`), bold (`B`) and oblique or italic
(`O`, `I`; `BO`, `BI`), at 8, 10, 12, 14, 18 and 24 points (`08` ... `24`:
11 to 34 pixels), under Adobe's permission notice (`ADOBE-COPYING.txt` in
the same folder). Another BDF font (bitmap glyphs up to 64 pixels wide,
fixed-width or proportional) copied into the folder can be used by its name
the same way.

Since 41c also scalable fonts: GNU FreeFont's FreeSans, FreeSerif and
FreeMono (sans-serif, serif and fixed-width), each regular and bold, as
TrueType (`.ttf`) and OpenType (`.otf`) files, under the GPL with the font
exception (`FreeFont-README.txt` and `FreeFont-COPYING.txt` beside them).
Programs open one at any size in pixels (`FreeSans.ttf` at 17); its text is
drawn smooth, each pixel by how much of it the letters cover. Another
TrueType or OpenType file copied into the folder (or anywhere, by its path)
can be opened the same way.

### The window system (being built)

RitterOS is growing a graphical window system. `windowd` is the window
server: it takes over the screen from the text console, shows each
program's window with a title bar and border, draws the mouse pointer, and
passes the mouse and keyboard to the window in front. It starts on demand,
not at boot — run `windowd &` (or, once set up, `elf start windowd`); while
it runs the text console waits, and it comes back when windowd stops. On a
machine with no graphics `windowd` reports it cannot take the screen.

Any user logged in at the machine's own screen and keyboard may start
`windowd`; root may from anywhere. A user logged in from elsewhere (over
SSH) may not: whoever has the screen also gets every key typed at the
machine, a password too (`windowd: cannot take the screen: Operation not
permitted`). The windows on it are its user's and root's only: a program of
another user is refused (`windowd: refused a program of user 1001 (this is
user 1000's screen)`, and the program says `cannot open a window: the
screen is another user's`), and a program never talks to another user's
`windowd`.

`windowd` by itself is only a compositor — it has no menu of its own — so
it opens a program when it starts, the file browser `owl` by default, from
which you open terminals and other apps; so the GUI is usable even on a
machine with only the graphical screen (no serial console). To change what
it opens, make `/system/config/windows` with a `startup` line:

```
startup = owl
double_click = 500
click_steady = on
refresh = auto
text_size = 15
theme = dark
accent = 5E81AC
```

`theme` (41g-5) is `dark` (when not set: Nord's dark colours) or `light`
(its light ones), for the desktop, the frames and menus, and the programs'
windows (`owl`, `term`, and every program drawn with libwidget's
widgets); `accent` is the colour of what is in front and chosen (the
window in front's title bar, menus' chosen items, selections, focus
rings), as six hexadecimal digits, the theme's blue `5E81AC` when not
set. windowd and each program read them when they start: start them again
after a change.

`text_size` (41d) is the size of text, in pixels an em (8-64), for programs
that draw with the scalable fonts at your size; 15 when not set (the
built-in font's height). A program reads it when it first opens such a
font: start it again after a change.

`refresh` (65e-34b) is how many frames a second the window system draws at
most: `auto` (when not set) is the screen's own rate, from the monitor's
EDID, else 60; a number (10-1000) sets it for a monitor that does not say.
With a graphics driver that tells the screen's vertical blank (66c-3), the
frames start there, in step with the screen; with its screen buffers
(66d-3) windowd page-flips, so no frame shows half drawn.
Whatever changes within one frame (a dragged window, the pointer, a
program's new picture) is drawn together, so the screen never falls behind.

`displays` (66a) shows the screen's mode, the graphics devices and, where
RitterOS has a driver (so far the J1900's Intel graphics), each output and
the monitor on it, read over its cable: its name, native mode and every
mode it lists. Monitors plugged in or pulled out are noticed within 2
seconds (the kernel's log: `gpu0: hdmi0 connected: 'DELL S2721DGF', ...`).
`displays set hdmi0 1920x1080` (66b-2) changes the mode (one the monitor
lists; `@HZ` picks the rate) until the next start (the J1900's Intel
graphics, and since 66g-2 the laptop's AMD graphics: there only the modes
its panel lists, at the size the firmware set); the console and
windowd redraw at the new size (66b-5). A line `hdmi0 = 1920x1080@60` in
`/system/config/displays` sets it at every start (66b-6). `displays sleep`
(37g-5c-2) lists what the last sleep changed in the graphics' registers (the
AMD Cezanne's and, since 37g-6, the Picasso's display pipe: before and
after).

`power` (37a, roadmap item 37) reports what the firmware offers for power
management, from its ACPI tables: the machine's profile and flags (among
them whether it prefers its deepest idle to sleep state S3, "Modern
Standby"), the sleep states it names (S3 suspend to RAM, S4 to disk, S5
off), the waking vector, and the power devices it describes (battery, AC
adapter, lid, buttons, the embedded controller, processors). It runs none
of the firmware's code yet: later steps of item 37 read the battery, watch
the lid and the buttons, and put the machine to sleep. `power tables DIR`
(root) saves the tables themselves as files, as Linux's `acpidump -b`
does. Since 37b-2 the kernel has an AML interpreter of its own (the ACPI
specification's machine language, the firmware's code): at the start it
runs the DSDT and the SSDTs into a namespace of objects (devices,
methods, regions, fields, names), which `power names` (root) lists, and `power eval NAME`
(root, 37b-3) evaluates one of them: a method run, a field read, its value
printed. Since 37b-4 the firmware's code reaches the machine (its memory and
devices, I/O ports, PCI configuration; never the kernel's own RAM), and
the boot runs its `_INI` methods as Linux does; since 37b-5 also a
laptop's embedded controller (the battery's, the lid's), so `power eval`
of the battery's `_BST` gives its real state; `power` reports what it
did, and since 37c-1 the batteries' charge and the AC adapter.

Sleep (37g): `power sleep SECONDS` (root) puts the machine into ACPI S3
(only the memory keeps its contents) and the RTC's alarm wakes it SECONDS
later; programs, files and network connections carry on (a card's link
comes up again, Wi-Fi joins again). Each driver keeps what its device
needs and sets it up again afterwards; a machine with a device whose
driver cannot does not sleep, and the kernel's log names the driver.
On the AMD laptops the display driver brings the panel back after the
wake (37g-5c-3, the Acer's Cezanne: the console shows again a few seconds
after the power light turns back from blinking orange; 37g-6, the
VivoBook's Picasso). Disks come back: a SATA disk after its link's reset,
a USB disk on the controller's own port as the same disk when it says it
is (its descriptor and serial number; behind a hub, or not the same, it
is found again as if plugged in), so a machine whose system disk is a USB
stick sleeps too; the Intel 8265's Wi-Fi starts its firmware again and
joins (37g-6). Before the sleep
the display is stopped, as Linux does: with the display reading memory
the Acer never reached S3. A reboot after a sleep restarts the machine as
any other (37g-5d).

The power button (37c-2a) switches the machine off as `poweroff` does:
the kernel takes the press from the firmware's interrupt (the SCI) and
starts `/system/<arch>/bin/poweroff`, which lets logd write its logs
first. `power_button = nothing` in `/system/config/power` makes a press
only a line in the kernel's log, `power_button = sleep` (37g-7) puts the
machine to sleep (the next press wakes it, and does nothing else).
A keyboard's Power key is the power button too (37g-7; it never reaches
programs): the Acer's embedded controller sends its button that way
while the machine is awake (scan code 0x55; the standard key is E0 5E,
or usage 0x66 on USB). On the Acer a sleep its button starts wakes
again by itself about 2 s later (its embedded controller wakes it as
the power button; a sleep typed as `power sleep`, or the lid, stays
asleep); left as it is. Holding the button for a few seconds switches any PC off in its chipset,
whatever the system does. Since 37c-2b the firmware's other events run
too: its general-purpose events' methods (except those only for waking
the machine), a power button the firmware describes as a device, and on
ARM machines the GED, through which their power button comes. A
notification from the firmware nothing takes yet is logged (`acpi:
Notify(...)`, the first 32). Since 37c-2c a laptop's embedded controller
tells about its events too (the lid, the AC adapter, the battery, special
keys: the kernel asks it for each one and runs the firmware's method):
closing or opening the lid and plugging or unplugging the adapter are
lines in the kernel's log (`acpi: the lid ...: closed`), and `power` shows
the lid's state. `lid = nothing` (the default) in `/system/config/power`
leaves it at that; `lid = sleep` (37g-7) puts the machine to sleep when
the lid closes (the power button, or on many laptops opening the lid,
wakes it). `sleep_after = MINUTES` (37g-7; 0, the default, never) puts it
to sleep after that long without a key, a click, a mouse move or typing
over SSH; `power sleep` (root) does it at once.

Since 37d-1 `power` also lists the processors' sleep states the firmware
offers (C1, C2, C3: the deeper, the less power and the longer to wake)
and, on processors with an energy counter, how much power the processor
used over the last second (it waits that second to measure). Since 37d-2
idle cores sleep in those states: each core takes the deepest whose
wake-up fits how long its recent idle periods lasted, as Linux does (only
where the processor's timer keeps running in them); the boot log's
`idle:` line says which states are used, `power` how often each was.

The screen goes off (37e) after `screen_off` minutes without a key, a
click or a mouse move (5 unless `/system/config/power` says otherwise; 0
keeps it on), at the console or under windowd: it turns black and nothing
is drawn on it. Any key or mouse move brings it back; that first key or
click only wakes the screen and goes nowhere else, so it cannot type into
or click on something you did not see. Typing over a serial cable wakes
it too (and is kept). The kernel's log says `screen: off after 5 minutes
without input` and `screen: on (input)`. (Since 37e-2b) on a laptop
whose graphics driver can (the AMD graphics of both laptops) the panel's
backlight goes off with the screen and comes back as bright as it was;
the log lines then name the panel: `(the backlight off: edp0)`. (Since
37e-2d) a monitor on the J1900's HDMI port gets no signal while the screen
is off, so it goes to standby by itself: `(the signal off: hdmi0)`. (Since
37e-2c) before that, after `screen_dim` minutes (2 unless the file says
otherwise; only when it comes before `screen_off`), the panel dims to a
third of its brightness (never below the lowest its firmware allows):
`screen: dimmed after 2 minutes without input (edp0 0x9c00 to 0x3400)`.
Any input brings the brightness back, and that key or click goes on as
usual (the screen was seen); waking from off brings both back.

Some drivers are kernel modules (67a): files in `/system/<arch>/drivers/`
(`NAME.mod`), loaded after the system's disk is mounted, and only those
whose hardware is on PCI. `drivers` lists them, loaded, not needed or
refused (and why); `nomodules` on the kernel command line loads none. So
far the network cards' drivers are (Realtek, Intel e1000 and igb,
MediaTek MT7921 Wi-Fi, USB network adapters), the graphics drivers
(Intel Bay Trail, AMD) and the sound driver (HD Audio, found by its PCI
class); a USB one stays loaded and takes its devices when
they are plugged in. Until the graphics module loads, the screen is the
firmware's. Drivers are written against the
driver interface (67b: `drivers/include/`, version 2.0, in the sources;
installed as `/system/include/drivers/`); `drivers/CLAUDE.md` is the guide
for writing one and `drivers/template/` a driver to start from, built
outside RitterOS's tree. A module made for a newer interface than the
kernel's is refused, and `drivers` says why; so is one made for another major
version (2.0 since 65e-44: each wait queue has its own lock, so a module
of 1.x has the old layout).

`double_click` is how many milliseconds may pass between the two presses of
a double click (100-5000; 500 when not set). The window system decides what
is a double click -- two presses of the left button within that time and
within 4 pixels -- so it is the same in every program.

`click_steady` (on when not set; `off` to switch it off) keeps a click where
you aimed on a touchpad: a finger pressing a touchpad's click rolls, and the
pointer creeps a pixel at a time into the press, while the button is held
and back as the finger lifts (measured on the laptop: up to 8, 13 and 15
pixels). Around a click the window system holds such slow movement: a press
counts where a creep that ran straight into it began, slow steps while the
button is down are held until they add up to 12 pixels (then it is a drag,
and the pointer catches up), and for 0.3 s after the release slow steps are
dropped. Movement of a few pixels at a time, a nudge followed by a pause,
and all movement away from clicks are unchanged.

A bare name is a program of the system (like the shell finds it), words are
the command and its arguments, and an empty `startup =` opens nothing (a
bare desktop). When that program ends, `windowd` **keeps the desktop up** —
the empty background waits for you to open something else or quit. To leave
the window system and return to the text console, use the desktop menu's
**Close window server** (below), or **Ctrl-Alt-Delete**, which leaves it even
when the program holding the screen is hung (the kernel ends that program
and gives the console back).

**Right-clicking the desktop background** (anywhere no window is) opens the
desktop menu: the apps, each by its name (**iBrowser**, **Image viewer**,
**owl**, **Terminal**, **Video player**), and **Close window server**,
which gives the screen back to the text console. The apps are the
bundles in `/system/<arch>/apps` (42b): a folder `NAME.program` holding
the program `NAME`, its file `info` (`name = ...` the label, `about = ...`
what it is, `types = ...` the file endings it can open) and `icon.png`,
its picture, 32x32, shown at half size left of its name, on its windows'
taskbar buttons and in `owl`; an app installed later is in the menu the
next time it opens. **`open FILE...`** at the shell
opens files as a double click in `owl` does (a folder in `owl`).

**File types** (42a). Which program a double click (and `open`) starts
for a file is yours to choose, by its ending, in `~/.config/filetypes`:
`ENDING = PROGRAM` lines, written with these when missing:

```
png = imageview
rvid = reel
html = ibrowser
wav = tune
txt = edit
```

(`ppm`, `bmp`, `jpg`, `jpeg` in `imageview`, `htm` in `ibrowser`, `md`,
`c`, `h` in `edit` too.) An app (in `/system/<arch>/apps`) opens the file
in its window, a command-line program (in `bin`, as `tune` and `edit`) in
a terminal window; an ending not in the list (nor in
`/system/config/filetypes`, an administrator's, if there is one) opens in
the editor. Apps open other files too, from their own menus, and owl's
**Open with...** (42c, a file's right-click menu and File) starts any app
whose `info` lists the file's ending, chosen in the Open dialog (the others
greyed). The programs started this way are `windowd`'s own, as the startup
one is.

**Notifications.** A box at the top right of the screen, over every window,
for 5 seconds: a program's title in bold and its text below, wrapped (up to
four at once, the newest on top). **`notify TEXT...`** at the shell shows
one (`make; notify make ended with $?`); programs send theirs through
libwindow's `window_notify`.

**Testing programs without a screen** (41h). `windowd headless WxH` is a
window server of your own with no screen, keyboard or mouse (any user may
start one, over SSH too): it listens where the variable `WINDOW_SOCKET`
says, and programs started with the same variable open their windows in
it. [`wsend`](commands/wsend.md) clicks, drags and types into it, holds
keys down (`wsend down ctrl` ... `wsend up ctrl`, for games), and waits
for the server or a window (`wsend wait`, `wsend wait TITLE`) instead of
a guessed pause; [`wshot`](commands/wshot.md) saves its screen as a PNG
picture:

```
$ export WINDOW_SOCKET=/tmp/w1
$ windowd headless 800x600 & wsend wait
$ imageview /test/solid.ppm & wsend wait /test/solid.ppm   # (its window's title)
$ wsend click 200 150; wshot /tmp/shot.png
```

A windowd on the screen refuses `wsend` (its input is the machine's own
keyboard and mouse); `wshot` takes any windowd's picture.

**Ctrl + click** is a right-click everywhere (the desktop menu, `owl`'s
menu, any app's), for a mouse with one button or a touchpad RitterOS only
knows as a plain PS/2 mouse.

**Touchpads.** A touchpad that the firmware describes as HID over I2C (most
laptops of recent years: ACPI's `PNP0C50`) is driven as a touchpad of its
own, `mouse1` on the laptop (its PS/2 compatibility mouse stays `mouse0`).
The boot shows where it is:

```
  i2c0:          DesignWare I2C at 0xfedc5000 (I2CD for TPAD), 400 kHz, FIFO 256/256, version 0x3230312a
  i2c-hid:       i2c0 address 0x15 (TPAD): HID 04f3:31b1 version 0x0001, report descriptor 385 bytes at 0x0002, input 0x0003 (up to 14 bytes)
  gpio:          AMD GPIO controller at 0xfed81500 (GPIO), GSI 7 (level, active low), vector 0x37
  mouse1:        touch pad 04f3:31b1 on i2c0 0x15, 101 x 73 mm, interrupt: GPIO pin 9
```

One finger moves the pointer (slow movements precisely, fast ones farther);
a tap is a click; **two fingers** tapping or pressing are the **right
button** (context menus); two fingers moving up or down **scroll**, the
content following the fingers; three fingers do nothing yet. A press of the
pad stays where you aimed: the pointer's creep as the finger presses is
taken back, the pointer stays while the pad is held until the finger moves
3 mm (then it drags), and as the finger lifts. So far the I2C controllers
of AMD's chipsets (`AMDI0010`) and AMD's GPIO controller are known; on
other machines a touchpad stays a PS/2 mouse.

Windows appear with their titles and the pointer; a click chooses and
raises a window (its title bar brightens), dragging a title bar moves it,
dragging an edge or a corner resizes it (the pointer becomes a double-headed
resize arrow over an edge -- horizontal, vertical or diagonal to match -- so
you can see where it will resize; the terminal reflows to the new number of
rows and columns, and the shell and programs are told the new
size), the `+` box in the title bar maximizes the window to fill the screen
(click `+` again to return it to its previous size and place; the `+ - X`
boxes stay in the corner while maximized), the `-` box minimizes the window
to a button at the bottom of the screen (click the button to bring it back
to its size and place), and the `X` at the right closes the window (its
program ends; the desktop stays up, so closing the last window leaves you on
the empty background, not back at the text console). A window always opens
on the screen, above the row of minimized buttons: one that would reach
past an edge is moved in, and one bigger than the screen is made as big as
fits (its program is asked to take that size, as when you resize it).
A program may also take the whole screen for a window (full screen: no
frame or title bar, in front of everything, until the program gives it
back, usually on Escape) or minimize it itself. Files can be dragged out
of `owl` onto another program's window: press on a file, move the
pointer (it carries a small page), and let go over the window; the program
gets the file's full path and decides what to do with it.
Graphical programs are written with the window toolkit `libwindow`
(`window.h`): open a titled window, draw into it (fill rectangles, draw
text in the shared font), show it, and read pointer and keyboard events.
`term` is the first graphical program: a terminal in a window running the
shell, so the whole command line works inside the window system. Start the
GUI with `windowd &` and the terminal opens by itself; you can open more
with `term` from inside it. Programs talk to `windowd` over the local
socket `/run/windowd`.

**Text fields and the clipboard.** A line where you type -- iBrowser's
address, the **Open** dialog's name -- is the same text field in every app:
a click places the cursor, dragging across text selects it (so does Shift
with the arrows, Home or End), a double click selects a word, Ctrl+A all of
it; typing replaces the selection, Backspace and Delete remove it. Ctrl+C
copies the selection, Ctrl+X cuts it, Ctrl+V pastes: the clipboard is the
window system's, shared by your programs (another user's programs never
reach your `windowd`), kept until the window system ends, up to 64 KiB.

In **term**, dragging across the text selects it (it shows inverted; the
scrollback too), a double click selects a word, **Ctrl+Shift+C** copies the
selection (each line without its trailing blanks) and **Ctrl+Shift+V**
types the clipboard into the program, line breaks as Enter. Plain Ctrl+C
stays the interrupt.

**Scrolling.** The mouse wheel scrolls the window under the pointer, and
lists that do not fit have a scroll bar at their right edge: press on it
to jump, drag it to move (on a touchpad, two fingers scroll; known only as
a PS/2 mouse, the bar is the way). `owl` and the **Open** dialog move 3 rows a wheel step; `term`
keeps the last 1000 lines that left its top, which the wheel or its bar
shows, and a key or new output brings it back to the bottom. A PS/2 mouse
reports its wheel when it can (the boot says `PS/2 mouse ... with a wheel`).

Windowed apps live in `/system/<arch>/apps`, each a bundle (42b:
`term.program/term`, `owl.program/owl`); command-line programs are in
`/system/<arch>/bin` next to the window server. You can run either by
name — the shell looks in both, and inside the bundles.

`owl` is the graphical file browser. It opens a window listing a directory —
directories first and marked, then files with their sizes, the current path
at the top — starting in your home directory (or `owl DIR` to start
elsewhere). Move the selection with the arrow keys or a click; **Backspace**
or the `..` row goes up a directory; **Enter**, or a double click on a row,
opens it. What "open" does depends on the item: a directory is
entered; a windowed app (a bundle, `NAME.program`, shown as `NAME` with its
icon) runs on its own (**Show contents**, in the Edit menu and a row's
right-click menu, goes into the bundle instead; **Open with...** there
and in the File menu opens a file in an app you choose: 42c); any
other program opens in a terminal that stays open so you can read its output;
and a data file opens by its type: a picture (`.ppm`, `.bmp`, `.png`, `.jpg`,
`.jpeg`) in `imageview`, a video (`.rvid`) in `reel`, a sound (`.wav`) in
`tune` (in a terminal), and anything else in the editor. `term -f FONT` (any font of
`/system/fonts`, e.g. `term -f 10x20`) sizes its rows and columns to that
font's cell; an unknown font falls back to 9x15 with a message. `term` can also be told a command
directly — `term CMD ARGS...` runs it in a window instead of the shell — which
is how `owl` opens programs.

`owl` has a **File** menu (New folder, Open, Up, Refresh, Quit), an **Edit**
menu (Rename, Delete, Copy, Paste), and a **Go** menu with shortcuts to your
home directory, the apps directory and the root (`/`); right-clicking an
entry brings up a menu for it (Open, Rename, Delete, Copy). New folder and Rename
ask for a name in a line at the top of the window — type it and press Enter
(Escape cancels). Delete asks `y/n` first, and removes a file or an empty
directory. Copy remembers the selected file or folder, and Paste copies it
into the directory you are looking at. Anything that goes wrong (and what
Copy is holding) shows in a line at the bottom.

Menus are part of the window system: a program declares the menu it needs (a
menu bar below the title bar, and context menus it pops up where you click),
and `windowd` draws them and tells the program which item you chose. So any
graphical program can offer menus the same way.

An elf is a program RitterOS starts at every boot and starts again
whenever it ends: a server like `sshd`, for example. (Other systems call
them services or daemons.) Each elf has a file in `/system/config/elves/`,
named after it:

```
command = sshd -p 22
start = on
```

`command` is the program and its words (a name without `/` is a program
of the system, like the shell finds it); `start = on` keeps it running,
`off` leaves it stopped. `elvesd`, which `login` starts at the boot when
there are elves, looks at the folder every 2 seconds: it starts an elf
that is on and not running, stops one that is off or whose file is gone
(SIGTERM, then SIGKILL after 5 seconds), and starts one that ended again
after a wait: 2 seconds, doubled (up to 60) while it keeps ending within
10 seconds of its start. Its lines and the elves' own output appear on
the screen, and also in their logs (`/system/logs/elvesd.log`, and
`NAME.log` for each elf):

```
elvesd: sshd started (process 3)
elvesd: crash ended (exit code 1); starting it again in 4 s
```

Every system has the elf `logd`, which keeps the logs: `login` makes its
file at the first start (`login: /system/config/elves/logd made: the elf
logd keeps the logs in /system/logs`). Switched off (`elf stop logd`),
it first writes what it still keeps; the kernel then keeps collecting
in memory (a limited amount: what does not fit is noted as lost) until
it runs again. `poweroff` and `reboot` let it write its last lines
first.

```
elf [start | stop | restart | remove NAME | add NAME COMMAND...]
```

`elf` alone lists the elves; everyone may look, only root may change
them:

```
ritteros:/$ elf
elvesd: running (process 3)
sshd             on   running (process 4)     sshd -p 22
```

| Command | Does |
|---|---|
| `elf add NAME COMMAND...` | makes an elf (on): its file, and `elvesd` if it does not run yet |
| `elf start NAME` | switches it on: `start = on` in its file (also for the next boots) |
| `elf stop NAME` | switches it off: `start = off` |
| `elf restart NAME` | ends its process; `elvesd` starts it again |
| `elf remove NAME` | deletes its file; `elvesd` stops it |

`elvesd` acts within 2 seconds. Messages: `elf: NAME: no such elf`,
`elf: start: only root may change the elves`, `elf: NAME: there is such
an elf already`. An elf finds its process by its program's name among
`elvesd`'s children: two elves of the same program show as one
`running (2 processes)`, and `restart` asks to stop and start instead.

### Pictures, sound and video

RitterOS reads images, sound and video through three small libraries
(`libimage`, `libaudio`, `libvideo`, used by programs; decoding is written
from each format's specification), and plays sound on machines with an
Intel HDA / Azalia audio controller (most PCs).

- `imageview [FILE]` shows a picture (PPM, BMP, PNG or JPEG) in a window, scaled
  to fit the window with its proportions kept (centred, the rest of the
  window dark grey), so resizing rescales it; photos are turned the way
  their camera recorded (EXIF orientation: a portrait photo, which cameras
  store lying on its side, shows upright); `q`, **File > Close** or the close
  box quits. Run it under the window system (`windowd &` first). Without a
  file, or to show another, **File > Open...** brings up the **Open** dialog
  (below); the window then takes the picture's size. PNG files of every
  kind are read (not animated ones); JPEG files grey or colour, baseline or
  progressive, as cameras and web pages make them (not the rare arithmetic-
  coded, 12-bit, lossless or CMYK ones). For a file it cannot read it prints
  `imageview: FILE: Invalid argument` and keeps showing what it showed.
- `reel [FILE]` plays a video (the simple RVID format so far) in a window —
  the frames at the video's rate, and its sound track through the speakers.
  The frames are scaled to fit the window with their proportions kept, so
  the window may be resized, and a video larger than the screen plays in a
  window that fits it. It stays open on the last frame when the video ends;
  **File > Open...** plays another (stopping the one playing).
- `tune FILE` plays a sound file (WAV) through the speakers.

The **Open dialog** is the same in every app that opens files (it comes
with the window library, `window_choose_file`): a window listing a folder —
`..` first, then folders (green, with a slash), then files. A click selects
a row and a double click opens it (a folder: go in; a file: that is the
choice); the arrow keys and **Enter** do the same. A name or a whole path
typed goes into the **Name** line at the bottom, and **Enter** opens it;
**Backspace** with nothing typed goes up a folder. **Open** chooses,
**Cancel** or the close box cancels. (42c) When it chooses an app (owl's
**Open with...**), an app's bundle is one row, its name without
`.program`, and what cannot be chosen is greyed.

Sound comes out of the device **`/dev/pcm0`** (present when there is an
audio controller): a program opens it and writes 48 kHz, 16-bit, stereo
PCM. One program at a time has it; to let several play at once, run the
mixer **`soundd`** (as an elf — `elf add soundd soundd` — or `soundd &`),
which owns `/dev/pcm0` and mixes everyone that connects to it. A machine
with no audio controller has no `/dev/pcm0` and these players say so.

The speakers belong to the person at the machine: `/dev/pcmN` opens for
root and for the user at the console (the user whose job has the console:
their programs may play wherever they run, in `term` too); a user logged in
over SSH meanwhile gets "Permission denied". `soundd` started by a user
mixes that user's (and root's) programs only and refuses others
(`soundd: refused a program of user 1001 (this is user 1000's sound)`);
run as an elf (root's) it mixes everyone's.

Each audio controller is a sound device: `/dev/pcm0`, `/dev/pcm1`, ...,
those with speakers or a headphone jack first, then line outs, then digital
ones (HDMI on a graphics card) last, so `pcm0` is the one you hear. The
driver, `hda`, is a module (see `drivers`), loaded after the system's disk
is mounted when the machine has an HD Audio controller. The boot's lines
say what each codec's connections are and which device it became; on the
laptop:

```
  hda:           05:00.6 (1022:15e3)
                 codec 0: 10ec:0295
                 pin 18: microphone, built in
                 pin 20: speaker, built in -> 2
                 pin 33: headphones, jack -> 2
  pcm0:          05:00.6 codec 0: speaker pin 20 (DAC 2), headphones pin 33 (DAC 2); 48 kHz 16-bit stereo
  pcm1:          01:00.1 codec 0: digital out pin 4 (DAC 8); 48 kHz 16-bit stereo
```

Speakers and headphones play together (there is no switch-over when
headphones are plugged in yet). Any device can be written to directly,
e.g. raw 48 kHz 16-bit stereo samples: `cat sound.raw > /dev/pcm1`.

### The web browser: iBrowser

```
ibrowser [-t] [URL]
```

**iBrowser** shows web pages in a window (run it under the window system,
`windowd &` first; `owl` opens it from `/system/<arch>/apps`). URL is an
`http://` or `https://` address (`http://` is assumed for `host/path`), or
an HTML file on the disk (a path such as `/home/anna/page.html`); without
one it shows its own start page.

- **At the top**: `<` goes back to the page before; `x` (shown while a
  page loads) stops the load, as **Escape** does; the address line
  shows the page's address, a text field like every app's (select, edit,
  copy and paste in it). Click it, or press **Ctrl-L** (all of it
  selected, so typing replaces it); **Enter** goes there.
- **Loading** (since 27q): a page loads in the background, its style
  sheets and script files with it (fetched at once); the page shown stays
  usable meanwhile (scroll it, follow another link, go back, quit) and is
  replaced when the new one is there. Files a page asks for later (a
  script file a script inserts, a style sheet not known before) come in
  the background too: such a script runs once its file is there, the page
  is styled again when a style sheet comes. The window never waits for a
  server; only a script that computes for a long time keeps it busy.
  A `<meta http-equiv="refresh" content="SECONDS; url=URL">` opens URL (or
  the page again) once the page is in and the seconds are over, in the
  page's place in the history. While a page's scripts run, its
  `<noscript>` parts are not shown (nor is a refresh inside one used). A server that does not
  answer is given up after 30 seconds, for the connection and again for
  each wait for data: the page then says `HOST: the server did not answer`
  (or `stopped answering`, if it went silent in the middle). With `-t`, a
  stopped load prints `ibrowser: stopped`.
- **At the bottom**: where the link under the pointer leads, or the page's
  title, or why a page could not be shown.
- **Links** are blue and underlined; a click follows one (red while the
  pointer is on it) when the button is let go without the pointer moving.
- **Selecting**: dragging across the page selects its text (white on blue),
  a double click a word; **Ctrl+C** copies it (lines as line breaks), while
  the address line does not have the keyboard.
- **Scrolling**: the wheel, the scroll bar at the right, the arrow keys,
  **Page Up**/**Page Down**, **Home**/**End**, and the space bar.
  **Backspace** (outside the address line) goes back, `q` quits.
- **File** menu: **Open File...** (a page from the disk, with the Open
  dialog), **Reload**, **Back**, **Quit**.
- **Scripts** (since 27p): a page's JavaScript runs on RitterOS's own
  engine (the one `js` uses), each `<script>` in turn as the page is read:
  inline ones and files (`src`), not modules (`type="module"`; their
  `nomodule` fallbacks run). While a script runs, what comes after it in
  the page is not there yet, as in other browsers. Scripts see the page
  through the document's objects: `document` (`getElementById`,
  `querySelector` and `querySelectorAll` with CSS selectors,
  `createElement`, `write`, ...), each element's (`textContent`,
  `innerHTML`, `appendChild` and the rest, attributes, `classList`,
  `style`, `getBoundingClientRect`), `window`, `location` (setting it opens
  that page), `navigator` (`userAgent` is `Mozilla/5.0 (RitterOS; ARCH)
  iBrowser/27`), `localStorage` (kept while the page is shown, nothing on
  the disk), `document.cookie` (the browser's cookies, below; not the
  HttpOnly ones), `matchMedia` and `getComputedStyle` (a few values). The page is styled and laid out once its scripts are done.
  Events (since 27q): listeners (`addEventListener` with `capture`,
  `once` and `passive`), `on...` properties and `onclick="..."` attributes
  (their text runs as a function of `event` with `this` the element; a
  bare name in it is a global, not one of the element's or its form's),
  `dispatchEvent` with the standard's capturing and bubbling, and `Event`,
  `CustomEvent`, `MouseEvent`, `PointerEvent`, `KeyboardEvent`,
  `FocusEvent`, `InputEvent`, `WheelEvent` and `SubmitEvent`. The browser
  fires `DOMContentLoaded` once the scripts ran, the window's `load` once
  the pictures are in (and each `<img>`'s `load` or `error`), the pointer's
  and mouse's events (`pointerdown`/`mousedown`, `pointerup`/`mouseup`,
  `click`, `dblclick`, `contextmenu`, `mousemove`, `mouseover`/`mouseout`,
  `mouseenter`/`mouseleave`) on the element under the pointer, keys on the
  control with the keyboard or the body (`keydown` and `keyup` with `key`,
  `code` and `keyCode` as a US keyboard has them, `keypress` for typed
  characters), `focus`/`blur`, `input` and `change`, `submit` and `reset`,
  `wheel`, `scroll`, `resize` and `hashchange`. A listener's
  `preventDefault` stops what the browser would have done: the link is not
  followed, the check box or radio button goes back, the form is not sent,
  the key types nothing, the wheel does not scroll; a press whose
  `mousedown` is canceled neither focuses nor selects. `element.click()`,
  `focus()` and `blur()`, a form's `submit()`, `requestSubmit()` and
  `reset()`, a `javascript:` link, `scrollTo`, `scrollBy` and
  `scrollIntoView` work. Timers: `setTimeout` and `setInterval` (a
  function, or a string to run), `requestAnimationFrame` (about every 16
  ms), `requestIdleCallback` and `queueMicrotask`; they run between the
  window's events, on the system's clock of 10 ms ticks. After a listener
  or a timer changed the page it is styled, laid out and drawn again.
  Promises, `async` functions and generators work (27r); their jobs run
  after each script, listener and timer, as browsers run them. `Intl`
  (27s) formats numbers and dates, sorts and pluralizes in English, as
  `js` describes. Requests
  (27r): `fetch` (with `Request`, `Response`, `Headers`, and
  `AbortController`/`AbortSignal` to stop one), `XMLHttpRequest`
  (asynchronous only: a synchronous one would stop the window, so `open`
  with `false` throws), `URLSearchParams` as a body; each request is made
  on a thread of its own and its answer comes back as a job. As in other
  browsers: a page's requests to its own origin carry cookies; to another
  origin none (unless `credentials: "include"`), and its answer is read
  only if the server allows it (CORS: `Access-Control-Allow-Origin`, and an
  `OPTIONS` preflight first for other methods or headers); `mode: "no-cors"`
  gives an empty answer of status 0; an https page cannot fetch http, and
  no page can fetch `file:` paths; headers like `Cookie` and `Host` are not
  a script's to set. Bodies are text (`text()`, `json()`); no
  `arrayBuffer` or `blob` yet. `console.log` (and the other
  console methods) print `ibrowser: console: TEXT` on the standard output,
  `alert` `ibrowser: console: alert: TEXT`; with `-t`, an error that ends
  a script (or a listener, a timer, an `onclick="..."`) prints `ibrowser:
  script error: NAME: MESSAGE at URL:LINE` (a line of the script; an
  attribute's or a string timer's code has no URL), and nothing is shown
  in the window, as in other browsers.
- **Cookies** (since 27q): what sites set with `Set-Cookie` (also on a
  redirection) and scripts with `document.cookie` is kept while iBrowser
  runs, nothing on the disk, and sent back with every request to the site
  and path it belongs to (RFC 6265: `Domain`, `Path`, `Expires`,
  `Max-Age`, `Secure` only over https, `HttpOnly` hidden from scripts,
  which cannot replace such a cookie either; at most 300 cookies, the
  oldest go first). Quitting iBrowser forgets them.
- **Forms** (since 27c): a click or **Tab** gives a control the keyboard
  (Tab goes on to the next one, after the last back to the first). Text
  fields are edited like the address line; a password shows `*` for each
  character and a text area its lines (both type at their end,
  **Backspace** takes the last character back, in a text area **Enter**
  starts a new line). A click or **Space** ticks a checkbox or chooses a
  radio button (the others of its name go off); a list (`<select>`) opens
  on a click, its option chosen with a click (or **Up**/**Down** and
  **Enter** while it is open; Up and Down also change it closed). A
  submit button (a click, Space or Enter), or **Enter** in a text field or
  password, sends the form: by GET (its data after the address's `?`) or
  POST (as the request's body), urlencoded as every browser does; a reset
  button puts the form back as the page had it. Back shows the page
  before again, freshly loaded (what was typed into it is gone).
- **Parts of a page** (since 27b): an address ending in `#part` shows the
  page scrolled to the element whose `id` is `part` (or an `<a
  name="part">`); a link to another part of the page shown only scrolls
  (nothing is loaded again), and Back returns to where you were. A
  `#part` the page does not have shows its top.

What it shows: text with headings, paragraphs, line breaks, lists (bullets
or numbers), preformatted text, tables (since 27i their columns as wide as
their contents need, as other browsers make them: between each cell's
longest word and its unbroken lines, the room shared out in proportion;
`colspan`, `rowspan`, captions, the head's rows first and the foot's
last, cells as high as their row with their contents placed by
`vertical-align` (middle by default), `border-spacing` and
`border-collapse`, and the old attributes `border`, `cellpadding`,
`cellspacing`, `width`, `height`, `valign`, `align`), rules, and PNG and JPEG pictures (`<img>`; a picture that cannot
be loaded shows its `alt` text). Since 27e pictures come in the
background: the page's text is there to read, scroll and click at once,
and each picture is put in as it arrives; once all are in, the page goes
back to its `#part` (or the place Back returns to) unless you have
scrolled meanwhile. Styles come from the page's `<style>`,
its linked style sheets (`<link rel="stylesheet">`) and `style=""`
attributes, in CSS's order (a more specific selector wins, then the later
rule): colours and backgrounds, font size and weight, underline and
strike-through, margins, padding and borders, width and centring
(`margin: 0 auto`), text alignment, `display` (`none`, `block`, `inline`,
`list-item`), `white-space: pre` and list styles. Selectors: element
names, `.class`, `#id`, `*`, and descendants (`div p`) and children
(`ul > li`). Since 27d also floats (`float: left|right`, text flowing
beside them, `clear`), `display: inline-block`, and `position: relative`
(moved from its place), `absolute` (placed in its nearest positioned
ancestor, or the page), `fixed` (placed in the window: it stays while
the page scrolls) with `top`, `right`, `bottom`, `left` and `z-index`
(boxes painted in CSS's order: what has a higher z-index is on top, and
a click goes to what is on top); `overflow` other than `visible` makes a
box hold its floats. Since 27f the rest of the CSS pages commonly use:
`@media` queries, judged against the window (its width and height, with
`min-`/`max-` or as `(width >= 600px)`, `orientation`, `screen` and `all`
but not `print`; the page is styled again when the window changes size),
`@supports` (true for the properties iBrowser reads), `@layer` (its rules
as plain rules); custom properties (`--name: value`, inherited) and
`var(--name, fallback)`; `calc()`, `min()`, `max()` and `clamp()`, with
`vw` and `vh` of the window (a width such as `calc(100% - 20px)` keeps its
percentage); `!important`; `inherit`, `initial` and `unset`; and
`::before` and `::after` with `content` (text, `attr(NAME)`, quotes).
More selectors: attributes (`[type]`, `[type="text"]`, `~=`, `|=`, `^=`,
`$=`, `*=`, and `i` for any case), `:root`, `:first-child`,
`:last-child`, `:only-child`, `:nth-child(An+B)` and `:nth-last-child`
with their `-of-type` forms, `:empty`, `:not()`, `:is()` and `:where()`,
`:link`, `:checked`, `:disabled`, and the next (`h1 + p`) and later
(`h1 ~ p`) sibling. States that need a pointer, the keyboard or a
history (`:hover`, `:focus`, `:active`, `:visited`) never match. Since
27g flexible boxes: `display: flex` and `inline-flex` (and, since 27l, `display: contents`:
an element without a box of its own, its children in its place) with
`flex-direction` (rows and columns, either way round), `flex-wrap` (rows),
`flex-flow`, `flex-grow`, `flex-shrink`, `flex-basis` and `flex`,
`justify-content`, `align-items`, `align-self` and `align-content`
(start, end, center, stretch, and the space- forms), `gap` (`row-gap`,
`column-gap`), `order`, and auto margins (`margin-left: auto` pushes the
rest of a row to its end); an item shrinks no narrower than its longest
word unless `min-width` says otherwise. Also `box-sizing: border-box`
and `min-width`. Since 27h the look of boxes and text: borders by side
(`border-top`, `border-left-color`, ...: each side its width, style and
colour; transparent ones take their room), rounded corners
(`border-radius`, also on pictures), `box-shadow` (the first one, its blur
drawn as a lighter edge), colours with alpha (`rgba()`, `hsla()`,
`#rrggbbaa`) and `opacity` mixed with what is beneath, `visibility:
hidden`, `background-image` with `url(...)` (loaded like pictures) and
linear gradients (`to right`, angles to the nearest side; radial ones as
top to bottom), `background-size` (`cover`, `contain`, lengths),
`background-position` and `background-repeat`, `overflow: hidden` (the
contents cut at the box's edges), `line-height`, `vertical-align` (`sub`,
`super`, `middle`, `top`, lengths), `text-transform`, and an inline
element's background, borders and side padding. Since 27m `transform:
translate()` (and `translateX`, `translateY`, `translate3d`; pixels or
percentages of the element's own box: centring with `translate(-50%)`;
scaling and rotating are not drawn), percentage heights (of a containing
box whose height is known: a given height, a positioned box's container,
the window for the page), `min-height` and `max-height` (the latter for a
box that clips), an absolutely positioned box between its `top` and
`bottom`, and the logical `inset-inline-start`/`-end`,
`inset-block-start`/`-end` (left to right). Since 27n grids: `display:
grid` and `inline-grid` with `grid-template-columns` and `-rows` (lengths,
percentages, `fr`, `auto`, `min-content`, `max-content`, `minmax()`,
`fit-content()`, `repeat()` with a count or `auto-fill`/`auto-fit`),
`grid-template-areas` and `grid-area` names, `grid-template`,
`grid-column`/`grid-row` (lines, negative lines, `span`) and their
`-start`/`-end`, `grid-auto-flow` (row or column), `grid-auto-rows` and
`-columns`, `gap`, `justify-items`/`-self`, `align-items`/`-self`,
`justify-content`, `align-content`; the `:has()` selector (with `>`, `+`,
`~`); `white-space: nowrap`. Since 27j SVG drawings,
in the page (`<svg>`) and as files (`<img src="logo.svg">`, background
pictures): paths, rectangles (rounded too), circles, ellipses, lines,
polylines, polygons and text, groups and nested drawings with
`transform` and `viewBox`, `<use>` of an element or a `<symbol>`;
filled (`fill`, `fill-rule`, `fill-opacity`) and stroked (`stroke`,
`stroke-width`, `stroke-linecap`), styled by the page's CSS like the rest
of it (`currentColor` too), drawn with smooth edges. Text is drawn (since
27k) in the Adobe faces of `/system/fonts`, chosen by `font-family`: Times
for serif faces and by default (as other browsers), Helvetica for sans
faces (Arial, Helvetica, Verdana, system-ui, ...; also an unknown web font),
Courier for monospace ones (`pre`, `code`, ...), at the size nearest the
font size (11, 14, 17, 20, 25 or 34 pixels; larger at twice a smaller one),
bold and italic as asked; `font-family: fixed` gives the misc-fixed faces
(and they are used when the Adobe files are missing). Web fonts
(`@font-face`) are not loaded.

Not (yet): file uploads (`<input
type=file>` is a plain button), `multipart/form-data` (such forms are sent
urlencoded), frames,
web fonts, subgrids, dense grid packing,
`baseline` alignment (taken as the start), transparency of pictures
over anything but the page's background, more than one background layer
or shadow (the first is drawn), dashed and dotted borders (drawn solid),
SVG gradients, patterns, clipping, masks, filters, markers and dashes (a
gradient's fallback colour, else grey, is drawn instead),
`position: sticky` (it is relative), scrolling inside a
box (`overflow` does not clip), and images other than PNG and JPEG.
Addresses are fetched like `fetch` fetches them: redirections followed (up
to 5), `https://` checked against the system's root
certificates; since 27b iBrowser also takes answers packed with gzip or
deflate (it asks for them with `Accept-Encoding: gzip, deflate`), as most
web servers send them.

`-t` also prints each page's text as it is laid out, and its links with
their places, on standard output (`ibrowser| ...` and `ibrowser: link N at
X,Y: URL` lines): the tests read them. Every page shown prints
`ibrowser: loaded URL (STATUS, TYPE, N bytes)` (a packed one `..., N
bytes, gzip from M)`, M the bytes that came); one that cannot be had,
`ibrowser: URL: why` (the window shows the reason too; a packed answer
that does not unpack: `the page's gzip data is damaged`). A scroll to a
`#part` (or back to a place) prints `ibrowser: scrolled to y N for
#part`, a missing one `ibrowser: #part is not on the page`; a scroll by
the reader (keys, wheel, scroll bar) `ibrowser: scrolled by hand to y N`.
The page's report (its text, links and controls) comes once its pictures
are in. Each form
control is listed with its place and state (`ibrowser: control N at X,Y
WxH: [text: q=VALUE]`; `[password: pw=N characters]`, `[checkbox:
NAME=VALUE on|off]`, `[select: NAME=OPTION]`, `[submit: LABEL]`, ...), a
change as `ibrowser: now [...]`, and a form sent as `ibrowser: sending
GET URL` or `ibrowser: sending POST URL (N bytes): DATA`.

---

## 10. Special directories

| Directory | Holds |
|---|---|
| `/home/NAME` | each user's home, owned by the user (`755`: others may look, not change; root's own home is `700`: private) |
| `/system/config/sudo` | who may use `sudo`: `users = NAME, NAME` (root's; there from the start with `users =` empty: only root) |
| `/home/NAME/.config` | the user's own settings (`700`), made at their first login at the machine: `login` (commands run at a console login), `windows` (the window system's, over `/system/config/windows`); a README explains them |
| `/system/<arch>/bin` | the programs (`<arch>`: `x86_64` or `aarch64`) |
| `/system/<arch>/kernel.elf` | the kernel the bootloader starts |
| `/system/<arch>/test` | test programs for the automated tests |
| `/system/<arch>/lib` | the shared libraries and `ld.so`, the dynamic loader (section 9) |
| `/system/fonts` | the window system's fonts (BDF files; TrueType and OpenType since 41c) |
| `/system/certs` | `cacert.pem`: the root certificates HTTPS trusts (Mozilla's list, section 8, `fetch`) |
| `/system/config/services` | (since 53d) the service names programs know (`getaddrinfo`, `getservbyname`): `name = port` lines, the same port for TCP and UDP (`http = 80`, `ssh = 22`, ...); a new disk gets a short list, root may add to it (`edit /system/config/services`) |
| `/system/config/cpu` | (since 64t) the processor's clock, read by the kernel at the start (a change counts from the next start): `clock = performance` (the processor chooses, favouring speed; the default, also without the file), `balanced`, `powersave`, `top` (its highest clock whenever it runs: fastest, more power while idle) or `firmware` (the firmware's own settings). Used on AMD processors with CPPC; elsewhere the boot log says it is not used. The file's comments list the values and what they measured on the laptop; `threads` shows the clock while running |
| `/system/config/power` | (since 37c-2a) power management, read by the kernel at the start (a change counts from the next start): `power_button = off` (a press switches the machine off as `poweroff` does; the default, also without the file), `nothing` (the press is only logged) or `sleep` (since 37g-7: the machine sleeps, the next press wakes it); (since 37c-2c) `lid = nothing` (closing the lid is only logged; the default) or `sleep` (since 37g-7); (since 37e) `screen_off = MINUTES` without input until the screen goes off (5 by default, 0 never) and `screen_dim = MINUTES` (2 by default; the panels dim to a third); (since 37g-7) `sleep_after = MINUTES` without input (typing over SSH counts) until the machine sleeps (0, the default: never). `power` shows the settings in use |
| `/system/config/network/virtual` | (since 38c) the virtual cards and bridges made at every start: `tapN = user NAME` and `brN = PORT...` lines, changed by `net tap` and `net bridge` (only their own lines), read by `net up` (section 8, "Virtual cards" and "Bridges") |
| `/system/config/swap` | (since 35a) whether swap is used: `on = yes` (or `no`, the default), changed by `swap on`, `swap off` and `swap make` (since 37e-1b only that line: the comments stay), read by the kernel at the start; without the file swap is off |
| `/system/swap` | (since 35a) the swap file, made by `swap make SIZE`: a fixed file only the kernel writes, encrypted (`swap`) |
| `/system/config/time` | the local time zone (22o): `zone = -05:00`, a fixed offset from UTC (+HH:MM or -HH:MM; none or 0: UTC), for programs' local time (`localtime`, `strftime`); no daylight saving rules; `zone = 0` when login writes it (37e-1b). Root's to edit (`edit /system/config/time`) |
| `/system/config/wifi` | the wireless link: `networks/SSID` per saved network (root only: `ssid`, `passphrase`, `security`, `automatic`) and `radio` (`country = 00`, `powersave = off` when wifid writes it, 37e-1b); the elf `wifid` reads them |
| `/system/config` | the system's settings, one plain text file per topic: `users` and `passwords` (written by `useradd` and `passwd`; the passwords file is root's only), `network/` (a file per network card, made by `net up`), `routing` and `elves/` (a file per elf, written by `elf`); a `README` explains them. Files of settings are `key = value` lines, `#` starts a comment line. (Since 37e-1b) whoever reads a settings file writes it when it is missing, every setting at its default with comments saying what it can be (a setting that is not set by default, such as `routing`'s, a line to remove the `#` from): the kernel writes `cpu`, `power`, `displays` and `swap` at the start, after its check of the disk; login `time`, `services` and `sudo`; `net up` `routing` (on a machine with a network card) and (since 38c) `network/virtual`; `wifid` `wifi/radio`. A new system gets them at its first start, and a file you delete comes back the next time it is looked for: change a value there instead of writing a file from memory. Settings that are a user's own go in the user's `~/.config` (windowd's `windows`); `/system/config/windows` is an optional file for everyone that nothing writes. A disk from before keeps them in `/system`: `login` moves them here at the next start |
| `/system/logs` | the logs, one plain text file per program, `NAME.log`, each line with its date and time (UTC) in front: `kernel.log` (the kernel's messages from the first boot line on), `login.log` (logins, refused logins and logouts, on the console and over SSH), and one per elf with what it printed (`elvesd.log`, `dhclient.log`, `sshd.log`, ...). The elf `logd` writes them every 10 seconds; a log that grows past 64 KiB becomes `NAME.log.old` (the one before it goes) and a new one begins. Read them with `cat` |
| `/test` | files the kernel's self-test checks |
| `/.snapshots/NAME` | snapshot NAME, read only (section 5) |
| `/volumes/NAME` | other disks' and sticks' file systems (section 7) |
| `/boot` | the system disk's EFI system partition (FAT32: the bootloaders and `ritteros/cmdline.txt`), read only |
| `/dev/NAME` | the disks and partitions (section 7): root may read and write them byte by byte, except where a mounted file system lies |
| `/dev/null` | (since 52c) everyone's: reading it gives nothing (the end at once), whatever is written to it is gone: `command > /dev/null` hides a command's output |
| `/dev/zero` | (since 52c) reads give zero bytes, as many as asked (`head -c 4096 /dev/zero > empty.bin`); writes are gone, as with /dev/null |
| `/dev/tty` | (since 52c) your terminal, whatever a program's input and output are: `sh -c 'echo seen > /dev/tty' > /dev/null` still shows `seen`. Without a terminal (a command run by `ssh HOST CMD`, an elf) it cannot be opened: `Device not configured` |
| `/dev/usbserialN` | (since 50e) a USB serial port (CDC ACM: a device's console such as the PlutoSDR's, a modem): what is written goes to the device, reads give what it sent (also what came before the port was opened, up to 16 KiB). Root's and the console user's, as the sound devices. `cu usbserial0` is a terminal on it |
| `/dev/tapN` | (since 38a) the wire of the virtual network card `tapN` (`net tap add`): a program that opens it reads each Ethernet frame the system sends through the card and writes the frames the card receives, one per `read` or `write`. Root's and the card's user's, one program at a time (section 8, "Virtual cards") |

`/volumes`, `/dev` and `/boot` are not on the disk: the kernel makes them
up, and `ls /` lists them after the disk's own entries. `ls /dev` lists
the disks and partitions, then `null`, `zero`, `tty`, the sound
devices `pcm0`, `pcm1`, ... (since 52l), the USB serial ports
`usbserial0`, `usbserial1`, ... (since 50e) and the virtual network cards'
`tap0`, `tap1`, ... (since 38a; `ls -l` shows `b` for a disk, `c` for the
others). `/.snapshots`
is made up too, but hidden, as names starting with a dot are: type its
name (or use `snapshot list`).

---

## 11. The rescue console

When `login` ends (Ctrl-D at `login:`) or cannot start, or with `rescue`
on the kernel command line, the kernel's own console takes over, with the
prompt `rescue>`. It only checks; it never shows or changes what files
contain.

| Command | Does |
|---|---|
| `help` | list the commands and the programs |
| `uptime`, `mem` | time since the timer started; free and usable memory |
| `ls [PATH]` | list a directory |
| `pci`, `usb` | the PCI and USB devices |
| `mouse` | the mice and where each pointer is (a quick way to check a mouse works) |
| `disks` | the disks and partitions, as the program `disks` shows them |
| `net` | the network cards, as the program `net` shows them |
| `threads`, `ps` | the kernel's threads, the processes |
| `cpus` | the processor's cores, as the program `cpus` shows them |
| `drivers` | the kernel's modules, as the program `drivers` shows them |
| `run PATH [ARGS]`, or a program's name | run a program (for example `sh` or `login` to go back) |
| `poweroff`, `reboot` | switch off, restart |

---

## 12. The kernel command line

Options for testing and for difficult hardware, in
`\ritteros\cmdline.txt` on the EFI system partition (`make run
CMDLINE="..."` writes it; in RitterOS, `cat /boot/ritteros/cmdline.txt`
shows it). The everyday ones:

| Option | Effect |
|---|---|
| `video=WxH` | screen mode, e.g. `video=1024x768` (default: the firmware's) |
| `rescue` | start the rescue console instead of `login` |
| `noruntime` | skip the firmware's runtime services (for firmware that hangs there) |
| `selftest` | run the large self-tests at the boot (slower) |
| `lockcheck` | check the order the kernel takes its locks in (for kernel work; `threads` reports it) |
| `nomodules` | load no kernel module (drivers kept as files in `/system/<arch>/drivers/`), for a machine where one misbehaves |
| `cpus=N` | start at most N of the processor's cores (every core is started otherwise, and all run programs; `cpus=1`: only the core the machine starts with, to take several cores out of the picture when something misbehaves) |
| `memtest` | test every free page of memory |
| `memory=N` | (since 35b) use only N MiB of memory (at least 64), to see swap at work on a machine with plenty; the boot's `memory:` line says so |
| `scrub` | verify every copy of every block on the disk and repair damaged ones |

The full list, including the write tests and deliberate crashes the
automated tests use, is in [`README.md`](../README.md#kernel-command-line).

---

## 13. Messages

Programs report errors as `program: what: reason`, for example
`mkdir: /work: File exists`. The reasons:

| Reason | Means |
|---|---|
| `No such file or directory` | the path does not exist (check spelling and the current directory) |
| `File exists` | something with that name is already there |
| `Is a directory` / `Not a directory` | a directory where a file was expected, or the other way round |
| `Directory not empty` | `rmdir` of a directory with something in it (use `rm -r`) |
| `Permission denied` | the permissions do not allow it (section 6) |
| `Operation not permitted` | only root may do this |
| `Read-only file system` | a snapshot, `/dev` or `/volumes` itself, `/boot` (or a volume mounted read only): nothing can be changed there |
| `Operation not supported` | the file system cannot do that (attributes on FAT) |
| `Network is unreachable` | no address (`net up`), no link, or no gateway for that network |
| `No route to host` | nobody in the local network has that address |
| `Timed out` | no answer in time |
| `Device or resource busy` | in use: a mounted volume, or one a program is in; in `/dev`, the blocks of a mounted file system; `mount` while a program writes to the disk (`disks`, `format`) |
| `Invalid cross-device link` | a rename between two volumes (`mv` copies instead) |
| `Invalid argument` | for example moving a directory into itself, or a bad name (on FAT also one with `\ / : * ? " < > |`) |
| `No space left on device` | the disk is full |
| `Input/output error` | the disk could not be read or written (or a block failed its checksum) |
