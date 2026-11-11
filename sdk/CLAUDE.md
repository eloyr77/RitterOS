# Developing programs for RitterOS (the SDK)

This folder is how a person or an AI agent on their own computer writes a
program for RitterOS in a repository of their own, builds it, copies it over
SSH to a PC running RitterOS, and runs it there. It is written for both
readers: an agent pointed at this file should be able to follow it from the
top, without asking.

RitterOS is a from-scratch operating system, not Linux, but with much of
POSIX: its own C library (all of C11's, and fork and exec, pipes, poll,
termios, BSD sockets, pthreads, regex and more) is in shared libraries
(`libritteros.so.1` and others), and its programs are ELF files linked
against them. This folder gives you the same build its own programs use,
run on your computer. RitterOS can also build programs itself, with its
own `cc` and `make` (see "Building on the machine").

What is here:

| Path | What |
|---|---|
| `sdk/CLAUDE.md` | this guide |
| `sdk/API.md` | every header, function and macro a program can use, made from the headers themselves |
| `sdk/ritteros.mk` | the build: a makefile fragment your project includes (build, deploy, run, stop) |
| `sdk/template/` | a new project to copy: `Makefile`, `hello.c`, its own `CLAUDE.md` |
| `sdk/examples/hellowin/` | a windowed app (window, keys, clicks, resize) |
| `sdk/examples/widgets/` | a windowed app made of libwidget's controls (since 41e) |

## Quick start (an agent can do these in order)

1. On your computer you need: `git`, GNU `make`, Python 3, an OpenSSH client
   (`ssh`, `ssh-keygen`), and **Zig 0.17.0** (its clang and lld are the cross
   compiler; download it for your platform from https://ziglang.org/download/
   and put `zig` on your PATH, or pass `ZIG=/path/to/zig`). QEMU and the rest
   of RitterOS's own toolchain are not needed.
2. Get the RitterOS source next to where your project will be:
   `git clone https://github.com/eloyr77/RitterOS.git`.
3. Start your project from the template, as a repository of its own:
   ```sh
   cp -R RitterOS/sdk/template myapp
   cd myapp && git init
   ```
4. In `myapp/Makefile`, set `APP` (the program's name), `KIND` (`bin` for a
   command-line program, `apps` for a windowed one) and `SRCS`. Rename
   `hello.c` to fit. `RITTEROS` defaults to `../RitterOS`.
5. `make` builds `build/x86_64/APP` (the first time it also builds RitterOS's
   library, in RitterOS's own tree).
6. Set up the RitterOS machine once (below): its network, `sshd`, your key.
7. `make deploy run HOST=root@ADDRESS` copies the program over and runs it;
   its output and exit code come back to you.

## Your project

The template's `Makefile` is all a project needs:

```make
RITTEROS ?= ../RitterOS
APP      := hello
KIND     := bin
SRCS     := hello.c
CFLAGS   += -Werror
include $(RITTEROS)/sdk/ritteros.mk
```

`APP` is the program's name on RitterOS, `KIND` is `bin` (command line) or
`apps` (windowed), `SRCS` its C sources (several are fine). Keep comments on
lines of their own: in make, `APP := hello  # name` makes the spaces before
`#` part of the value.

| Command | Does |
|---|---|
| `make` | build `build/ARCH/APP` |
| `make deploy HOST=root@ADDRESS` | copy it to `/system/ARCH/KIND/APP` there and make it executable |
| `make run HOST=... [ARGS="a b"]` | run it there; its output and exit code come back |
| `make stop HOST=...` | end it there (finds it with `ps`, ends it with `kill`) |
| `make clean` | remove `build/` |

`ARCH` is `x86_64` (the default: every real PC RitterOS runs on today) or
`aarch64` (RitterOS on QEMU's ARM machine). `SSH_OPTS` passes ssh options,
e.g. `SSH_OPTS="-i ~/.ssh/ritteros"` or `-p 2222`. `HOST` and `SSH_OPTS` may
be set in the environment once instead of on every command. ssh is run with
`BatchMode=yes` (it never waits for a typed password: without a working key
it fails at once) and `StrictHostKeyChecking=accept-new` (it trusts a
machine's host key the first time).

**Linking.** By default (`LINK = shared`) your program uses the machine's
shared libraries in `/system/<arch>/lib` (`libritteros.so.1`, and
`libwindow.so.1`, `libimage.so.1`, `libaudio.so.1`, `libvideo.so.1`,
`libdraw.so.1`, `libwidget.so.1`, `libtls.so.1` if it calls them), loaded by `ld.so` when it starts: a small file, and the
library code is shared with every other program there. `LINK = static`
links it with the archive `libritteros.a` instead: a bigger file carrying
the code it uses, independent of the machine's libraries.

After pulling a newer RitterOS, `make` rebuilds your program against it (it
runs `make sdk` in the RitterOS tree, which does nothing when it is up to
date). **When the machine's libraries are updated,** your program needs no
rebuild: a `.so.1` library stays compatible (`scripts/abicheck.py` keeps it
so), so it runs with the new one at its next start. A program built against
a newer RitterOS than the machine runs may need a function the machine's
libraries lack: `ld.so` then stops it at its start with `ld.so: NAME: NAME
needs FUNCTION, which none of its libraries has` (exit 127), and `ssh HOST
'ldd NAME'` lists every such function without running it. Update the
machine, or build against its version.

## Setting up the RitterOS machine (once)

A person at the machine does this; afterwards an agent needs nothing there.

1. **The system.** Boot RitterOS (a stick, or installed with `osinstall`).
   The first start asks for root's password.
2. **The network.** Log in as root at the machine. With a network card
   (connected to a network with DHCP), logging in runs `net up` by itself:
   it configures the card and starts the elves (background programs) for
   names, the firewall and **sshd**, the SSH server on port 22 (the firewall
   lets port 22 in). `net` shows the machine's address
   (`eth0 ... 192.168.1.50/24`). See `docs/manual.md` ("The network", "sshd").
3. **Your key**, for the user you work as (root, or your own user: see
   the next step). RitterOS's sshd takes **Ed25519 keys only**. On your
   computer, with USER that user's name:
   ```sh
   ssh-keygen -t ed25519 -f ~/.ssh/ritteros -N ''
   ssh USER@ADDRESS 'mkdir -p .ssh'
   ssh USER@ADDRESS 'cat >> .ssh/authorized_keys' < ~/.ssh/ritteros.pub
   ```
   (An SSH command runs in that user's home.) The last two ask for that
   user's password (a person types it; never put a password in a file or a
   chat). `ssh-copy-id` does not work: it runs a POSIX shell script
   RitterOS's shell cannot. Check that the key works:
   `ssh -i ~/.ssh/ritteros -o BatchMode=yes USER@ADDRESS uptime`.
   Then `export HOST=USER@ADDRESS SSH_OPTS="-i ~/.ssh/ritteros"`.
4. **Working as your own user** (41h-7) instead of root: root adds the user
   (`useradd dev`, `passwd dev`) and lets it use sudo, in
   `/system/config/sudo`:
   ```
   users = dev
   nopassword = dev
   ```
   `make deploy` writes under `/system` through sudo, in one ssh call.
   `nopassword` (root's choice) lets that user's sudo run without a
   password, so an agent deploys with nothing typed (whoever has the key
   has root then: keep the key safe). Without it `make deploy` asks the
   developer for the password once, hidden, and hands it to sudo; sudo
   remembers a password 5 minutes only within one session, and every `ssh
   HOST COMMAND` is a session of its own. Without `nopassword`, a deploy
   with nothing typed (no terminal: an agent) stops with `make deploy: HOST
   needs a password for sudo ...` rather than going on. `make deploy` run
   on the machine itself over ssh (`ssh HOST 'cd hello && make deploy'`)
   has sudo read ssh's input, not a terminal: use `ssh -t` (a terminal to
   type the password in) or `nopassword`.
5. **For windowed apps,** the window system must be running on the
   machine's screen. Two different things here:
   - *Starting* the window system: at the machine itself, by a user logged
     in there, `windowd &` (it opens the file browser `owl`; the desktop
     stays up until you quit it from its right-click menu), or as the elf
     `windowd`. Over SSH only root may start it (and then gets every key
     typed at the machine); another user over SSH is refused (`windowd:
     cannot take the screen: Operation not permitted`), as the screen's
     owner sees what the person at the keyboard types.
   - *Using* it from SSH works: once windowd runs, a windowed app started
     over SSH (`make run`) opens its window on that screen, provided it runs
     as windowd's own user or as root (so apps deployed and run as root over
     SSH always appear). Another user's are refused (`window_open` fails
     with EACCES). The sdk test does exactly this with hellowin.

The first connection shows the machine's host key fingerprint; sshd prints
the same one on the machine's screen when it makes the key. A reinstalled
machine has a new key: `ssh-keygen -R ADDRESS` forgets the old one.

**Developing against QEMU instead:** in the RitterOS tree, `make run` boots
RitterOS in QEMU with the guest's port 22 at the Mac's port 2222: use
`HOST=root@localhost SSH_OPTS="-p 2222 -i ~/.ssh/ritteros"` (and the same key
setup, with `-p 2222`). `make run ARCH=aarch64` is the ARM machine: build
with `ARCH=aarch64` for it.

## Deploy, run, stop

What the make targets do, as plain ssh commands (for one-off use, or a
project without make):

```sh
ssh root@ADDRESS 'cat > /system/x86_64/bin/hello' < build/x86_64/hello   # copy in
ssh root@ADDRESS 'chmod 755 /system/x86_64/bin/hello'                    # executable
ssh root@ADDRESS 'hello agent'                                           # run
ssh root@ADDRESS ps                                                      # what runs
ssh root@ADDRESS 'kill 23'                                               # end process 23
ssh root@ADDRESS 'cat /system/logs/kernel.log' > kernel.log              # copy a file out
```

As your own user (41h-7), what writes under `/system` goes through sudo:
`ssh dev@ADDRESS 'sudo sh -c "cat > /system/x86_64/bin/hello"' < build/x86_64/hello`
(with `nopassword`; else sudo reads your password as the first line of
its input, which a person types: `make deploy` does that for you).

A command without a terminal (`ssh HOST COMMAND`) runs on raw pipes:
standard input, output and error pass bytes unchanged, so binaries and data
files copy cleanly in both directions, stderr stays apart from stdout, and
the command's exit code becomes ssh's.

- **Where programs go:** command-line programs in `/system/<arch>/bin`,
  a single file each; (42b) a windowed app is a bundle, the folder
  `/system/<arch>/apps/APP.program/` holding the program `APP`, its file
  `info` and its `icon.png`. The shell, windowd, `open` and the program
  search (`execvp`) find a bare name in `bin`, then as `APP.program/APP`;
  `owl` shows a bundle anywhere as one item with its icon and runs it.
  Writing there needs root. `KIND = apps` makes `make deploy` build the
  bundle: the program, and the project's `APP.info` and `APP.png` (if
  there) as `info` and `icon.png`, and removes an old flat
  `/system/ARCH/apps/APP` from before bundles (42b). Nothing else moves
  old apps: a machine updated in place by copying files keeps its flat
  `apps/NAME` programs and 41g-6's `.NAME.info`/`.NAME.png` files beside
  the new bundles; delete them there (a machine installed with
  `osinstall` has only bundles). The desktop menu (a right-click on the
  desktop's background) lists the bundles in apps; `info` is `key = value`
  lines as the settings files':

  ```
  name = Sketch pad
  about = draws with the mouse
  types = skp
  ```

  `name` is the menu's label (APP without it), `about` one line about it,
  `types` the file endings it can open (42a: which program a double click
  starts is the user's choice, `~/.config/filetypes`, `ENDING = PROGRAM`;
  tell users the line to add, as `skp = sketch`); (42c) owl's Open
  with... offers the app for every file whose ending is in `types`
  (`window_app_opens(bundle, file)` asks the same).
  `window_app_for(path, buf, size)` asks that list (APP into buf; 0 an
  app, run in its window; 1 a program in bin, run in a terminal; -1
  none). (41g-3) `icon.png` is 32x32 with transparency, shown at half size
  in the menu, in owl and on the taskbar buttons of the program's windows
  (windowd knows a window's program by `getprogname()`, the last part of
  its argv[0]).
- **Notifications** (41g-4): `window_notify(win, "Backup", "done: 3 files")`
  shows a box at the top right for 5 seconds, over every window, the title
  in bold (0 for none) and the text wrapped below (up to 511 bytes). `win`
  may be 0: a program without a window (a command-line tool, a script's
  `notify TEXT...`) reaches windowd by a connection of its own. It returns
  -1 when windowd is not running; nothing comes back when the box goes.
- **Testing a windowed program without the screen** (41h-1): a headless
  window server of your own, over SSH, beside the machine's:

  ```sh
  export WINDOW_SOCKET=/tmp/test1           # where it listens; libwindow connects there
  windowd headless 800x600 & w=$!          # no screen, no keyboard or mouse
  wsend wait                                # (41h-6) until it answers
  ./hello & wsend wait hello                # its window, in the headless one (by its title)
  wshot /tmp/hello.png                      # (41h-2) its screen as a PNG
  wsend click 200 150; wsend type hi        # (41h-3) a click and keys, as from devices
  wsend key enter; wshot /tmp/after.png     # (each handled before wsend returns)
  wsend down up; wsend down ctrl            # (41h-5) a game: walk forward, fire held
  wsend up ctrl; wsend up up                # (each key held until its `up`)
  kill $w                                   # the headless one ends
  ```

  `wsend key|down|up NAME` knows a-z, 0-9, space, enter, tab, escape,
  backspace, the arrows, home, end, pageup, pagedown, insert, delete,
  ctrl, shift, alt and f1-f12: a game reading physical keys
  (`WINDOW_KEY_DOWN`/`_UP`) gets them as from a keyboard.

  Copy the picture to your computer and look at it, or compare pixels in
  a test (`scp`, or `ssh HOST cat /tmp/hello.png > hello.png`).

  Every program started with that `WINDOW_SOCKET` uses it; without it,
  `/run/windowd` (the screen's). The headless one starts no `startup`
  program.
- **New files are made with mode 0644.** Root passes every permission check
  in RitterOS, so root can run such a file anyway, but other users cannot
  (`sh: hello: Permission denied`), and `owl` opens a non-executable file in
  the editor instead of running it: chmod 755 once after the first copy
  (`make deploy` always does). Copying over an existing program keeps its
  mode. Overwriting a program that is running is safe: it was loaded into
  memory.
- **A command-line program** runs to its end. `ssh HOST 'cmd'` returns its
  exit code as ssh's own; `make run` shows a non-zero one as
  `make: *** [run] Error N` (and fails).
- **A windowed app** opens on the machine's screen, and the ssh session stays
  open, bringing its output, until the window closes (its X, or `make stop`).
  Run it in the background on your side (`make run HOST=... &`, or your
  agent's background mode) and read its output as it comes.
- **Exit codes** from `ssh HOST 'cmd'`: the program's own; 128 + a signal
  when one ended it (143 after `kill`, 137 after `kill -KILL`, 139 a
  segmentation fault); 127 when there is no such program (`sh: NAME: command
  not found`); 126 when it is there but cannot start (`sh: NAME: Exec format
  error`: built for the other architecture; `sh: NAME: Permission denied`: a
  user other than root, and no x bit). At its prompt the shell also prints
  `NAME: exit code N` for a line whose last command failed, and `NAME:
  stopped by signal N` (or `NAME: killed`) when a signal ended it; over
  `ssh HOST 'cmd'` (sh -c) it prints neither (51a): the exit code is ssh's.
- **A crash** (a page fault and the like) is reported by the kernel on the
  machine's screen and in its log, not on the ssh session:
  `ssh HOST 'cat /system/logs/kernel.log'` shows the line (the log is written
  every 10 s), as `NAME: page fault (...) at pc 0x...` with the program's
  code address. Programs are placed at a random address (22l), so the line
  also gives the address as linked: `(0x... as linked)`; a disassembler of
  your computer's (`llvm-objdump -d`, or binutils' `objdump -d`, on
  `build/x86_64/NAME`) shows which function that is in.

## Building on the machine

RitterOS has its own C compiler, assembler, linker and archiver (`cc`, `as`,
`ld`, `ar`, since 60) and `make` (59), and it rebuilds itself with them,
byte for byte as RitterOS's own build on a Mac does (63b). So a project can
also be built on the machine itself, without Zig and without a RitterOS
checkout: since 63f the machine has this SDK in `/system/sdk`
(`ritteros.mk`, this guide, `API.md`), and the template's Makefile takes
`/system/sdk/ritteros.mk` when there is no checkout. The same Makefile then
builds with the machine's `cc` and `ld`, for the machine's own
architecture (another `ARCH` is refused), shared or static (`LINK`), and
`make deploy`, `make run` and `make stop` work on that machine itself (no
`HOST`).

The machine's `cc` makes slower code than the SDK's clang, since items 39
and 40 (39a-40e) 0.9 to 1.8 times clang's time on the J1900: a float dot
product 1.8 ns an element against clang's 1.7, a 19-tap FIR filter on
complex floats 57 ns a sample against 47, a rotation through a `static
inline` complex multiply 15 against 14, an int16 FIR 29 against 32, saxpy
1.4 against 0.8. Build with the SDK what must be fast. On x86-64 cc packs simple
loops into SSE2 (4 ints or floats, 8 int16 products a pass): a loop over
`a[i]` (i an int, its bound read once before it: not a volatile in the
test), element-wise arithmetic or an int sum; float sums stay in order. On x86-64 floats and doubles stay in registers except across a call,
which keeps no xmm register (an inner loop without calls gains the most);
small `static` functions are inlined, structures too, and small local
structures live in registers.

A whole project goes there in one ssh call with `tar` (since 63h; on a Mac
`COPYFILE_DISABLE=1` keeps Apple's `._` files out), and comes back the same
way:

```sh
COPYFILE_DISABLE=1 tar cf - hello | ssh HOST 'tar xf -'   # (into the user's home)
ssh HOST 'cd hello && make deploy run ARGS=there'
ssh HOST 'tar cf - hello' | tar xf -                      # (its changes back)
```

`tar` keeps the files' times (since 63m), so a project sent again builds
only what changed: a file whose time is older than its object is not
compiled again. The times are your computer's clock: if the machine's
clock runs ahead of it, an edited file can arrive "older" than the object
built before and be skipped. Then take it apart with `tar xmf -` (each file
gets the machine's time now: everything sent is built again), or set the
machine's clock. `make` goes by whole seconds.

One file alone also goes with `cat`: `ssh HOST 'cat > hello/hello.c' < hello.c`
(it gets the time now, so it is built again).

A Makefile made before 63f includes `$(RITTEROS)/sdk/ritteros.mk` only:
take the template's lines (`RITTEROS_MK := $(firstword $(wildcard ...))`).
Without the SDK, `cc` alone builds a program too:

```sh
cc -o hello hello.c                  # linked with libritteros.so.1, like the system's programs
cc -o hellowin hellowin.c -lwindow   # and another of the system's libraries
```

`cc` reads RitterOS's headers from `/system/include` (the same headers as
`RITTEROS/user/include` here) and takes C11 with the GNU extensions
RitterOS's own sources use (`__attribute__`, statement expressions,
`typeof`, inline assembly, ...); `-O0` switches its optimizations off,
other `-O...`, `-W...` and `-g` are taken and have no effect; `-MD`/`-MMD`, `-MP`, `-MF` and `-MT` write make's dependency
files (since 63g; `ritteros.mk` passes `-MMD -MP`, so a changed header
rebuilds the objects that include it). The manual's "cc" section lists the
rest. The machine's `make` runs one job at a time (no `-j`).

## The remote shell

`ssh HOST 'COMMAND'` runs `sh -c COMMAND` in the logged-in user's home
(`/home/root` for root, `/home/dev` for dev). The
shell is small:

- works: `;` between commands, pipes `|`, `&&` `||`, `!`, `$?`, `<` `>` `>>`
  redirection with `2>` and `2>&1` (since 51a), `*` patterns, quotes, a
  trailing `&` (a pipeline), `$NAME` and `${NAME}`, `NAME=VALUE`, `export`,
  `unset`, `cd`, `pwd`, `exit`, `help` (every program and what it does).
- works too (since 51b): `$(...)` and backquotes, `$((...))`, `$$ $! $# $0
  $1... $@`, `~`, the patterns `? [abc] [!x]`, `\` quoting; unquoted
  expansions are split into words at spaces (quote them to keep them whole).
  A pattern's quoted parts are plain text, the rest still a pattern:
  `"$dir"/*.c` (since 68h) and `case "$x" in "pre-"*)` (since 68g). A
  machine installed before them expands no word with a quote in it and
  takes a partly quoted case pattern as plain text: a script meant for any
  machine writes `$dir/*.c` and `pre-*)` unquoted.
- works too (since 63i): here-documents, `<<WORD` and `<<-WORD` (quoted
  `<<'END'`: the lines as they are), so one `ssh HOST sh < script` can write
  several files (`cat > F <<'END' ... END`); for many files, or binary ones,
  `tar` is the way.
- works too (since 51c): `if`/`elif`/`else`, `while`, `until`, `for`,
  `case`, functions, `{ }`, `( )`, `break`/`continue`/`return`, `set -e -u
  -x -f --`, `shift`, `read`, `eval`, `.`, `exec`, `#` comments, scripts
  (`sh FILE ARGS`, or a file made executable with `chmod 755`: with a
  `#!/system/<arch>/bin/sh` line the system runs it, without one the shell
  does, since 52g; since 57b `#!/bin/sh` and `#!/usr/bin/env` work too:
  `/bin`, `/usr/bin`, `/lib` and `/etc` are links into `/system`, and
  `/tmp` is everyone's, sticky).
- programs for scripts (since 51d): `true`, `false`, `test` and `[`,
  `printf`, `sleep` (fractions too), `date [-u] [-d @SECONDS] [+FORMAT]`,
  `basename`, `dirname`.
- text commands (since 51e): `head`, `tail` (`-f` too), `wc`, `sort` (`-k`,
  `-n`, `-h`, `-V`, ...), `uniq`, `cut`, `tr`, `tee`, `xargs`; `grep -E`
  (since 68f also POSIX's `-q`, `-s`, `-x`, `-F`, `-e`, `-f`: a machine
  installed before has none of them, so a script for any machine tests
  with `case` instead of `grep -q`) and `sed` (POSIX's, with `-E` and `-i`) and `awk` (POSIX's) since 58;
  `make` (POSIX's with GNU make's common features) since 59, and a C
  compiler since 60: `cc`, `as`, `ld` and `ar` (see "Building on the
  machine").
- `/dev/null`, `/dev/zero` and `/dev/tty` (since 52c, listed by `ls /dev` since 52l; `/dev/tty` cannot be
  opened in an `ssh HOST CMD` command, which has no terminal: ENXIO).
- `/dev/tapN` (since 38a): the wire of a virtual network card, for a program that
  carries Ethernet frames itself (a VPN, a simulated network). Root makes the card
  (`net tap add [user NAME]`, or `ritteros_tap(TAP_ADD, uid)`); the program opens
  `/dev/tapN` with `O_RDWR` (root or the card's user; one program at a time, else
  EBUSY; EACCES for another user). Each `read` gives one whole frame the system sent
  through the card (14-byte header and data, no checksum; a buffer of 1514 bytes
  always holds one, a shorter one gets it cut), and waits for one unless `O_NONBLOCK`
  (EAGAIN); each `write` is one frame the card receives (14 to 1514 bytes, else
  EINVAL); a frame for another MAC address is taken and dropped, as a card's filter
  does. `poll` gives POLLIN when a frame waits, POLLOUT always. The card's link is
  up while the program holds it; frames not read when it closes are dropped. Its
  address is the system's (`/system/config/network/tapN`, `net up`), not the
  program's: the program is the other end of the cable.
- each ssh call starts afresh: a `cd` lasts only within one call.
- background jobs and windowed apps hold the session's output open, so the
  ssh call does not return until they end.

Useful programs there (all listed by `help`): `ls`, `cat`, `cp`, `mv`, `rm`,
`mkdir [-p]`, `chmod MODE` (octal), `find`, `edit`, `ps`, `kill`, `mem`,
`uptime`, `uname -a` (the release and the commit it was built from), `net`, `ping`, `fetch` (HTTP and HTTPS), `elf` (the background programs),
`windowd`, `term`, `owl`.

## Writing the program

A RitterOS program built with the SDK is an ELF executable linked against
RitterOS's shared libraries (`LINK = shared`, the default), or one carrying
the code it uses (`LINK = static`): `main(argc, argv)` is called by
RitterOS's start code, and its return value is the exit code. It includes only RitterOS's headers
(`RITTEROS/user/include`), never a host's.

**Libraries.** Everything below is in RitterOS's shared libraries
(`/system/<arch>/lib`, the Library column of `API.md` says which) and in the
static archive `libritteros.a`; your program links one or the other
(`LINK`, above). The system's own SSH, cipher, GPT and mkfs code is static
only. Your own libraries are static (more `SRCS`, or an archive added to the
link).

**The complete list is [`API.md`](API.md):** every header, every function
and macro with its declaration, the library each header's functions are in, and the headers as they are, with
their comments; made from the headers by `scripts/sdkapi.py`, so it is
always current. It marks what is the system's own (for RitterOS's own
programs; do not use it). The summary:

| Header | What |
|---|---|
| `stdio.h` | `printf`, `dprintf(fd, ...)`, `snprintf`, `sprintf`, `vsnprintf`, `puts`, `putchar`, `perror`, `rename`, `remove`. Conversions `d i u x X o c s p`, and `f F e E g G a A` exactly (every digit of the double, rounded to nearest with ties to even; `L`: long double at double's precision); flags `- 0 + # space`, width, precision, `hh`/`h`/`l`/`ll`/`z`/`j`/`t`/`L`. Streams: `FILE`, `stdin`/`stdout`/`stderr`, `fopen` (`r w a`, `+`, `b`), `fdopen`, `freopen`, `fclose`, `fread`, `fwrite`, `fseek`, `ftell`, `rewind`, `fgetpos`/`fsetpos`, `fflush`, `setvbuf`/`setbuf` (`_IOFBF`, `_IOLBF`, `_IONBF`), `fgetc`, `fgets`, `ungetc`, `fputc`, `fputs`, `fprintf`, `feof`, `ferror`, `fileno`. A file stream is buffered (written at `fflush`, `fclose` or `exit`); `printf` and `stdout`/`stderr` write at once. `scanf`, `fscanf`, `sscanf` (and the `v` forms): `d i u x X o p e f g a c s [set] n`, `*`, widths, `hh`/`h`/`l`/`ll`/`L`. Temporary files: `tmpnam`, `tmpfile` (removed at `fclose` or exit), in `~/.tmp`, the user's own. `popen`/`pclose` (58c: a command through sh -c, read or written as a stream). |
| `stdlib.h` | `malloc`, `calloc`, `realloc`, `free`, `aligned_alloc`, `exit`, `atexit`, `quick_exit`/`at_quick_exit`, `_Exit`, `abort` (SIGABRT), `mkstemp`, `atoi`, `atol`, `atoll`, `atof`, `strtol`, `strtoul`, `strtoll`, `strtoull`, `strtod`, `strtof`, `strtold` (double precision), `abs`, `labs`, `llabs`, `div`, `ldiv`, `lldiv`, `qsort`, `bsearch`, `rand`/`srand` (RAND_MAX 2147483647; not for secrets), `system` (runs `sh -c`; the status for `WEXITSTATUS`), `getenv`/`setenv`/`unsetenv`/`putenv` (programs started get the environment; `environ` in `unistd.h`; `main` may take `envp` as its third argument), the multibyte calls (`mblen`, `mbtowc`, `wctomb`, `mbstowcs`, `wcstombs`: one byte per character, MB_CUR_MAX 1) |
| `string.h`, `strings.h` | `strlen`, `strnlen`, `strcmp`, `strncmp`, `strcoll`, `strxfrm`, `strchr`, `strrchr`, `strstr`, `strspn`, `strcspn`, `strpbrk`, `strtok`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strdup`, `strndup`, `memcmp`, `memchr`, `memcpy`, `memccpy`, `memmove`, `memset`, `strerror`; `strcasecmp`, `strncasecmp` |
| `ctype.h`, `assert.h`, `inttypes.h` | `isalpha` and the other classes, `tolower`, `toupper` (ASCII); `assert` (`NDEBUG` switches it off; a failure prints the expression and aborts); the `PRId64`-style macros |
| `setjmp.h`, `locale.h` | `setjmp`/`longjmp` (no `sigsetjmp`); the "C" locale only: `setlocale` ("C", "POSIX" or ""; others refused), `localeconv` |
| `time.h` | `time`, `clock` (CPU time, `CLOCKS_PER_SEC` 1000000), `difftime`, `mktime`, `gmtime`, `localtime` (and `_r`), `strftime` (C99's conversions), `asctime`, `ctime`; `timespec_get`, `clock_gettime` (`CLOCK_REALTIME`, `CLOCK_MONOTONIC`, `CLOCK_PROCESS_CPUTIME_ID`), `nanosleep` (in 10 ms ticks); local time = UTC + `zone` of `/system/config/time` (no daylight saving) |
| `math.h` | C99's set, our own code in double-double arithmetic (results within an ulp, nearly always the nearest double; `sin(1e300)` too): `sin` `cos` `tan` `asin` `acos` `atan` `atan2`, `sinh` `cosh` `tanh` `asinh` `acosh` `atanh`, `exp` `exp2` `expm1` `log` `log2` `log10` `log1p` `pow` `sqrt` `cbrt` `hypot`, `floor` `ceil` `trunc` `round` `fmod` (exact) `modf` `frexp` `ldexp` `scalbn` `copysign` `fmin` `fmax` `fabs`, each with its `f` form; `isnan` `isinf` `isfinite` `isnormal` `signbit` `fpclassify`, `M_PI` and the other `M_` constants; since 22z also `rint` `nearbyint` `lrint` `llrint` (the current rounding direction), `lround` `llround`, `remainder` `remquo` `fdim` `fma` (exact) `nextafter` `nexttoward` `ilogb` `logb` `scalbln` `nan`, `erf` `erfc` `tgamma` `lgamma` (no `signgam`), `isgreater` and the other comparisons, `float_t`/`double_t`, and the `l` form of everything (double's precision); errno is never set |
| `wchar.h`, `wctype.h` | wide characters (`wchar_t`, `wint_t`, `WEOF`) in the "C" locale, one byte each (a byte's value; above 255 no byte: EILSEQ): `wcslen` and the other `wcs*`/`wmem*` string calls, `wcstol`/`wcstoll`/`wcstoul`/`wcstoull`/`wcstod`/`wcstof`/`wcstold`, `wcsftime`, `btowc`, `wctob`, `mbrtowc`, `wcrtomb`, `mbrlen`, `mbsinit`, `mbsrtowcs`, `wcsrtombs`; streams `fgetwc`, `fgetws`, `ungetwc`, `fputwc`, `fputws`, `getwchar`, `putwchar`, `fwide` (the byte calls do not set the orientation); `wprintf`, `fwprintf`, `swprintf` (negative when it did not fit), `wscanf`, `fwscanf`, `swscanf` and their `v` forms; `printf`'s `%ls`/`%lc` and `scanf`'s `%ls`/`%lc`/`%l[`; `iswalpha` and the other classes, `towlower`, `towupper`, `wctype`/`iswctype`, `wctrans`/`towctrans` (ASCII). UTF-8: `uchar.h` |
| `uchar.h` | UTF-8 always, whatever the locale: `mbrtoc32`/`c32rtomb` (code points), `mbrtoc16`/`c16rtomb` (UTF-16: a surrogate pair as two calls), `mbrtoc8`/`c8rtomb` (C23's `char8_t`, a byte at a time); a character may arrive in pieces (`(size_t)-2`, kept in the `mbstate_t`); invalid UTF-8 (overlong, surrogates, above U+10FFFF) is `(size_t)-1` with EILSEQ |
| `complex.h` | `double complex` (`complex`, `I`, `CMPLX`) and its `float`/`long double` forms: `creal`, `cimag`, `cabs`, `carg`, `conj`, `cproj`, `cexp`, `clog`, `cpow`, `csqrt`, `csin`, `ccos`, `ctan`, `casin`, `cacos`, `catan`, `csinh`, `ccosh`, `ctanh`, `casinh`, `cacosh`, `catanh`, each with `f` and `l`; within 2-3 ulps of the exact value in each part (ctan, ctanh: 3-4), C's branch cuts and Annex G's infinities; `*` and `/` as Annex G (the compiler calls the library's `__muldc3` and friends). `long double` is double's precision; on ARM its arithmetic goes through the library's binary128 helpers |
| `fenv.h` | the rounding direction (`fegetround`, `fesetround`: `FE_TONEAREST`, `FE_UPWARD`, `FE_DOWNWARD`, `FE_TOWARDZERO`) and the exception flags (`feclearexcept`, `fetestexcept`, `feraiseexcept`, `fegetexceptflag`, `fesetexceptflag`), the whole environment (`fegetenv`, `fesetenv`, `feholdexcept`, `feupdateenv`, `FE_DFL_ENV`); per thread, never trapping; math.h's functions expect rounding to nearest |
| `unistd.h` | `read`, `write`, `close`, `lseek`, `dup`, `dup2`, `pipe` (since 52a), `sleep`, `usleep`, `getpid`, `getuid`, `chdir`, `getcwd`, `unlink`, `rmdir`, `isatty`, `spawn`; the exec family (since 55b): `execve`, `execv`, `execvp` (a bare name in `PATH`, else `/system/<arch>/bin` and `apps`; a file without `#!` that is not a program runs through `sh`), `execl`, `execlp`, `execle`: the program becomes another one, keeping its process number, folder, umask, blocked signals, ignored signals and descriptors (not those marked `FD_CLOEXEC`); its other threads end; `fork` (since 56a: a copy of the program, its memory copied a page at a time when written, `MAP_SHARED` memory shared; only the calling thread goes on in the child; shared buffers from `ritteros_share` are not passed on), `_exit`; `getppid`, `setpgid`, `getpgid`, `setsid` (56b); `symlink`, `readlink`, `link` (57a: symbolic and hard links on RitterFS, EPERM on FAT) |
| `fcntl.h` | `open(path, flags, ...)`: `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`, `O_APPEND`, `O_EXCL` (52j), `O_NONBLOCK` (53b); `fcntl` (53b: F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD/F_SETFD, F_GETFL/F_SETFL: with O_NONBLOCK reads, writes, accept and connect never wait: EAGAIN, EINPROGRESS); with `O_CREAT` a third argument, the new file's mode (since 52b; the umask is taken away: 022 at the start, so 0666 gives 0644); `creat(path, mode)` |
| `sys/socket.h`, `netinet/in.h`, `netinet/tcp.h`, `arpa/inet.h`, `sys/un.h` | BSD sockets (since 53a): `socket` (AF_INET: SOCK_STREAM TCP, SOCK_DGRAM UDP; AF_INET6 since 64d: IPv6, and IPv4 peers too as `::ffff:a.b.c.d` unless `IPV6_V6ONLY` is set before `bind`; AF_UNIX: local streams), `bind` (a port: every address of the machine), `connect`, `listen`, `accept`, `send`/`recv` (MSG_WAITALL, MSG_NOSIGNAL, MSG_DONTWAIT since 53b), `sendto`/`recvfrom`, `shutdown` (half close), `getsockname`/`getpeername`, `setsockopt`/`getsockopt` (SO_RCVTIMEO/SO_SNDTIMEO act; SO_REUSEADDR, SO_KEEPALIVE, SO_BROADCAST, TCP_NODELAY kept), `inet_pton`/`inet_ntop` (IPv6 too)/`inet_aton`/`inet_addr`/`inet_ntoa`, `htons` & co. |
| `sys/time.h` | `struct timeval`, `gettimeofday` (since 53a) |
| `pthread.h`, `sched.h` | POSIX threads (since 54a): `pthread_create`/`join`/`detach`/`exit`/`self`/`equal`, attributes (detach state; the stack size, 8 MiB by default, up to 63 MiB: since 54d), mutexes (normal, recursive, error-checking; `trylock`, `timedlock`), condition variables (`timedwait` on CLOCK_REALTIME or CLOCK_MONOTONIC), `pthread_once`, keys with destructors, `pthread_sigmask`, `sched_yield`; since 54b read-write locks (a waiting writer first), barriers, spin locks; since 54c `pthread_cancel` (deferred at the waiting calls, or asynchronous), `pthread_setcancelstate`/`_setcanceltype`, `pthread_testcancel`, `pthread_cleanup_push`/`_pop`; `pthread_atfork` (56a); up to 256 threads a program (16 until 54d) |
| `spawn.h` | `posix_spawn`, `posix_spawnp` (since 55c; a name looked up as `execvp` does), file actions (`posix_spawn_file_actions_addopen`, `_adddup2`, `_addclose`, on any descriptor), attributes (`POSIX_SPAWN_SETPGROUP`, `SETSIGMASK`, `SETSIGDEF`, `RESETIDS`); since 56b by fork and exec, as POSIX: the child has every descriptor not marked FD_CLOEXEC and the ignored signals; they return an error number, not -1 |
| `regex.h` | `regcomp`, `regexec`, `regerror`, `regfree` (since 58a): POSIX's basic and extended syntax (`REG_EXTENDED`), `REG_ICASE`, `REG_NOSUB`, `REG_NEWLINE`, `REG_NOTBOL`/`REG_NOTEOL`; the leftmost-longest match, groups in `regmatch_t`; back references in both syntaxes |
| `netdb.h` | `getaddrinfo`/`freeaddrinfo`/`gai_strerror` (since 53d: names through dnsd, services from `/system/config/services`; IPv6 since 64d: AAAA records are asked for only when the machine has an IPv6 address beyond the link, and then come first; `AF_UNSPEC`, `AF_INET`, `AF_INET6` with `AI_V4MAPPED`/`AI_ALL`), `getnameinfo` (numbers, IPv6 too: no reverse lookup), `gethostbyname` (IPv4), `getservbyname`/`getservbyport`, `h_errno` |
| `sys/select.h` | `select`, `fd_set` with `FD_ZERO`/`FD_SET`/`FD_CLR`/`FD_ISSET` (since 53c, over poll; FD_SETSIZE 256) |
| `poll.h` | `poll(fds, count, timeout_ms)` (since 52d; up to 256 descriptors, as many as a program may have open since 52k): pipes, pseudo-terminals, the terminal, sockets (UDP, TCP, local), `/dev/tapN` (since 38a); files never wait |
| `termios.h` | `tcgetattr`, `tcsetattr`, `tcflush`, `tcdrain`, `cfmakeraw`, `cf[gs]et[io]speed` (since 52e): ICANON (raw input with VMIN/VTIME), ECHO, ISIG, ICRNL act; the rest is kept; the terminal is put back when the program ends |
| `sys/stat.h`, `dirent.h` | `stat`, `fstat` (since 52b: also pipes `S_ISFIFO`, terminals `S_ISCHR`, sockets `S_ISSOCK`), `lstat` (57a: a link itself, `S_ISLNK`, `DT_LNK` in readdir; `st_nlink` counts a file's names), `mkdir`, `chmod`, `chown`, `umask` (since 52h: per program, passed on to the programs it starts); `opendir`, `readdir`, `closedir` |
| `sys/mman.h` | `mmap`, `munmap`, `mprotect`: anonymous memory, and files mapped into memory (a read-only mapping shares one copy of the file's pages with every program that maps it) |
| `signal.h` | `kill`, `raise`, `signal` and `sigaction` with handlers (they run when the signal comes, on the program's stack; faults too: `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE`), `sa_mask`, `SA_RESETHAND`, `SA_NODEFER`, the `sigset_t` calls, `sig_atomic_t`; interrupted calls return `EINTR` (no `SA_RESTART`); `sigprocmask` (since 52f: per thread; new threads and, since 52i, spawned programs take their creator's set), `sigpending`, `sigsuspend` (52i); no `SA_SIGINFO` |
| `thread.h` | `thread_create(fn, arg)`, `thread_join`, `thread_self`: threads of one program |
| `threads.h` | C11's threads on top of them: `thrd_create`, `thrd_join`, `thrd_detach`, `thrd_current`, `thrd_equal`, `thrd_exit`, `thrd_sleep`, `thrd_yield`; `mtx_init` (`mtx_plain`, `mtx_timed`, `mtx_recursive`), `mtx_lock`, `mtx_timedlock`, `mtx_trylock`, `mtx_unlock`; `cnd_init`, `cnd_wait`, `cnd_timedwait`, `cnd_signal`, `cnd_broadcast`; `call_once`; `tss_create`/`tss_get`/`tss_set`/`tss_delete` (64 keys, destructors at thread end); `thread_local`. Waits go through the kernel's futex (no spinning); timed calls take absolute TIME_UTC times, in 10 ms ticks; at most 256 threads per program (16 until 54d) (a detached thread's number is free again once it ended); mutexes and condition variables are the program's own |
| `dlfcn.h` | `dlopen` (a library by name in `/system/<arch>/lib`, or a path), `dlsym`, `dlclose` (it stays loaded), `dlerror`: plug-ins (not in a `LINK = static` program) |
| `ritteros.h` | everything RitterOS-specific: `ritteros_spawn` (start a program; `ritteros_spawn_env` with an environment of your choice), `ritteros_pipe`, sockets (`ritteros_socket`, `_connect`, `_bind`, `_listen`, `_accept`, `_sendto`, `_recvfrom`), `ritteros_resolve` (DNS), the network cards (`ritteros_netcard`, `ritteros_netinfo`; since 38a-38b `ritteros_tap`, `ritteros_bridge`), local sockets, shared memory, `ritteros_time` (ns since 1970), `ritteros_random`, `config_get`, `sha256`, `crc32`, `PROGRAM_HELP` |
| `window.h` | windowed apps: `window_open`, `window_fill`, `window_text`, `window_text_font`, `window_blit`, `window_flush`, `window_wait` and `window_wait_timeout` (events: pointer, key, resize, close, menu, double click: windowd decides what is one, the same for every app, scroll: the wheel's steps, key down/up: physical keys for games, `WKEY_*`), `window_scrollbar`/`window_scrollbar_hit` (scroll bars), menus, `window_choose_file` (the shared Open dialog; `window_choose` with a filter, 42c), `window_app_opens` (an app bundle's `types` against a file, 42c), `window_field_*` (a one-line text field: selecting with the mouse and Shift, editing, Ctrl+A/C/X/V; give it every event, -1 means the event is yours), `window_clipboard_set`/`_get` (windowd's clipboard, the screen's user's), `window_timer` (`WINDOW_TIMER` events), `window_fd` (the window in a program's own `poll`) |
| `wfont.h` | fonts: `wfont_find("10x20")` (a BDF font of `/system/fonts`; 0 if missing), `wfont_open("FreeSans.ttf", 17)` (since 41c: a TrueType or OpenType font at any size in pixels, drawn smooth; size 0: the user's `text_size`), `wfont_wrap`/`wfont_layout` (since 41d: text over several lines), `wfont_regular()` (the built-in 9x15, the fallback), `wfont_width`, `wfont_height`, `wfont_text_width` |
| `widget.h` | controls (`libwidget`, since 41e): `widget_row`/`_column`/`_grid` and `widget_add`, `widget_stretch`; `widget_label`, `widget_button`, `widget_check`, `widget_radio`, `widget_slider`, `widget_progress`, `widget_tabs` (pages titled by their text; since 41e-2); `widget_list`, `widget_table`, `widget_tree` (rows given as text: lines, tabs between a table's cells, a tree's leading tabs its level; since 41e-3); `widget_editor` (multi-line or one-line text editing with selection and the clipboard; since 41e-4); `widget_message`, `widget_question` (dialogs over a window, run until answered; since 41e-5); `widget_on` (a callback), `widget_value`/`_set_value`, `widget_text`/`_set_text`; `widget_run`, `widget_quit`, `widget_free` |
| `draw.h` | 2D drawing (`libdraw`, since 41b) on any pixel buffer, a window's own (`window_pixels`) or an image's: `draw_rect`, `draw_line`, `draw_polygon`, `draw_circle`, `draw_ellipse`, `draw_round_rect`, filled or outlined, `draw_gradient` (two colours between two points), `draw_image` (a libimage `struct image` scaled and turned, its alpha blended), anti-aliased by exact area, colours with alpha (`draw_rgba`), `draw_clip` |
| `image.h`, `audio.h`, `video.h` | images (`image_load` reads PPM, BMP, PNG and JPEG; `image_save` writes PNG, BMP or PPM by the file's ending, PNG since 41h-2), WAV sound, RVID video (`media.h` has the shared types); play sound by writing 48 kHz 16-bit stereo to `/dev/pcm0` |

**Not there** (so do not reach for it): `sysconf`; other locales
than "C" (ASCII only).

**Limits that bite:**

- **Per-thread data works** (since 22i): `__thread` variables (with an
  initial value or zeroed), one copy per thread, and `errno` is per thread.
  A thread's copy starts from the initial values, not from its creator's.
- **Threads run at the same time** (since item 31): a program's threads
  run on every core at once (`threads` shows each one's `cpu`; `cpus=N` on
  the kernel command line caps the cores). Data two threads share needs a
  lock (pthread or C11 mutexes, read-write locks, condition variables:
  they wait in the kernel, never spin) or atomic operations. A plain flag
  or counter, `volatile` too, is a data race: increments get lost, and on
  ARM another core may see a flag before the data written ahead of it.
  For atomics use the `__atomic_` functions, which both compilers make
  into real atomic instructions: `__atomic_fetch_add(&n, 1,
  __ATOMIC_RELAXED)`, and `__atomic_store_n(&ready, 1, __ATOMIC_RELEASE)`
  read with `__atomic_load_n(&ready, __ATOMIC_ACQUIRE)`. C11's
  `<stdatomic.h>` and `_Atomic` work with both too (RitterOS's `cc` since
  65e-52; only scalars of up to 8 bytes): `_Atomic int n; n++` is one
  atomic step. clang refuses `+=` and `-=` of an atomic pointer, and its
  `op=` on an `_Atomic _Bool` stores the unconverted value (`b += 5` makes
  6, and a later `b -= 1` never ends): keep to `++`/`--` and plain
  assignments there. `pthread_spin_lock`
  gives the processor up at every failed try: use a mutex. In the library,
  `malloc` and `free` take any thread; `printf` and `dprintf` write up to
  256 bytes in one `write` (`puts` too), so short lines of two threads do
  not mix; every function on a `FILE` holds the stream's lock for its call
  (since 65e-53, as POSIX asks: a line `fputs` writes stays whole), and
  `flockfile`/`funlockfile` hold it across several, with
  `getc_unlocked`/`putc_unlocked` inside (`fflush(0)` passes streams other
  threads hold while the caller holds one itself, instead of waiting); `rand`, `strtok`, `localtime`,
  `gmtime`, `asctime`, `ctime`
  share their state between threads (`localtime_r`, `gmtime_r` do not).
- **AVX and AVX2 only where the processor has them** (x86, since 32a).
  SSE2 is on every x86-64 processor and NEON on every ARM one, and both
  compilers use them by themselves. AVX and AVX2 (the 256-bit YMM
  registers) work where the processor has them and the system saves them:
  the boot log then says `FP/SIMD: x87, SSE and AVX (XSAVE, ...)` (a
  Ryzen 7 5800H does; the J1900, a Celeron, has no AVX: an AVX instruction
  there stops the program with `SIGILL`). The SDK builds for every x86-64
  processor, so the compiler never uses AVX on its own: mark the functions
  that should with `__attribute__((target("avx2")))` (zig's
  `<immintrin.h>` has the intrinsics), and call them only after asking the
  processor, with an SSE or plain way otherwise. RitterOS's own `cc`
  (building on the machine) has `<immintrin.h>` too since 32d, the same
  intrinsics with clang's results; since 32h its vectors stay in
  registers and a `target("avx2")` function is AVX code there too (VEX
  encodings, vzeroupper, never inlined into code without the mark): a
  FIR filter 3.6 ns a sample against clang's 1.4. Keep the AVX work in
  such marked functions, called after the check below; the guard keeps
  the AVX part to x86-64. Vectors in cc have no operators, subscripts or brace
  initializers: use the header's functions (`_mm256_add_epi32`,
  `_mm256_setr_epi32`, `_mm256_storeu_si256`), which clang has as well.
  Include `<immintrin.h>` itself (cc has no `<emmintrin.h>` and the like):

  ```c
  #if defined(__x86_64__)
  #include <immintrin.h>
  static int have_avx2(void)
  {
      unsigned a, b, c, d, lo, hi;
      __asm__ ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
      if (!(c >> 27 & 1) || !(c >> 28 & 1))      /* OSXSAVE, AVX */
          return 0;
      __asm__ ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
      if ((lo & 6) != 6)                          /* the system saves SSE and AVX */
          return 0;
      __asm__ ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
      return b >> 5 & 1;                          /* AVX2 */
  }
  __attribute__((target("avx2")))
  static void add_avx2(int32_t *d, const int32_t *s, size_t n) { /* _mm256_... */ }
  #else
  static int have_avx2(void) { return 0; }
  #endif
  ```

  Ask once at the start and keep the answer. What it gives (32b, the
  laptop): adding 32-bit integers in the cache 122 GB/s with SSE2, 226
  with AVX2 (1.86x); over 64 MiB of memory 30 against 35 GB/s (1.15x: the
  memory is the limit). So it pays for work on data that stays in the
  cache (filters, FFTs, image operations on blocks), little for one pass
  over large data.
- **Windowed apps, the parts** (item 41; each below, with code): one
  header and library per part, every interface recorded in `sdk/abi/`.

  | Part | Header, library | Since |
  |---|---|---|
  | A window, its events, timers, the clipboard, menus, drag and drop, full screen | `window.h`, libwindow | 15e, 41a, 41f |
  | Shapes, gradients, images on any pixels | `draw.h`, libdraw | 41b |
  | Fonts at any size, text over lines | `wfont.h`, libwindow | 41c, 41d |
  | Controls: buttons, check boxes, sliders, lists, tables, trees, tabs, an editor, dialogs | `widget.h`, libwidget | 41e |
  | The app file (menu, file types, icon), opening files, notifications, the user's colours | `window.h`, a bundle's `info` and `icon.png` | 41g, 42 |
  | Testing without a screen: `windowd headless`, `wsend`, `wshot` | `WINDOW_SOCKET` | 41h |

  `sdk/examples/hellowin/` is a window drawn by hand, `widgets/` a form of
  controls, `notes/` a whole app: controls, a dialog, a notification, an
  app file, and its own test (`test.sh`) run without a screen.
- **A window's basics** (`window.h`): open it, draw into it, show it, and
  read its events until it closes; the colours are native pixels from
  `window_rgb` (or `window_color`'s 0xRRGGBB taken apart):

  ```c
  struct window *win = window_open(400, 300, "Sketch");    /* 0: windowd is not running */
  uint32_t back = window_rgb(win, 0x2E, 0x34, 0x40), ink = window_rgb(win, 255, 255, 255);
  window_fill(win, 0, 0, 400, 300, back);
  window_text(win, 10, 10, "Hello", ink, WINDOW_TRANSPARENT);   /* the built-in 9x15 font */
  window_flush(win);                                        /* windowd shows what was drawn */
  struct window_event ev;
  while (window_wait(win, &ev)) {                           /* 0: the window closed */
      switch (ev.type) {
      case WINDOW_POINTER:      /* ev.x, ev.y in the window; ev.buttons held (1 left, 2 right) */
      case WINDOW_DOUBLE_CLICK: /* windowd's rule, the same in every program */
      case WINDOW_SCROLL:       /* ev.y wheel steps, negative up */
      case WINDOW_KEY:          /* ev.key a byte as a terminal gets it (arrows: ESC [ A ...) */
      case WINDOW_TEXT:         /* ev.key a typed character, whole (Unicode): take text from here */
      case WINDOW_KEY_DOWN:     /* ev.key a physical key, WKEY_* (games); WINDOW_KEY_UP too */
      case WINDOW_RESIZE:       /* ev.x, ev.y the new size: window_resize(win, ev.x, ev.y), redraw */
      case WINDOW_MENU:         /* ev.menu the chosen item's id */
          break;
      }
  }
  window_close(win);
  ```

  Menus are the program's to declare and windowd's to draw: a menu bar
  below the title bar, and a context menu at a point (on a right-click,
  `ev.buttons & 2`); either sends the chosen id as `WINDOW_MENU`:

  ```c
  static const struct window_menu_item file[] = { { 1, "Open" }, { 2, "Quit" } };
  static const struct window_menu bar[] = { { "File", file, 2 } };
  window_set_menu(win, bar, 1);                             /* 0 menus: none */
  static const struct window_menu_item here[] = { { 3, "Copy" }, { 4, "Paste" } };
  window_popup_menu(win, ev.x, ev.y, here, 2);              /* at the pointer */
  ```

  The shared Open dialog, a one-line text field and the clipboard:

  ```c
  char path[256];
  if (window_choose_file(win, "Open a sketch", "/home/me", path, sizeof(path)) == 1)
      load(path);                                           /* 0 cancelled, -1 no window */
  /* (42c) window_choose(win, title, folder, accept, data, path, size): only
   * what accept(path, data) allows can be chosen (the rest greyed; an app's
   * bundle one item): int sketches(const char *p, void *d) { return
   * strstr(p, ".skp") != 0; } */
  struct window_field *name = window_field_new(win, 10, 40, 200);   /* x, y, width */
  /* in the loop: window_field_event(name, &ev) is -1 for an event not the
   * field's (yours), 1 when Enter was pressed; window_field_draw(name) when
   * you redraw; window_field_text(name) its text */
  char clip[1024];
  window_clipboard_set(win, "copied", 6);
  window_clipboard_get(win, clip, sizeof(clip));            /* its length, or -1 */
  ```

  libwidget's `widget_grid(columns)` lays children out row by row in
  columns (a form's labels and fields), as `widget_row` and
  `widget_column` do in one line. Text in a font of your choosing goes
  through `window_text_font(win, font, x, y, text, fg, bg)` (`wfont_bold()`
  the built-in bold, `wfont_find("10x20")`, `wfont_open("FreeSans.ttf",
  0)`), or `wfont_text` onto any pixels. The theme's colours by their full
  names: `WINDOW_COLOR_BACKGROUND`, `WINDOW_COLOR_PANEL`,
  `WINDOW_COLOR_FACE`, `WINDOW_COLOR_FACE_PRESSED`, `WINDOW_COLOR_HOVER`,
  `WINDOW_COLOR_TEXT`, `WINDOW_COLOR_EDGE`, `WINDOW_COLOR_ACCENT`,
  `WINDOW_COLOR_SELECTION` (see the user's colours below); the pointer's
  shapes `WINDOW_CURSOR_ARROW`, `WINDOW_CURSOR_TEXT`, `WINDOW_CURSOR_HAND`;
  a scroll bar is `WINDOW_SCROLLBAR_W` pixels wide (`window_scrollbar`).
- **A windowed app reads its events:** call `window_wait` regularly (a thread
  of its own if the app is busy). Events wait for it in a small queue at
  `windowd`, pointer moves merged into the latest; an app that stops reading
  only stops itself, but its window then freezes. A lock shared with that
  thread must be fair (see `term.c`'s ticket lock): a plain flag polled with
  `usleep` starves the waiter.
- **Animations, timers and other descriptors** (since 41a): no thread is
  needed for them. `window_timer(win, ms, id)` makes a `WINDOW_TIMER`
  event (`ev.key` = id) every ms milliseconds until set to 0 (8 per
  window; a late one is not caught up in a burst);
  `window_wait_timeout(win, &ev, ms)` waits at most ms (1 an event, 0
  none, -1 closed) and `window_wait` waits for good, both handing out
  timers that are due. An app that waits on its own sockets or pipes puts
  `window_fd(win)` in its `poll` and, after every wake-up, takes all
  events:

  ```c
  struct pollfd p[2] = { { window_fd(win), POLLIN, 0 }, { sock, POLLIN, 0 } };
  window_timer(win, 16, 1);                       /* about 60 frames a second */
  for (;;) {
      poll(p, 2, 16);                             /* (no longer than the next timer) */
      struct window_event ev;
      int r;
      while ((r = window_wait_timeout(win, &ev, 0)) > 0)
          if (ev.type == WINDOW_TIMER) draw_frame(win);
      if (r < 0) break;                           /* the window closed */
      if (p[1].revents & POLLIN) read_socket(sock);
  }
  ```

  Timers do not wake such a poll by themselves; its timeout must not pass
  the next one. Waits count the system's 100 Hz tick, so a timer comes
  due up to one or two ticks (10-20 ms) late, never early.
- **Controls** (since 41e): build a tree of widgets and let `widget_run`
  lay it out, draw it and run the window; every program's buttons and
  check boxes then look and behave alike (Tab and Shift+Tab move the
  keyboard, Space and Enter press). `sdk/examples/widgets/` is a whole
  program:

  ```c
  struct widget *root = widget_column(), *ok = widget_button("OK");
  widget_add(root, widget_label("Ready?"));
  widget_add(root, ok);
  widget_on(ok, pressed, root);       /* pressed(w, data) calls widget_quit(data) */
  widget_run(win, root);              /* until widget_quit or the window's X */
  widget_free(root);
  ```

  The other controls (41e-2 to 41e-5), each a widget added the same way;
  `widget_value`/`widget_text` read them, `widget_on`'s function runs at
  every change:

  ```c
  struct widget *volume = widget_slider(0, 100, 40);          /* a value 0-100 */
  struct widget *busy = widget_progress(100, 0);              /* widget_set_value(busy, 70) */
  struct widget *files = widget_list("a.txt\nb.txt\nc.txt");  /* a line a row; value: the chosen */
  struct widget *sizes = widget_table(2, "Name\tSize\na.txt\t12\nb.txt\t3400");  /* first line: header */
  struct widget *dirs = widget_tree("home\n\tme\n\t\tnotes\nsystem");  /* tabs: the level */
  struct widget *note = widget_editor("", 6);                 /* 6 lines; 1: a one-line entry */
  struct widget *pages = widget_tabs(), *general = widget_column();   /* each child a page */
  widget_set_text(general, "General");                        /* its title */
  widget_add(pages, general);
  widget_set_text(files, "x\ny");                             /* new rows, none chosen */
  if (widget_question(win, "Delete", "Delete the file?", "Delete", "Cancel"))   /* a dialog: 1 yes */
      widget_message(win, "Done", "It is deleted.");          /* an OK box; both wait for the answer */
  ```

  Link with `-lwidget`. (41f-1) A window also gets `WINDOW_FOCUS` events
  (`ev.key` 1 when it got the keyboard, 0 when it lost it).
  (41f-3) `WINDOW_ENTER` and `WINDOW_LEAVE` come when the pointer comes
  over the window's content and leaves it (hover highlights end).
  (41f-5) `window_dialog_open(parent, w, h, title)` opens a window tied to
  another: centred over it, in front, the parent taking no input until it
  closes.
  (41f-4) `WINDOW_TEXT` gives a typed character whole (`ev.key` its
  Unicode code point; dead keys and AltGr included, never a key's escape
  sequence or a control byte): take text from it, not from `WINDOW_KEY`'s
  bytes. `window_set_cursor(win, WINDOW_CURSOR_TEXT)` (or `_HAND`,
  `_ARROW`) sets the pointer's shape over the window's content.
  (41f-2) `window_set_limits(win, min_w, min_h, max_w, max_h)` keeps the
  user's resizing and maximizing within sizes (`widget_run` sets the
  least to what its layout needs, and grows a window opened smaller).
  (41f-6) `window_fullscreen(win, 1)` gives the window the whole screen,
  without its frame, in front of everything (a video, a game, a slide
  show), `window_fullscreen(win, 0)` puts it back; the new size comes as a
  `WINDOW_RESIZE` to answer with `window_resize`, and leaving full screen
  is the program's to offer (windowd has no key for it: Escape is the
  usual one). `window_minimize(win)` sends it to the taskbar. Drag and
  drop: on the pointer's move that starts a drag (the left button still
  held after a press in the window) call `window_drag(win, text, n)`; the
  window the button is let go over gets `WINDOW_DROP` (`ev.x`, `ev.y`
  where it fell) and reads `window_drop_text(win)`. A file is dragged as
  its full path, several one per line (`owl` drags its entries so):

  ```c
  if (ev.type == WINDOW_DROP)
      open_file(window_drop_text(win));   /* e.g. "/home/me/notes.txt" */
  ```
- **Drawing shapes** (since 41b): `libdraw` draws on a `struct
  draw_canvas`, any 32-bit pixel buffer. On a window it draws on the
  window's own pixels, so nothing is copied; after a `window_resize` make
  the canvas again (the pixels move):

  ```c
  struct draw_canvas c = { window_pixels(win), window_width(win), window_height(win),
                           window_width(win) };
  window_shifts(win, &c.red_shift, &c.green_shift, &c.blue_shift);
  draw_rect(&c, 10, 10, 120, 40, 0, draw_rgba(40, 90, 200, 255));     /* filled */
  draw_rect(&c, 10, 10, 120, 40, 1, draw_rgba(255, 255, 255, 255));   /* a 1-pixel outline inside it */
  draw_line(&c, 10, 70.5f, 130, 70.5f, 1, draw_rgba(255, 0, 0, 128)); /* half transparent */
  window_flush(win);
  ```

  Coordinates are floats: pixel (i, j) is the square from (i, j) to (i + 1,
  j + 1), so a 1-pixel horizontal line is sharp on y = j + 0.5, and a
  rectangle on whole coordinates is sharp. Every edge is anti-aliased by the
  exact area it covers; link with `-ldraw`.
- **Smooth text at any size** (since 41c): `wfont_open("FreeSans.ttf", 17)`
  opens a scalable font (FreeSans, FreeSerif, FreeMono, each with a Bold, as
  `.ttf` or `.otf` in `/system/fonts`, or any TrueType/OpenType file by its
  path) at 17 pixels an em; draw with it as with any font:

  ```c
  const struct font *f = wfont_open("FreeSans.ttf", 17);
  if (!f)
      f = wfont_regular();                        /* (the built-in fallback) */
  window_text_font(win, f, 10, 10, "Hello", window_rgb(win, 255, 255, 255), WINDOW_TRANSPARENT);
  int w = wfont_text_width(f, "Hello"), line = wfont_height(f);
  ```

  Letters are blended into what is behind them (transparent background), so
  draw the background first; with a background colour each letter's cell is
  filled. Fonts are kept for the program's life, and may be used from
  several threads.
- **Text over several lines** (since 41d): `wfont_layout` draws text
  wrapped to a width (breaking after spaces, inside a word only when it is
  wider than a line, and at newlines) and returns its height; with a 0
  buffer it only measures. `wfont_wrap` gives the bytes of the first line,
  for drawing lines yourself. Open fonts at size 0 to follow the user's
  `text_size` (`~/.config/windows`, 15 if not set):

  ```c
  const struct font *f = wfont_open("FreeSans.ttf", 0);   /* the user's size */
  int h = wfont_layout(0, 0, 0, 0, f, 0, 0, 300, text, 0, 0);      /* measure */
  wfont_layout(window_pixels(win), window_width(win), window_width(win), window_height(win), f,
               10, 10, 300, text, window_rgb(win, 255, 255, 255), WFONT_TRANSPARENT);
  ```
- **The user's colours** (since 41g-5): `theme = dark|light` and `accent =
  RRGGBB` in `~/.config/windows`. `window_color(WINDOW_COLOR_TEXT)` gives
  the theme's colour for a part as 0xRRGGBB: BACKGROUND, PANEL, FACE,
  FACE_PRESSED, HOVER, TEXT, EDGE, ACCENT (focus, progress) and SELECTION
  (chosen rows, the title bar in front); the accent, when set, is both
  ACCENT and SELECTION. libwidget's widgets use them already; draw your own
  parts with them too, so a program looks right in both themes:

  ```c
  uint32_t c = window_color(WINDOW_COLOR_BACKGROUND);
  window_fill(win, 0, 0, w, h, window_rgb(win, c >> 16 & 255, c >> 8 & 255, c & 255));
  ```
- **Games read physical keys, not bytes** (since 15p): `WINDOW_KEY` is the
  byte a terminal would get (text: no releases, nothing for Ctrl, Shift or
  Alt alone, repeats while held). `WINDOW_KEY_DOWN` and `WINDOW_KEY_UP` come
  for every physical key, modifiers included, once per press and release;
  `ev.key` is its USB HID usage ID, named `WKEY_*` in `window.h` (where the
  key is, not what it types). Keep a table of held keys:

  ```c
  static bool held[256];
  struct window_event ev;
  while (window_wait(win, &ev)) {
      if (ev.type == WINDOW_KEY_DOWN) held[ev.key] = true;
      else if (ev.type == WINDOW_KEY_UP) held[ev.key] = false;
      /* ... held[WKEY_UP], held[WKEY_LCTRL] (fire), held[WKEY_SPACE] ... */
  }
  ```

  A window that loses the front gets the releases of the keys still held,
  so nothing stays down.
- **Stacks are 8 MiB** (since 54d; 64 KiB before): the main thread's and
  each thread's by default, up to 63 MiB for a pthread that asks
  (`pthread_attr_setstacksize`); their pages are made only when touched.
  At most 256 threads a program. Running past a stack's end stops the
  program with a page fault.
- **Memory is made when first touched:** `malloc` and anonymous `mmap`
  have no limit but the machine's memory, and a page costs memory only from
  its first touch (a large mapping that is never used is free). There is no
  paging to disk: touching more than the machine has ends the program. A
  file's pages are read when it is mapped; a private writable mapping of a
  file shares them until a page is written. A shared writable mapping
  (`MAP_SHARED`, the file opened `O_RDWR`) writes the file: `msync`, `munmap`
  or the program's end put the changes there; other programs mapping it see
  them at once. `malloc` may be called from
  several threads at once, and `free` gives memory back to the system (a
  block of 128 KiB or more at once; small blocks when a whole 1 MiB arena
  of them is free).
- **TLS** (`<tls.h>`, `libtls.so.1`, since 23d) is OpenBSD's libtls
  interface (a subset): `tls_client`, `tls_configure(t, 0)` (the defaults),
  `tls_connect(t, host, "443")`, `tls_read`/`tls_write`, `tls_close`,
  `tls_free`; a call that fails returns -1 and `tls_error` says why. TLS 1.3
  only: a server that knows only 1.2 is refused. The server's chain is
  checked against `/system/certs/cacert.pem` (Mozilla's roots, since 23e;
  `tls_config_set_ca_file` names other ones), with its host name and dates. `tls_read` returns 0 at the end; `tls_close`
  then fails if the server just closed the connection without saying so
  (close_notify): the data may be cut short, so check a length you know.
- **A command-line program in `bin` declares**
  `PROGRAM_HELP("usage", "what it does")` (from `ritteros.h`), which `help`
  shows; the template has one.

**Network cards: real, virtual, bridges** (since 38a, 38b). A program
sees every card the same way, whatever it is: `eth0` (a wired card),
`wlan0` (Wi-Fi), `tap0` (a virtual card whose wire is a program), `br0` (a
bridge joining cards into one network). `ritteros_netcard(i, &info)` lists
them by place (0 first, until -1 with `ENODEV`), `ritteros_netinfo` finds
one by name (`struct sys_netinfo`: MAC, link speed, IPv4 settings); a
link-local IPv6 address takes any of them as its scope (`fe80::1%tap0`).
A bridge's port shows no link (`link_speed` 0): its traffic is the
bridge's, and the bridge has the address. The link a program reads is
at most 10 ms old (since 38d the system asks a card's driver at most once
a timer tick). The firewall's rules
(`on tap0`, `in br0`, `out tap1`) and `net default` take every kind of
name.

- **Making them** is root's: `ritteros_tap(TAP_ADD, uid)` gives N of the
  new `tapN`, whose `/dev/tapN` that user may open (`net tap add user
  NAME` does the same); `ritteros_tap(TAP_REMOVE, N)` (EBUSY while open).
  `ritteros_bridge(BRIDGE_ADD, -1, "eth0")` makes a bridge with that port
  and gives its N; `ritteros_bridge(BRIDGE_ADD, N, "tap0")` adds a port;
  `ritteros_bridge(BRIDGE_REMOVE, N, "tap0")` takes one out, card 0 the
  whole bridge. ENOTSUP: the card cannot take other machines' frames (a
  Wi-Fi card); EBUSY: a port already. A bridge's address comes from its
  settings (`/system/config/network/brN`, `net up`), as any card's.
  These calls change the running system only: what they make is gone at
  the next start. To keep it, run `net tap add` / `net bridge add` (they
  also write their line in `/system/config/network/virtual`, which `net
  up` makes again at every start, since 38c), or change that file's line
  yourself (`tapN = user NAME`, `brN = PORT...`).
- **Being a tap's wire** is any program's that the card's user runs (see
  `/dev/tapN` above for the rules): it reads the frames the system sends
  through the card and writes the ones the card receives. The system's
  side of the card has the address (`/system/config/network/tapN`); the
  program is what sits at the other end of the cable: another machine
  with its own MAC address and IP address (a VPN's far side, a simulated
  network), which answers ARP for its address itself. Through a bridge the
  same program is a machine on the bridge's whole network.

```c
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    int fd = open("/dev/tap0", O_RDWR);         /* root's, or the tap's user's */
    if (fd < 0) {
        perror("/dev/tap0");                    /* EBUSY: another program holds it */
        return 1;
    }
    unsigned char frame[1514];                  /* the largest frame: one whole frame a read */
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 1000) <= 0)
            continue;
        ssize_t n = read(fd, frame, sizeof(frame));
        if (n < 14)
            break;
        printf("%zd bytes, type %02x%02x, to %02x:%02x:%02x:%02x:%02x:%02x\n", n,
               frame[12], frame[13], frame[0], frame[1], frame[2], frame[3], frame[4], frame[5]);
        /* write(fd, reply, length) hands a frame to the system as received */
    }
    close(fd);
    return 0;
}
```

`user/test/taptest.c` in the tree is a complete one: it answers ARP and
ping for an address, and pings another machine through a bridge.

**Where to learn by example** (in the RitterOS tree): `user/bin/*.c` are
the command-line programs (`cat.c`, `ls.c` are short; `fetch.c` uses
sockets, DNS and libtls; `tune.c` plays a WAV; `soundd.c` uses threads),
`user/apps/*.c` the windowed apps (`term.c`, `owl.c` with menus,
`imageview.c`, `reel.c`), `sdk/examples/hellowin/` the smallest windowed app.

## Troubleshooting

| You see | Because | Do |
|---|---|---|
| `sh: hello: Exec format error` (exit 126) | built for the other architecture | `make ARCH=x86_64` for a real PC |
| `sh: hello: Permission denied` (exit 126) | run by a user other than root, and not executable | `make deploy`, or `chmod 755 PATH` there |
| `sh: hello: command not found` (exit 127) | not there, or in the other of `bin`/`apps` | check `KIND`; `ssh HOST 'ls /system/x86_64/bin'` |
| `Permission denied (publickey,password)` | the key is not in `authorized_keys`, or is not Ed25519 | redo the key step; `ssh -v` shows which key was offered |
| `REMOTE HOST IDENTIFICATION HAS CHANGED` | the machine was reinstalled (new host key) | `ssh-keygen -R ADDRESS` |
| `Connection refused`, or a timeout | sshd not running, or no network | at the machine: `net`, `net up`, `elf start sshd` |
| `cannot open a window (is windowd running?)` | no window system on the machine | at the machine: `windowd &` |
| `make run` does not return | a windowed app (or a background job) is still running | close its window or `make stop`; run windowed apps in the background |
| exit code 139, nothing printed | the program crashed | `ssh HOST 'cat /system/logs/kernel.log'` for the fault line |
| `ld.so: NAME: NAME needs FUNCTION, which none of its libraries has` (exit 127) | built against a newer RitterOS than the machine runs | update the machine, or build against its version; `ldd NAME` there lists all of them |
| `ld.so: NAME: libX.so.1 is not in /system/<arch>/lib/` (exit 127) | the machine lacks that library | update the machine, or `LINK = static` |
| `Function not implemented` | a static program calling the kernel for something an older RitterOS lacks | update the machine |
| a compile or link error: an unknown function or header | not in RitterOS's C library | use what `user/include` declares ([`API.md`](API.md)) |

## For RitterOS's maintainers

This folder is part of RitterOS's interface: a change to the library, the
headers, `user/user.ld`, the start code, the shell's syntax or sshd changes
this guide and the template with it, and a changed header needs
`python3 scripts/sdkapi.py` (it rewrites `API.md`; a new header must be
added to its list first). The shared libraries' interfaces are recorded in
`sdk/abi/` and kept compatible by `scripts/abicheck.py`: a removed or changed
function, type or constant is refused (it needs a new major version, e.g.
`libimage.so.2`, installed next to the old one), an addition is recorded with
`--update`. The `sdk` test in `scripts/test.sh`
builds the template and `examples/hellowin` through `sdk/ritteros.mk` exactly
as an outside project would, deploys them over ssh to a booted RitterOS, and
runs, stops and checks them on both architectures.
