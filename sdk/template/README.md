# hello

A program for [RitterOS](https://github.com/eloyr77/RitterOS), built with its
SDK.

```sh
make                                   # build build/x86_64/hello
make deploy run HOST=root@ADDRESS      # copy it to a RitterOS machine and run it there
```

It needs a RitterOS checkout at `../RitterOS`, Zig 0.17.0 and an SSH key
the machine accepts: see `RitterOS/sdk/CLAUDE.md`. `CLAUDE.md` here is for
AI agents working on this program.
