# make: build targets from a Makefile's rules (POSIX make with common GNU make)

```
make [-f MAKEFILE]... [-C DIR] [-nsikqBteSw] [NAME=VALUE]... [TARGET]...
```

Reads `GNUmakefile`, `makefile` or `Makefile` from the current folder (the
first of them there, by its exact name), or each MAKEFILE given with `-f`
(`-f -` reads the standard input), and makes each TARGET: the first target
in the file (or `.DEFAULT_GOAL`) if none is named. A target is remade when
it is missing, older than one of its prerequisites, or `.PHONY`; its
prerequisites are made first. `make` has been in RitterOS since step 59:
POSIX's make with what most Makefiles take from GNU make, enough for the
SDK's `ritteros.mk` and RitterOS's own Makefile. It runs one job at a time.

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

What it reads:

- **Rules:** `TARGETS: PREREQUISITES`, then recipe lines that start with a
  tab (or `; RECIPE` on the rule's line); `| ORDER-ONLY` prerequisites
  (made first, but never a reason to remake); pattern rules (`%.o: %.c`),
  static pattern rules (`$(OBJS): %.o: %.c`), suffix rules (`.c.o:`),
  `.DEFAULT`, and built-in rules for C: `%.o` from `%.c` or `%.s`, a
  program from `%.o` or `%.c` (`$(CC)` is `cc`, also `$(AS)`, `$(LD)`,
  `$(AR)`, `$(RM)` = `rm -f`, `$(COMPILE.c)`, `$(LINK.c)`, `$(LINK.o)`).
- **Special targets:** `.PHONY`, `.SILENT`, `.IGNORE`, `.SUFFIXES`,
  `.DEFAULT`, `.PRECIOUS`; `.SECONDARY`, `.INTERMEDIATE`,
  `.NOTPARALLEL`, `.DELETE_ON_ERROR`, `.POSIX`, `.SECONDEXPANSION`,
  `.EXPORT_ALL_VARIABLES` and `.ONESHELL` are taken without an effect.
- **Recipes:** each line runs in its own `sh -c`, shown before it runs;
  `@` in front: not shown; `-`: its failure is ignored; `+`: run under
  `-n` too (as a line with `$(MAKE)` is). A failure stops `make` unless
  `-k` or `-i`.
- **Variables:** `=` (expanded when used), `:=` and `::=` (expanded now),
  `?=` (if not set), `+=`, `!=` (a command's output), `override`, `define
  ... endef`, `export` and `unexport`; `TARGET: NAME = VALUE` for one
  target's recipe (not its prerequisites', unlike GNU make). NAME=VALUE on
  the command line wins over the Makefile's, the environment's lose to it
  (unless `-e`). References: `$(NAME)`, `${NAME}`, `$X`, `$(NAME:.c=.o)`
  and `$(NAME:%.c=%.o)`, the automatic `$@ $< $^ $+ $? $* $|` and their
  `D`/`F` forms (`$(@D)`: the target's folder). Also `$(MAKE)`, `CURDIR`,
  `MAKEFILE_LIST`, `MAKEFLAGS`, `MAKELEVEL`.
- **Functions:** `subst patsubst strip findstring filter filter-out sort
  word wordlist words firstword lastword dir notdir suffix basename
  addsuffix addprefix join wildcard realpath abspath if or and foreach
  call value origin flavor shell eval error warning info`.
- **Directives:** `ifeq`/`ifneq`/`ifdef`/`ifndef`, `else`, `endif`;
  `include`, `-include` and `sinclude` (no message for a missing file);
  `vpath` is taken and ignored.

A `make` started by a recipe gets the options and command-line variables
through `MAKEFLAGS` (the letters `n s k i e` and the variables) and says
where it works: `make[1]: Entering directory '/src/lib'` (`make[0]` for
the first `make` under `-C`). Times are a second fine: a file changed within the same
second as its target does not count as newer.

## Options

| Option | Meaning |
|---|---|
| `-f MAKEFILE` | read MAKEFILE (`-` is the standard input); several are read in turn |
| `-C DIR` | change to DIR first (`make[0]: Entering directory '...'` before the work and `make[0]: Leaving directory '...'` after it) |
| `-n` | show the commands that would run, run none (lines with `+` or `$(MAKE)` still run) |
| `-s` | do not show the commands |
| `-k` | go on after a failure, with whatever does not need what failed |
| `-S` | stop at the first failure (undoes `-k`) |
| `-i` | ignore every failure |
| `-q` | run nothing; exit code 1 if something is out of date |
| `-B` | remake every target |
| `-t` | touch the targets (give them the time now) instead of remaking them |
| `-e` | the environment's variables win over the Makefile's |
| `-w`, `--print-directory` | say `Entering directory`/`Leaving directory` (the default: under `-C` and in a recipe's make) |
| `--no-print-directory` | do not say it, also under `-C` and in a recipe's make |
| `-j [N]`, `-jN` | taken and ignored: one job at a time |
| `-r` | taken and ignored (the built-in rules stay) |

Letters may be combined (`-ns`); `-f` and `-C` take the rest of their word
or the next word (`-Csub`, `-C sub`).

## Exit status and messages

- `0`: everything is made; `make: 'prog' is up to date.` or `make: Nothing
  to be done for 'all'.` when nothing had to run.
- `1`: with `-q`, something is out of date.
- `2` after an error:
  - `make: *** [prog] Error 1`: a recipe line failed (with `-i` or `-`:
    `make: [prog] Error 1 (ignored)` and it goes on); with `-k`, `make:
    Target 'all' not remade because of errors.`
  - `make: *** No rule to make target 'x.c', needed by 'x.o'.  Stop.`
  - `Makefile:12: *** missing separator.  Stop.` and the like: a mistake in
    the file, with its name and line (`$(error TEXT)` stops this way too).
  - `make: *** No targets specified and no makefile found.  Stop.`
  - `make: *** DIR: No such file or directory.  Stop.` (`-C`).
  - `make: invalid option -- 'x'` with `usage: make [-f FILE] [-C DIR]
    [-nsikq] [NAME=VALUE]... [TARGET]...`, `make: unknown option '--x'`,
    `make: option requires an argument -- 'f'`.

## Examples

```
ritteros:/$ cd /home/root/hello
ritteros:/home/root/hello$ make
cc -O2 -c hello.c -o hello.o
cc -o hello hello.o
ritteros:/home/root/hello$ make
make: 'hello' is up to date.
ritteros:/home/root/hello$ make -n clean
rm -f hello hello.o
ritteros:/home/root/hello$ make CFLAGS=-O0 -B
ritteros:/$ make -C /src -s sdk
```

## See also

[`cc`](cc.md), [`as`](as.md), [`ld`](ld.md), [`ar`](ar.md),
[`touch`](touch.md), [`sh`](sh.md)
