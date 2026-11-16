# Porting programs to RitterOS

This guide is for porting existing C and C++ software (libraries,
command-line programs, a browser engine) to RitterOS: how to build it
outside the RitterOS tree, what the system offers, and what it does not
have yet. Items 43 to 46 of the roadmap made the system side (43a-43e:
the C library, the kernel's calls, C++, outside builds, a port's own
libraries; 44: job control; 45: what the WebKit port asked for, a port
of its own, PORT=RitterOS, on the Win port's stack (Skia, curl,
OpenSSL, no GLib): precise timed waits, SOCK_SEQPACKET, large
reservations, JIT memory, semaphores across programs; 46: system
libraries from upstream: zlib, OpenSSL, nghttp2, brotli, libpsl and curl); the details of every call are in
[`sdk/API.md`](../sdk/API.md), the SDK's guide is
[`sdk/CLAUDE.md`](../sdk/CLAUDE.md).

## The target

- **Processors:** x86-64 (every real PC RitterOS runs on) and AArch64
  (QEMU's ARM machine). The compilers' target is `x86_64-unknown-none-elf`
  or `aarch64-unknown-none-elf`: there is no `__linux__`, `__unix__` or
  `__APPLE__`. Code that chooses a platform by those macros needs a case
  for RitterOS (`#ifdef RITTEROS` is not predefined: CMake's `RITTEROS`
  variable and Meson's `host_machine.system() == 'ritteros'` tell a build).
- **Programs** are position-independent ELF executables loaded by
  `/system/<arch>/lib/ld.so`, which binds every symbol at the start (no lazy
  binding, no copy relocations), randomized. Static linking is possible for
  C (`libritteros.a`), not for C++.
- **Libraries** are ELF shared objects. (43e) A port's own libraries go
  into its bundle's `lib` folder, as macOS's apps keep theirs: the program
  records `$ORIGIN/lib` (RUNPATH; the three builds write it), and `ld.so`
  looks in the RUNPATH folders of the file that needs a library, then the
  program's, then `/system/<arch>/lib` (RitterOS's own libraries). No
  `LD_LIBRARY_PATH`; `$ORIGIN` is ignored in a setuid program run by
  another user. Up to 511 libraries a program (43e-2), any number named by
  one file.
- **The toolchain** is the pinned Zig 0.17.0 (its clang and lld), run on a
  Mac; RitterOS's own `cc` builds C on the machine, not C++. What RitterOS
  installs itself is built by clang.

## Three ways to build

All three make the same link (RitterOS's start code, `user/user.ld`,
`ld.so`, the shared libraries the program uses, the C++ ones for C++). Run
`make sdk` in the RitterOS checkout first (`ritteros.mk` does it itself).

| Build | Use | Example |
|---|---|---|
| `sdk/ritteros.mk` | a small project of your own: `SRCS` of `.c` and `.cpp` files; `make deploy run HOST=...` copies and runs it | `sdk/template`, `sdk/examples/hello-cpp` |
| `sdk/ritteros.cmake` | software built by CMake: `cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$RITTEROS/sdk/ritteros.cmake [-DRITTEROS_ARCH=aarch64]` | `sdk/examples/hello-cmake` |
| `sdk/ritteros.meson` | software built by Meson: `meson setup build --cross-file $RITTEROS/sdk/ritteros.meson` (x86_64; for AArch64 a copy with `arch` and `ritteros` set, as its first lines say) | `sdk/examples/hello-meson` |

The pinned CMake 4.3.4, Meson 1.11.2, Ninja 1.13.2 and pkgconf 2.4.3 are in
`$RITTEROS/tools/venv/bin` (`scripts/setup-toolchain.sh` installs them): put
that folder first in `PATH`.

**pkg-config.** Both files point pkg-config at `sdk/pkgconfig`, where
RitterOS's libraries have their `.pc` files: `ritteros`, `libwindow`,
`libimage`, `libaudio`, `libvideo`, `libdraw`, `libwidget`, `libtls`,
`libkeyring`, `libc++`. A port's own libraries add theirs (`PKG_CONFIG_PATH` is cleared:
add a folder with `PKG_CONFIG_LIBDIR`, or install `.pc` files beside
RitterOS's in your own copy).

**Other builds** (Autotools, plain makefiles) take the same pieces by hand:
`CC="$ZIG clang --target=x86_64-unknown-none-elf"`, `CFLAGS` as
`sdk/ritteros.meson`'s `c_args` (`CXXFLAGS` its `cpp_args`), and for the
link `LDFLAGS="--target=x86_64-linux-gnu -nostdlib -fuse-ld=$RITTEROS/sdk/ritteros-ld"`:
`ritteros-ld` turns the compiler's link into RitterOS's. (Not tested with
Autotools yet; `--host=x86_64-linux` makes `configure` treat it as a cross
build.)

## C++

libc++, libc++abi and libunwind (LLVM 22.1.8, the pinned Zig's sources)
are `/system/<arch>/lib/libc++.so.1`, `libc++abi.so.1` and `libunwind.so.1`,
configured by `user/cxx/__config_site`, which every C++ compile gets with
`-include` (the three builds pass it; a build of your own must too). C++23,
exceptions and RTTI, the whole library: threads (pthreads), locales
(`"C"`, `"POSIX"`, `"C.UTF-8"`), `<filesystem>`, `<format>`, `<print>`,
`<regex>`, `<random>` (`std::random_device` reads `getentropy`).
Not there: the time-zone database (`std::chrono::tzdb`), the parallel
algorithms' threads (they run serially), static C++ libraries.
Exceptions need the unwinding tables (`-funwind-tables`, the default of the
three builds) and the frames' index (`--eh-frame-hdr`, the link's): a C++
program linked without it ends in `std::terminate` at its first throw.
Every RitterOS header is `extern "C"` for C++.

## What the system has

The C library is C11 and most of POSIX 2008 with the Linux and BSD calls
ported software uses most. By area (each with its step; `sdk/API.md` has
every declaration):

- **Strings, memory, standard library** (43a-1): `strlcpy`, `strsep`,
  `memmem`, `strcasestr`, `strverscmp`, `getline`, `getdelim`,
  `open_memstream`, `fmemopen`, `fopencookie`, `asprintf`, `getopt_long`,
  `glob`, `fnmatch`, `search.h` (`hsearch`, `tsearch`),
  `err.h`, `libgen.h`, `timegm`, `strptime`, `posix_memalign`,
  `reallocarray`, `qsort_r`, `struct stat` with POSIX's fields and
  nanosecond times, `getpwnam_r`, `grp.h`.
- **Locales and text** (43a-2): `newlocale`, `uselocale`, the `*_l` calls,
  `nl_langinfo`, UTF-8 in `C.UTF-8` (multibyte calls, `wcwidth`, Unicode
  classes and case mapping), `iconv` (UTF-8/16/32, ASCII, ISO-8859-1,
  Windows-1252, `//TRANSLIT`), gettext's calls (they return the text).
- **System and processes** (43a-3): `sysconf`, `getrlimit`, `getrusage`,
  `alarm`, `pause`, `daemon`, `syslog`, `getauxval` (with `AT_HWCAP`:
  43b-6), `getrandom`, `getentropy`, `wait4`, spawn's `addchdir`.
- **Runtimes** (43a-4): `__cxa_atexit`, `__cxa_thread_atexit_impl`,
  `dl_iterate_phdr`, `dladdr`, `sigsetjmp`, POSIX semaphores,
  `pthread_getattr_np` (a thread's stack, for garbage collectors),
  `sched.h`, `ppoll`, `pselect`.
- **Files** (43b-1): `pread`/`pwrite`, `truncate`, `fsync`, `fchmod`,
  `fchown`, every `*at` call (`openat`, `fstatat`, `unlinkat`, ...),
  `O_CLOEXEC` everywhere (`pipe2`, `accept4`, `dup3`), `readv`/`writev`,
  `statvfs`, `fdopendir`, `scandir`.
- **Large reservations** (45c): `mmap` of tens of GiB `PROT_NONE` with
  `MAP_NORESERVE` (taken; no memory is reserved ahead anyway) costs no
  pages until touched (the mmap window is 64 TiB); pieces opened with
  `mprotect` or mapped over with `MAP_FIXED`.
- **JIT memory** (45d): W^X stays (no mapping writable and executable at
  once: `ENOTSUP`); a JIT maps one memory file twice (`memfd_create`, one
  `MAP_SHARED` view `PROT_READ | PROT_WRITE`, one `PROT_READ |
  PROT_EXEC`), writes code through the first and runs it through the
  second, after `__builtin___clear_cache` over it (the C library's
  `__clear_cache`: on AArch64 programs may clean the caches, as on Linux;
  on x86-64 nothing to do). No per-thread W/X switch (memory protection
  keys).
- **Sequenced packets** (45b): `AF_UNIX` `SOCK_SEQPACKET` sockets
  (`socket`, `socketpair`, named listeners): each send is one message kept
  whole (64 KiB the most, else `EMSGSIZE`; descriptors go with their
  message), each receive one message (the rest dropped: `MSG_TRUNC`); a
  stream socket cannot connect to a packet listener (`EPROTOTYPE`).
- **Passing descriptors** (43b-2): `sendmsg`/`recvmsg` with `SCM_RIGHTS`
  over local sockets, `socketpair`.
- **Shared memory** (43b-3): `memfd_create` (with seals), `shm_open`
  (`/dev/shm`), `madvise`, named and process-shared semaphores (futexes in
  shared memory work between programs: 45e checks a `sem_t` in a memory
  file and an `eventfd` passed over a socket to another program, mapped at
  its own address there, as a browser's processes share them).
- **Waiting** (43b-4): `eventfd`, `timerfd`, `flock`, `fcntl`'s record
  locks (`F_SETLK`, `F_SETLKW`, `F_GETLK`), `lockf`.
- **Timed waits** (45a): `nanosleep`, `pthread_cond_timedwait` and the
  other timed waits on a futex, `poll`/`select` timeouts and `timerfd`
  end within about a millisecond of their time (the core's timer armed
  for each deadline); the scheduler's tick stays 100 Hz.
- **Stopping and job control** (44a-44c): the stop signals (`SIGSTOP`,
  `SIGTSTP`, `SIGTTIN`, `SIGTTOU`) stop a process and `SIGCONT` continues
  it (a call the stop interrupted goes on), `waitpid`'s `WUNTRACED` and
  `WCONTINUED` (`WIFSTOPPED`, `WSTOPSIG`, `WIFCONTINUED`), `SIGCHLD`'s
  `CLD_STOPPED` and `CLD_CONTINUED`, `SA_NOCLDSTOP`; at a terminal Ctrl-Z
  is `SIGTSTP`, a background job's read `SIGTTIN`, a write with `TOSTOP`
  or a background `tcsetattr` `SIGTTOU`, an orphaned group with a stopped
  member gets `SIGHUP` and `SIGCONT`; the shell's `jobs`, `fg`, `bg`.
- **Signals** (43b-5): `SA_SIGINFO` with `siginfo_t` and a `ucontext_t`
  with glibc's register names (a handler may read and change them),
  `sigaltstack`, `SIGCHLD`, `kill` with groups and signal 0,
  `pthread_kill`.
- **System libraries from upstream** (46): libraries ported software
  links instead of bringing its own copies, unchanged from their
  projects' sources, in `/system/<arch>/lib` under upstream's names, their
  headers in `/system/include`: zlib 1.4 (`libz.so.1`, 46a), OpenSSL 3.5
  (`libcrypto.so.3`, `libssl.so.3`, the `openssl` program; 46b): its
  default CA file is the system's roots (`/system/config/openssl/cert.pem`
  links `/system/certs/cacert.pem`), no `openssl.cnf` is needed, the
  assembly is on (AES-NI and AVX on x86-64, the ARMv8 crypto
  extensions), no loadable modules (no legacy provider, no engines).
  nghttp2 1.71 (`libnghttp2.so.14`), brotli 1.2 (`libbrotlicommon.so.1`,
  `libbrotlidec.so.1`, `libbrotlienc.so.1`) and libpsl 0.23 (`libpsl.so.5`,
  the Public Suffix List built in, no IDNA library: give it ASCII names)
  since 46c; curl 8.23 (`libcurl.so.4` and the `curl` program, 46d-2:
  OpenSSL, HTTP/2, gzip and brotli, libpsl's cookies, the threaded
  resolver, IPv6, proxies, WebSockets; the CA bundle
  `/system/certs/cacert.pem`). (46e) A port finds them as on Linux:
  `find_package(OpenSSL)`, `(CURL)`, `(ZLIB)`, `(LibPSL)` with
  sdk/ritteros.cmake, pkg-config (`openssl`, `libcurl`, `zlib`, `libpsl`,
  `libbrotlidec`, `libnghttp2`; sdk/pkgconfig), `-lssl`-style links
  through sdk/ritteros-ld and ritteros.mk's `LDLIBS` (sdk/CLAUDE.md says
  how; `sdk/examples/netlibs` uses each).
- **Headers as POSIX's** (46a, 46b): the standard headers declare only
  standard names (`errno.h`, `fcntl.h`, `signal.h`, `poll.h` and the others
  no longer bring in `ritteros.h`: include it for RitterOS's own calls);
  RitterOS's CRC-32 is `ritteros_crc32` (zlib's `crc32` is zlib's);
  `optarg`, `optind`, `opterr` and `optopt` are real variables (a label
  or member of those names compiles); `sys/time.h` gives `select` and
  `fd_set` too; `sys/ioctl.h` has `ioctl` with `FIONBIO`, `FIOCLEX` and
  `FIONCLEX` (others `ENOTTY`).
- **Initializers as on Linux** (71d, the browser port's request 9):
  `__attribute__((constructor))`, `DT_INIT_ARRAY` and C++'s static
  objects run with the environment already there (`getenv` works, the
  program's name, `getauxval`), and each gets `argc, argv, envp` as glibc
  passes them (`dlopen`'s libraries the environment as it is then).
- **The SDK's CMake as ports expect it** (71b, the browser port's
  requests): one compiler program (`sdk/zig-clang`: projects that run the
  compiler themselves), `LINKER:` options, `-pthread` and `-lm`,
  `-lpthread`, `-ldl`, `-lrt` (empty archives: libritteros has it all).
- **Headers as other systems have them** (71a, the browser port's
  requests): `<string.h>` gives `<strings.h>`'s `strcasecmp` and the rest
  too (as glibc by default); `<sys/time.h>` has BSD's `timerclear`,
  `timerisset`, `timercmp`, `timeradd`, `timersub`, `TIMEVAL_TO_TIMESPEC`,
  `TIMESPEC_TO_TIMEVAL`; `<sys/mman.h>` has `MAP_FILE` (0); `<assert.h>`
  has C11's `static_assert`.
- **Threads and the system** (43b-6): `pthread_setname_np`,
  `CLOCK_THREAD_CPUTIME_ID`, `getsid`, spawn's `addfchdir`, `getifaddrs`,
  `if_nametoindex`.
- **Before item 43**: BSD sockets (IPv4 and IPv6, local streams), `poll`,
  `select`, pthreads (cancellation too), `fork`, `exec*`, `posix_spawn`,
  `mmap` (files, anonymous, shared), `dlopen`, `regex.h`, TLS 1.3
  (`libtls`), windows (`libwindow`, `libdraw`, `libwidget`), images,
  sound; (70b) passwords kept for the user (`libkeyring`, `keyring.h`:
  where a browser keeps its saved logins, instead of a file of its own).

## What it does not have (yet)

Plan around these; each is a known gap, not an accident:

- `epoll`, `inotify` (left out of item 43 on purpose: `poll` is there;
  `SOCK_SEQPACKET` came with 45b), `AF_PACKET`, `ioctl`'s network calls
  and terminal sizes (`TIOCGWINSZ`; 46b's `ioctl` has `FIONBIO` and
  close-on-exec only).
- Memory locking: `mlock`, `munlock`, `mlockall` fail with `ENOSYS`
  (pages cannot be kept out of swap, which is encrypted with a key made
  at each `swap on`); OpenSSL's secure heap goes on without it.
- `/dev/urandom` and `/dev/random` (use `getrandom` or `getentropy`).
- `SA_RESTART` does nothing: a call a handled signal interrupted returns
  `EINTR` (one a stop interrupted goes on after `SIGCONT`).
- A per-thread switch between writable and executable for JIT memory
  (memory protection keys, `pkey_mprotect`): use the dual mapping (45d).
- Media and graphics beyond what the SDK has (`/dev/pcm0` for sound, with
  OSS's `SNDCTL_DSP_GETODELAY` and `SNDCTL_DSP_RESET` (68v) and no other
  OSS request: the format is fixed at 48 kHz 16-bit stereo,
  libimage and libvideo's decoders, windows drawn by the program on the
  CPU): no hardware video decoding, no GPU acceleration (OpenGL, EGL,
  Vulkan).
- `timer_create` (POSIX timers; `timerfd` and `alarm` are there),
  `getcontext`/`makecontext`/`swapcontext`.
- Process-shared pthread mutexes (`PTHREAD_PROCESS_SHARED`: semaphores
  are), a memfd mapped `MAP_PRIVATE` for writing.
- `LD_LIBRARY_PATH` (RUNPATH with `$ORIGIN` is there since 43e: a port's
  libraries live in its bundle).
- Real-time signals (`SIGRTMIN`...), `sigqueue`, `sigwaitinfo`.
- The time-zone database: local time is one fixed offset
  (`/system/config/time`).

## Installing and running

`make deploy` (ritteros.mk) copies one program. (43e) A port with libraries
is a bundle: the program, its helper programs and its `lib` folder in
`/system/<arch>/apps/NAME.program/` (windowed: also `info` and `icon.png`;
sdk/CLAUDE.md, "An app's own libraries"), copied whole with tar as root
(`tar cf - -C stage NAME.program | ssh root@HOST 'tar xf - -C /system/x86_64/apps'`).
Give a library a soname with its major version (`libfoo.so.1`): ld.so
loads by the soname the program records.

**When something goes wrong:** `ldd PROGRAM` on the machine lists the
libraries it needs and every symbol none of them has (a program built
against a newer RitterOS than the machine runs); `ld.so` says the same at
the start (exit 127). A crash prints the program, the address and the
signal on the console and in `/system/logs/kernel.log`; `threads` shows a
program's threads, `cpus` where locks are waited for.
