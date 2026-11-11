# CLAUDE.md

This repository is a program for RitterOS, built with RitterOS's SDK. The
SDK's guide, `RITTEROS/sdk/CLAUDE.md` (with the RitterOS checkout at
`../RitterOS`, that is `../RitterOS/sdk/CLAUDE.md`), is the reference for
everything here: read it before changing anything.

## The program

- `hello.c`: prints a greeting. (Keep this section current as the program
  grows: what it does, its files, how to use it.)
- Its name on RitterOS is `APP` in the `Makefile`; `KIND` is `bin` (a
  command-line program) or `apps` (a windowed one).

## Setup

- A RitterOS checkout at `../RitterOS` (or `make RITTEROS=/path`), Zig
  0.17.0, GNU make, Python 3, an OpenSSH client. (On RitterOS itself the
  same Makefile builds with the machine's own `cc`: it takes
  `/system/sdk/ritteros.mk`, no checkout or Zig needed; the guide's
  "Building on the machine".)
- The RitterOS machine to run it on: `HOST=root@ADDRESS` and
  `SSH_OPTS="-i ~/.ssh/ritteros"` in the environment (the guide's "Setting
  up the RitterOS machine" makes the key work).

## Working

- `make` builds `build/x86_64/APP`; `make deploy run` copies it to the
  machine and runs it (its output and exit code come back); `make stop` ends
  it there; `ARCH=aarch64` for RitterOS on QEMU's ARM machine.
- A change is done when it runs on the machine, not when it compiles: build,
  deploy and run after every change, and read what it printed.
- A windowed app keeps `make run` open until its window closes: run it in
  the background, read its output, `make stop` when done.
- A crash shows only as exit code 139 (or 128 + another signal): the fault
  line is in `ssh $HOST 'cat /system/logs/kernel.log'`.

## Rules (RitterOS is not Linux)

- Only RitterOS's headers (`../RitterOS/user/include`): check that a
  function is there before using it; `../RitterOS/sdk/API.md` lists every
  one (and marks the system's own, which programs outside RitterOS's tree do
  not use). It links to the machine's shared libraries (`LINK = shared`;
  `LINK = static` for a program that carries its own). Much of C11 and POSIX
  is there (stdio, math.h, pthreads, BSD sockets, poll, fork and the exec
  family, posix_spawn). The guide lists what is there.
- Stacks are 8 MiB, their pages made when touched; memory has no paging to
  disk.
- The machine's shell is a small POSIX-like one (pipes, `&&`, `2>`, `$VAR`,
  `if`/`for`, functions): the guide's "The remote shell" lists what works.
- Never put root's password in a file, a commit or a chat.
