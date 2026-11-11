# hello: the first RitterOS program: where it is in memory

```
hello
```

The first program RitterOS ran in user mode, kept as a small check that
programs start: it greets and prints two addresses, where its own code is
(the function `main`) and where its stack is (a local variable). The
numbers differ from one start to the next, because every program is placed
at random addresses (since step 22l): the code somewhere between 4 GiB and
8 TiB, the stack just below 128 TiB, near the top of the program's half
of the address space. It takes no arguments (any given are ignored).

## Exit status and messages

- `0`: always.

## Examples

```
ritteros:/$ hello
Hello from user mode!
  my code is at  0x000003A6C8D8502C
  my stack is at 0x00007FFFC420ACEC
ritteros:/$ hello
Hello from user mode!
  my code is at  0x000000917CCE902C
  my stack is at 0x00007FFFE01B3D5C
```

## See also

[`ps`](ps.md), [`threads`](threads.md), [`mem`](mem.md), [`cc`](cc.md)
(`cc -o hello hello.c` builds such a program on RitterOS)
