# The RitterOS programs' API

This file is made by `scripts/sdkapi.py` from the headers themselves
(`user/include/` and `common/syscalls.h`); do not edit it by hand. The `sdk`
test fails when it no longer matches them. The guide is [CLAUDE.md](CLAUDE.md).

## Libraries

Since step 22c RitterOS's own programs use **shared libraries**, each one a
file in `/system/<arch>/lib` that `ld.so` (the dynamic loader, in the same
place) maps into a program when it starts: one copy of a library's code in
memory serves every program, and a library can be updated on its own. There
are eight: `libritteros.so.1` (the C library, the system calls, the network
and DNS helpers, hashes, users and settings), `libwindow.so.1`,
`libimage.so.1`, `libaudio.so.1`, `libvideo.so.1`, `libdraw.so.1` (2D
drawing, since 41b), `libwidget.so.1` (controls, since 41e) and `libtls.so.1` (TLS 1.3 connections, since 23d); the Library column
below says which holds a header's functions. The number after `.so` is the
major version: it changes only with a change that would break programs
built before (a new function does not), and such a library would be
installed next to the old one. Libraries export functions only, never data
(`errno` is a function behind its name). The system's own SSH, cipher, GPT
and mkfs code has no shared library: it is in the static archive
`libritteros.a` only, which also holds everything else.

**Programs built with the SDK** link to the shared libraries by default
(`LINK = shared` in `sdk/ritteros.mk`, since 22d); with `LINK = static` they
link `libritteros.a` instead and carry the parts they use (libtls's
included), running on any RitterOS since the calls they make exist. Your
own libraries are static: more files in your project's `SRCS`, or an
archive of your own added to the link. Third-party C code works when it compiles freestanding against these
headers (no host C library: it may only use what is declared here).

**Who it is for.** "apps": for any program. "the system's own": used by
RitterOS's own programs (sshd, disks, osinstall, windowd); it may change
without notice, so programs outside the tree do not use it. `ritteros.h`
is both: its calls for root only or for one system program (the firewall,
logs, users and passwords, elves, mounting, devices, snapshots, the screen,
the network's settings) are the system's own, marked "system" in the index
below. `common/syscalls.h` is the
kernel's interface; programs use the library's calls rather than raw
`SYS_*` numbers, but they need its flags, constants and structures.

## The headers

| Header | What | For | Library |
|---|---|---|---|
| [`stdio.h`](#stdio-h) | formatted output | apps | `libritteros.so.1` |
| [`stdlib.h`](#stdlib-h) | memory, exit, numbers | apps | `libritteros.so.1` |
| [`string.h`](#string-h) | strings and memory | apps | `libritteros.so.1` |
| [`strings.h`](#strings-h) | comparing strings without case | apps | `libritteros.so.1` |
| [`ctype.h`](#ctype-h) | classes of characters (ASCII) | apps | `libritteros.so.1` |
| [`assert.h`](#assert-h) | assert | apps | `libritteros.so.1` |
| [`math.h`](#math-h) | C99's mathematics: trigonometry, exponentials, logarithms, powers | apps | `libritteros.so.1` |
| [`fenv.h`](#fenv-h) | the floating-point environment: rounding directions and exception flags | apps | `libritteros.so.1` |
| [`wchar.h`](#wchar-h) | wide characters and strings, wide streams and formatting (the C locale: a byte each) | apps | `libritteros.so.1` |
| [`wctype.h`](#wctype-h) | classes of wide characters (ASCII) | apps | `libritteros.so.1` |
| [`uchar.h`](#uchar-h) | Unicode characters: UTF-8 always, UTF-16 and code points | apps | `libritteros.so.1` |
| [`complex.h`](#complex-h) | complex numbers: C99's functions and the compiler's * and / helpers | apps | `libritteros.so.1` |
| [`threads.h`](#threads-h) | C11's threads, mutexes, condition variables, call_once, thread-specific storage | apps | `libritteros.so.1` |
| [`setjmp.h`](#setjmp-h) | setjmp and longjmp: jumping back out of deep calls | apps | `libritteros.so.1` |
| [`locale.h`](#locale-h) | the C locale (the only one) | apps | `libritteros.so.1` |
| [`time.h`](#time-h) | the clock, CPU time, dates and their text | apps | `libritteros.so.1` |
| [`inttypes.h`](#inttypes-h) | printf conversions for stdint.h's types | apps | - |
| [`unistd.h`](#unistd-h) | files, processes, sleeping | apps | `libritteros.so.1` |
| [`fcntl.h`](#fcntl-h) | opening files | apps | `libritteros.so.1` |
| [`errno.h`](#errno-h) | error numbers | apps | `libritteros.so.1` |
| [`sys/types.h`](#sys-types-h) | basic types | apps | - |
| [`sys/stat.h`](#sys-stat-h) | file status, directories, permissions | apps | `libritteros.so.1` |
| [`poll.h`](#poll-h) | waiting for several descriptors at once (poll) | apps | `libritteros.so.1` |
| [`sys/socket.h`](#sys-socket-h) | BSD sockets: TCP, UDP and local connections by address | apps | `libritteros.so.1` |
| [`netinet/in.h`](#netinet-in-h) | IPv4 addresses (sockaddr_in) and byte order | apps | `libritteros.so.1` |
| [`netinet/tcp.h`](#netinet-tcp-h) | TCP's socket option | apps | - |
| [`arpa/inet.h`](#arpa-inet-h) | IPv4 addresses as text | apps | `libritteros.so.1` |
| [`sys/un.h`](#sys-un-h) | local socket addresses (sockaddr_un) | apps | - |
| [`sys/time.h`](#sys-time-h) | struct timeval, gettimeofday, utimes | apps | `libritteros.so.1` |
| [`sys/utsname.h`](#sys-utsname-h) | the system's names: uname (release, commit, machine) | apps | `libritteros.so.1` |
| [`utime.h`](#utime-h) | a file's times in seconds (utime) | apps | `libritteros.so.1` |
| [`sys/select.h`](#sys-select-h) | select: waiting for descriptors by sets | apps | `libritteros.so.1` |
| [`pthread.h`](#pthread-h) | POSIX threads, mutexes, condition variables, keys | apps | `libritteros.so.1` |
| [`sched.h`](#sched-h) | sched_yield | apps | `libritteros.so.1` |
| [`spawn.h`](#spawn-h) | posix_spawn: starting a program with file actions and attributes | apps | `libritteros.so.1` |
| [`regex.h`](#regex-h) | regular expressions: regcomp, regexec (POSIX basic and extended) | apps | `libritteros.so.1` |
| [`netdb.h`](#netdb-h) | names and services: getaddrinfo, getnameinfo, gethostbyname | apps | `libritteros.so.1` |
| [`termios.h`](#termios-h) | a terminal's modes: raw input, echo, Ctrl-C (tcgetattr, tcsetattr) | apps | `libritteros.so.1` |
| [`sys/wait.h`](#sys-wait-h) | how a child ended | apps | `libritteros.so.1` |
| [`sys/mman.h`](#sys-mman-h) | memory mappings: anonymous memory, files shared between programs | apps | `libritteros.so.1` |
| [`dlfcn.h`](#dlfcn-h) | loading shared libraries while running (dlopen, dlsym) | apps | `libritteros.so.1` |
| [`dirent.h`](#dirent-h) | reading directories | apps | `libritteros.so.1` |
| [`signal.h`](#signal-h) | signals | apps | `libritteros.so.1` |
| [`pwd.h`](#pwd-h) | users | apps | `libritteros.so.1` |
| [`thread.h`](#thread-h) | threads | apps | `libritteros.so.1` |
| [`ritteros.h`](#ritteros-h) | RitterOS's own calls | apps; parts are the system's own | `libritteros.so.1` |
| [`window.h`](#window-h) | windowed apps (libwindow) | apps | `libwindow.so.1` |
| [`media.h`](#media-h) | the media libraries' types | apps | - |
| [`image.h`](#image-h) | images (libimage) | apps | `libimage.so.1` |
| [`audio.h`](#audio-h) | sound (libaudio) | apps | `libaudio.so.1` |
| [`video.h`](#video-h) | video (libvideo) | apps | `libvideo.so.1` |
| [`draw.h`](#draw-h) | 2D drawing, anti-aliased, on any pixel buffer (libdraw) | apps | `libdraw.so.1` |
| [`widget.h`](#widget-h) | controls for windowed programs: buttons, check boxes, layouts (libwidget) | apps | `libwidget.so.1` |
| [`wfont.h`](#wfont-h) | the windows' font (cell sizes for laying out text) | apps | `libwindow.so.1` |
| [`tls.h`](#tls-h) | TLS 1.3 connections, OpenBSD's libtls interface (libtls) | apps | `libtls.so.1` |
| [`crypto.h`](#crypto-h) | SHA-512, X25519, Ed25519, ChaCha20-Poly1305 (written for SSH); AES-GCM, SHA-384, HKDF, RSA and ECDSA checks (for TLS) | apps | `libritteros.a` only |
| [`x509.h`](#x509-h) | X.509 certificates: reading, chains to trusted roots, host names (libtls uses it) | the system's own | `libritteros.a` only |
| [`ssh.h`](#ssh-h) | the SSH protocol (sshd, ssh) | the system's own | `libritteros.a` only |
| [`gpt.h`](#gpt-h) | GPT partition tables (disks, osinstall) | the system's own | `libritteros.a` only |
| [`mkfs.h`](#mkfs-h) | making file systems (format, osinstall) | the system's own | `libritteros.a` only |
| [`windowproto.h`](#windowproto-h) | the window server's protocol (window.h speaks it) | the system's own | - |
| [`common/syscalls.h`](#common-syscalls-h) | system call numbers, flags and structures, shared with the kernel | apps | - |

## Every function and macro

1183 of them, by name. Each links to its header, where its comment says what it does. "system": the system's own (see above); do not use it outside RitterOS's tree.

| Name | For | Header | Declaration |
|---|---|---|---|
| `__addtf3` | apps | [`math.h`](#math-h) | `long double __addtf3(long double a, long double b)` |
| `__cmptf2` | apps | [`math.h`](#math-h) | `int __cmptf2(long double a, long double b)` |
| `__divdc3` | apps | [`complex.h`](#complex-h) | `double _Complex __divdc3(double a, double b, double c, double d)` |
| `__divsc3` | apps | [`complex.h`](#complex-h) | `float _Complex __divsc3(float a, float b, float c, float d)` |
| `__divtc3` | apps | [`complex.h`](#complex-h) | `long double _Complex __divtc3(long double a, long double b, long double c, long double d)` |
| `__divtf3` | apps | [`math.h`](#math-h) | `long double __divtf3(long double a, long double b)` |
| `__divxc3` | apps | [`complex.h`](#complex-h) | `long double _Complex __divxc3(long double a, long double b, long double c, long double d)` |
| `__eqtf2` | apps | [`math.h`](#math-h) | `int __eqtf2(long double a, long double b)` |
| `__errno` | apps | [`errno.h`](#errno-h) | `int *__errno(void)` |
| `__extenddftf2` | apps | [`math.h`](#math-h) | `long double __extenddftf2(double a)` |
| `__extendsftf2` | apps | [`math.h`](#math-h) | `long double __extendsftf2(float a)` |
| `__fixtfdi` | apps | [`math.h`](#math-h) | `long __fixtfdi(long double a)` |
| `__fixtfsi` | apps | [`math.h`](#math-h) | `int __fixtfsi(long double a)` |
| `__fixunstfdi` | apps | [`math.h`](#math-h) | `unsigned long __fixunstfdi(long double a)` |
| `__fixunstfsi` | apps | [`math.h`](#math-h) | `unsigned int __fixunstfsi(long double a)` |
| `__floatditf` | apps | [`math.h`](#math-h) | `long double __floatditf(long a)` |
| `__floatsitf` | apps | [`math.h`](#math-h) | `long double __floatsitf(int a)` |
| `__floatunditf` | apps | [`math.h`](#math-h) | `long double __floatunditf(unsigned long a)` |
| `__floatunsitf` | apps | [`math.h`](#math-h) | `long double __floatunsitf(unsigned int a)` |
| `__getf2` | apps | [`math.h`](#math-h) | `int __getf2(long double a, long double b)` |
| `__gttf2` | apps | [`math.h`](#math-h) | `int __gttf2(long double a, long double b)` |
| `__h_errno` | apps | [`netdb.h`](#netdb-h) | `int *__h_errno(void)` |
| `__letf2` | apps | [`math.h`](#math-h) | `int __letf2(long double a, long double b)` |
| `__lttf2` | apps | [`math.h`](#math-h) | `int __lttf2(long double a, long double b)` |
| `__muldc3` | apps | [`complex.h`](#complex-h) | `double _Complex __muldc3(double a, double b, double c, double d)` |
| `__mulsc3` | apps | [`complex.h`](#complex-h) | `float _Complex __mulsc3(float a, float b, float c, float d)` |
| `__multc3` | apps | [`complex.h`](#complex-h) | `long double _Complex __multc3(long double a, long double b, long double c, long double d)` |
| `__multf3` | apps | [`math.h`](#math-h) | `long double __multf3(long double a, long double b)` |
| `__mulxc3` | apps | [`complex.h`](#complex-h) | `long double _Complex __mulxc3(long double a, long double b, long double c, long double d)` |
| `__negtf2` | apps | [`math.h`](#math-h) | `long double __negtf2(long double a)` |
| `__netf2` | apps | [`math.h`](#math-h) | `int __netf2(long double a, long double b)` |
| `__powitf2` | apps | [`math.h`](#math-h) | `long double __powitf2(long double a, int b)` |
| `__pthread_cleanup_pop` | apps | [`pthread.h`](#pthread-h) | `void __pthread_cleanup_pop(struct __pthread_cleanup *c, int execute)` |
| `__pthread_cleanup_push` | apps | [`pthread.h`](#pthread-h) | `void __pthread_cleanup_push(struct __pthread_cleanup *c, void (*fn)(void *), void *arg)` |
| `__ritteros_dl_setup` | apps | [`ritteros.h`](#ritteros-h) | `int __ritteros_dl_setup(const struct ritteros_dl_loader *loader)` |
| `__ritteros_environ` | apps | [`unistd.h`](#unistd-h) | `char ***__ritteros_environ(void)` |
| `__ritteros_in6addr_any` | apps | [`netinet/in.h`](#netinet-in-h) | `const struct in6_addr *__ritteros_in6addr_any(void)` |
| `__ritteros_in6addr_loopback` | apps | [`netinet/in.h`](#netinet-in-h) | `const struct in6_addr *__ritteros_in6addr_loopback(void)` |
| `__ritteros_start` | apps | [`ritteros.h`](#ritteros-h) | `void __ritteros_start(long *sp)` |
| `__ritteros_tls_add` | apps | [`ritteros.h`](#ritteros-h) | `int __ritteros_tls_add(const struct ritteros_tls_module *module)` |
| `__ritteros_tls_setup` | apps | [`ritteros.h`](#ritteros-h) | `int __ritteros_tls_setup(const struct ritteros_tls_module *modules, int count)` |
| `__subtf3` | apps | [`math.h`](#math-h) | `long double __subtf3(long double a, long double b)` |
| `__tls_get_addr` | apps | [`ritteros.h`](#ritteros-h) | `void *__tls_get_addr(void *index)` |
| `__trunctfdf2` | apps | [`math.h`](#math-h) | `double __trunctfdf2(long double a)` |
| `__trunctfsf2` | apps | [`math.h`](#math-h) | `float __trunctfsf2(long double a)` |
| `__unordtf2` | apps | [`math.h`](#math-h) | `int __unordtf2(long double a, long double b)` |
| `_Exit` | apps | [`stdlib.h`](#stdlib-h) | `void _Exit(int code)` |
| `_exit` | apps | [`unistd.h`](#unistd-h) | `void _exit(int code)` |
| `abort` | apps | [`stdlib.h`](#stdlib-h) | `void abort(void)` |
| `abs` | apps | [`stdlib.h`](#stdlib-h) | `int abs(int v)` |
| `accept` | apps | [`sys/socket.h`](#sys-socket-h) | `int accept(int fd, struct sockaddr *address, socklen_t *length)` |
| `acos` | apps | [`math.h`](#math-h) | `double acos(double x)` |
| `acosf` | apps | [`math.h`](#math-h) | `float acosf(float x)` |
| `acosh` | apps | [`math.h`](#math-h) | `double acosh(double x)` |
| `acoshf` | apps | [`math.h`](#math-h) | `float acoshf(float x)` |
| `acoshl` | apps | [`math.h`](#math-h) | `long double acoshl(long double x)` |
| `acosl` | apps | [`math.h`](#math-h) | `long double acosl(long double x)` |
| `aes_ccm_open` | apps | [`crypto.h`](#crypto-h) | `bool aes_ccm_open(const struct aes *a, const uint8_t nonce[13], const void *aad, size_t aad_n, const void *in, void *out, size_t n, const uint8_t *tag, size_t tag_length)` |
| `aes_ccm_seal` | apps | [`crypto.h`](#crypto-h) | `void aes_ccm_seal(const struct aes *a, const uint8_t nonce[13], const void *aad, size_t aad_n, const void *in, void *out, size_t n, uint8_t *tag, size_t tag_length)` |
| `aes_cmac` | apps | [`crypto.h`](#crypto-h) | `void aes_cmac(const struct aes *a, const void *data, size_t n, uint8_t tag[16])` |
| `aes_decrypt` | apps | [`crypto.h`](#crypto-h) | `void aes_decrypt(const struct aes *a, const uint8_t in[16], uint8_t out[16])` |
| `aes_decrypt4` | apps | [`crypto.h`](#crypto-h) | `void aes_decrypt4(const struct aes *a, const uint8_t in[64], uint8_t out[64])` |
| `aes_encrypt` | apps | [`crypto.h`](#crypto-h) | `void aes_encrypt(const struct aes *a, const uint8_t in[16], uint8_t out[16])` |
| `aes_encrypt4` | apps | [`crypto.h`](#crypto-h) | `void aes_encrypt4(const struct aes *a, const uint8_t in[64], uint8_t out[64])` |
| `aes_gcm_open` | apps | [`crypto.h`](#crypto-h) | `bool aes_gcm_open(const struct aes *a, const uint8_t iv[12], const void *aad, size_t aad_n, const void *in, void *out, size_t n, const uint8_t tag[16])` |
| `aes_gcm_seal` | apps | [`crypto.h`](#crypto-h) | `void aes_gcm_seal(const struct aes *a, const uint8_t iv[12], const void *aad, size_t aad_n, const void *in, void *out, size_t n, uint8_t tag[16])` |
| `aes_init` | apps | [`crypto.h`](#crypto-h) | `void aes_init(struct aes *a, const uint8_t *key, size_t length)` |
| `aes_key_unwrap` | apps | [`crypto.h`](#crypto-h) | `bool aes_key_unwrap(const struct aes *kek, const uint8_t *in, size_t n, uint8_t *out)` |
| `aes_key_wrap` | apps | [`crypto.h`](#crypto-h) | `void aes_key_wrap(const struct aes *kek, const uint8_t *in, size_t n, uint8_t *out)` |
| `aligned_alloc` | apps | [`stdlib.h`](#stdlib-h) | `void *aligned_alloc(size_t alignment, size_t size)` |
| `asctime` | apps | [`time.h`](#time-h) | `char *asctime(const struct tm *tm)` |
| `asin` | apps | [`math.h`](#math-h) | `double asin(double x)` |
| `asinf` | apps | [`math.h`](#math-h) | `float asinf(float x)` |
| `asinh` | apps | [`math.h`](#math-h) | `double asinh(double x)` |
| `asinhf` | apps | [`math.h`](#math-h) | `float asinhf(float x)` |
| `asinhl` | apps | [`math.h`](#math-h) | `long double asinhl(long double x)` |
| `asinl` | apps | [`math.h`](#math-h) | `long double asinl(long double x)` |
| `assert` | apps | [`assert.h`](#assert-h) | `#define assert(expression)` |
| `assert` | apps | [`assert.h`](#assert-h) | `#define assert(expression)` |
| `at_quick_exit` | apps | [`stdlib.h`](#stdlib-h) | `int at_quick_exit(void (*fn)(void))` |
| `atan` | apps | [`math.h`](#math-h) | `double atan(double x)` |
| `atan2` | apps | [`math.h`](#math-h) | `double atan2(double y, double x)` |
| `atan2f` | apps | [`math.h`](#math-h) | `float atan2f(float y, float x)` |
| `atan2l` | apps | [`math.h`](#math-h) | `long double atan2l(long double y, long double x)` |
| `atanf` | apps | [`math.h`](#math-h) | `float atanf(float x)` |
| `atanh` | apps | [`math.h`](#math-h) | `double atanh(double x)` |
| `atanhf` | apps | [`math.h`](#math-h) | `float atanhf(float x)` |
| `atanhl` | apps | [`math.h`](#math-h) | `long double atanhl(long double x)` |
| `atanl` | apps | [`math.h`](#math-h) | `long double atanl(long double x)` |
| `atexit` | apps | [`stdlib.h`](#stdlib-h) | `int atexit(void (*fn)(void))` |
| `atof` | apps | [`stdlib.h`](#stdlib-h) | `double atof(const char *s)` |
| `atoi` | apps | [`stdlib.h`](#stdlib-h) | `int atoi(const char *s)` |
| `atol` | apps | [`stdlib.h`](#stdlib-h) | `long atol(const char *s)` |
| `atoll` | apps | [`stdlib.h`](#stdlib-h) | `long long atoll(const char *s)` |
| `audio_free` | apps | [`audio.h`](#audio-h) | `void audio_free(struct audio_buffer *buf)` |
| `audio_info` | apps | [`audio.h`](#audio-h) | `int audio_info(const char *path, struct audio_buffer *out)` |
| `audio_load` | apps | [`audio.h`](#audio-h) | `int audio_load(const char *path, struct audio_buffer *out)` |
| `audio_load_memory` | apps | [`audio.h`](#audio-h) | `int audio_load_memory(const void *data, long n, struct audio_buffer *out)` |
| `audio_mix` | apps | [`audio.h`](#audio-h) | `int audio_mix(struct audio_buffer *dst, const struct audio_buffer *src, float gain)` |
| `audio_resample` | apps | [`audio.h`](#audio-h) | `int audio_resample(const struct audio_buffer *src, int rate, struct audio_buffer *out)` |
| `audio_save` | apps | [`audio.h`](#audio-h) | `int audio_save(const char *path, const struct audio_buffer *buf)` |
| `base64_decode` | system | [`ssh.h`](#ssh-h) | `long base64_decode(const char *in, size_t n, uint8_t *out, size_t size)` |
| `base64_encode` | system | [`ssh.h`](#ssh-h) | `size_t base64_encode(const uint8_t *in, size_t n, char *out, bool padding)` |
| `bind` | apps | [`sys/socket.h`](#sys-socket-h) | `int bind(int fd, const struct sockaddr *address, socklen_t length)` |
| `bsearch` | apps | [`stdlib.h`](#stdlib-h) | `void *bsearch(const void *key, const void *base, size_t count, size_t size, int (*compare)(const void *key, const void *element))` |
| `btowc` | apps | [`wchar.h`](#wchar-h) | `wint_t btowc(int c)` |
| `c16rtomb` | apps | [`uchar.h`](#uchar-h) | `size_t c16rtomb(char *s, char16_t c, mbstate_t *state)` |
| `c32rtomb` | apps | [`uchar.h`](#uchar-h) | `size_t c32rtomb(char *s, char32_t c, mbstate_t *state)` |
| `c8rtomb` | apps | [`uchar.h`](#uchar-h) | `size_t c8rtomb(char *s, char8_t c, mbstate_t *state)` |
| `cabs` | apps | [`complex.h`](#complex-h) | `double cabs(double _Complex z)` |
| `cabsf` | apps | [`complex.h`](#complex-h) | `float cabsf(float _Complex z)` |
| `cabsl` | apps | [`complex.h`](#complex-h) | `long double cabsl(long double _Complex z)` |
| `cacos` | apps | [`complex.h`](#complex-h) | `double _Complex cacos(double _Complex z)` |
| `cacosf` | apps | [`complex.h`](#complex-h) | `float _Complex cacosf(float _Complex z)` |
| `cacosh` | apps | [`complex.h`](#complex-h) | `double _Complex cacosh(double _Complex z)` |
| `cacoshf` | apps | [`complex.h`](#complex-h) | `float _Complex cacoshf(float _Complex z)` |
| `cacoshl` | apps | [`complex.h`](#complex-h) | `long double _Complex cacoshl(long double _Complex z)` |
| `cacosl` | apps | [`complex.h`](#complex-h) | `long double _Complex cacosl(long double _Complex z)` |
| `call_once` | apps | [`threads.h`](#threads-h) | `void call_once(once_flag *flag, void (*func)(void))` |
| `calloc` | apps | [`stdlib.h`](#stdlib-h) | `void *calloc(size_t count, size_t size)` |
| `carg` | apps | [`complex.h`](#complex-h) | `double carg(double _Complex z)` |
| `cargf` | apps | [`complex.h`](#complex-h) | `float cargf(float _Complex z)` |
| `cargl` | apps | [`complex.h`](#complex-h) | `long double cargl(long double _Complex z)` |
| `casin` | apps | [`complex.h`](#complex-h) | `double _Complex casin(double _Complex z)` |
| `casinf` | apps | [`complex.h`](#complex-h) | `float _Complex casinf(float _Complex z)` |
| `casinh` | apps | [`complex.h`](#complex-h) | `double _Complex casinh(double _Complex z)` |
| `casinhf` | apps | [`complex.h`](#complex-h) | `float _Complex casinhf(float _Complex z)` |
| `casinhl` | apps | [`complex.h`](#complex-h) | `long double _Complex casinhl(long double _Complex z)` |
| `casinl` | apps | [`complex.h`](#complex-h) | `long double _Complex casinl(long double _Complex z)` |
| `catan` | apps | [`complex.h`](#complex-h) | `double _Complex catan(double _Complex z)` |
| `catanf` | apps | [`complex.h`](#complex-h) | `float _Complex catanf(float _Complex z)` |
| `catanh` | apps | [`complex.h`](#complex-h) | `double _Complex catanh(double _Complex z)` |
| `catanhf` | apps | [`complex.h`](#complex-h) | `float _Complex catanhf(float _Complex z)` |
| `catanhl` | apps | [`complex.h`](#complex-h) | `long double _Complex catanhl(long double _Complex z)` |
| `catanl` | apps | [`complex.h`](#complex-h) | `long double _Complex catanl(long double _Complex z)` |
| `cbrt` | apps | [`math.h`](#math-h) | `double cbrt(double x)` |
| `cbrtf` | apps | [`math.h`](#math-h) | `float cbrtf(float x)` |
| `cbrtl` | apps | [`math.h`](#math-h) | `long double cbrtl(long double x)` |
| `ccos` | apps | [`complex.h`](#complex-h) | `double _Complex ccos(double _Complex z)` |
| `ccosf` | apps | [`complex.h`](#complex-h) | `float _Complex ccosf(float _Complex z)` |
| `ccosh` | apps | [`complex.h`](#complex-h) | `double _Complex ccosh(double _Complex z)` |
| `ccoshf` | apps | [`complex.h`](#complex-h) | `float _Complex ccoshf(float _Complex z)` |
| `ccoshl` | apps | [`complex.h`](#complex-h) | `long double _Complex ccoshl(long double _Complex z)` |
| `ccosl` | apps | [`complex.h`](#complex-h) | `long double _Complex ccosl(long double _Complex z)` |
| `ceil` | apps | [`math.h`](#math-h) | `double ceil(double x)` |
| `ceilf` | apps | [`math.h`](#math-h) | `float ceilf(float x)` |
| `ceill` | apps | [`math.h`](#math-h) | `long double ceill(long double x)` |
| `cexp` | apps | [`complex.h`](#complex-h) | `double _Complex cexp(double _Complex z)` |
| `cexpf` | apps | [`complex.h`](#complex-h) | `float _Complex cexpf(float _Complex z)` |
| `cexpl` | apps | [`complex.h`](#complex-h) | `long double _Complex cexpl(long double _Complex z)` |
| `cfgetispeed` | apps | [`termios.h`](#termios-h) | `speed_t cfgetispeed(const struct termios *t)` |
| `cfgetospeed` | apps | [`termios.h`](#termios-h) | `speed_t cfgetospeed(const struct termios *t)` |
| `cfmakeraw` | apps | [`termios.h`](#termios-h) | `void cfmakeraw(struct termios *t)` |
| `cfsetispeed` | apps | [`termios.h`](#termios-h) | `int cfsetispeed(struct termios *t, speed_t speed)` |
| `cfsetospeed` | apps | [`termios.h`](#termios-h) | `int cfsetospeed(struct termios *t, speed_t speed)` |
| `chacha20` | apps | [`crypto.h`](#crypto-h) | `void chacha20(const uint8_t key[32], const uint8_t block[16], const void *in, void *out, size_t n)` |
| `chdir` | apps | [`unistd.h`](#unistd-h) | `int chdir(const char *path)` |
| `chmod` | apps | [`sys/stat.h`](#sys-stat-h) | `int chmod(const char *path, mode_t mode)` |
| `chown` | apps | [`sys/stat.h`](#sys-stat-h) | `int chown(const char *path, uid_t uid, gid_t gid)` |
| `cimag` | apps | [`complex.h`](#complex-h) | `double cimag(double _Complex z)` |
| `cimagf` | apps | [`complex.h`](#complex-h) | `float cimagf(float _Complex z)` |
| `cimagl` | apps | [`complex.h`](#complex-h) | `long double cimagl(long double _Complex z)` |
| `clearerr` | apps | [`stdio.h`](#stdio-h) | `void clearerr(FILE *f)` |
| `clock` | apps | [`time.h`](#time-h) | `clock_t clock(void)` |
| `clock_gettime` | apps | [`time.h`](#time-h) | `int clock_gettime(clockid_t clock, struct timespec *ts)` |
| `clog` | apps | [`complex.h`](#complex-h) | `double _Complex clog(double _Complex z)` |
| `clogf` | apps | [`complex.h`](#complex-h) | `float _Complex clogf(float _Complex z)` |
| `clogl` | apps | [`complex.h`](#complex-h) | `long double _Complex clogl(long double _Complex z)` |
| `close` | apps | [`unistd.h`](#unistd-h) | `int close(int fd)` |
| `closedir` | apps | [`dirent.h`](#dirent-h) | `int closedir(DIR *dir)` |
| `CMPLX` | apps | [`complex.h`](#complex-h) | `#define CMPLX(x, y)` |
| `CMPLXF` | apps | [`complex.h`](#complex-h) | `#define CMPLXF(x, y)` |
| `CMPLXL` | apps | [`complex.h`](#complex-h) | `#define CMPLXL(x, y)` |
| `cnd_broadcast` | apps | [`threads.h`](#threads-h) | `int cnd_broadcast(cnd_t *c)` |
| `cnd_destroy` | apps | [`threads.h`](#threads-h) | `void cnd_destroy(cnd_t *c)` |
| `cnd_init` | apps | [`threads.h`](#threads-h) | `int cnd_init(cnd_t *c)` |
| `cnd_signal` | apps | [`threads.h`](#threads-h) | `int cnd_signal(cnd_t *c)` |
| `cnd_timedwait` | apps | [`threads.h`](#threads-h) | `int cnd_timedwait(cnd_t *c, mtx_t *m, const struct timespec *until)` |
| `cnd_wait` | apps | [`threads.h`](#threads-h) | `int cnd_wait(cnd_t *c, mtx_t *m)` |
| `config_get` | apps | [`ritteros.h`](#ritteros-h) | `int config_get(const char *path, const char *key, char *value, size_t size)` |
| `conj` | apps | [`complex.h`](#complex-h) | `double _Complex conj(double _Complex z)` |
| `conjf` | apps | [`complex.h`](#complex-h) | `float _Complex conjf(float _Complex z)` |
| `conjl` | apps | [`complex.h`](#complex-h) | `long double _Complex conjl(long double _Complex z)` |
| `connect` | apps | [`sys/socket.h`](#sys-socket-h) | `int connect(int fd, const struct sockaddr *address, socklen_t length)` |
| `copysign` | apps | [`math.h`](#math-h) | `double copysign(double x, double y)` |
| `copysignf` | apps | [`math.h`](#math-h) | `float copysignf(float x, float y)` |
| `copysignl` | apps | [`math.h`](#math-h) | `long double copysignl(long double x, long double y)` |
| `cos` | apps | [`math.h`](#math-h) | `double cos(double x)` |
| `cosf` | apps | [`math.h`](#math-h) | `float cosf(float x)` |
| `cosh` | apps | [`math.h`](#math-h) | `double cosh(double x)` |
| `coshf` | apps | [`math.h`](#math-h) | `float coshf(float x)` |
| `coshl` | apps | [`math.h`](#math-h) | `long double coshl(long double x)` |
| `cosl` | apps | [`math.h`](#math-h) | `long double cosl(long double x)` |
| `cpow` | apps | [`complex.h`](#complex-h) | `double _Complex cpow(double _Complex z, double _Complex w)` |
| `cpowf` | apps | [`complex.h`](#complex-h) | `float _Complex cpowf(float _Complex z, float _Complex w)` |
| `cpowl` | apps | [`complex.h`](#complex-h) | `long double _Complex cpowl(long double _Complex z, long double _Complex w)` |
| `cproj` | apps | [`complex.h`](#complex-h) | `double _Complex cproj(double _Complex z)` |
| `cprojf` | apps | [`complex.h`](#complex-h) | `float _Complex cprojf(float _Complex z)` |
| `cprojl` | apps | [`complex.h`](#complex-h) | `long double _Complex cprojl(long double _Complex z)` |
| `crc32` | apps | [`ritteros.h`](#ritteros-h) | `uint32_t crc32(const void *data, size_t size)` |
| `crc32c` | apps | [`ritteros.h`](#ritteros-h) | `uint32_t crc32c(const void *data, size_t size)` |
| `creal` | apps | [`complex.h`](#complex-h) | `double creal(double _Complex z)` |
| `crealf` | apps | [`complex.h`](#complex-h) | `float crealf(float _Complex z)` |
| `creall` | apps | [`complex.h`](#complex-h) | `long double creall(long double _Complex z)` |
| `creat` | apps | [`fcntl.h`](#fcntl-h) | `int creat(const char *path, mode_t mode)` |
| `crypto_equal` | apps | [`crypto.h`](#crypto-h) | `bool crypto_equal(const void *a, const void *b, size_t n)` |
| `csin` | apps | [`complex.h`](#complex-h) | `double _Complex csin(double _Complex z)` |
| `csinf` | apps | [`complex.h`](#complex-h) | `float _Complex csinf(float _Complex z)` |
| `csinh` | apps | [`complex.h`](#complex-h) | `double _Complex csinh(double _Complex z)` |
| `csinhf` | apps | [`complex.h`](#complex-h) | `float _Complex csinhf(float _Complex z)` |
| `csinhl` | apps | [`complex.h`](#complex-h) | `long double _Complex csinhl(long double _Complex z)` |
| `csinl` | apps | [`complex.h`](#complex-h) | `long double _Complex csinl(long double _Complex z)` |
| `csqrt` | apps | [`complex.h`](#complex-h) | `double _Complex csqrt(double _Complex z)` |
| `csqrtf` | apps | [`complex.h`](#complex-h) | `float _Complex csqrtf(float _Complex z)` |
| `csqrtl` | apps | [`complex.h`](#complex-h) | `long double _Complex csqrtl(long double _Complex z)` |
| `ctan` | apps | [`complex.h`](#complex-h) | `double _Complex ctan(double _Complex z)` |
| `ctanf` | apps | [`complex.h`](#complex-h) | `float _Complex ctanf(float _Complex z)` |
| `ctanh` | apps | [`complex.h`](#complex-h) | `double _Complex ctanh(double _Complex z)` |
| `ctanhf` | apps | [`complex.h`](#complex-h) | `float _Complex ctanhf(float _Complex z)` |
| `ctanhl` | apps | [`complex.h`](#complex-h) | `long double _Complex ctanhl(long double _Complex z)` |
| `ctanl` | apps | [`complex.h`](#complex-h) | `long double _Complex ctanl(long double _Complex z)` |
| `ctime` | apps | [`time.h`](#time-h) | `char *ctime(const time_t *t)` |
| `difftime` | apps | [`time.h`](#time-h) | `double difftime(time_t end, time_t start)` |
| `div` | apps | [`stdlib.h`](#stdlib-h) | `div_t div(int numerator, int denominator)` |
| `dlclose` | apps | [`dlfcn.h`](#dlfcn-h) | `int dlclose(void *handle)` |
| `dlerror` | apps | [`dlfcn.h`](#dlfcn-h) | `char *dlerror(void)` |
| `dlopen` | apps | [`dlfcn.h`](#dlfcn-h) | `void *dlopen(const char *file, int mode)` |
| `dlsym` | apps | [`dlfcn.h`](#dlfcn-h) | `void *dlsym(void *handle, const char *name)` |
| `dns_answer` | system | [`ritteros.h`](#ritteros-h) | `int dns_answer(const uint8_t *m, size_t length, uint32_t *address, uint32_t *ttl)` |
| `dns_answer_aaaa` | apps | [`ritteros.h`](#ritteros-h) | `int dns_answer_aaaa(const uint8_t *m, size_t length, uint8_t address[16], uint32_t *ttl)` |
| `dns_failure` | system | [`ritteros.h`](#ritteros-h) | `size_t dns_failure(const uint8_t *query, size_t length, int rcode, uint8_t *out, size_t size)` |
| `dns_query` | system | [`ritteros.h`](#ritteros-h) | `size_t dns_query(const char *name, uint16_t id, uint8_t *out, size_t size)` |
| `dns_query_type` | apps | [`ritteros.h`](#ritteros-h) | `size_t dns_query_type(const char *name, uint16_t id, uint16_t type, uint8_t *out, size_t size)` |
| `dns_question` | system | [`ritteros.h`](#ritteros-h) | `bool dns_question(const uint8_t *m, size_t length, char *name, size_t size)` |
| `dns_question_type` | apps | [`ritteros.h`](#ritteros-h) | `bool dns_question_type(const uint8_t *m, size_t length, char *name, size_t size, uint16_t *type)` |
| `dns_reply` | system | [`ritteros.h`](#ritteros-h) | `size_t dns_reply(const uint8_t *query, size_t length, uint32_t address, uint32_t ttl, uint8_t *out, size_t size)` |
| `dns_reply_aaaa` | apps | [`ritteros.h`](#ritteros-h) | `size_t dns_reply_aaaa(const uint8_t *query, size_t length, const uint8_t address[16], uint32_t ttl, uint8_t *out, size_t size)` |
| `dprintf` | apps | [`stdio.h`](#stdio-h) | `int dprintf(int fd, const char *format, ...)` |
| `draw_circle` | apps | [`draw.h`](#draw-h) | `void draw_circle(struct draw_canvas *c, float cx, float cy, float r, float line, uint32_t color)` |
| `draw_clip` | apps | [`draw.h`](#draw-h) | `void draw_clip(struct draw_canvas *c, int x, int y, int w, int h)` |
| `draw_ellipse` | apps | [`draw.h`](#draw-h) | `void draw_ellipse(struct draw_canvas *c, float cx, float cy, float rx, float ry, float line, uint32_t color)` |
| `draw_gradient` | apps | [`draw.h`](#draw-h) | `void draw_gradient(struct draw_canvas *c, float x, float y, float w, float h, float x0, float y0, uint32_t from, float x1, float y1, uint32_t to)` |
| `draw_image` | apps | [`draw.h`](#draw-h) | `void draw_image(struct draw_canvas *c, const struct image *img, float x, float y, float w, float h, float angle)` |
| `draw_line` | apps | [`draw.h`](#draw-h) | `void draw_line(struct draw_canvas *c, float x0, float y0, float x1, float y1, float width, uint32_t color)` |
| `draw_polygon` | apps | [`draw.h`](#draw-h) | `void draw_polygon(struct draw_canvas *c, const float *xy, int n, float line, uint32_t color)` |
| `draw_rect` | apps | [`draw.h`](#draw-h) | `void draw_rect(struct draw_canvas *c, float x, float y, float w, float h, float line, uint32_t color)` |
| `draw_rgba` | apps | [`draw.h`](#draw-h) | `uint32_t draw_rgba(int r, int g, int b, int a)` |
| `draw_round_rect` | apps | [`draw.h`](#draw-h) | `void draw_round_rect(struct draw_canvas *c, float x, float y, float w, float h, float radius, float line, uint32_t color)` |
| `dup` | apps | [`unistd.h`](#unistd-h) | `int dup(int fd)` |
| `dup2` | apps | [`unistd.h`](#unistd-h) | `int dup2(int fd, int newfd)` |
| `ecdsa_verify` | apps | [`crypto.h`](#crypto-h) | `bool ecdsa_verify(int bits, const uint8_t *pub, size_t pub_len, const uint8_t *digest, size_t dlen, const uint8_t *r, size_t rlen, const uint8_t *s, size_t slen)` |
| `ed25519_public` | apps | [`crypto.h`](#crypto-h) | `void ed25519_public(uint8_t public_key[32], const uint8_t seed[32])` |
| `ed25519_sign` | apps | [`crypto.h`](#crypto-h) | `void ed25519_sign(uint8_t signature[64], const uint8_t seed[32], const uint8_t public_key[32], const void *message, size_t n)` |
| `ed25519_verify` | apps | [`crypto.h`](#crypto-h) | `bool ed25519_verify(const uint8_t signature[64], const uint8_t public_key[32], const void *message, size_t n)` |
| `elvesd_pid` | system | [`ritteros.h`](#ritteros-h) | `int elvesd_pid(void)` |
| `elvesd_start` | system | [`ritteros.h`](#ritteros-h) | `int elvesd_start(const char *bin_dir)` |
| `erf` | apps | [`math.h`](#math-h) | `double erf(double x)` |
| `erfc` | apps | [`math.h`](#math-h) | `double erfc(double x)` |
| `erfcf` | apps | [`math.h`](#math-h) | `float erfcf(float x)` |
| `erfcl` | apps | [`math.h`](#math-h) | `long double erfcl(long double x)` |
| `erff` | apps | [`math.h`](#math-h) | `float erff(float x)` |
| `erfl` | apps | [`math.h`](#math-h) | `long double erfl(long double x)` |
| `execl` | apps | [`unistd.h`](#unistd-h) | `int execl(const char *path, const char *arg, ...)` |
| `execle` | apps | [`unistd.h`](#unistd-h) | `int execle(const char *path, const char *arg, ...)` |
| `execlp` | apps | [`unistd.h`](#unistd-h) | `int execlp(const char *name, const char *arg, ...)` |
| `execv` | apps | [`unistd.h`](#unistd-h) | `int execv(const char *path, char *const argv[])` |
| `execve` | apps | [`unistd.h`](#unistd-h) | `int execve(const char *path, char *const argv[], char *const envp[])` |
| `execvp` | apps | [`unistd.h`](#unistd-h) | `int execvp(const char *name, char *const argv[])` |
| `exit` | apps | [`stdlib.h`](#stdlib-h) | `void exit(int code)` |
| `exp` | apps | [`math.h`](#math-h) | `double exp(double x)` |
| `exp2` | apps | [`math.h`](#math-h) | `double exp2(double x)` |
| `exp2f` | apps | [`math.h`](#math-h) | `float exp2f(float x)` |
| `exp2l` | apps | [`math.h`](#math-h) | `long double exp2l(long double x)` |
| `expf` | apps | [`math.h`](#math-h) | `float expf(float x)` |
| `expl` | apps | [`math.h`](#math-h) | `long double expl(long double x)` |
| `expm1` | apps | [`math.h`](#math-h) | `double expm1(double x)` |
| `expm1f` | apps | [`math.h`](#math-h) | `float expm1f(float x)` |
| `expm1l` | apps | [`math.h`](#math-h) | `long double expm1l(long double x)` |
| `fabs` | apps | [`math.h`](#math-h) | `double fabs(double x)` |
| `fabsf` | apps | [`math.h`](#math-h) | `float fabsf(float x)` |
| `fabsl` | apps | [`math.h`](#math-h) | `long double fabsl(long double x)` |
| `fclose` | apps | [`stdio.h`](#stdio-h) | `int fclose(FILE *f)` |
| `fcntl` | apps | [`fcntl.h`](#fcntl-h) | `int fcntl(int fd, int command, ...)` |
| `FD_CLR` | apps | [`sys/select.h`](#sys-select-h) | `#define FD_CLR(fd, set)` |
| `FD_ISSET` | apps | [`sys/select.h`](#sys-select-h) | `#define FD_ISSET(fd, set)` |
| `FD_SET` | apps | [`sys/select.h`](#sys-select-h) | `#define FD_SET(fd, set)` |
| `FD_ZERO` | apps | [`sys/select.h`](#sys-select-h) | `#define FD_ZERO(set)` |
| `fdim` | apps | [`math.h`](#math-h) | `double fdim(double x, double y)` |
| `fdimf` | apps | [`math.h`](#math-h) | `float fdimf(float x, float y)` |
| `fdiml` | apps | [`math.h`](#math-h) | `long double fdiml(long double x, long double y)` |
| `fdopen` | apps | [`stdio.h`](#stdio-h) | `FILE *fdopen(int fd, const char *mode)` |
| `feclearexcept` | apps | [`fenv.h`](#fenv-h) | `int feclearexcept(int excepts)` |
| `fegetenv` | apps | [`fenv.h`](#fenv-h) | `int fegetenv(fenv_t *env)` |
| `fegetexceptflag` | apps | [`fenv.h`](#fenv-h) | `int fegetexceptflag(fexcept_t *flags, int excepts)` |
| `fegetround` | apps | [`fenv.h`](#fenv-h) | `int fegetround(void)` |
| `feholdexcept` | apps | [`fenv.h`](#fenv-h) | `int feholdexcept(fenv_t *env)` |
| `feof` | apps | [`stdio.h`](#stdio-h) | `int feof(FILE *f)` |
| `feraiseexcept` | apps | [`fenv.h`](#fenv-h) | `int feraiseexcept(int excepts)` |
| `ferror` | apps | [`stdio.h`](#stdio-h) | `int ferror(FILE *f)` |
| `fesetenv` | apps | [`fenv.h`](#fenv-h) | `int fesetenv(const fenv_t *env)` |
| `fesetexceptflag` | apps | [`fenv.h`](#fenv-h) | `int fesetexceptflag(const fexcept_t *flags, int excepts)` |
| `fesetround` | apps | [`fenv.h`](#fenv-h) | `int fesetround(int round)` |
| `fetestexcept` | apps | [`fenv.h`](#fenv-h) | `int fetestexcept(int excepts)` |
| `feupdateenv` | apps | [`fenv.h`](#fenv-h) | `int feupdateenv(const fenv_t *env)` |
| `fflush` | apps | [`stdio.h`](#stdio-h) | `int fflush(FILE *f)` |
| `fgetc` | apps | [`stdio.h`](#stdio-h) | `int fgetc(FILE *f)` |
| `fgetpos` | apps | [`stdio.h`](#stdio-h) | `int fgetpos(FILE *f, fpos_t *pos)` |
| `fgets` | apps | [`stdio.h`](#stdio-h) | `char *fgets(char *s, int size, FILE *f)` |
| `fgetwc` | apps | [`wchar.h`](#wchar-h) | `wint_t fgetwc(FILE *f)` |
| `fgetws` | apps | [`wchar.h`](#wchar-h) | `wchar_t *fgetws(wchar_t *s, int size, FILE *f)` |
| `fileno` | apps | [`stdio.h`](#stdio-h) | `int fileno(FILE *f)` |
| `flockfile` | apps | [`stdio.h`](#stdio-h) | `void flockfile(FILE *f)` |
| `floor` | apps | [`math.h`](#math-h) | `double floor(double x)` |
| `floorf` | apps | [`math.h`](#math-h) | `float floorf(float x)` |
| `floorl` | apps | [`math.h`](#math-h) | `long double floorl(long double x)` |
| `fma` | apps | [`math.h`](#math-h) | `double fma(double x, double y, double z)` |
| `fmaf` | apps | [`math.h`](#math-h) | `float fmaf(float x, float y, float z)` |
| `fmal` | apps | [`math.h`](#math-h) | `long double fmal(long double x, long double y, long double z)` |
| `fmax` | apps | [`math.h`](#math-h) | `double fmax(double x, double y)` |
| `fmaxf` | apps | [`math.h`](#math-h) | `float fmaxf(float x, float y)` |
| `fmaxl` | apps | [`math.h`](#math-h) | `long double fmaxl(long double x, long double y)` |
| `fmin` | apps | [`math.h`](#math-h) | `double fmin(double x, double y)` |
| `fminf` | apps | [`math.h`](#math-h) | `float fminf(float x, float y)` |
| `fminl` | apps | [`math.h`](#math-h) | `long double fminl(long double x, long double y)` |
| `fmod` | apps | [`math.h`](#math-h) | `double fmod(double x, double y)` |
| `fmodf` | apps | [`math.h`](#math-h) | `float fmodf(float x, float y)` |
| `fmodl` | apps | [`math.h`](#math-h) | `long double fmodl(long double x, long double y)` |
| `fopen` | apps | [`stdio.h`](#stdio-h) | `FILE *fopen(const char *path, const char *mode)` |
| `fork` | apps | [`unistd.h`](#unistd-h) | `pid_t fork(void)` |
| `fpclassify` | apps | [`math.h`](#math-h) | `#define fpclassify(x)` |
| `fprintf` | apps | [`stdio.h`](#stdio-h) | `int fprintf(FILE *f, const char *format, ...)` |
| `fputc` | apps | [`stdio.h`](#stdio-h) | `int fputc(int c, FILE *f)` |
| `fputs` | apps | [`stdio.h`](#stdio-h) | `int fputs(const char *s, FILE *f)` |
| `fputwc` | apps | [`wchar.h`](#wchar-h) | `wint_t fputwc(wchar_t wc, FILE *f)` |
| `fputws` | apps | [`wchar.h`](#wchar-h) | `int fputws(const wchar_t *s, FILE *f)` |
| `fread` | apps | [`stdio.h`](#stdio-h) | `size_t fread(void *buf, size_t size, size_t count, FILE *f)` |
| `free` | apps | [`stdlib.h`](#stdlib-h) | `void free(void *ptr)` |
| `freeaddrinfo` | apps | [`netdb.h`](#netdb-h) | `void freeaddrinfo(struct addrinfo *list)` |
| `freopen` | apps | [`stdio.h`](#stdio-h) | `FILE *freopen(const char *path, const char *mode, FILE *f)` |
| `frexp` | apps | [`math.h`](#math-h) | `double frexp(double x, int *e)` |
| `frexpf` | apps | [`math.h`](#math-h) | `float frexpf(float x, int *e)` |
| `frexpl` | apps | [`math.h`](#math-h) | `long double frexpl(long double x, int *e)` |
| `fscanf` | apps | [`stdio.h`](#stdio-h) | `int fscanf(FILE *f, const char *format, ...)` |
| `fseek` | apps | [`stdio.h`](#stdio-h) | `int fseek(FILE *f, long offset, int whence)` |
| `fsetpos` | apps | [`stdio.h`](#stdio-h) | `int fsetpos(FILE *f, const fpos_t *pos)` |
| `fstat` | apps | [`sys/stat.h`](#sys-stat-h) | `int fstat(int fd, struct stat *st)` |
| `ftell` | apps | [`stdio.h`](#stdio-h) | `long ftell(FILE *f)` |
| `ftrylockfile` | apps | [`stdio.h`](#stdio-h) | `int ftrylockfile(FILE *f)` |
| `funlockfile` | apps | [`stdio.h`](#stdio-h) | `void funlockfile(FILE *f)` |
| `futimens` | apps | [`sys/stat.h`](#sys-stat-h) | `int futimens(int fd, const struct timespec times[2])` |
| `fwide` | apps | [`wchar.h`](#wchar-h) | `int fwide(FILE *f, int mode)` |
| `fwprintf` | apps | [`wchar.h`](#wchar-h) | `int fwprintf(FILE *f, const wchar_t *format, ...)` |
| `fwrite` | apps | [`stdio.h`](#stdio-h) | `size_t fwrite(const void *buf, size_t size, size_t count, FILE *f)` |
| `fwscanf` | apps | [`wchar.h`](#wchar-h) | `int fwscanf(FILE *f, const wchar_t *format, ...)` |
| `gai_strerror` | apps | [`netdb.h`](#netdb-h) | `const char *gai_strerror(int error)` |
| `getaddrinfo` | apps | [`netdb.h`](#netdb-h) | `int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result)` |
| `getc` | apps | [`stdio.h`](#stdio-h) | `int getc(FILE *f)` |
| `getc_unlocked` | apps | [`stdio.h`](#stdio-h) | `int getc_unlocked(FILE *f)` |
| `getchar` | apps | [`stdio.h`](#stdio-h) | `int getchar(void)` |
| `getchar_unlocked` | apps | [`stdio.h`](#stdio-h) | `int getchar_unlocked(void)` |
| `getcwd` | apps | [`unistd.h`](#unistd-h) | `char *getcwd(char *buffer, size_t size)` |
| `getenv` | apps | [`stdlib.h`](#stdlib-h) | `char *getenv(const char *name)` |
| `geteuid` | apps | [`unistd.h`](#unistd-h) | `uid_t geteuid(void)` |
| `getgid` | apps | [`unistd.h`](#unistd-h) | `gid_t getgid(void)` |
| `gethostbyname` | apps | [`netdb.h`](#netdb-h) | `struct hostent *gethostbyname(const char *name)` |
| `getnameinfo` | apps | [`netdb.h`](#netdb-h) | `int getnameinfo(const struct sockaddr *address, socklen_t length, char *host, socklen_t host_size, char *service, socklen_t service_size, int flags)` |
| `getpeername` | apps | [`sys/socket.h`](#sys-socket-h) | `int getpeername(int fd, struct sockaddr *address, socklen_t *length)` |
| `getpgid` | apps | [`unistd.h`](#unistd-h) | `pid_t getpgid(pid_t pid)` |
| `getpgrp` | apps | [`unistd.h`](#unistd-h) | `pid_t getpgrp(void)` |
| `getpid` | apps | [`unistd.h`](#unistd-h) | `pid_t getpid(void)` |
| `getppid` | apps | [`unistd.h`](#unistd-h) | `pid_t getppid(void)` |
| `getprogname` | apps | [`stdlib.h`](#stdlib-h) | `const char *getprogname(void)` |
| `getpwnam` | apps | [`pwd.h`](#pwd-h) | `struct passwd *getpwnam(const char *name)` |
| `getpwuid` | apps | [`pwd.h`](#pwd-h) | `struct passwd *getpwuid(uid_t uid)` |
| `getservbyname` | apps | [`netdb.h`](#netdb-h) | `struct servent *getservbyname(const char *name, const char *protocol)` |
| `getservbyport` | apps | [`netdb.h`](#netdb-h) | `struct servent *getservbyport(int port, const char *protocol)` |
| `getsockname` | apps | [`sys/socket.h`](#sys-socket-h) | `int getsockname(int fd, struct sockaddr *address, socklen_t *length)` |
| `getsockopt` | apps | [`sys/socket.h`](#sys-socket-h) | `int getsockopt(int fd, int level, int option, void *value, socklen_t *length)` |
| `gettimeofday` | apps | [`sys/time.h`](#sys-time-h) | `int gettimeofday(struct timeval *tv, void *zone)` |
| `getuid` | apps | [`unistd.h`](#unistd-h) | `uid_t getuid(void)` |
| `getwc` | apps | [`wchar.h`](#wchar-h) | `wint_t getwc(FILE *f)` |
| `getwchar` | apps | [`wchar.h`](#wchar-h) | `wint_t getwchar(void)` |
| `gmtime` | apps | [`time.h`](#time-h) | `struct tm *gmtime(const time_t *t)` |
| `gmtime_r` | apps | [`time.h`](#time-h) | `struct tm *gmtime_r(const time_t *t, struct tm *result)` |
| `gpt_empty` | system | [`gpt.h`](#gpt-h) | `void gpt_empty(const struct sys_device *disk, struct gpt_table *t)` |
| `gpt_entry_at` | system | [`gpt.h`](#gpt-h) | `struct gpt_entry *gpt_entry_at(struct gpt_table *t, uint32_t i)` |
| `gpt_fit_protective_mbr` | system | [`gpt.h`](#gpt-h) | `bool gpt_fit_protective_mbr(int fd, const struct sys_device *disk)` |
| `gpt_new_guid` | system | [`gpt.h`](#gpt-h) | `void gpt_new_guid(uint8_t *g)` |
| `gpt_parse_guid` | system | [`gpt.h`](#gpt-h) | `bool gpt_parse_guid(const char *s, uint8_t *g)` |
| `gpt_read` | system | [`gpt.h`](#gpt-h) | `bool gpt_read(int fd, const struct sys_device *disk, struct gpt_table *t)` |
| `gpt_set_name` | system | [`gpt.h`](#gpt-h) | `void gpt_set_name(struct gpt_entry *e, const char *name)` |
| `gpt_unused` | system | [`gpt.h`](#gpt-h) | `bool gpt_unused(const struct gpt_entry *e)` |
| `gpt_write` | system | [`gpt.h`](#gpt-h) | `bool gpt_write(int fd, const struct sys_device *disk, struct gpt_table *t)` |
| `gpt_write_protective_mbr` | system | [`gpt.h`](#gpt-h) | `bool gpt_write_protective_mbr(int fd, const struct sys_device *disk)` |
| `guard_format` | system | [`ritteros.h`](#ritteros-h) | `void guard_format(const struct sys_guard_rule *rule, char *out, size_t size)` |
| `guard_format6` | apps | [`ritteros.h`](#ritteros-h) | `void guard_format6(const struct sys_guard6_rule *rule, char *out, size_t size)` |
| `guard_parse` | system | [`ritteros.h`](#ritteros-h) | `int guard_parse(const char *line, struct sys_guard *g, char *why, size_t size)` |
| `guard_read` | system | [`ritteros.h`](#ritteros-h) | `int guard_read(const char *path, struct sys_guard *g, char *why, size_t size)` |
| `guard_read6` | apps | [`ritteros.h`](#ritteros-h) | `int guard_read6(const char *path, struct sys_guard *g, struct sys_guard6 *g6, char *why, size_t size)` |
| `hkdf_expand` | apps | [`crypto.h`](#crypto-h) | `int hkdf_expand(size_t size, const uint8_t *prk, const void *info, size_t info_length, uint8_t *out, size_t length)` |
| `hkdf_extract` | apps | [`crypto.h`](#crypto-h) | `void hkdf_extract(size_t size, const void *salt, size_t salt_length, const void *ikm, size_t ikm_length, uint8_t *prk)` |
| `hmac_sha1` | apps | [`crypto.h`](#crypto-h) | `void hmac_sha1(const void *key, size_t key_length, const void *data, size_t n, uint8_t out[SHA1_SIZE])` |
| `hmac_sha256` | apps | [`ritteros.h`](#ritteros-h) | `void hmac_sha256(const void *key, size_t key_length, const void *data, size_t n, uint8_t out[SHA256_SIZE])` |
| `hmac_sha384` | apps | [`crypto.h`](#crypto-h) | `void hmac_sha384(const void *key, size_t key_length, const void *data, size_t n, uint8_t out[SHA384_SIZE])` |
| `htonl` | apps | [`netinet/in.h`](#netinet-in-h) | `uint32_t htonl(uint32_t v)` |
| `htons` | apps | [`netinet/in.h`](#netinet-in-h) | `uint16_t htons(uint16_t v)` |
| `hypot` | apps | [`math.h`](#math-h) | `double hypot(double x, double y)` |
| `hypotf` | apps | [`math.h`](#math-h) | `float hypotf(float x, float y)` |
| `hypotl` | apps | [`math.h`](#math-h) | `long double hypotl(long double x, long double y)` |
| `ieee80211_kdf_sha256` | apps | [`crypto.h`](#crypto-h) | `void ieee80211_kdf_sha256(const void *key, size_t key_length, const char *label, const void *context, size_t n, uint8_t *out, size_t out_length)` |
| `ieee80211_prf` | apps | [`crypto.h`](#crypto-h) | `void ieee80211_prf(const void *key, size_t key_length, const char *label, const void *data, size_t n, uint8_t *out, size_t out_length)` |
| `ilogb` | apps | [`math.h`](#math-h) | `int ilogb(double x)` |
| `ilogbf` | apps | [`math.h`](#math-h) | `int ilogbf(float x)` |
| `ilogbl` | apps | [`math.h`](#math-h) | `int ilogbl(long double x)` |
| `image_free` | apps | [`image.h`](#image-h) | `void image_free(struct image *img)` |
| `image_info` | apps | [`image.h`](#image-h) | `int image_info(const char *path, struct image *out)` |
| `image_load` | apps | [`image.h`](#image-h) | `int image_load(const char *path, struct image *out)` |
| `image_load_memory` | apps | [`image.h`](#image-h) | `int image_load_memory(const void *data, long n, struct image *out)` |
| `image_save` | apps | [`image.h`](#image-h) | `int image_save(const char *path, const struct image *img)` |
| `image_scale` | apps | [`image.h`](#image-h) | `int image_scale(const struct image *src, int w, int h, struct image *out)` |
| `IN6_IS_ADDR_LINKLOCAL` | apps | [`netinet/in.h`](#netinet-in-h) | `#define IN6_IS_ADDR_LINKLOCAL(a)` |
| `IN6_IS_ADDR_LOOPBACK` | apps | [`netinet/in.h`](#netinet-in-h) | `#define IN6_IS_ADDR_LOOPBACK(a)` |
| `IN6_IS_ADDR_MULTICAST` | apps | [`netinet/in.h`](#netinet-in-h) | `#define IN6_IS_ADDR_MULTICAST(a)` |
| `IN6_IS_ADDR_SITELOCAL` | apps | [`netinet/in.h`](#netinet-in-h) | `#define IN6_IS_ADDR_SITELOCAL(a)` |
| `IN6_IS_ADDR_UNSPECIFIED` | apps | [`netinet/in.h`](#netinet-in-h) | `#define IN6_IS_ADDR_UNSPECIFIED(a)` |
| `IN6_IS_ADDR_V4MAPPED` | apps | [`netinet/in.h`](#netinet-in-h) | `#define IN6_IS_ADDR_V4MAPPED(a)` |
| `inet_addr` | apps | [`arpa/inet.h`](#arpa-inet-h) | `in_addr_t inet_addr(const char *text)` |
| `inet_aton` | apps | [`arpa/inet.h`](#arpa-inet-h) | `int inet_aton(const char *text, struct in_addr *out)` |
| `inet_ntoa` | apps | [`arpa/inet.h`](#arpa-inet-h) | `char *inet_ntoa(struct in_addr address)` |
| `inet_ntop` | apps | [`arpa/inet.h`](#arpa-inet-h) | `const char *inet_ntop(int family, const void *address, char *out, socklen_t size)` |
| `inet_pton` | apps | [`arpa/inet.h`](#arpa-inet-h) | `int inet_pton(int family, const char *text, void *out)` |
| `ip_format` | apps | [`ritteros.h`](#ritteros-h) | `void ip_format(uint32_t ip, char *out, size_t size)` |
| `ip_parse` | apps | [`ritteros.h`](#ritteros-h) | `bool ip_parse(const char *text, uint32_t *ip, uint32_t *prefix)` |
| `isalnum` | apps | [`ctype.h`](#ctype-h) | `int isalnum(int c)` |
| `isalpha` | apps | [`ctype.h`](#ctype-h) | `int isalpha(int c)` |
| `isatty` | apps | [`unistd.h`](#unistd-h) | `int isatty(int fd)` |
| `isblank` | apps | [`ctype.h`](#ctype-h) | `int isblank(int c)` |
| `iscntrl` | apps | [`ctype.h`](#ctype-h) | `int iscntrl(int c)` |
| `isdigit` | apps | [`ctype.h`](#ctype-h) | `int isdigit(int c)` |
| `isfinite` | apps | [`math.h`](#math-h) | `#define isfinite(x)` |
| `isgraph` | apps | [`ctype.h`](#ctype-h) | `int isgraph(int c)` |
| `isgreater` | apps | [`math.h`](#math-h) | `#define isgreater(x, y)` |
| `isgreaterequal` | apps | [`math.h`](#math-h) | `#define isgreaterequal(x, y)` |
| `isinf` | apps | [`math.h`](#math-h) | `#define isinf(x)` |
| `isless` | apps | [`math.h`](#math-h) | `#define isless(x, y)` |
| `islessequal` | apps | [`math.h`](#math-h) | `#define islessequal(x, y)` |
| `islessgreater` | apps | [`math.h`](#math-h) | `#define islessgreater(x, y)` |
| `islower` | apps | [`ctype.h`](#ctype-h) | `int islower(int c)` |
| `isnan` | apps | [`math.h`](#math-h) | `#define isnan(x)` |
| `isnormal` | apps | [`math.h`](#math-h) | `#define isnormal(x)` |
| `isprint` | apps | [`ctype.h`](#ctype-h) | `int isprint(int c)` |
| `ispunct` | apps | [`ctype.h`](#ctype-h) | `int ispunct(int c)` |
| `isspace` | apps | [`ctype.h`](#ctype-h) | `int isspace(int c)` |
| `isunordered` | apps | [`math.h`](#math-h) | `#define isunordered(x, y)` |
| `isupper` | apps | [`ctype.h`](#ctype-h) | `int isupper(int c)` |
| `iswalnum` | apps | [`wctype.h`](#wctype-h) | `int iswalnum(wint_t wc)` |
| `iswalpha` | apps | [`wctype.h`](#wctype-h) | `int iswalpha(wint_t wc)` |
| `iswblank` | apps | [`wctype.h`](#wctype-h) | `int iswblank(wint_t wc)` |
| `iswcntrl` | apps | [`wctype.h`](#wctype-h) | `int iswcntrl(wint_t wc)` |
| `iswctype` | apps | [`wctype.h`](#wctype-h) | `int iswctype(wint_t wc, wctype_t type)` |
| `iswdigit` | apps | [`wctype.h`](#wctype-h) | `int iswdigit(wint_t wc)` |
| `iswgraph` | apps | [`wctype.h`](#wctype-h) | `int iswgraph(wint_t wc)` |
| `iswlower` | apps | [`wctype.h`](#wctype-h) | `int iswlower(wint_t wc)` |
| `iswprint` | apps | [`wctype.h`](#wctype-h) | `int iswprint(wint_t wc)` |
| `iswpunct` | apps | [`wctype.h`](#wctype-h) | `int iswpunct(wint_t wc)` |
| `iswspace` | apps | [`wctype.h`](#wctype-h) | `int iswspace(wint_t wc)` |
| `iswupper` | apps | [`wctype.h`](#wctype-h) | `int iswupper(wint_t wc)` |
| `iswxdigit` | apps | [`wctype.h`](#wctype-h) | `int iswxdigit(wint_t wc)` |
| `isxdigit` | apps | [`ctype.h`](#ctype-h) | `int isxdigit(int c)` |
| `kill` | apps | [`signal.h`](#signal-h) | `int kill(pid_t pid, int signal)` |
| `labs` | apps | [`stdlib.h`](#stdlib-h) | `long labs(long v)` |
| `ldexp` | apps | [`math.h`](#math-h) | `double ldexp(double x, int n)` |
| `ldexpf` | apps | [`math.h`](#math-h) | `float ldexpf(float x, int n)` |
| `ldexpl` | apps | [`math.h`](#math-h) | `long double ldexpl(long double x, int n)` |
| `ldiv` | apps | [`stdlib.h`](#stdlib-h) | `ldiv_t ldiv(long numerator, long denominator)` |
| `lgamma` | apps | [`math.h`](#math-h) | `double lgamma(double x)` |
| `lgammaf` | apps | [`math.h`](#math-h) | `float lgammaf(float x)` |
| `lgammal` | apps | [`math.h`](#math-h) | `long double lgammal(long double x)` |
| `link` | apps | [`unistd.h`](#unistd-h) | `int link(const char *from, const char *to)` |
| `listen` | apps | [`sys/socket.h`](#sys-socket-h) | `int listen(int fd, int backlog)` |
| `llabs` | apps | [`stdlib.h`](#stdlib-h) | `long long llabs(long long v)` |
| `lldiv` | apps | [`stdlib.h`](#stdlib-h) | `lldiv_t lldiv(long long numerator, long long denominator)` |
| `llrint` | apps | [`math.h`](#math-h) | `long long llrint(double x)` |
| `llrintf` | apps | [`math.h`](#math-h) | `long long llrintf(float x)` |
| `llrintl` | apps | [`math.h`](#math-h) | `long long llrintl(long double x)` |
| `llround` | apps | [`math.h`](#math-h) | `long long llround(double x)` |
| `llroundf` | apps | [`math.h`](#math-h) | `long long llroundf(float x)` |
| `llroundl` | apps | [`math.h`](#math-h) | `long long llroundl(long double x)` |
| `localeconv` | apps | [`locale.h`](#locale-h) | `struct lconv *localeconv(void)` |
| `localtime` | apps | [`time.h`](#time-h) | `struct tm *localtime(const time_t *t)` |
| `localtime_r` | apps | [`time.h`](#time-h) | `struct tm *localtime_r(const time_t *t, struct tm *result)` |
| `log` | apps | [`math.h`](#math-h) | `double log(double x)` |
| `log10` | apps | [`math.h`](#math-h) | `double log10(double x)` |
| `log10f` | apps | [`math.h`](#math-h) | `float log10f(float x)` |
| `log10l` | apps | [`math.h`](#math-h) | `long double log10l(long double x)` |
| `log1p` | apps | [`math.h`](#math-h) | `double log1p(double x)` |
| `log1pf` | apps | [`math.h`](#math-h) | `float log1pf(float x)` |
| `log1pl` | apps | [`math.h`](#math-h) | `long double log1pl(long double x)` |
| `log2` | apps | [`math.h`](#math-h) | `double log2(double x)` |
| `log2f` | apps | [`math.h`](#math-h) | `float log2f(float x)` |
| `log2l` | apps | [`math.h`](#math-h) | `long double log2l(long double x)` |
| `logb` | apps | [`math.h`](#math-h) | `double logb(double x)` |
| `logbf` | apps | [`math.h`](#math-h) | `float logbf(float x)` |
| `logbl` | apps | [`math.h`](#math-h) | `long double logbl(long double x)` |
| `logf` | apps | [`math.h`](#math-h) | `float logf(float x)` |
| `logl` | apps | [`math.h`](#math-h) | `long double logl(long double x)` |
| `logs_flush` | system | [`ritteros.h`](#ritteros-h) | `void logs_flush(void)` |
| `longjmp` | apps | [`setjmp.h`](#setjmp-h) | `void longjmp(jmp_buf env, int value)` |
| `lrint` | apps | [`math.h`](#math-h) | `long lrint(double x)` |
| `lrintf` | apps | [`math.h`](#math-h) | `long lrintf(float x)` |
| `lrintl` | apps | [`math.h`](#math-h) | `long lrintl(long double x)` |
| `lround` | apps | [`math.h`](#math-h) | `long lround(double x)` |
| `lroundf` | apps | [`math.h`](#math-h) | `long lroundf(float x)` |
| `lroundl` | apps | [`math.h`](#math-h) | `long lroundl(long double x)` |
| `lseek` | apps | [`unistd.h`](#unistd-h) | `off_t lseek(int fd, off_t offset, int whence)` |
| `lstat` | apps | [`sys/stat.h`](#sys-stat-h) | `int lstat(const char *path, struct stat *st)` |
| `malloc` | apps | [`stdlib.h`](#stdlib-h) | `void *malloc(size_t size)` |
| `mblen` | apps | [`stdlib.h`](#stdlib-h) | `int mblen(const char *s, size_t n)` |
| `mbrlen` | apps | [`wchar.h`](#wchar-h) | `size_t mbrlen(const char *s, size_t n, mbstate_t *state)` |
| `mbrtoc16` | apps | [`uchar.h`](#uchar-h) | `size_t mbrtoc16(char16_t *c, const char *s, size_t n, mbstate_t *state)` |
| `mbrtoc32` | apps | [`uchar.h`](#uchar-h) | `size_t mbrtoc32(char32_t *c, const char *s, size_t n, mbstate_t *state)` |
| `mbrtoc8` | apps | [`uchar.h`](#uchar-h) | `size_t mbrtoc8(char8_t *c, const char *s, size_t n, mbstate_t *state)` |
| `mbrtowc` | apps | [`wchar.h`](#wchar-h) | `size_t mbrtowc(wchar_t *wc, const char *s, size_t n, mbstate_t *state)` |
| `mbsinit` | apps | [`wchar.h`](#wchar-h) | `int mbsinit(const mbstate_t *state)` |
| `mbsrtowcs` | apps | [`wchar.h`](#wchar-h) | `size_t mbsrtowcs(wchar_t *dst, const char **src, size_t n, mbstate_t *state)` |
| `mbstowcs` | apps | [`stdlib.h`](#stdlib-h) | `size_t mbstowcs(wchar_t *dst, const char *src, size_t n)` |
| `mbtowc` | apps | [`stdlib.h`](#stdlib-h) | `int mbtowc(wchar_t *wc, const char *s, size_t n)` |
| `memccpy` | apps | [`string.h`](#string-h) | `void *memccpy(void *dst, const void *src, int c, size_t n)` |
| `memchr` | apps | [`string.h`](#string-h) | `void *memchr(const void *s, int c, size_t n)` |
| `memcmp` | apps | [`string.h`](#string-h) | `int memcmp(const void *a, const void *b, size_t n)` |
| `memcpy` | apps | [`string.h`](#string-h) | `void *memcpy(void *dst, const void *src, size_t n)` |
| `memmove` | apps | [`string.h`](#string-h) | `void *memmove(void *dst, const void *src, size_t n)` |
| `memrchr` | apps | [`string.h`](#string-h) | `void *memrchr(const void *s, int c, size_t n)` |
| `memset` | apps | [`string.h`](#string-h) | `void *memset(void *dst, int value, size_t n)` |
| `mkdir` | apps | [`sys/stat.h`](#sys-stat-h) | `int mkdir(const char *path, unsigned int mode)` |
| `mkfs_exfat` | system | [`mkfs.h`](#mkfs-h) | `bool mkfs_exfat(int fd, const struct exfat_layout *l, const uint16_t *label, int label_units, uint64_t hidden, bool whole_disk, uint64_t device_bytes)` |
| `mkfs_exfat_label` | system | [`mkfs.h`](#mkfs-h) | `int mkfs_exfat_label(const char *label, uint16_t *out)` |
| `mkfs_exfat_plan` | system | [`mkfs.h`](#mkfs-h) | `bool mkfs_exfat_plan(uint64_t bytes, uint32_t sector, struct exfat_layout *l)` |
| `mkfs_fat32` | system | [`mkfs.h`](#mkfs-h) | `bool mkfs_fat32(int fd, const struct fat_layout *l, const char *label, uint64_t hidden, bool whole_disk, uint64_t device_bytes, uint64_t now)` |
| `mkfs_fat_label_valid` | system | [`mkfs.h`](#mkfs-h) | `bool mkfs_fat_label_valid(const char *label)` |
| `mkfs_fat_plan` | system | [`mkfs.h`](#mkfs-h) | `bool mkfs_fat_plan(uint64_t bytes, uint32_t sector, struct fat_layout *l)` |
| `mkfs_put16` | system | [`mkfs.h`](#mkfs-h) | `void mkfs_put16(uint8_t *p, uint32_t v)` |
| `mkfs_put32` | system | [`mkfs.h`](#mkfs-h) | `void mkfs_put32(uint8_t *p, uint32_t v)` |
| `mkfs_put64` | system | [`mkfs.h`](#mkfs-h) | `void mkfs_put64(uint8_t *p, uint64_t v)` |
| `mkfs_ritterfs` | system | [`mkfs.h`](#mkfs-h) | `bool mkfs_ritterfs(int fd, const char *label, bool whole_disk, uint64_t device_bytes, uint64_t now)` |
| `mkfs_ritterfs_check` | system | [`mkfs.h`](#mkfs-h) | `const char *mkfs_ritterfs_check(uint64_t bytes)` |
| `mkfs_write` | system | [`mkfs.h`](#mkfs-h) | `bool mkfs_write(int fd, uint64_t offset, const void *data, size_t n)` |
| `mkfs_zero` | system | [`mkfs.h`](#mkfs-h) | `bool mkfs_zero(int fd, uint64_t offset, uint64_t n)` |
| `mkstemp` | apps | [`stdlib.h`](#stdlib-h) | `int mkstemp(char *name)` |
| `mktime` | apps | [`time.h`](#time-h) | `time_t mktime(struct tm *tm)` |
| `mmap` | apps | [`sys/mman.h`](#sys-mman-h) | `void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)` |
| `modf` | apps | [`math.h`](#math-h) | `double modf(double x, double *whole)` |
| `modff` | apps | [`math.h`](#math-h) | `float modff(float x, float *whole)` |
| `modfl` | apps | [`math.h`](#math-h) | `long double modfl(long double x, long double *whole)` |
| `mprotect` | apps | [`sys/mman.h`](#sys-mman-h) | `int mprotect(void *addr, size_t length, int prot)` |
| `msync` | apps | [`sys/mman.h`](#sys-mman-h) | `int msync(void *addr, size_t length, int flags)` |
| `mtx_destroy` | apps | [`threads.h`](#threads-h) | `void mtx_destroy(mtx_t *m)` |
| `mtx_init` | apps | [`threads.h`](#threads-h) | `int mtx_init(mtx_t *m, int type)` |
| `mtx_lock` | apps | [`threads.h`](#threads-h) | `int mtx_lock(mtx_t *m)` |
| `mtx_timedlock` | apps | [`threads.h`](#threads-h) | `int mtx_timedlock(mtx_t *m, const struct timespec *until)` |
| `mtx_trylock` | apps | [`threads.h`](#threads-h) | `int mtx_trylock(mtx_t *m)` |
| `mtx_unlock` | apps | [`threads.h`](#threads-h) | `int mtx_unlock(mtx_t *m)` |
| `munmap` | apps | [`sys/mman.h`](#sys-mman-h) | `int munmap(void *addr, size_t length)` |
| `nan` | apps | [`math.h`](#math-h) | `double nan(const char *tag)` |
| `nanf` | apps | [`math.h`](#math-h) | `float nanf(const char *tag)` |
| `nanl` | apps | [`math.h`](#math-h) | `long double nanl(const char *tag)` |
| `nanosleep` | apps | [`time.h`](#time-h) | `int nanosleep(const struct timespec *req, struct timespec *rem)` |
| `nearbyint` | apps | [`math.h`](#math-h) | `double nearbyint(double x)` |
| `nearbyintf` | apps | [`math.h`](#math-h) | `float nearbyintf(float x)` |
| `nearbyintl` | apps | [`math.h`](#math-h) | `long double nearbyintl(long double x)` |
| `nextafter` | apps | [`math.h`](#math-h) | `double nextafter(double x, double y)` |
| `nextafterf` | apps | [`math.h`](#math-h) | `float nextafterf(float x, float y)` |
| `nextafterl` | apps | [`math.h`](#math-h) | `long double nextafterl(long double x, long double y)` |
| `nexttoward` | apps | [`math.h`](#math-h) | `double nexttoward(double x, long double y)` |
| `nexttowardf` | apps | [`math.h`](#math-h) | `float nexttowardf(float x, long double y)` |
| `nexttowardl` | apps | [`math.h`](#math-h) | `long double nexttowardl(long double x, long double y)` |
| `ntohl` | apps | [`netinet/in.h`](#netinet-in-h) | `uint32_t ntohl(uint32_t v)` |
| `ntohs` | apps | [`netinet/in.h`](#netinet-in-h) | `uint16_t ntohs(uint16_t v)` |
| `open` | apps | [`fcntl.h`](#fcntl-h) | `int open(const char *path, int flags, ...)` |
| `opendir` | apps | [`dirent.h`](#dirent-h) | `DIR *opendir(const char *path)` |
| `p256_add` | apps | [`crypto.h`](#crypto-h) | `bool p256_add(const uint8_t a[64], const uint8_t b[64], uint8_t sum[64])` |
| `p256_sswu` | apps | [`crypto.h`](#crypto-h) | `void p256_sswu(const uint8_t u[32], uint8_t point[64])` |
| `password_check` | system | [`ritteros.h`](#ritteros-h) | `int password_check(const char *user, const char *password)` |
| `password_exists` | system | [`ritteros.h`](#ritteros-h) | `bool password_exists(const char *user)` |
| `password_set` | system | [`ritteros.h`](#ritteros-h) | `int password_set(const char *user, const char *password)` |
| `pbkdf2_sha1` | apps | [`crypto.h`](#crypto-h) | `void pbkdf2_sha1(const void *password, size_t password_length, const uint8_t *salt, size_t salt_length, uint32_t iterations, uint8_t *out, size_t out_length)` |
| `pbkdf2_sha256` | apps | [`ritteros.h`](#ritteros-h) | `void pbkdf2_sha256(const void *password, size_t password_length, const uint8_t *salt, size_t salt_length, uint32_t iterations, uint8_t *out, size_t out_length)` |
| `pclose` | apps | [`stdio.h`](#stdio-h) | `int pclose(FILE *stream)` |
| `pem_next` | system | [`x509.h`](#x509-h) | `size_t pem_next(const char **text, const char *end, uint8_t *out, size_t size)` |
| `perror` | apps | [`stdio.h`](#stdio-h) | `void perror(const char *s)` |
| `pipe` | apps | [`unistd.h`](#unistd-h) | `int pipe(int fds[2])` |
| `poll` | apps | [`poll.h`](#poll-h) | `int poll(struct pollfd *fds, nfds_t count, int timeout)` |
| `poly1305` | apps | [`crypto.h`](#crypto-h) | `void poly1305(const uint8_t key[32], const void *message, size_t n, uint8_t tag[16])` |
| `popen` | apps | [`stdio.h`](#stdio-h) | `FILE *popen(const char *command, const char *mode)` |
| `posix_spawn` | apps | [`spawn.h`](#spawn-h) | `int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attr, char *const argv[], char *const envp[])` |
| `posix_spawn_file_actions_addclose` | apps | [`spawn.h`](#spawn-h) | `int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions, int fd)` |
| `posix_spawn_file_actions_adddup2` | apps | [`spawn.h`](#spawn-h) | `int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions, int fd, int newfd)` |
| `posix_spawn_file_actions_addopen` | apps | [`spawn.h`](#spawn-h) | `int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions, int fd, const char *path, int oflag, mode_t mode)` |
| `posix_spawn_file_actions_destroy` | apps | [`spawn.h`](#spawn-h) | `int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions)` |
| `posix_spawn_file_actions_init` | apps | [`spawn.h`](#spawn-h) | `int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions)` |
| `posix_spawnattr_destroy` | apps | [`spawn.h`](#spawn-h) | `int posix_spawnattr_destroy(posix_spawnattr_t *attr)` |
| `posix_spawnattr_getflags` | apps | [`spawn.h`](#spawn-h) | `int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags)` |
| `posix_spawnattr_getpgroup` | apps | [`spawn.h`](#spawn-h) | `int posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup)` |
| `posix_spawnattr_getsigdefault` | apps | [`spawn.h`](#spawn-h) | `int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr, sigset_t *signals)` |
| `posix_spawnattr_getsigmask` | apps | [`spawn.h`](#spawn-h) | `int posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *mask)` |
| `posix_spawnattr_init` | apps | [`spawn.h`](#spawn-h) | `int posix_spawnattr_init(posix_spawnattr_t *attr)` |
| `posix_spawnattr_setflags` | apps | [`spawn.h`](#spawn-h) | `int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags)` |
| `posix_spawnattr_setpgroup` | apps | [`spawn.h`](#spawn-h) | `int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup)` |
| `posix_spawnattr_setsigdefault` | apps | [`spawn.h`](#spawn-h) | `int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *signals)` |
| `posix_spawnattr_setsigmask` | apps | [`spawn.h`](#spawn-h) | `int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *mask)` |
| `posix_spawnp` | apps | [`spawn.h`](#spawn-h) | `int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attr, char *const argv[], char *const envp[])` |
| `pow` | apps | [`math.h`](#math-h) | `double pow(double x, double y)` |
| `powf` | apps | [`math.h`](#math-h) | `float powf(float x, float y)` |
| `powl` | apps | [`math.h`](#math-h) | `long double powl(long double x, long double y)` |
| `printf` | apps | [`stdio.h`](#stdio-h) | `int printf(const char *format, ...)` |
| `process_named` | apps | [`ritteros.h`](#ritteros-h) | `int process_named(const char *name)` |
| `PROGRAM_HELP` | apps | [`ritteros.h`](#ritteros-h) | `#define PROGRAM_HELP(usage, description)` |
| `pthread_atfork` | apps | [`pthread.h`](#pthread-h) | `int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void))` |
| `pthread_attr_destroy` | apps | [`pthread.h`](#pthread-h) | `int pthread_attr_destroy(pthread_attr_t *attr)` |
| `pthread_attr_getdetachstate` | apps | [`pthread.h`](#pthread-h) | `int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *state)` |
| `pthread_attr_getstacksize` | apps | [`pthread.h`](#pthread-h) | `int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size)` |
| `pthread_attr_init` | apps | [`pthread.h`](#pthread-h) | `int pthread_attr_init(pthread_attr_t *attr)` |
| `pthread_attr_setdetachstate` | apps | [`pthread.h`](#pthread-h) | `int pthread_attr_setdetachstate(pthread_attr_t *attr, int state)` |
| `pthread_attr_setstacksize` | apps | [`pthread.h`](#pthread-h) | `int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size)` |
| `pthread_barrier_destroy` | apps | [`pthread.h`](#pthread-h) | `int pthread_barrier_destroy(pthread_barrier_t *b)` |
| `pthread_barrier_init` | apps | [`pthread.h`](#pthread-h) | `int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *attr, unsigned count)` |
| `pthread_barrier_wait` | apps | [`pthread.h`](#pthread-h) | `int pthread_barrier_wait(pthread_barrier_t *b)` |
| `pthread_barrierattr_destroy` | apps | [`pthread.h`](#pthread-h) | `int pthread_barrierattr_destroy(pthread_barrierattr_t *attr)` |
| `pthread_barrierattr_init` | apps | [`pthread.h`](#pthread-h) | `int pthread_barrierattr_init(pthread_barrierattr_t *attr)` |
| `pthread_cancel` | apps | [`pthread.h`](#pthread-h) | `int pthread_cancel(pthread_t thread)` |
| `pthread_cleanup_pop` | apps | [`pthread.h`](#pthread-h) | `#define pthread_cleanup_pop(execute)` |
| `pthread_cleanup_push` | apps | [`pthread.h`](#pthread-h) | `#define pthread_cleanup_push(fn, arg)` |
| `pthread_cond_broadcast` | apps | [`pthread.h`](#pthread-h) | `int pthread_cond_broadcast(pthread_cond_t *c)` |
| `pthread_cond_destroy` | apps | [`pthread.h`](#pthread-h) | `int pthread_cond_destroy(pthread_cond_t *c)` |
| `pthread_cond_init` | apps | [`pthread.h`](#pthread-h) | `int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr)` |
| `pthread_cond_signal` | apps | [`pthread.h`](#pthread-h) | `int pthread_cond_signal(pthread_cond_t *c)` |
| `pthread_cond_timedwait` | apps | [`pthread.h`](#pthread-h) | `int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *until)` |
| `pthread_cond_wait` | apps | [`pthread.h`](#pthread-h) | `int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)` |
| `pthread_condattr_destroy` | apps | [`pthread.h`](#pthread-h) | `int pthread_condattr_destroy(pthread_condattr_t *attr)` |
| `pthread_condattr_getclock` | apps | [`pthread.h`](#pthread-h) | `int pthread_condattr_getclock(const pthread_condattr_t *attr, clockid_t *clock)` |
| `pthread_condattr_init` | apps | [`pthread.h`](#pthread-h) | `int pthread_condattr_init(pthread_condattr_t *attr)` |
| `pthread_condattr_setclock` | apps | [`pthread.h`](#pthread-h) | `int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock)` |
| `pthread_create` | apps | [`pthread.h`](#pthread-h) | `int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)` |
| `pthread_detach` | apps | [`pthread.h`](#pthread-h) | `int pthread_detach(pthread_t thread)` |
| `pthread_equal` | apps | [`pthread.h`](#pthread-h) | `int pthread_equal(pthread_t a, pthread_t b)` |
| `pthread_exit` | apps | [`pthread.h`](#pthread-h) | `void pthread_exit(void *value)` |
| `pthread_getspecific` | apps | [`pthread.h`](#pthread-h) | `void *pthread_getspecific(pthread_key_t key)` |
| `pthread_join` | apps | [`pthread.h`](#pthread-h) | `int pthread_join(pthread_t thread, void **value)` |
| `pthread_key_create` | apps | [`pthread.h`](#pthread-h) | `int pthread_key_create(pthread_key_t *key, void (*destructor)(void *value))` |
| `pthread_key_delete` | apps | [`pthread.h`](#pthread-h) | `int pthread_key_delete(pthread_key_t key)` |
| `pthread_mutex_destroy` | apps | [`pthread.h`](#pthread-h) | `int pthread_mutex_destroy(pthread_mutex_t *m)` |
| `pthread_mutex_init` | apps | [`pthread.h`](#pthread-h) | `int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)` |
| `pthread_mutex_lock` | apps | [`pthread.h`](#pthread-h) | `int pthread_mutex_lock(pthread_mutex_t *m)` |
| `pthread_mutex_timedlock` | apps | [`pthread.h`](#pthread-h) | `int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *until)` |
| `pthread_mutex_trylock` | apps | [`pthread.h`](#pthread-h) | `int pthread_mutex_trylock(pthread_mutex_t *m)` |
| `pthread_mutex_unlock` | apps | [`pthread.h`](#pthread-h) | `int pthread_mutex_unlock(pthread_mutex_t *m)` |
| `pthread_mutexattr_destroy` | apps | [`pthread.h`](#pthread-h) | `int pthread_mutexattr_destroy(pthread_mutexattr_t *attr)` |
| `pthread_mutexattr_gettype` | apps | [`pthread.h`](#pthread-h) | `int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *kind)` |
| `pthread_mutexattr_init` | apps | [`pthread.h`](#pthread-h) | `int pthread_mutexattr_init(pthread_mutexattr_t *attr)` |
| `pthread_mutexattr_setpshared` | apps | [`pthread.h`](#pthread-h) | `int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int shared)` |
| `pthread_mutexattr_settype` | apps | [`pthread.h`](#pthread-h) | `int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int kind)` |
| `pthread_once` | apps | [`pthread.h`](#pthread-h) | `int pthread_once(pthread_once_t *once, void (*init)(void))` |
| `pthread_rwlock_destroy` | apps | [`pthread.h`](#pthread-h) | `int pthread_rwlock_destroy(pthread_rwlock_t *l)` |
| `pthread_rwlock_init` | apps | [`pthread.h`](#pthread-h) | `int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *attr)` |
| `pthread_rwlock_rdlock` | apps | [`pthread.h`](#pthread-h) | `int pthread_rwlock_rdlock(pthread_rwlock_t *l)` |
| `pthread_rwlock_timedrdlock` | apps | [`pthread.h`](#pthread-h) | `int pthread_rwlock_timedrdlock(pthread_rwlock_t *l, const struct timespec *until)` |
| `pthread_rwlock_timedwrlock` | apps | [`pthread.h`](#pthread-h) | `int pthread_rwlock_timedwrlock(pthread_rwlock_t *l, const struct timespec *until)` |
| `pthread_rwlock_tryrdlock` | apps | [`pthread.h`](#pthread-h) | `int pthread_rwlock_tryrdlock(pthread_rwlock_t *l)` |
| `pthread_rwlock_trywrlock` | apps | [`pthread.h`](#pthread-h) | `int pthread_rwlock_trywrlock(pthread_rwlock_t *l)` |
| `pthread_rwlock_unlock` | apps | [`pthread.h`](#pthread-h) | `int pthread_rwlock_unlock(pthread_rwlock_t *l)` |
| `pthread_rwlock_wrlock` | apps | [`pthread.h`](#pthread-h) | `int pthread_rwlock_wrlock(pthread_rwlock_t *l)` |
| `pthread_rwlockattr_destroy` | apps | [`pthread.h`](#pthread-h) | `int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr)` |
| `pthread_rwlockattr_init` | apps | [`pthread.h`](#pthread-h) | `int pthread_rwlockattr_init(pthread_rwlockattr_t *attr)` |
| `pthread_self` | apps | [`pthread.h`](#pthread-h) | `pthread_t pthread_self(void)` |
| `pthread_setcancelstate` | apps | [`pthread.h`](#pthread-h) | `int pthread_setcancelstate(int state, int *old)` |
| `pthread_setcanceltype` | apps | [`pthread.h`](#pthread-h) | `int pthread_setcanceltype(int type, int *old)` |
| `pthread_setspecific` | apps | [`pthread.h`](#pthread-h) | `int pthread_setspecific(pthread_key_t key, const void *value)` |
| `pthread_sigmask` | apps | [`pthread.h`](#pthread-h) | `int pthread_sigmask(int how, const sigset_t *set, sigset_t *old)` |
| `pthread_spin_destroy` | apps | [`pthread.h`](#pthread-h) | `int pthread_spin_destroy(pthread_spinlock_t *s)` |
| `pthread_spin_init` | apps | [`pthread.h`](#pthread-h) | `int pthread_spin_init(pthread_spinlock_t *s, int shared)` |
| `pthread_spin_lock` | apps | [`pthread.h`](#pthread-h) | `int pthread_spin_lock(pthread_spinlock_t *s)` |
| `pthread_spin_trylock` | apps | [`pthread.h`](#pthread-h) | `int pthread_spin_trylock(pthread_spinlock_t *s)` |
| `pthread_spin_unlock` | apps | [`pthread.h`](#pthread-h) | `int pthread_spin_unlock(pthread_spinlock_t *s)` |
| `pthread_testcancel` | apps | [`pthread.h`](#pthread-h) | `void pthread_testcancel(void)` |
| `putc` | apps | [`stdio.h`](#stdio-h) | `int putc(int c, FILE *f)` |
| `putc_unlocked` | apps | [`stdio.h`](#stdio-h) | `int putc_unlocked(int c, FILE *f)` |
| `putchar` | apps | [`stdio.h`](#stdio-h) | `int putchar(int c)` |
| `putchar_unlocked` | apps | [`stdio.h`](#stdio-h) | `int putchar_unlocked(int c)` |
| `putenv` | apps | [`stdlib.h`](#stdlib-h) | `int putenv(char *string)` |
| `puts` | apps | [`stdio.h`](#stdio-h) | `int puts(const char *s)` |
| `putwc` | apps | [`wchar.h`](#wchar-h) | `wint_t putwc(wchar_t wc, FILE *f)` |
| `putwchar` | apps | [`wchar.h`](#wchar-h) | `wint_t putwchar(wchar_t wc)` |
| `qsort` | apps | [`stdlib.h`](#stdlib-h) | `void qsort(void *base, size_t count, size_t size, int (*compare)(const void *a, const void *b))` |
| `quick_exit` | apps | [`stdlib.h`](#stdlib-h) | `void quick_exit(int code)` |
| `raise` | apps | [`signal.h`](#signal-h) | `int raise(int signal)` |
| `rand` | apps | [`stdlib.h`](#stdlib-h) | `int rand(void)` |
| `read` | apps | [`unistd.h`](#unistd-h) | `ssize_t read(int fd, void *buffer, size_t size)` |
| `readdir` | apps | [`dirent.h`](#dirent-h) | `struct dirent *readdir(DIR *dir)` |
| `readlink` | apps | [`unistd.h`](#unistd-h) | `ssize_t readlink(const char *path, char *buffer, size_t size)` |
| `realloc` | apps | [`stdlib.h`](#stdlib-h) | `void *realloc(void *ptr, size_t size)` |
| `recv` | apps | [`sys/socket.h`](#sys-socket-h) | `ssize_t recv(int fd, void *data, size_t size, int flags)` |
| `recvfrom` | apps | [`sys/socket.h`](#sys-socket-h) | `ssize_t recvfrom(int fd, void *data, size_t size, int flags, struct sockaddr *from, socklen_t *length)` |
| `regcomp` | apps | [`regex.h`](#regex-h) | `int regcomp(regex_t *re, const char *pattern, int flags)` |
| `regerror` | apps | [`regex.h`](#regex-h) | `size_t regerror(int error, const regex_t *re, char *buffer, size_t size)` |
| `regexec` | apps | [`regex.h`](#regex-h) | `int regexec(const regex_t *re, const char *string, size_t nmatch, regmatch_t pmatch[], int flags)` |
| `regfree` | apps | [`regex.h`](#regex-h) | `void regfree(regex_t *re)` |
| `remainder` | apps | [`math.h`](#math-h) | `double remainder(double x, double y)` |
| `remainderf` | apps | [`math.h`](#math-h) | `float remainderf(float x, float y)` |
| `remainderl` | apps | [`math.h`](#math-h) | `long double remainderl(long double x, long double y)` |
| `remove` | apps | [`stdio.h`](#stdio-h) | `int remove(const char *path)` |
| `remquo` | apps | [`math.h`](#math-h) | `double remquo(double x, double y, int *quo)` |
| `remquof` | apps | [`math.h`](#math-h) | `float remquof(float x, float y, int *quo)` |
| `remquol` | apps | [`math.h`](#math-h) | `long double remquol(long double x, long double y, int *quo)` |
| `rename` | apps | [`stdio.h`](#stdio-h) | `int rename(const char *from, const char *to)` |
| `rewind` | apps | [`stdio.h`](#stdio-h) | `void rewind(FILE *f)` |
| `rint` | apps | [`math.h`](#math-h) | `double rint(double x)` |
| `rintf` | apps | [`math.h`](#math-h) | `float rintf(float x)` |
| `rintl` | apps | [`math.h`](#math-h) | `long double rintl(long double x)` |
| `ritteros_accept` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_accept(int fd, uint32_t *ip, uint16_t *port)` |
| `ritteros_assert_failed` | apps | [`assert.h`](#assert-h) | `void ritteros_assert_failed(const char *expression, const char *file, int line, const char *function)` |
| `ritteros_attr` | apps | [`ritteros.h`](#ritteros-h) | `int64_t ritteros_attr(int op, const char *path, const char *key, void *buffer, size_t size)` |
| `ritteros_bind` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_bind(int fd, uint16_t port)` |
| `ritteros_bind_local` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_bind_local(int fd, const char *path)` |
| `ritteros_bindcard` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_bindcard(int fd, const char *card)` |
| `ritteros_bridge` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_bridge(int op, int bridge, const char *card)` |
| `ritteros_connect` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_connect(int fd, uint32_t ip, uint16_t port)` |
| `ritteros_connect_local` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_connect_local(int fd, const char *path)` |
| `ritteros_copy` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_copy(const char *from, const char *to)` |
| `ritteros_copy_bytes` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_copy_bytes(const char *from, const char *to, char *failed, size_t size)` |
| `ritteros_default_route` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_default_route(int op, const char *card)` |
| `ritteros_device` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_device(int index, struct sys_device *out)` |
| `ritteros_display_acquire` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_display_acquire(void)` |
| `ritteros_display_buffers` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_display_buffers(struct sys_display_buffers *buffers)` |
| `ritteros_display_event` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_display_event(struct sys_input_event *event, uint32_t timeout_ms)` |
| `ritteros_display_flip` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_display_flip(int buffer, uint64_t *frame)` |
| `ritteros_display_flush` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_display_flush(const void *pixels, uint32_t stride, uint32_t x, uint32_t y, uint32_t w, uint32_t h)` |
| `ritteros_display_info` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_display_info(struct sys_display *info)` |
| `ritteros_display_modes` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_display_modes(struct sys_display_modes *modes)` |
| `ritteros_display_release` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_display_release(void)` |
| `ritteros_display_set_mode` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_display_set_mode(const char *output, uint32_t width, uint32_t height, uint32_t refresh_mhz, char *said, size_t said_size)` |
| `ritteros_display_vblank` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_display_vblank(uint64_t *frame, uint32_t timeout_ms)` |
| `ritteros_echo` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_echo(bool on)` |
| `ritteros_efivar` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_efivar(int op, struct sys_efivar *v)` |
| `ritteros_forward` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_forward(int mode)` |
| `ritteros_grow` | system | [`ritteros.h`](#ritteros-h) | `int64_t ritteros_grow(const char *partition)` |
| `ritteros_guard` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_guard(int op, struct sys_guard *g)` |
| `ritteros_guard6` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_guard6(int op, struct sys_guard6 *g)` |
| `ritteros_listen` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_listen(int fd, int backlog)` |
| `ritteros_local_connect` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_local_connect(const char *path)` |
| `ritteros_local_listen` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_local_listen(const char *path, int backlog)` |
| `ritteros_log_as` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_log_as(const char *name)` |
| `ritteros_log_take` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_log_take(void *buffer, size_t size)` |
| `ritteros_log_write` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_log_write(const char *name, const char *text)` |
| `ritteros_masquerade` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_masquerade(int op, const char *card)` |
| `ritteros_mount` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_mount(const char *device, const char *path, char *buffer, size_t size)` |
| `ritteros_mount_point` | apps | [`ritteros.h`](#ritteros-h) | `bool ritteros_mount_point(const char *path)` |
| `ritteros_netcard` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_netcard(int index, struct sys_netinfo *info)` |
| `ritteros_netinfo` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_netinfo(struct sys_netinfo *info)` |
| `ritteros_netinfo6` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_netinfo6(struct sys_netinfo6 *info)` |
| `ritteros_netlease6` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_netlease6(const struct sys_netlease6 *lease)` |
| `ritteros_netset` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_netset(const struct sys_netset *settings)` |
| `ritteros_netset6` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_netset6(const struct sys_netset6 *settings)` |
| `ritteros_peer_uid` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_peer_uid(int fd)` |
| `ritteros_ping` | apps | [`ritteros.h`](#ritteros-h) | `int64_t ritteros_ping(uint32_t ip, uint16_t id, uint16_t seq, size_t size, uint32_t timeout_ms)` |
| `ritteros_ping6` | apps | [`ritteros.h`](#ritteros-h) | `int64_t ritteros_ping6(const struct sys_inaddr *to, uint16_t id, uint16_t seq, size_t size, uint32_t timeout_ms)` |
| `ritteros_pipe` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_pipe(int fds[2])` |
| `ritteros_power` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_power(int how)` |
| `ritteros_power_eval` | apps | [`ritteros.h`](#ritteros-h) | `int64_t ritteros_power_eval(const char *path, char *buffer, size_t size)` |
| `ritteros_power_names` | apps | [`ritteros.h`](#ritteros-h) | `int64_t ritteros_power_names(char *buffer, size_t size)` |
| `ritteros_power_sleep` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_power_sleep(unsigned seconds)` |
| `ritteros_power_table` | apps | [`ritteros.h`](#ritteros-h) | `int64_t ritteros_power_table(int index, void *buffer, size_t size)` |
| `ritteros_pty_open` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_pty_open(int fds[2])` |
| `ritteros_pty_size` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_pty_size(int fd, int rows, int cols)` |
| `ritteros_random` | apps | [`ritteros.h`](#ritteros-h) | `int64_t ritteros_random(void *buffer, size_t n)` |
| `ritteros_recvfrom` | apps | [`ritteros.h`](#ritteros-h) | `ssize_t ritteros_recvfrom(int fd, void *data, size_t size, uint32_t *ip, uint16_t *port, uint32_t timeout_ms)` |
| `ritteros_remove_tree` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_remove_tree(const char *path)` |
| `ritteros_report` | apps | [`ritteros.h`](#ritteros-h) | `int64_t ritteros_report(int which, char *buffer, size_t size)` |
| `ritteros_resolve` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_resolve(const char *name, uint32_t *ip)` |
| `ritteros_resolve6` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_resolve6(const char *name, uint8_t address[16])` |
| `ritteros_sendto` | apps | [`ritteros.h`](#ritteros-h) | `ssize_t ritteros_sendto(int fd, const void *data, size_t size, uint32_t ip, uint16_t port)` |
| `ritteros_session_stamp` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_session_stamp(int op)` |
| `ritteros_set_thread_pointer` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_set_thread_pointer(void *tp)` |
| `ritteros_share_create` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_share_create(size_t size)` |
| `ritteros_share_map` | apps | [`ritteros.h`](#ritteros-h) | `void *ritteros_share_map(int fd)` |
| `ritteros_share_recv` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_share_recv(int socket_fd)` |
| `ritteros_share_send` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_share_send(int socket_fd, int buffer_fd)` |
| `ritteros_share_size` | apps | [`ritteros.h`](#ritteros-h) | `long ritteros_share_size(int fd)` |
| `ritteros_snapshot` | system | [`ritteros.h`](#ritteros-h) | `int64_t ritteros_snapshot(int op, const char *name, struct sys_snapshot *buffer, size_t size)` |
| `ritteros_socket` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_socket(int type)` |
| `ritteros_spawn` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_spawn(const char *path, char *const argv[], int flags, const int fds[3])` |
| `ritteros_spawn_as` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_spawn_as(const char *path, char *const argv[], int flags, const int fds[3], uid_t uid, gid_t gid)` |
| `ritteros_spawn_env` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_spawn_env(const char *path, char *const argv[], char *const envp[], int flags, const int fds[3])` |
| `ritteros_stderr` | apps | [`stdio.h`](#stdio-h) | `FILE *ritteros_stderr(void)` |
| `ritteros_stdin` | apps | [`stdio.h`](#stdio-h) | `FILE *ritteros_stdin(void)` |
| `ritteros_stdout` | apps | [`stdio.h`](#stdio-h) | `FILE *ritteros_stdout(void)` |
| `ritteros_swap` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_swap(int op, struct sys_swap *status)` |
| `ritteros_sysinfo` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_sysinfo(struct sys_info *info)` |
| `ritteros_tap` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_tap(int op, unsigned value)` |
| `ritteros_terminal_line_editor` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_terminal_line_editor(void)` |
| `ritteros_terminal_raw` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_terminal_raw(bool on)` |
| `ritteros_terminal_size` | apps | [`ritteros.h`](#ritteros-h) | `int ritteros_terminal_size(int *rows, int *cols)` |
| `ritteros_time` | apps | [`ritteros.h`](#ritteros-h) | `uint64_t ritteros_time(void)` |
| `ritteros_unmount` | system | [`ritteros.h`](#ritteros-h) | `int ritteros_unmount(const char *path_or_device)` |
| `ritteros_wlan` | apps | [`ritteros.h`](#ritteros-h) | `int64_t ritteros_wlan(int op, const char *card, void *buffer, size_t length, uint64_t value)` |
| `rmdir` | apps | [`unistd.h`](#unistd-h) | `int rmdir(const char *path)` |
| `round` | apps | [`math.h`](#math-h) | `double round(double x)` |
| `roundf` | apps | [`math.h`](#math-h) | `float roundf(float x)` |
| `roundl` | apps | [`math.h`](#math-h) | `long double roundl(long double x)` |
| `rsa_verify_pkcs1` | apps | [`crypto.h`](#crypto-h) | `bool rsa_verify_pkcs1(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, const uint8_t *digest, size_t dlen, const uint8_t *sig, size_t slen)` |
| `rsa_verify_pss` | apps | [`crypto.h`](#crypto-h) | `bool rsa_verify_pss(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, const uint8_t *digest, size_t dlen, const uint8_t *sig, size_t slen)` |
| `S_ISBLK` | apps | [`sys/stat.h`](#sys-stat-h) | `#define S_ISBLK(mode)` |
| `S_ISCHR` | apps | [`sys/stat.h`](#sys-stat-h) | `#define S_ISCHR(mode)` |
| `S_ISDIR` | apps | [`sys/stat.h`](#sys-stat-h) | `#define S_ISDIR(mode)` |
| `S_ISFIFO` | apps | [`sys/stat.h`](#sys-stat-h) | `#define S_ISFIFO(mode)` |
| `S_ISLNK` | apps | [`sys/stat.h`](#sys-stat-h) | `#define S_ISLNK(mode)` |
| `S_ISREG` | apps | [`sys/stat.h`](#sys-stat-h) | `#define S_ISREG(mode)` |
| `S_ISSOCK` | apps | [`sys/stat.h`](#sys-stat-h) | `#define S_ISSOCK(mode)` |
| `sae_check_confirm` | apps | [`crypto.h`](#crypto-h) | `bool sae_check_confirm(const struct sae *s, uint16_t peer_send_confirm, const uint8_t confirm[32])` |
| `sae_commit` | apps | [`crypto.h`](#crypto-h) | `bool sae_commit(struct sae *s, const uint8_t *rand, const uint8_t *mask)` |
| `sae_confirm` | apps | [`crypto.h`](#crypto-h) | `void sae_confirm(const struct sae *s, uint16_t send_confirm, uint8_t out[32])` |
| `sae_process` | apps | [`crypto.h`](#crypto-h) | `bool sae_process(struct sae *s, const uint8_t peer_scalar[32], const uint8_t peer_element[64])` |
| `sae_pt` | apps | [`crypto.h`](#crypto-h) | `void sae_pt(uint8_t pt[64], const void *ssid, size_t ssid_n, const void *password, size_t n, const void *id, size_t id_n)` |
| `sae_pwe_from_pt` | apps | [`crypto.h`](#crypto-h) | `bool sae_pwe_from_pt(struct sae *s, const uint8_t pt[64], const uint8_t mac_a[6], const uint8_t mac_b[6])` |
| `sae_pwe_hunt` | apps | [`crypto.h`](#crypto-h) | `bool sae_pwe_hunt(struct sae *s, const uint8_t mac_a[6], const uint8_t mac_b[6], const void *password, size_t n)` |
| `scalbln` | apps | [`math.h`](#math-h) | `double scalbln(double x, long n)` |
| `scalblnf` | apps | [`math.h`](#math-h) | `float scalblnf(float x, long n)` |
| `scalblnl` | apps | [`math.h`](#math-h) | `long double scalblnl(long double x, long n)` |
| `scalbn` | apps | [`math.h`](#math-h) | `double scalbn(double x, int n)` |
| `scalbnf` | apps | [`math.h`](#math-h) | `float scalbnf(float x, int n)` |
| `scalbnl` | apps | [`math.h`](#math-h) | `long double scalbnl(long double x, int n)` |
| `scanf` | apps | [`stdio.h`](#stdio-h) | `int scanf(const char *format, ...)` |
| `sched_yield` | apps | [`sched.h`](#sched-h) | `int sched_yield(void)` |
| `select` | apps | [`sys/select.h`](#sys-select-h) | `int select(int count, fd_set *read, fd_set *write, fd_set *except, struct timeval *timeout)` |
| `send` | apps | [`sys/socket.h`](#sys-socket-h) | `ssize_t send(int fd, const void *data, size_t size, int flags)` |
| `sendto` | apps | [`sys/socket.h`](#sys-socket-h) | `ssize_t sendto(int fd, const void *data, size_t size, int flags, const struct sockaddr *to, socklen_t length)` |
| `setbuf` | apps | [`stdio.h`](#stdio-h) | `void setbuf(FILE *f, char *buf)` |
| `setenv` | apps | [`stdlib.h`](#stdlib-h) | `int setenv(const char *name, const char *value, int overwrite)` |
| `setgid` | apps | [`unistd.h`](#unistd-h) | `int setgid(gid_t gid)` |
| `setjmp` | apps | [`setjmp.h`](#setjmp-h) | `int setjmp(jmp_buf env)` |
| `setlocale` | apps | [`locale.h`](#locale-h) | `char *setlocale(int category, const char *locale)` |
| `setpgid` | apps | [`unistd.h`](#unistd-h) | `int setpgid(pid_t pid, pid_t group)` |
| `setsid` | apps | [`unistd.h`](#unistd-h) | `pid_t setsid(void)` |
| `setsockopt` | apps | [`sys/socket.h`](#sys-socket-h) | `int setsockopt(int fd, int level, int option, const void *value, socklen_t length)` |
| `setuid` | apps | [`unistd.h`](#unistd-h) | `int setuid(uid_t uid)` |
| `setvbuf` | apps | [`stdio.h`](#stdio-h) | `int setvbuf(FILE *f, char *buf, int mode, size_t size)` |
| `sha1` | apps | [`crypto.h`](#crypto-h) | `void sha1(const void *data, size_t n, uint8_t out[SHA1_SIZE])` |
| `sha1_final` | apps | [`crypto.h`](#crypto-h) | `void sha1_final(struct sha1 *s, uint8_t out[SHA1_SIZE])` |
| `sha1_init` | apps | [`crypto.h`](#crypto-h) | `void sha1_init(struct sha1 *s)` |
| `sha1_update` | apps | [`crypto.h`](#crypto-h) | `void sha1_update(struct sha1 *s, const void *data, size_t n)` |
| `sha256` | apps | [`ritteros.h`](#ritteros-h) | `void sha256(const void *data, size_t n, uint8_t out[SHA256_SIZE])` |
| `sha256_final` | apps | [`ritteros.h`](#ritteros-h) | `void sha256_final(struct sha256 *s, uint8_t out[SHA256_SIZE])` |
| `sha256_init` | apps | [`ritteros.h`](#ritteros-h) | `void sha256_init(struct sha256 *s)` |
| `sha256_update` | apps | [`ritteros.h`](#ritteros-h) | `void sha256_update(struct sha256 *s, const void *data, size_t n)` |
| `sha384` | apps | [`crypto.h`](#crypto-h) | `void sha384(const void *data, size_t n, uint8_t out[SHA384_SIZE])` |
| `sha384_final` | apps | [`crypto.h`](#crypto-h) | `void sha384_final(struct sha512 *s, uint8_t out[SHA384_SIZE])` |
| `sha384_init` | apps | [`crypto.h`](#crypto-h) | `void sha384_init(struct sha512 *s)` |
| `sha512` | apps | [`crypto.h`](#crypto-h) | `void sha512(const void *data, size_t n, uint8_t out[SHA512_SIZE])` |
| `sha512_final` | apps | [`crypto.h`](#crypto-h) | `void sha512_final(struct sha512 *s, uint8_t out[SHA512_SIZE])` |
| `sha512_init` | apps | [`crypto.h`](#crypto-h) | `void sha512_init(struct sha512 *s)` |
| `sha512_update` | apps | [`crypto.h`](#crypto-h) | `void sha512_update(struct sha512 *s, const void *data, size_t n)` |
| `shutdown` | apps | [`sys/socket.h`](#sys-socket-h) | `int shutdown(int fd, int how)` |
| `sigaction` | apps | [`signal.h`](#signal-h) | `int sigaction(int signal, const struct sigaction *action, struct sigaction *old)` |
| `sigaddset` | apps | [`signal.h`](#signal-h) | `int sigaddset(sigset_t *set, int signal)` |
| `sigdelset` | apps | [`signal.h`](#signal-h) | `int sigdelset(sigset_t *set, int signal)` |
| `sigemptyset` | apps | [`signal.h`](#signal-h) | `int sigemptyset(sigset_t *set)` |
| `sigfillset` | apps | [`signal.h`](#signal-h) | `int sigfillset(sigset_t *set)` |
| `sigismember` | apps | [`signal.h`](#signal-h) | `int sigismember(const sigset_t *set, int signal)` |
| `signal` | apps | [`signal.h`](#signal-h) | `sighandler_t signal(int signal, sighandler_t action)` |
| `signbit` | apps | [`math.h`](#math-h) | `#define signbit(x)` |
| `sigpending` | apps | [`signal.h`](#signal-h) | `int sigpending(sigset_t *set)` |
| `sigprocmask` | apps | [`signal.h`](#signal-h) | `int sigprocmask(int how, const sigset_t *set, sigset_t *old)` |
| `sigsuspend` | apps | [`signal.h`](#signal-h) | `int sigsuspend(const sigset_t *mask)` |
| `sin` | apps | [`math.h`](#math-h) | `double sin(double x)` |
| `sinf` | apps | [`math.h`](#math-h) | `float sinf(float x)` |
| `sinh` | apps | [`math.h`](#math-h) | `double sinh(double x)` |
| `sinhf` | apps | [`math.h`](#math-h) | `float sinhf(float x)` |
| `sinhl` | apps | [`math.h`](#math-h) | `long double sinhl(long double x)` |
| `sinl` | apps | [`math.h`](#math-h) | `long double sinl(long double x)` |
| `sleep` | apps | [`unistd.h`](#unistd-h) | `unsigned int sleep(unsigned int seconds)` |
| `snprintf` | apps | [`stdio.h`](#stdio-h) | `int snprintf(char *buffer, size_t size, const char *format, ...)` |
| `socket` | apps | [`sys/socket.h`](#sys-socket-h) | `int socket(int family, int type, int protocol)` |
| `spawn` | apps | [`unistd.h`](#unistd-h) | `pid_t spawn(const char *path, char *const argv[])` |
| `spawn_new_group` | apps | [`unistd.h`](#unistd-h) | `pid_t spawn_new_group(const char *path, char *const argv[])` |
| `sprintf` | apps | [`stdio.h`](#stdio-h) | `int sprintf(char *buffer, const char *format, ...)` |
| `sqrt` | apps | [`math.h`](#math-h) | `double sqrt(double x)` |
| `sqrtf` | apps | [`math.h`](#math-h) | `float sqrtf(float x)` |
| `sqrtl` | apps | [`math.h`](#math-h) | `long double sqrtl(long double x)` |
| `srand` | apps | [`stdlib.h`](#stdlib-h) | `void srand(unsigned int seed)` |
| `sscanf` | apps | [`stdio.h`](#stdio-h) | `int sscanf(const char *text, const char *format, ...)` |
| `ssh_buf_init` | system | [`ssh.h`](#ssh-h) | `void ssh_buf_init(struct ssh_buf *b, uint8_t type)` |
| `ssh_disconnect` | system | [`ssh.h`](#ssh-h) | `void ssh_disconnect(struct ssh *s, uint32_t reason, const char *text)` |
| `ssh_fingerprint` | system | [`ssh.h`](#ssh-h) | `void ssh_fingerprint(const uint8_t public_key[32], char *out, size_t size)` |
| `ssh_get_bool` | system | [`ssh.h`](#ssh-h) | `bool ssh_get_bool(struct ssh_reader *r)` |
| `ssh_get_string` | system | [`ssh.h`](#ssh-h) | `const uint8_t *ssh_get_string(struct ssh_reader *r, size_t *n)` |
| `ssh_get_text` | system | [`ssh.h`](#ssh-h) | `bool ssh_get_text(struct ssh_reader *r, char *out, size_t size)` |
| `ssh_get_u32` | system | [`ssh.h`](#ssh-h) | `uint32_t ssh_get_u32(struct ssh_reader *r)` |
| `ssh_get_u8` | system | [`ssh.h`](#ssh-h) | `uint8_t ssh_get_u8(struct ssh_reader *r)` |
| `ssh_key_blob` | system | [`ssh.h`](#ssh-h) | `size_t ssh_key_blob(const uint8_t public_key[32], uint8_t out[51])` |
| `ssh_put_bool` | system | [`ssh.h`](#ssh-h) | `void ssh_put_bool(struct ssh_buf *b, bool v)` |
| `ssh_put_mpint` | system | [`ssh.h`](#ssh-h) | `void ssh_put_mpint(struct ssh_buf *b, const uint8_t *big_endian, size_t n)` |
| `ssh_put_string` | system | [`ssh.h`](#ssh-h) | `void ssh_put_string(struct ssh_buf *b, const void *data, size_t n)` |
| `ssh_put_text` | system | [`ssh.h`](#ssh-h) | `void ssh_put_text(struct ssh_buf *b, const char *s)` |
| `ssh_put_u32` | system | [`ssh.h`](#ssh-h) | `void ssh_put_u32(struct ssh_buf *b, uint32_t v)` |
| `ssh_put_u8` | system | [`ssh.h`](#ssh-h) | `void ssh_put_u8(struct ssh_buf *b, uint8_t v)` |
| `ssh_reader_init` | system | [`ssh.h`](#ssh-h) | `void ssh_reader_init(struct ssh_reader *r, const uint8_t *data, size_t n)` |
| `ssh_receive` | system | [`ssh.h`](#ssh-h) | `int ssh_receive(struct ssh *s)` |
| `ssh_send` | system | [`ssh.h`](#ssh-h) | `int ssh_send(struct ssh *s, const struct ssh_buf *b)` |
| `ssh_start` | system | [`ssh.h`](#ssh-h) | `int ssh_start(struct ssh *s, int fd, bool server)` |
| `ssh_string_is` | system | [`ssh.h`](#ssh-h) | `bool ssh_string_is(const uint8_t *s, size_t n, const char *text)` |
| `stat` | apps | [`sys/stat.h`](#sys-stat-h) | `int stat(const char *path, struct stat *st)` |
| `STATUS_EXITED` | apps | [`common/syscalls.h`](#common-syscalls-h) | `#define STATUS_EXITED(code)` |
| `STATUS_SIGNALED` | apps | [`common/syscalls.h`](#common-syscalls-h) | `#define STATUS_SIGNALED(sig)` |
| `stpcpy` | apps | [`string.h`](#string-h) | `char *stpcpy(char *dst, const char *src)` |
| `strcasecmp` | apps | [`strings.h`](#strings-h) | `int strcasecmp(const char *a, const char *b)` |
| `strcat` | apps | [`string.h`](#string-h) | `char *strcat(char *dst, const char *src)` |
| `strchr` | apps | [`string.h`](#string-h) | `char *strchr(const char *s, int c)` |
| `strcmp` | apps | [`string.h`](#string-h) | `int strcmp(const char *a, const char *b)` |
| `strcoll` | apps | [`string.h`](#string-h) | `int strcoll(const char *a, const char *b)` |
| `strcpy` | apps | [`string.h`](#string-h) | `char *strcpy(char *dst, const char *src)` |
| `strcspn` | apps | [`string.h`](#string-h) | `size_t strcspn(const char *s, const char *reject)` |
| `strdup` | apps | [`string.h`](#string-h) | `char *strdup(const char *s)` |
| `strerror` | apps | [`string.h`](#string-h) | `const char *strerror(int error)` |
| `strftime` | apps | [`time.h`](#time-h) | `size_t strftime(char *s, size_t max, const char *format, const struct tm *tm)` |
| `strlen` | apps | [`string.h`](#string-h) | `size_t strlen(const char *s)` |
| `strncasecmp` | apps | [`strings.h`](#strings-h) | `int strncasecmp(const char *a, const char *b, size_t n)` |
| `strncat` | apps | [`string.h`](#string-h) | `char *strncat(char *dst, const char *src, size_t n)` |
| `strncmp` | apps | [`string.h`](#string-h) | `int strncmp(const char *a, const char *b, size_t n)` |
| `strncpy` | apps | [`string.h`](#string-h) | `char *strncpy(char *dst, const char *src, size_t n)` |
| `strndup` | apps | [`string.h`](#string-h) | `char *strndup(const char *s, size_t n)` |
| `strnlen` | apps | [`string.h`](#string-h) | `size_t strnlen(const char *s, size_t n)` |
| `strpbrk` | apps | [`string.h`](#string-h) | `char *strpbrk(const char *s, const char *breaks)` |
| `strrchr` | apps | [`string.h`](#string-h) | `char *strrchr(const char *s, int c)` |
| `strspn` | apps | [`string.h`](#string-h) | `size_t strspn(const char *s, const char *accept)` |
| `strstr` | apps | [`string.h`](#string-h) | `char *strstr(const char *haystack, const char *needle)` |
| `strtod` | apps | [`stdlib.h`](#stdlib-h) | `double strtod(const char *s, char **end)` |
| `strtof` | apps | [`stdlib.h`](#stdlib-h) | `float strtof(const char *s, char **end)` |
| `strtok` | apps | [`string.h`](#string-h) | `char *strtok(char *s, const char *delimiters)` |
| `strtol` | apps | [`stdlib.h`](#stdlib-h) | `long strtol(const char *s, char **end, int base)` |
| `strtold` | apps | [`stdlib.h`](#stdlib-h) | `long double strtold(const char *s, char **end)` |
| `strtoll` | apps | [`stdlib.h`](#stdlib-h) | `long long strtoll(const char *s, char **end, int base)` |
| `strtoul` | apps | [`stdlib.h`](#stdlib-h) | `unsigned long strtoul(const char *s, char **end, int base)` |
| `strtoull` | apps | [`stdlib.h`](#stdlib-h) | `unsigned long long strtoull(const char *s, char **end, int base)` |
| `strxfrm` | apps | [`string.h`](#string-h) | `size_t strxfrm(char *dst, const char *src, size_t n)` |
| `swprintf` | apps | [`wchar.h`](#wchar-h) | `int swprintf(wchar_t *s, size_t size, const wchar_t *format, ...)` |
| `swscanf` | apps | [`wchar.h`](#wchar-h) | `int swscanf(const wchar_t *s, const wchar_t *format, ...)` |
| `symlink` | apps | [`unistd.h`](#unistd-h) | `int symlink(const char *target, const char *path)` |
| `syscall6` | system | [`ritteros.h`](#ritteros-h) | `int64_t syscall6(int64_t number, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f)` |
| `system` | apps | [`stdlib.h`](#stdlib-h) | `int system(const char *command)` |
| `tan` | apps | [`math.h`](#math-h) | `double tan(double x)` |
| `tanf` | apps | [`math.h`](#math-h) | `float tanf(float x)` |
| `tanh` | apps | [`math.h`](#math-h) | `double tanh(double x)` |
| `tanhf` | apps | [`math.h`](#math-h) | `float tanhf(float x)` |
| `tanhl` | apps | [`math.h`](#math-h) | `long double tanhl(long double x)` |
| `tanl` | apps | [`math.h`](#math-h) | `long double tanl(long double x)` |
| `tcdrain` | apps | [`termios.h`](#termios-h) | `int tcdrain(int fd)` |
| `tcflush` | apps | [`termios.h`](#termios-h) | `int tcflush(int fd, int which)` |
| `tcgetattr` | apps | [`termios.h`](#termios-h) | `int tcgetattr(int fd, struct termios *t)` |
| `tcgetpgrp` | apps | [`unistd.h`](#unistd-h) | `pid_t tcgetpgrp(int fd)` |
| `tcsetattr` | apps | [`termios.h`](#termios-h) | `int tcsetattr(int fd, int when, const struct termios *t)` |
| `tcsetpgrp` | apps | [`unistd.h`](#unistd-h) | `int tcsetpgrp(int fd, pid_t group)` |
| `tgamma` | apps | [`math.h`](#math-h) | `double tgamma(double x)` |
| `tgammaf` | apps | [`math.h`](#math-h) | `float tgammaf(float x)` |
| `tgammal` | apps | [`math.h`](#math-h) | `long double tgammal(long double x)` |
| `thrd_create` | apps | [`threads.h`](#threads-h) | `int thrd_create(thrd_t *thr, thrd_start_t func, void *arg)` |
| `thrd_current` | apps | [`threads.h`](#threads-h) | `thrd_t thrd_current(void)` |
| `thrd_detach` | apps | [`threads.h`](#threads-h) | `int thrd_detach(thrd_t thr)` |
| `thrd_equal` | apps | [`threads.h`](#threads-h) | `int thrd_equal(thrd_t a, thrd_t b)` |
| `thrd_exit` | apps | [`threads.h`](#threads-h) | `void thrd_exit(int result)` |
| `thrd_join` | apps | [`threads.h`](#threads-h) | `int thrd_join(thrd_t thr, int *result)` |
| `thrd_sleep` | apps | [`threads.h`](#threads-h) | `int thrd_sleep(const struct timespec *duration, struct timespec *remaining)` |
| `thrd_yield` | apps | [`threads.h`](#threads-h) | `void thrd_yield(void)` |
| `thread_create` | apps | [`thread.h`](#thread-h) | `int thread_create(int (*fn)(void *arg), void *arg)` |
| `thread_exit` | apps | [`thread.h`](#thread-h) | `void thread_exit(int result)` |
| `thread_join` | apps | [`thread.h`](#thread-h) | `int thread_join(int number, int *result)` |
| `thread_self` | apps | [`thread.h`](#thread-h) | `int thread_self(void)` |
| `time` | apps | [`time.h`](#time-h) | `time_t time(time_t *t)` |
| `timespec_get` | apps | [`time.h`](#time-h) | `int timespec_get(struct timespec *ts, int base)` |
| `tls_client` | apps | [`tls.h`](#tls-h) | `struct tls *tls_client(void)` |
| `tls_close` | apps | [`tls.h`](#tls-h) | `int tls_close(struct tls *ctx)` |
| `tls_config_error` | apps | [`tls.h`](#tls-h) | `const char *tls_config_error(struct tls_config *config)` |
| `tls_config_free` | apps | [`tls.h`](#tls-h) | `void tls_config_free(struct tls_config *config)` |
| `tls_config_insecure_noverifycert` | apps | [`tls.h`](#tls-h) | `void tls_config_insecure_noverifycert(struct tls_config *config)` |
| `tls_config_insecure_noverifyname` | apps | [`tls.h`](#tls-h) | `void tls_config_insecure_noverifyname(struct tls_config *config)` |
| `tls_config_insecure_noverifytime` | apps | [`tls.h`](#tls-h) | `void tls_config_insecure_noverifytime(struct tls_config *config)` |
| `tls_config_new` | apps | [`tls.h`](#tls-h) | `struct tls_config *tls_config_new(void)` |
| `tls_config_set_ca_file` | apps | [`tls.h`](#tls-h) | `int tls_config_set_ca_file(struct tls_config *config, const char *file)` |
| `tls_config_set_ca_mem` | apps | [`tls.h`](#tls-h) | `int tls_config_set_ca_mem(struct tls_config *config, const uint8_t *pem, size_t length)` |
| `tls_config_verify` | apps | [`tls.h`](#tls-h) | `void tls_config_verify(struct tls_config *config)` |
| `tls_configure` | apps | [`tls.h`](#tls-h) | `int tls_configure(struct tls *ctx, struct tls_config *config)` |
| `tls_conn_cipher` | apps | [`tls.h`](#tls-h) | `const char *tls_conn_cipher(struct tls *ctx)` |
| `tls_conn_servername` | apps | [`tls.h`](#tls-h) | `const char *tls_conn_servername(struct tls *ctx)` |
| `tls_conn_version` | apps | [`tls.h`](#tls-h) | `const char *tls_conn_version(struct tls *ctx)` |
| `tls_connect` | apps | [`tls.h`](#tls-h) | `int tls_connect(struct tls *ctx, const char *host, const char *port)` |
| `tls_connect_servername` | apps | [`tls.h`](#tls-h) | `int tls_connect_servername(struct tls *ctx, const char *host, const char *port, const char *servername)` |
| `tls_connect_socket` | apps | [`tls.h`](#tls-h) | `int tls_connect_socket(struct tls *ctx, int socket, const char *servername)` |
| `tls_default_ca_cert_file` | apps | [`tls.h`](#tls-h) | `const char *tls_default_ca_cert_file(void)` |
| `tls_error` | apps | [`tls.h`](#tls-h) | `const char *tls_error(struct tls *ctx)` |
| `tls_free` | apps | [`tls.h`](#tls-h) | `void tls_free(struct tls *ctx)` |
| `tls_handshake` | apps | [`tls.h`](#tls-h) | `int tls_handshake(struct tls *ctx)` |
| `tls_init` | apps | [`tls.h`](#tls-h) | `int tls_init(void)` |
| `tls_read` | apps | [`tls.h`](#tls-h) | `ssize_t tls_read(struct tls *ctx, void *buf, size_t length)` |
| `tls_reset` | apps | [`tls.h`](#tls-h) | `void tls_reset(struct tls *ctx)` |
| `tls_write` | apps | [`tls.h`](#tls-h) | `ssize_t tls_write(struct tls *ctx, const void *buf, size_t length)` |
| `tmpfile` | apps | [`stdio.h`](#stdio-h) | `FILE *tmpfile(void)` |
| `tmpnam` | apps | [`stdio.h`](#stdio-h) | `char *tmpnam(char *s)` |
| `tolower` | apps | [`ctype.h`](#ctype-h) | `int tolower(int c)` |
| `toupper` | apps | [`ctype.h`](#ctype-h) | `int toupper(int c)` |
| `towctrans` | apps | [`wctype.h`](#wctype-h) | `wint_t towctrans(wint_t wc, wctrans_t mapping)` |
| `towlower` | apps | [`wctype.h`](#wctype-h) | `wint_t towlower(wint_t wc)` |
| `towupper` | apps | [`wctype.h`](#wctype-h) | `wint_t towupper(wint_t wc)` |
| `trunc` | apps | [`math.h`](#math-h) | `double trunc(double x)` |
| `truncf` | apps | [`math.h`](#math-h) | `float truncf(float x)` |
| `truncl` | apps | [`math.h`](#math-h) | `long double truncl(long double x)` |
| `tss_create` | apps | [`threads.h`](#threads-h) | `int tss_create(tss_t *key, tss_dtor_t dtor)` |
| `tss_delete` | apps | [`threads.h`](#threads-h) | `void tss_delete(tss_t key)` |
| `tss_get` | apps | [`threads.h`](#threads-h) | `void *tss_get(tss_t key)` |
| `tss_set` | apps | [`threads.h`](#threads-h) | `int tss_set(tss_t key, void *value)` |
| `umask` | apps | [`sys/stat.h`](#sys-stat-h) | `mode_t umask(mode_t mask)` |
| `uname` | apps | [`sys/utsname.h`](#sys-utsname-h) | `int uname(struct utsname *name)` |
| `ungetc` | apps | [`stdio.h`](#stdio-h) | `int ungetc(int c, FILE *f)` |
| `ungetwc` | apps | [`wchar.h`](#wchar-h) | `wint_t ungetwc(wint_t wc, FILE *f)` |
| `unlink` | apps | [`unistd.h`](#unistd-h) | `int unlink(const char *path)` |
| `unsetenv` | apps | [`stdlib.h`](#stdlib-h) | `int unsetenv(const char *name)` |
| `user_file_set` | system | [`ritteros.h`](#ritteros-h) | `int user_file_set(const char *path, const char *user, const char *line, unsigned mode)` |
| `user_name_valid` | system | [`ritteros.h`](#ritteros-h) | `bool user_name_valid(const char *name)` |
| `usleep` | apps | [`unistd.h`](#unistd-h) | `int usleep(useconds_t microseconds)` |
| `utf8_next` | apps | [`wfont.h`](#wfont-h) | `uint32_t utf8_next(const char **s)` |
| `utime` | apps | [`utime.h`](#utime-h) | `int utime(const char *path, const struct utimbuf *times)` |
| `utimensat` | apps | [`sys/stat.h`](#sys-stat-h) | `int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)` |
| `utimes` | apps | [`sys/time.h`](#sys-time-h) | `int utimes(const char *path, const struct timeval times[2])` |
| `vdprintf` | apps | [`stdio.h`](#stdio-h) | `int vdprintf(int fd, const char *format, va_list ap)` |
| `vfprintf` | apps | [`stdio.h`](#stdio-h) | `int vfprintf(FILE *f, const char *format, va_list ap)` |
| `vfscanf` | apps | [`stdio.h`](#stdio-h) | `int vfscanf(FILE *f, const char *format, va_list ap)` |
| `vfwprintf` | apps | [`wchar.h`](#wchar-h) | `int vfwprintf(FILE *f, const wchar_t *format, va_list ap)` |
| `vfwscanf` | apps | [`wchar.h`](#wchar-h) | `int vfwscanf(FILE *f, const wchar_t *format, va_list ap)` |
| `video_audio` | apps | [`video.h`](#video-h) | `int video_audio(struct video *v, struct audio_buffer *out)` |
| `video_close` | apps | [`video.h`](#video-h) | `void video_close(struct video *v)` |
| `video_info` | apps | [`video.h`](#video-h) | `void video_info(struct video *v, int *w, int *h, int *fps, int *has_audio)` |
| `video_open` | apps | [`video.h`](#video-h) | `int video_open(const char *path, struct video **v)` |
| `video_read_frame` | apps | [`video.h`](#video-h) | `int video_read_frame(struct video *v, struct image *frame, int64_t *pts_ns)` |
| `vprintf` | apps | [`stdio.h`](#stdio-h) | `int vprintf(const char *format, va_list ap)` |
| `vscanf` | apps | [`stdio.h`](#stdio-h) | `int vscanf(const char *format, va_list ap)` |
| `vsnprintf` | apps | [`stdio.h`](#stdio-h) | `int vsnprintf(char *buffer, size_t size, const char *format, va_list ap)` |
| `vsprintf` | apps | [`stdio.h`](#stdio-h) | `int vsprintf(char *buffer, const char *format, va_list ap)` |
| `vsscanf` | apps | [`stdio.h`](#stdio-h) | `int vsscanf(const char *text, const char *format, va_list ap)` |
| `vswprintf` | apps | [`wchar.h`](#wchar-h) | `int vswprintf(wchar_t *s, size_t size, const wchar_t *format, va_list ap)` |
| `vswscanf` | apps | [`wchar.h`](#wchar-h) | `int vswscanf(const wchar_t *s, const wchar_t *format, va_list ap)` |
| `vwprintf` | apps | [`wchar.h`](#wchar-h) | `int vwprintf(const wchar_t *format, va_list ap)` |
| `vwscanf` | apps | [`wchar.h`](#wchar-h) | `int vwscanf(const wchar_t *format, va_list ap)` |
| `wait` | apps | [`sys/wait.h`](#sys-wait-h) | `pid_t wait(int *status)` |
| `waitpid` | apps | [`sys/wait.h`](#sys-wait-h) | `pid_t waitpid(pid_t pid, int *status, int flags)` |
| `wcrtomb` | apps | [`wchar.h`](#wchar-h) | `size_t wcrtomb(char *s, wchar_t wc, mbstate_t *state)` |
| `wcscat` | apps | [`wchar.h`](#wchar-h) | `wchar_t *wcscat(wchar_t *dst, const wchar_t *src)` |
| `wcschr` | apps | [`wchar.h`](#wchar-h) | `wchar_t *wcschr(const wchar_t *s, wchar_t c)` |
| `wcscmp` | apps | [`wchar.h`](#wchar-h) | `int wcscmp(const wchar_t *a, const wchar_t *b)` |
| `wcscoll` | apps | [`wchar.h`](#wchar-h) | `int wcscoll(const wchar_t *a, const wchar_t *b)` |
| `wcscpy` | apps | [`wchar.h`](#wchar-h) | `wchar_t *wcscpy(wchar_t *dst, const wchar_t *src)` |
| `wcscspn` | apps | [`wchar.h`](#wchar-h) | `size_t wcscspn(const wchar_t *s, const wchar_t *reject)` |
| `wcsftime` | apps | [`wchar.h`](#wchar-h) | `size_t wcsftime(wchar_t *s, size_t size, const wchar_t *format, const struct tm *tm)` |
| `wcslen` | apps | [`wchar.h`](#wchar-h) | `size_t wcslen(const wchar_t *s)` |
| `wcsncat` | apps | [`wchar.h`](#wchar-h) | `wchar_t *wcsncat(wchar_t *dst, const wchar_t *src, size_t n)` |
| `wcsncmp` | apps | [`wchar.h`](#wchar-h) | `int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)` |
| `wcsncpy` | apps | [`wchar.h`](#wchar-h) | `wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n)` |
| `wcspbrk` | apps | [`wchar.h`](#wchar-h) | `wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept)` |
| `wcsrchr` | apps | [`wchar.h`](#wchar-h) | `wchar_t *wcsrchr(const wchar_t *s, wchar_t c)` |
| `wcsrtombs` | apps | [`wchar.h`](#wchar-h) | `size_t wcsrtombs(char *dst, const wchar_t **src, size_t n, mbstate_t *state)` |
| `wcsspn` | apps | [`wchar.h`](#wchar-h) | `size_t wcsspn(const wchar_t *s, const wchar_t *accept)` |
| `wcsstr` | apps | [`wchar.h`](#wchar-h) | `wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle)` |
| `wcstod` | apps | [`wchar.h`](#wchar-h) | `double wcstod(const wchar_t *s, wchar_t **end)` |
| `wcstof` | apps | [`wchar.h`](#wchar-h) | `float wcstof(const wchar_t *s, wchar_t **end)` |
| `wcstok` | apps | [`wchar.h`](#wchar-h) | `wchar_t *wcstok(wchar_t *s, const wchar_t *delimiters, wchar_t **place)` |
| `wcstol` | apps | [`wchar.h`](#wchar-h) | `long wcstol(const wchar_t *s, wchar_t **end, int base)` |
| `wcstold` | apps | [`wchar.h`](#wchar-h) | `long double wcstold(const wchar_t *s, wchar_t **end)` |
| `wcstoll` | apps | [`wchar.h`](#wchar-h) | `long long wcstoll(const wchar_t *s, wchar_t **end, int base)` |
| `wcstombs` | apps | [`stdlib.h`](#stdlib-h) | `size_t wcstombs(char *dst, const wchar_t *src, size_t n)` |
| `wcstoul` | apps | [`wchar.h`](#wchar-h) | `unsigned long wcstoul(const wchar_t *s, wchar_t **end, int base)` |
| `wcstoull` | apps | [`wchar.h`](#wchar-h) | `unsigned long long wcstoull(const wchar_t *s, wchar_t **end, int base)` |
| `wcsxfrm` | apps | [`wchar.h`](#wchar-h) | `size_t wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n)` |
| `wctob` | apps | [`wchar.h`](#wchar-h) | `int wctob(wint_t wc)` |
| `wctomb` | apps | [`stdlib.h`](#stdlib-h) | `int wctomb(char *s, wchar_t wc)` |
| `wctrans` | apps | [`wctype.h`](#wctype-h) | `wctrans_t wctrans(const char *name)` |
| `wctype` | apps | [`wctype.h`](#wctype-h) | `wctype_t wctype(const char *name)` |
| `WEXITSTATUS` | apps | [`sys/wait.h`](#sys-wait-h) | `#define WEXITSTATUS(status)` |
| `wfont_bold` | apps | [`wfont.h`](#wfont-h) | `const struct font *wfont_bold(void)` |
| `wfont_find` | apps | [`wfont.h`](#wfont-h) | `const struct font *wfont_find(const char *name)` |
| `wfont_height` | apps | [`wfont.h`](#wfont-h) | `int wfont_height(const struct font *font)` |
| `wfont_layout` | apps | [`wfont.h`](#wfont-h) | `int wfont_layout(uint32_t *buf, uint32_t stride, uint32_t bw, uint32_t bh, const struct font *font, int x, int y, int width, const char *utf8, uint32_t fg, uint32_t bg)` |
| `wfont_open` | apps | [`wfont.h`](#wfont-h) | `const struct font *wfont_open(const char *path, int size)` |
| `wfont_regular` | apps | [`wfont.h`](#wfont-h) | `const struct font *wfont_regular(void)` |
| `wfont_text` | apps | [`wfont.h`](#wfont-h) | `int wfont_text(uint32_t *buf, uint32_t stride, uint32_t bw, uint32_t bh, const struct font *font, int x, int y, const char *utf8, uint32_t fg, uint32_t bg)` |
| `wfont_text_width` | apps | [`wfont.h`](#wfont-h) | `int wfont_text_width(const struct font *font, const char *utf8)` |
| `wfont_width` | apps | [`wfont.h`](#wfont-h) | `int wfont_width(const struct font *font)` |
| `wfont_wrap` | apps | [`wfont.h`](#wfont-h) | `int wfont_wrap(const struct font *font, const char *utf8, int width)` |
| `widget_add` | apps | [`widget.h`](#widget-h) | `void widget_add(struct widget *parent, struct widget *child)` |
| `widget_button` | apps | [`widget.h`](#widget-h) | `struct widget *widget_button(const char *utf8)` |
| `widget_check` | apps | [`widget.h`](#widget-h) | `struct widget *widget_check(const char *utf8, int on)` |
| `widget_column` | apps | [`widget.h`](#widget-h) | `struct widget *widget_column(void)` |
| `widget_editor` | apps | [`widget.h`](#widget-h) | `struct widget *widget_editor(const char *utf8, int lines)` |
| `widget_free` | apps | [`widget.h`](#widget-h) | `void widget_free(struct widget *root)` |
| `widget_grid` | apps | [`widget.h`](#widget-h) | `struct widget *widget_grid(int columns)` |
| `widget_label` | apps | [`widget.h`](#widget-h) | `struct widget *widget_label(const char *utf8)` |
| `widget_list` | apps | [`widget.h`](#widget-h) | `struct widget *widget_list(const char *lines)` |
| `widget_message` | apps | [`widget.h`](#widget-h) | `void widget_message(struct window *parent, const char *title, const char *text)` |
| `widget_on` | apps | [`widget.h`](#widget-h) | `void widget_on(struct widget *w, void (*fn)(struct widget *w, void *data), void *data)` |
| `widget_progress` | apps | [`widget.h`](#widget-h) | `struct widget *widget_progress(int max, int value)` |
| `widget_question` | apps | [`widget.h`](#widget-h) | `int widget_question(struct window *parent, const char *title, const char *text, const char *yes, const char *no)` |
| `widget_quit` | apps | [`widget.h`](#widget-h) | `void widget_quit(struct widget *root)` |
| `widget_radio` | apps | [`widget.h`](#widget-h) | `struct widget *widget_radio(const char *utf8, struct widget *group, int on)` |
| `widget_row` | apps | [`widget.h`](#widget-h) | `struct widget *widget_row(void)` |
| `widget_run` | apps | [`widget.h`](#widget-h) | `int widget_run(struct window *win, struct widget *root)` |
| `widget_set_text` | apps | [`widget.h`](#widget-h) | `void widget_set_text(struct widget *w, const char *utf8)` |
| `widget_set_value` | apps | [`widget.h`](#widget-h) | `void widget_set_value(struct widget *w, int value)` |
| `widget_slider` | apps | [`widget.h`](#widget-h) | `struct widget *widget_slider(int min, int max, int value)` |
| `widget_stretch` | apps | [`widget.h`](#widget-h) | `void widget_stretch(struct widget *w, int weight)` |
| `widget_table` | apps | [`widget.h`](#widget-h) | `struct widget *widget_table(int columns, const char *rows)` |
| `widget_tabs` | apps | [`widget.h`](#widget-h) | `struct widget *widget_tabs(void)` |
| `widget_text` | apps | [`widget.h`](#widget-h) | `const char *widget_text(const struct widget *w)` |
| `widget_tree` | apps | [`widget.h`](#widget-h) | `struct widget *widget_tree(const char *lines)` |
| `widget_value` | apps | [`widget.h`](#widget-h) | `int widget_value(const struct widget *w)` |
| `WIFEXITED` | apps | [`sys/wait.h`](#sys-wait-h) | `#define WIFEXITED(status)` |
| `WIFSIGNALED` | apps | [`sys/wait.h`](#sys-wait-h) | `#define WIFSIGNALED(status)` |
| `window_app_for` | apps | [`window.h`](#window-h) | `int window_app_for(const char *path, char *buf, size_t size)` |
| `window_app_opens` | apps | [`window.h`](#window-h) | `int window_app_opens(const char *bundle, const char *file)` |
| `window_blit` | apps | [`window.h`](#window-h) | `void window_blit(struct window *win, int x, int y, int w, int h, const uint32_t *src)` |
| `window_choose` | apps | [`window.h`](#window-h) | `int window_choose(struct window *parent, const char *title, const char *folder, int (*accept)(const char *path, void *data), void *data, char *path, int size)` |
| `window_choose_file` | apps | [`window.h`](#window-h) | `int window_choose_file(struct window *parent, const char *title, const char *folder, char *path, int size)` |
| `window_clipboard_get` | apps | [`window.h`](#window-h) | `int window_clipboard_get(struct window *win, char *buf, size_t size)` |
| `window_clipboard_set` | apps | [`window.h`](#window-h) | `int window_clipboard_set(struct window *win, const char *utf8, size_t n)` |
| `window_close` | apps | [`window.h`](#window-h) | `void window_close(struct window *win)` |
| `window_color` | apps | [`window.h`](#window-h) | `uint32_t window_color(int which)` |
| `window_dialog_open` | apps | [`window.h`](#window-h) | `struct window *window_dialog_open(struct window *parent, int w, int h, const char *title)` |
| `window_drag` | apps | [`window.h`](#window-h) | `int window_drag(struct window *win, const char *text, size_t n)` |
| `window_drop_text` | apps | [`window.h`](#window-h) | `const char *window_drop_text(struct window *win)` |
| `window_fd` | apps | [`window.h`](#window-h) | `int window_fd(struct window *win)` |
| `window_field_draw` | apps | [`window.h`](#window-h) | `void window_field_draw(struct window_field *f)` |
| `window_field_event` | apps | [`window.h`](#window-h) | `int window_field_event(struct window_field *f, const struct window_event *ev)` |
| `window_field_focus` | apps | [`window.h`](#window-h) | `void window_field_focus(struct window_field *f, int on)` |
| `window_field_focused` | apps | [`window.h`](#window-h) | `int window_field_focused(const struct window_field *f)` |
| `window_field_free` | apps | [`window.h`](#window-h) | `void window_field_free(struct window_field *f)` |
| `window_field_height` | apps | [`window.h`](#window-h) | `int window_field_height(const struct window_field *f)` |
| `window_field_new` | apps | [`window.h`](#window-h) | `struct window_field *window_field_new(struct window *win, int x, int y, int w)` |
| `window_field_place` | apps | [`window.h`](#window-h) | `void window_field_place(struct window_field *f, int x, int y, int w)` |
| `window_field_set_text` | apps | [`window.h`](#window-h) | `void window_field_set_text(struct window_field *f, const char *utf8)` |
| `window_field_text` | apps | [`window.h`](#window-h) | `const char *window_field_text(const struct window_field *f)` |
| `window_fill` | apps | [`window.h`](#window-h) | `void window_fill(struct window *win, int x, int y, int w, int h, uint32_t color)` |
| `window_flush` | apps | [`window.h`](#window-h) | `void window_flush(struct window *win)` |
| `window_fullscreen` | apps | [`window.h`](#window-h) | `void window_fullscreen(struct window *win, int on)` |
| `window_height` | apps | [`window.h`](#window-h) | `int window_height(struct window *win)` |
| `window_minimize` | apps | [`window.h`](#window-h) | `void window_minimize(struct window *win)` |
| `window_notify` | apps | [`window.h`](#window-h) | `int window_notify(struct window *win, const char *title, const char *text)` |
| `window_open` | apps | [`window.h`](#window-h) | `struct window *window_open(int w, int h, const char *title)` |
| `window_pixels` | apps | [`window.h`](#window-h) | `uint32_t *window_pixels(struct window *win)` |
| `window_popup_menu` | apps | [`window.h`](#window-h) | `int window_popup_menu(struct window *win, int x, int y, const struct window_menu_item *items, int count)` |
| `window_resize` | apps | [`window.h`](#window-h) | `int window_resize(struct window *win, int w, int h)` |
| `window_rgb` | apps | [`window.h`](#window-h) | `uint32_t window_rgb(struct window *win, int r, int g, int b)` |
| `window_scrollbar` | apps | [`window.h`](#window-h) | `void window_scrollbar(struct window *win, int x, int y, int h, int first, int shown, int total)` |
| `window_scrollbar_hit` | apps | [`window.h`](#window-h) | `int window_scrollbar_hit(int y, int h, int py, int shown, int total)` |
| `window_set_cursor` | apps | [`window.h`](#window-h) | `void window_set_cursor(struct window *win, int shape)` |
| `window_set_limits` | apps | [`window.h`](#window-h) | `void window_set_limits(struct window *win, int min_w, int min_h, int max_w, int max_h)` |
| `window_set_menu` | apps | [`window.h`](#window-h) | `int window_set_menu(struct window *win, const struct window_menu *menus, int count)` |
| `window_shifts` | apps | [`window.h`](#window-h) | `void window_shifts(struct window *win, int *red, int *green, int *blue)` |
| `window_text` | apps | [`window.h`](#window-h) | `void window_text(struct window *win, int x, int y, const char *utf8, uint32_t fg, uint32_t bg)` |
| `window_text_font` | apps | [`window.h`](#window-h) | `void window_text_font(struct window *win, const struct font *font, int x, int y, const char *utf8, uint32_t fg, uint32_t bg)` |
| `window_timer` | apps | [`window.h`](#window-h) | `int window_timer(struct window *win, int ms, int id)` |
| `window_wait` | apps | [`window.h`](#window-h) | `int window_wait(struct window *win, struct window_event *ev)` |
| `window_wait_timeout` | apps | [`window.h`](#window-h) | `int window_wait_timeout(struct window *win, struct window_event *ev, int ms)` |
| `window_width` | apps | [`window.h`](#window-h) | `int window_width(struct window *win)` |
| `wmemchr` | apps | [`wchar.h`](#wchar-h) | `wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n)` |
| `wmemcmp` | apps | [`wchar.h`](#wchar-h) | `int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n)` |
| `wmemcpy` | apps | [`wchar.h`](#wchar-h) | `wchar_t *wmemcpy(wchar_t *dst, const wchar_t *src, size_t n)` |
| `wmemmove` | apps | [`wchar.h`](#wchar-h) | `wchar_t *wmemmove(wchar_t *dst, const wchar_t *src, size_t n)` |
| `wmemset` | apps | [`wchar.h`](#wchar-h) | `wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n)` |
| `wprintf` | apps | [`wchar.h`](#wchar-h) | `int wprintf(const wchar_t *format, ...)` |
| `write` | apps | [`unistd.h`](#unistd-h) | `ssize_t write(int fd, const void *buffer, size_t size)` |
| `wscanf` | apps | [`wchar.h`](#wchar-h) | `int wscanf(const wchar_t *format, ...)` |
| `WTERMSIG` | apps | [`sys/wait.h`](#sys-wait-h) | `#define WTERMSIG(status)` |
| `x25519` | apps | [`crypto.h`](#crypto-h) | `void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])` |
| `x25519_base` | apps | [`crypto.h`](#crypto-h) | `void x25519_base(uint8_t out[32], const uint8_t scalar[32])` |
| `x509_error` | system | [`x509.h`](#x509-h) | `const char *x509_error(int e)` |
| `x509_host_matches` | system | [`x509.h`](#x509-h) | `int x509_host_matches(const struct x509 *c, const char *host)` |
| `x509_parse` | system | [`x509.h`](#x509-h) | `int x509_parse(struct x509 *c, const uint8_t *der, size_t len)` |
| `x509_signed_by` | system | [`x509.h`](#x509-h) | `int x509_signed_by(const struct x509 *child, const struct x509 *parent)` |
| `x509_verify_chain` | system | [`x509.h`](#x509-h) | `int x509_verify_chain(const struct x509 *chain, int count, const struct x509 *roots, int nroots, const char *host, int64_t now)` |

## The headers, as they are

<a id="stdio-h"></a>

### stdio.h

`user/include/stdio.h` (in RitterOS's tree) -- formatted output; for apps.

Formatted output and streams. printf's conversions: %d %i %u %x %X %o %c %s %p %%, and since 22q %f %F %e %E %g %G %a %A exactly (every digit of a double's value, rounded to nearest with ties to even; L for long double, printed at double's precision); the length modifiers hh h l ll z j t L, the flags - 0 + # and space, a width and a precision, both also as '*'.

```c
#define EOF (-1)

int printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
int dprintf(int fd, const char *format, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buffer, size_t size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buffer, size_t size, const char *format, va_list ap);
int vdprintf(int fd, const char *format, va_list ap);
/* (22q) Into buffer without a size: snprintf is the safe one. */
int sprintf(char *buffer, const char *format, ...) __attribute__((format(printf, 2, 3)));
int vsprintf(char *buffer, const char *format, va_list ap);
int puts(const char *s);                /* s and a newline */

/* Move or rename a file or directory (to must not exist); 0, or -1 with
 * errno set. */
int rename(const char *from, const char *to);
int putchar(int c);

/* Streams (22m): files through a buffer, as C's FILE. stdin, stdout and
 * stderr are the descriptors 0, 1 and 2 (stdout and stderr not buffered, so
 * they keep their order with printf). Modes "r", "w", "a", with "+" for
 * both ways; "b" is accepted and changes nothing (files are bytes). The
 * stream functions return EOF or 0 on failure as C's do, errno set. */
typedef struct ritteros_file FILE;
FILE *ritteros_stdin(void);
FILE *ritteros_stdout(void);
FILE *ritteros_stderr(void);
#define stdin  (ritteros_stdin())
#define stdout (ritteros_stdout())
#define stderr (ritteros_stderr())

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ   4096

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
/* (58c) A command run by sh -c, as a stream: "r" reads its output, "w"
 * writes its input; pclose waits for it and gives its status (as wait's:
 * WEXITSTATUS). At most 32 open at once. */
FILE *popen(const char *command, const char *mode);
int pclose(FILE *stream);
int fclose(FILE *f);
size_t fread(void *buf, size_t size, size_t count, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t count, FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int fflush(FILE *f);                    /* 0: every stream */
int fgetc(FILE *f);
int getc(FILE *f);
int getchar(void);
int ungetc(int c, FILE *f);
char *fgets(char *s, int size, FILE *f);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int fputs(const char *s, FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
void clearerr(FILE *f);
int fileno(FILE *f);
int fprintf(FILE *f, const char *format, ...) __attribute__((format(printf, 2, 3)));
int vfprintf(FILE *f, const char *format, va_list ap);
int vprintf(const char *format, va_list ap);
/* Remove a file or an empty folder. */
int remove(const char *path);

/* Temporary files (22n), in the user's own ~/.tmp (made when first needed,
 * 0700). tmpnam: a name no file has (into s, L_tmpnam bytes, or a buffer of
 * its own for s = 0; 0 when no home folder); tmpfile: such a file, open for
 * reading and writing, removed again at fclose or exit. */
#define L_tmpnam 256
#define TMP_MAX  1000000
char *tmpnam(char *s);
FILE *tmpfile(void);

/* Reading numbers and words (22m; from streams and with floating point
 * since 22q): %d %i %u %x %X %o %p (with hh, h, l, ll), %e %f %g %a and
 * their capitals (float; l: double, L: long double), %c, %s, %[set], %n,
 * %%, widths and %*... (assigned to nothing); the number of fields
 * assigned, or EOF if the input ended before the first. A stream gives
 * back one byte at the end (what scanf looked at and did not take). */
int sscanf(const char *text, const char *format, ...) __attribute__((format(scanf, 2, 3)));
int vsscanf(const char *text, const char *format, va_list ap);
int scanf(const char *format, ...) __attribute__((format(scanf, 1, 2)));
int fscanf(FILE *f, const char *format, ...) __attribute__((format(scanf, 2, 3)));
int vscanf(const char *format, va_list ap);
int vfscanf(FILE *f, const char *format, va_list ap);

/* (22q) "s: the error's text" on stderr (errno's; without s, only the text). */
void perror(const char *s);
/* (22q) How a stream writes: _IOFBF when its buffer is full, _IOLBF at every
 * line break too, _IONBF at once (the buffer stays the stream's own). */
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
int setvbuf(FILE *f, char *buf, int mode, size_t size);
void setbuf(FILE *f, char *buf);
/* (22q) The stream on another file (stdout on a file: descriptor 1 again). */
FILE *freopen(const char *path, const char *mode, FILE *f);
/* (22q) A place in a stream, to come back to. */
typedef long fpos_t;
int fgetpos(FILE *f, fpos_t *pos);
int fsetpos(FILE *f, const fpos_t *pos);

/* (65e-53) Every stream function holds the stream's lock for its call;
 * flockfile holds it across several (nesting), ftrylockfile only if free
 * (0, else nonzero). The _unlocked functions take no lock: between
 * flockfile and funlockfile. */
void flockfile(FILE *f);
int ftrylockfile(FILE *f);
void funlockfile(FILE *f);
int getc_unlocked(FILE *f);
int getchar_unlocked(void);
int putc_unlocked(int c, FILE *f);
int putchar_unlocked(int c);
```

<a id="stdlib-h"></a>

### stdlib.h

`user/include/stdlib.h` (in RitterOS's tree) -- memory, exit, numbers; for apps.

```c
#define EXIT_SUCCESS    0
#define EXIT_FAILURE    1

/* End the program with an exit code (0-255; 0: success). */
__attribute__((noreturn)) void exit(int code);

/* The heap (user/lib/malloc.c): memory from the kernel in large arenas, a
 * free list inside them. Returns 0 (and errno ENOMEM) if none is left. free
 * of 0 does nothing; realloc(0, n) is malloc, realloc(p, 0) frees. */
void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
/* (22p) Memory at an address that is a multiple of alignment (a power of
 * two); freed with free. 0 and EINVAL for another alignment. */
void *aligned_alloc(size_t alignment, size_t size);

/* The decimal number at the start of s (spaces and a sign before it
 * allowed; 0 if there is none). */
long atol(const char *s);
int atoi(const char *s);
long long atoll(const char *s);                     /* (22p) */

/* Numbers from text (22m): as C's -- spaces, a sign, then digits in base
 * (0: from the prefix, 0x hexadecimal, 0 octal); *end (if not 0) where
 * reading stopped. strtod/atof read decimal numbers with a fraction and an
 * exponent (and inf, nan). */
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double strtod(const char *s, char **end);
double atof(const char *s);
/* (22p) strtof: strtod's value rounded to float; strtold: strtod's value
 * (long double has double's precision here: exact for every double, but
 * not the extra digits of an 80-bit or 128-bit long double). */
float strtof(const char *s, char **end);
long double strtold(const char *s, char **end);

int abs(int v);
long labs(long v);
long long llabs(long long v);

/* (22p) A quotient and its remainder at once (rounded toward zero). */
typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;
div_t div(int numerator, int denominator);
ldiv_t ldiv(long numerator, long denominator);
lldiv_t lldiv(long long numerator, long long denominator);

/* (22p) Sorting and searching an array of count elements of size bytes, by
 * compare (negative, 0, positive: before, the same, after). qsort is not
 * stable (equal elements may change places); bsearch needs it sorted. */
void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *a, const void *b));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *key, const void *element));

/* (22p) Pseudo-random numbers, 0 to RAND_MAX: the same series for the same
 * seed (srand; 1 at the start). Not for secrets: ritteros_random is. */
#define RAND_MAX 2147483647
int rand(void);
void srand(unsigned int seed);

/* (22p) Functions exit calls first, the last given first (up to 32 each);
 * quick_exit calls at_quick_exit's and ends without flushing streams;
 * _Exit ends at once (no functions, no flushing). 0, or nonzero when full. */
int atexit(void (*fn)(void));
int at_quick_exit(void (*fn)(void));
__attribute__((noreturn)) void quick_exit(int code);
__attribute__((noreturn)) void _Exit(int code);

/* (22p) Multibyte characters in the "C" locale: every byte is a character
 * of its own (MB_CUR_MAX 1; UTF-8 is not decoded here). mblen/mbtowc: the
 * bytes of the character at s (0 for the NUL, -1 for none); wctomb: the
 * bytes written (-1, EILSEQ, for a character above 255). */
#define MB_CUR_MAX 1
int mblen(const char *s, size_t n);
int mbtowc(wchar_t *wc, const char *s, size_t n);
int wctomb(char *s, wchar_t wc);
size_t mbstowcs(wchar_t *dst, const char *src, size_t n);
size_t wcstombs(char *dst, const wchar_t *src, size_t n);

/* (22t) Environment variables: getenv a variable's value (0 if not set);
 * setenv sets one (overwrite 0: only if not set yet), unsetenv removes one,
 * putenv puts the string "NAME=value" itself in; 0, or -1 and errno
 * (EINVAL: a name with '=' or empty). Programs started later get them. */
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *string);

/* (41g-3) The program's name: the last part of its argv[0] (BSD's and
 * macOS's getprogname), "" when it had none. */
const char *getprogname(void);

/* Run a command with the shell (sh -c COMMAND): its status as waitpid gives
 * it (WEXITSTATUS), or -1 if it could not start; system(0) is 1 (there is a
 * shell). */
int system(const char *command);

/* A new file from a name ending in "XXXXXX" (22n): the X's become random
 * letters and digits no file has; the file is made (mode 0600) and its
 * descriptor, open for reading and writing, returned; -1 and errno
 * (EINVAL: no XXXXXX at the end). */
int mkstemp(char *name);

/* End the program at once, as a failed assert does: with SIGABRT (22r; a
 * handler may run first, but the program ends even if it returns). */
__attribute__((noreturn)) void abort(void);
```

<a id="string-h"></a>

### string.h

`user/include/string.h` (in RitterOS's tree) -- strings and memory; for apps.

```c
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
size_t strcspn(const char *s, const char *reject);    /* bytes before one of reject */
char *strcpy(char *dst, const char *src);
/* (32e) stpcpy: strcpy, giving the end of dst (its NUL); memrchr: memchr
 * from the end (the last c in n bytes, or 0; GNU's). */
char *stpcpy(char *dst, const char *src);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
void *memrchr(const void *s, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);

/* (22m) A copy on the heap (strndup: of at most n bytes); 0 if out of memory. */
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
/* As C's: n bytes always written (padded with NULs; no NUL if src is longer). */
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);

/* (22p) strspn: bytes at the start that are all in accept; strpbrk: the
 * first of breaks (or 0); strtok: the next word between delimiters (s for
 * the first call, 0 for the rest; it writes NULs into s); strnlen: strlen
 * up to n; memccpy: copies until c (the place after it in dst, or 0);
 * strcoll and strxfrm as strcmp and strncpy (the "C" locale's order). */
size_t strspn(const char *s, const char *accept);
char *strpbrk(const char *s, const char *breaks);
char *strtok(char *s, const char *delimiters);
size_t strnlen(const char *s, size_t n);
void *memccpy(void *dst, const void *src, int c, size_t n);
int strcoll(const char *a, const char *b);
size_t strxfrm(char *dst, const char *src, size_t n);

/* The text for an error number, e.g. "No such file or directory". */
const char *strerror(int error);
```

<a id="strings-h"></a>

### strings.h

`user/include/strings.h` (in RitterOS's tree) -- comparing strings without case; for apps.

strings.h (22m): comparing text without regard to case (ASCII letters), as BSD and POSIX have it.

```c
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
```

<a id="ctype-h"></a>

### ctype.h

`user/include/ctype.h` (in RitterOS's tree) -- classes of characters (ASCII); for apps.

ctype.h (22m): classes of characters, for ASCII (other byte values are in no class; text is UTF-8, whose multi-byte characters these do not judge).

```c
int isalnum(int c);
int isalpha(int c);
int isblank(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);
int tolower(int c);
int toupper(int c);
```

<a id="assert-h"></a>

### assert.h

`user/include/assert.h` (in RitterOS's tree) -- assert; for apps.

assert.h (22m): assert(expression) ends the program when the expression is false, saying which and where ("assertion failed: x > 0 (game.c:42, main)"), with exit code 134; NDEBUG defined before this header leaves every assert out. Not guarded: the header may come again with NDEBUG changed, as C allows.

```c
#undef assert
#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
__attribute__((noreturn)) void ritteros_assert_failed(const char *expression, const char *file,
                                                      int line, const char *function);
#define assert(expression) \
    ((expression) ? (void)0 : ritteros_assert_failed(#expression, __FILE__, __LINE__, __func__))
#endif
```

<a id="math-h"></a>

### math.h

`user/include/math.h` (in RitterOS's tree) -- C99's mathematics: trigonometry, exponentials, logarithms, powers; for apps.

math.h: C99's mathematics (22m: absolute values, square roots, rounding; 22s: the rest). Our own code, in double-double arithmetic (about 32 significant digits inside): results are within one unit in the last place of the exact value, nearly always the nearest double to it. Arguments of sin, cos and tan are reduced exactly, also very large ones (sin(1e300)). The float forms (sinf, ...) are the double ones rounded to float. errno is never set (math_errhandling is 0): domain errors give NaN, overflows HUGE_VAL, as C's Annex F says.

```c
#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_1_PI      0.31830988618379067154
#define M_2_PI      0.63661977236758134308
#define M_2_SQRTPI  1.12837916709551257390
#define M_E         2.7182818284590452354
#define M_LOG2E     1.4426950408889634074
#define M_LOG10E    0.43429448190325182765
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_SQRT2     1.41421356237309504880
#define M_SQRT1_2   0.70710678118654752440
#define HUGE_VAL    __builtin_huge_val()
#define HUGE_VALF   __builtin_huge_valf()
#define HUGE_VALL   __builtin_huge_vall()
#define INFINITY    __builtin_inff()
#define NAN         __builtin_nanf("")
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x)  __builtin_signbit(x)
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#define fpclassify(x) \
    __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define math_errhandling 0
/* (22z) */
#define FP_ILOGB0   (-2147483647 - 1)
#define FP_ILOGBNAN (-2147483647 - 1)
#define isgreater(x, y)      __builtin_isgreater(x, y)
#define isgreaterequal(x, y) __builtin_isgreaterequal(x, y)
#define isless(x, y)         __builtin_isless(x, y)
#define islessequal(x, y)    __builtin_islessequal(x, y)
#define islessgreater(x, y)  __builtin_islessgreater(x, y)
#define isunordered(x, y)    __builtin_isunordered(x, y)
typedef float float_t;
typedef double double_t;

double fabs(double x);
float fabsf(float x);
double sqrt(double x);
float sqrtf(float x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);                 /* halfway cases away from zero */
double fmod(double x, double y);
float floorf(float x);
float ceilf(float x);
float truncf(float x);
float roundf(float x);
float fmodf(float x, float y);

/* (22s) Trigonometry, in radians. */
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);       /* the angle of (x, y), -pi to pi */
float sinf(float x);
float cosf(float x);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);

/* (22s) Hyperbolic functions. */
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);
float asinhf(float x);
float acoshf(float x);
float atanhf(float x);

/* (22s) Exponentials, logarithms and powers. expm1 and log1p keep their
 * precision near 0 (e^x - 1, ln(1 + x)). */
double exp(double x);
double exp2(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);
double pow(double x, double y);
double cbrt(double x);
double hypot(double x, double y);       /* sqrt(x^2 + y^2) without overflow */
float expf(float x);
float exp2f(float x);
float expm1f(float x);
float logf(float x);
float log2f(float x);
float log10f(float x);
float log1pf(float x);
float powf(float x, float y);
float cbrtf(float x);
float hypotf(float x, float y);

/* (22s) The parts of a number: x = m * 2^e (frexp: m in [0.5, 1)), x *
 * 2^n (ldexp, scalbn), the whole and fraction parts (modf); copysign,
 * fmin and fmax (a NaN loses to a number). */
double frexp(double x, int *e);
double ldexp(double x, int n);
double scalbn(double x, int n);
double modf(double x, double *whole);
double copysign(double x, double y);
double fmin(double x, double y);
double fmax(double x, double y);
float frexpf(float x, int *e);
float ldexpf(float x, int n);
float scalbnf(float x, int n);
float modff(float x, float *whole);
float copysignf(float x, float y);
float fminf(float x, float y);
float fmaxf(float x, float y);

/* (22z) Rounding in the current direction (fenv.h's fesetround): rint
 * (raises inexact for a fraction), nearbyint (never), lrint and llrint
 * (out of range: invalid, LONG_MIN); lround and llround round halfway
 * cases away from zero. */
double rint(double x);
double nearbyint(double x);
long lrint(double x);
long long llrint(double x);
long lround(double x);
long long llround(double x);
float rintf(float x);
float nearbyintf(float x);
long lrintf(float x);
long long llrintf(float x);
long lroundf(float x);
long long llroundf(float x);

/* (22z) Exact operations: remainder (x - n y, n the nearest integer to
 * x / y, ties even) and remquo (also n's low 31 bits, with x / y's sign),
 * fdim (x - y or 0), fma (x * y + z rounded once), nextafter and
 * nexttoward (the next double toward y), ilogb and logb (the exponent;
 * ilogb(0) is FP_ILOGB0), scalbln (scalbn with a long), nan (a quiet NaN;
 * the text is not used). */
double remainder(double x, double y);
double remquo(double x, double y, int *quo);
double fdim(double x, double y);
double fma(double x, double y, double z);
double nextafter(double x, double y);
double nexttoward(double x, long double y);
int ilogb(double x);
double logb(double x);
double scalbln(double x, long n);
double nan(const char *tag);
float remainderf(float x, float y);
float remquof(float x, float y, int *quo);
float fdimf(float x, float y);
float fmaf(float x, float y, float z);
float nextafterf(float x, float y);
float nexttowardf(float x, long double y);
int ilogbf(float x);
float logbf(float x);
float scalblnf(float x, long n);
float nanf(const char *tag);

/* (22z) The error function, its complement 1 - erf (precise also where
 * it is tiny), the gamma function and the logarithm of its size (no
 * signgam: lgamma's sign is tgamma's). */
double erf(double x);
double erfc(double x);
double tgamma(double x);
double lgamma(double x);
float erff(float x);
float erfcf(float x);
float tgammaf(float x);
float lgammaf(float x);

/* (22z) The long double forms of all of the above: the double functions,
 * long double having double's precision here (fabsl and copysignl keep
 * every bit). */
long double fabsl(long double x);
long double sqrtl(long double x);
long double floorl(long double x);
long double ceill(long double x);
long double truncl(long double x);
long double roundl(long double x);
long double rintl(long double x);
long double nearbyintl(long double x);
long lrintl(long double x);
long long llrintl(long double x);
long lroundl(long double x);
long long llroundl(long double x);
long double fmodl(long double x, long double y);
long double remainderl(long double x, long double y);
long double remquol(long double x, long double y, int *quo);
long double fdiml(long double x, long double y);
long double fmal(long double x, long double y, long double z);
long double fminl(long double x, long double y);
long double fmaxl(long double x, long double y);
long double copysignl(long double x, long double y);
long double nextafterl(long double x, long double y);
long double nexttowardl(long double x, long double y);
long double nanl(const char *tag);
long double frexpl(long double x, int *e);
long double ldexpl(long double x, int n);
long double scalbnl(long double x, int n);
long double scalblnl(long double x, long n);
long double modfl(long double x, long double *whole);
int ilogbl(long double x);
long double logbl(long double x);
long double sinl(long double x);
long double cosl(long double x);
long double tanl(long double x);
long double asinl(long double x);
long double acosl(long double x);
long double atanl(long double x);
long double atan2l(long double y, long double x);
long double sinhl(long double x);
long double coshl(long double x);
long double tanhl(long double x);
long double asinhl(long double x);
long double acoshl(long double x);
long double atanhl(long double x);
long double expl(long double x);
long double exp2l(long double x);
long double expm1l(long double x);
long double logl(long double x);
long double log2l(long double x);
long double log10l(long double x);
long double log1pl(long double x);
long double powl(long double x, long double y);
long double cbrtl(long double x);
long double hypotl(long double x, long double y);
long double erfl(long double x);
long double erfcl(long double x);
long double tgammal(long double x);
long double lgammal(long double x);

/* (22x) The compiler's helpers for long double on AArch64, where it is
 * IEEE binary128 in software: arithmetic, comparisons and conversions, at
 * double's precision (as the rest of long double here); called by compiled
 * code, not by programs. In the library on both architectures (x86-64's
 * long double is the x87's own and never calls them). */
long double __addtf3(long double a, long double b);
long double __subtf3(long double a, long double b);
long double __multf3(long double a, long double b);
long double __divtf3(long double a, long double b);
long double __negtf2(long double a);
long double __powitf2(long double a, int b);
int __eqtf2(long double a, long double b);
int __netf2(long double a, long double b);
int __lttf2(long double a, long double b);
int __letf2(long double a, long double b);
int __gttf2(long double a, long double b);
int __getf2(long double a, long double b);
int __cmptf2(long double a, long double b);
int __unordtf2(long double a, long double b);
long double __extendsftf2(float a);
long double __extenddftf2(double a);
float __trunctfsf2(long double a);
double __trunctfdf2(long double a);
int __fixtfsi(long double a);
long __fixtfdi(long double a);
unsigned int __fixunstfsi(long double a);
unsigned long __fixunstfdi(long double a);
long double __floatsitf(int a);
long double __floatditf(long a);
long double __floatunsitf(unsigned int a);
long double __floatunditf(unsigned long a);
```

<a id="fenv-h"></a>

### fenv.h

`user/include/fenv.h` (in RitterOS's tree) -- the floating-point environment: rounding directions and exception flags; for apps.

The floating-point environment (step 22u): the rounding direction and the exception flags (sticky: set by an operation that was invalid, divided by zero, overflowed, underflowed or was inexact) of the calling thread, as the processor keeps them (x86-64: SSE's MXCSR and the x87's control and status words, both set together; AArch64: FPCR and FPSR). Every thread and every new program starts rounding to nearest with no flags set; the kernel keeps them per thread. Exceptions never trap. math.h's functions expect rounding to nearest.

```c
/* (The same numbers on both architectures; the library translates them
 * into the processor's bits.) */
#define FE_INVALID      0x01
#define FE_DIVBYZERO    0x02
#define FE_OVERFLOW     0x04
#define FE_UNDERFLOW    0x08
#define FE_INEXACT      0x10
#define FE_ALL_EXCEPT   0x1F
#define FE_TONEAREST    0
#define FE_UPWARD       1
#define FE_DOWNWARD     2
#define FE_TOWARDZERO   3
typedef unsigned int fexcept_t;
/* The processor's registers as they were: x86-64 the x87's control word
 * (low half) and status word (high half), then MXCSR; AArch64 FPCR, FPSR. */
typedef struct {
    unsigned int control, status;
} fenv_t;

/* The environment every program starts with (for fesetenv, feupdateenv). */
#define FE_DFL_ENV ((const fenv_t *)-1)

/* The exception flags: cleared, saved, raised (set; nothing traps), set
 * back from a saved copy, tested (which of excepts are set). 0 on success. */
int feclearexcept(int excepts);
int fegetexceptflag(fexcept_t *flags, int excepts);
int feraiseexcept(int excepts);
int fesetexceptflag(const fexcept_t *flags, int excepts);
int fetestexcept(int excepts);
/* The rounding direction: FE_TONEAREST and the others; fesetround 0 on
 * success (nonzero for a value that is not one of them). */
int fegetround(void);
int fesetround(int round);
/* The whole environment: saved, set; feholdexcept saves it and clears the
 * flags, feupdateenv sets a saved one and raises the flags set meanwhile. */
int fegetenv(fenv_t *env);
int feholdexcept(fenv_t *env);
int fesetenv(const fenv_t *env);
int feupdateenv(const fenv_t *env);
```

<a id="wchar-h"></a>

### wchar.h

`user/include/wchar.h` (in RitterOS's tree) -- wide characters and strings, wide streams and formatting (the C locale: a byte each); for apps.

wchar.h (step 22v): wide characters and strings, C11's set. RitterOS has the "C" locale only, one byte per character (MB_CUR_MAX 1, as stdlib.h's mbtowc): a byte's wide character is its value (0-255), and a wide character above 255 has no byte (EILSEQ where one is needed: wcrtomb, fputwc, %ls in printf). For UTF-8 there is uchar.h (always UTF-8). Streams: the wide calls read and write bytes; fwide records a stream's orientation (the byte calls do not set it). Formatting: wprintf's and wscanf's conversions are printf's and scanf's.

```c
#ifndef RITTEROS_WINT_T
#define RITTEROS_WINT_T
typedef __WINT_TYPE__ wint_t;
#endif
#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

/* A conversion's state between calls (uchar.h's UTF-8: the bytes still
 * needed and the bits so far). All zero: the initial state. */
#ifndef RITTEROS_MBSTATE_T
#define RITTEROS_MBSTATE_T
typedef struct {
    unsigned int need, bits;
} mbstate_t;
#endif

struct tm;

/* Strings, as string.h's (counts in wide characters). wcstok's third
 * argument keeps its place between calls. */
size_t wcslen(const wchar_t *s);
wchar_t *wcscpy(wchar_t *dst, const wchar_t *src);
wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wcscat(wchar_t *dst, const wchar_t *src);
wchar_t *wcsncat(wchar_t *dst, const wchar_t *src, size_t n);
int wcscmp(const wchar_t *a, const wchar_t *b);
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
int wcscoll(const wchar_t *a, const wchar_t *b);
size_t wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
size_t wcsspn(const wchar_t *s, const wchar_t *accept);
size_t wcscspn(const wchar_t *s, const wchar_t *reject);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept);
wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle);
wchar_t *wcstok(wchar_t *s, const wchar_t *delimiters, wchar_t **place);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wmemcpy(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wmemmove(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n);

/* Numbers from wide text, as strtol and strtod. */
long wcstol(const wchar_t *s, wchar_t **end, int base);
long long wcstoll(const wchar_t *s, wchar_t **end, int base);
unsigned long wcstoul(const wchar_t *s, wchar_t **end, int base);
unsigned long long wcstoull(const wchar_t *s, wchar_t **end, int base);
double wcstod(const wchar_t *s, wchar_t **end);
float wcstof(const wchar_t *s, wchar_t **end);
long double wcstold(const wchar_t *s, wchar_t **end);

/* A date as wide text, as strftime. */
size_t wcsftime(wchar_t *s, size_t size, const wchar_t *format, const struct tm *tm);

/* Bytes and wide characters (one byte each, see above). mbrtowc: the
 * character at s into *wc; 1, 0 for the NUL, (size_t)-2 for n = 0.
 * wcrtomb: wc's byte into s; 1, or (size_t)-1 (EILSEQ). The s...rtowcs
 * forms convert strings, with *src moved past what was converted (0 at
 * the end). */
wint_t btowc(int c);
int wctob(wint_t wc);
int mbsinit(const mbstate_t *state);
size_t mbrlen(const char *s, size_t n, mbstate_t *state);
size_t mbrtowc(wchar_t *wc, const char *s, size_t n, mbstate_t *state);
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *state);
size_t mbsrtowcs(wchar_t *dst, const char **src, size_t n, mbstate_t *state);
size_t wcsrtombs(char *dst, const wchar_t **src, size_t n, mbstate_t *state);

/* Wide characters on streams. fwide: mode > 0 makes a stream wide, < 0
 * byte-oriented (once); the result tells which it is (0: neither yet). */
int fwide(FILE *f, int mode);
wint_t fgetwc(FILE *f);
wint_t getwc(FILE *f);
wint_t getwchar(void);
wchar_t *fgetws(wchar_t *s, int size, FILE *f);
wint_t ungetwc(wint_t wc, FILE *f);
wint_t fputwc(wchar_t wc, FILE *f);
wint_t putwc(wchar_t wc, FILE *f);
wint_t putwchar(wchar_t wc);
int fputws(const wchar_t *s, FILE *f);

/* Formatted: printf's conversions with a wide format (%s takes bytes, %ls
 * wide characters, in every form). swprintf writes at most size wide
 * characters with the NUL, and is negative when more were needed. */
int wprintf(const wchar_t *format, ...);
int fwprintf(FILE *f, const wchar_t *format, ...);
int swprintf(wchar_t *s, size_t size, const wchar_t *format, ...);
int vwprintf(const wchar_t *format, va_list ap);
int vfwprintf(FILE *f, const wchar_t *format, va_list ap);
int vswprintf(wchar_t *s, size_t size, const wchar_t *format, va_list ap);
int wscanf(const wchar_t *format, ...);
int fwscanf(FILE *f, const wchar_t *format, ...);
int swscanf(const wchar_t *s, const wchar_t *format, ...);
int vwscanf(const wchar_t *format, va_list ap);
int vfwscanf(FILE *f, const wchar_t *format, va_list ap);
int vswscanf(const wchar_t *s, const wchar_t *format, va_list ap);
```

<a id="wctype-h"></a>

### wctype.h

`user/include/wctype.h` (in RitterOS's tree) -- classes of wide characters (ASCII); for apps.

wctype.h (step 22v): classes of wide characters, as ctype.h's: the "C" locale's, ASCII (characters above 127 are in no class and have no other case). wctype and wctrans name a class or a mapping ("alpha", "toupper", ...; 0 for a name not known).

```c
#ifndef RITTEROS_WINT_T
#define RITTEROS_WINT_T
typedef __WINT_TYPE__ wint_t;
#endif
#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

typedef unsigned long wctype_t;
typedef unsigned long wctrans_t;

int iswalnum(wint_t wc);
int iswalpha(wint_t wc);
int iswblank(wint_t wc);
int iswcntrl(wint_t wc);
int iswdigit(wint_t wc);
int iswgraph(wint_t wc);
int iswlower(wint_t wc);
int iswprint(wint_t wc);
int iswpunct(wint_t wc);
int iswspace(wint_t wc);
int iswupper(wint_t wc);
int iswxdigit(wint_t wc);
int iswctype(wint_t wc, wctype_t type);
wctype_t wctype(const char *name);
wint_t towlower(wint_t wc);
wint_t towupper(wint_t wc);
wint_t towctrans(wint_t wc, wctrans_t mapping);
wctrans_t wctrans(const char *name);
```

<a id="uchar-h"></a>

### uchar.h

`user/include/uchar.h` (in RitterOS's tree) -- Unicode characters: UTF-8 always, UTF-16 and code points; for apps.

uchar.h (step 22w): Unicode characters and UTF-8. Unlike wchar.h's calls (the "C" locale: a byte each), these always read and write UTF-8, the text of RitterOS: char32_t holds a code point, char16_t a UTF-16 unit (characters above U+FFFF are two: a surrogate pair), char8_t (C23) a UTF-8 byte. Invalid UTF-8 (overlong forms, surrogates, above U+10FFFF, a byte out of place) is EILSEQ.  mbrtoXX: the character at s (at most n bytes) into *c; the bytes it took, 0 for the NUL, (size_t)-2 for a character not complete yet (the bytes are kept in *state), (size_t)-1 for invalid UTF-8, (size_t)-3 for a unit given from *state without taking a byte (the second half of a pair, the rest of a char8_t's character). XXrtomb: c's UTF-8 into s; the bytes written (0 while a pair's or a character's first part is kept in *state), (size_t)-1 for a value that is not one. A null state uses one of the library's own.

```c
typedef unsigned char char8_t;
typedef __CHAR16_TYPE__ char16_t;
typedef __CHAR32_TYPE__ char32_t;

#ifndef RITTEROS_MBSTATE_T
#define RITTEROS_MBSTATE_T
typedef struct {
    unsigned int need, bits;
} mbstate_t;
#endif

size_t mbrtoc8(char8_t *c, const char *s, size_t n, mbstate_t *state);
size_t c8rtomb(char *s, char8_t c, mbstate_t *state);
size_t mbrtoc16(char16_t *c, const char *s, size_t n, mbstate_t *state);
size_t c16rtomb(char *s, char16_t c, mbstate_t *state);
size_t mbrtoc32(char32_t *c, const char *s, size_t n, mbstate_t *state);
size_t c32rtomb(char *s, char32_t c, mbstate_t *state);
```

<a id="complex-h"></a>

### complex.h

`user/include/complex.h` (in RitterOS's tree) -- complex numbers: C99's functions and the compiler's * and / helpers; for apps.

complex.h (step 22x): C99's complex numbers. The functions are our own, computed in double from math.h's (Kahan's formulas for the inverse functions, which keep their branch cuts and precision near them); the float forms are the double ones rounded, the long double ones the double ones widened (long double at double's precision, as strtold). Infinities and NaNs as C's Annex G for cabs, carg, cproj, cexp, clog and csqrt. Branch cuts: along the negative real axis for clog, csqrt and cpow, the real axis outside [-1, 1] for casin, cacos and catanh, and so on, as C.

```c
#define complex _Complex
#define _Complex_I (__extension__ 1.0iF)
#define I _Complex_I
#define CMPLX(x, y) __builtin_complex((double)(x), (double)(y))
#define CMPLXF(x, y) __builtin_complex((float)(x), (float)(y))
#define CMPLXL(x, y) __builtin_complex((long double)(x), (long double)(y))

double creal(double _Complex z);
double cimag(double _Complex z);
double cabs(double _Complex z);                 /* |z|, as hypot */
double carg(double _Complex z);                 /* the angle, -pi to pi */
double _Complex conj(double _Complex z);
double _Complex cproj(double _Complex z);       /* infinities onto the Riemann sphere's one */
double _Complex cexp(double _Complex z);
double _Complex clog(double _Complex z);
double _Complex cpow(double _Complex z, double _Complex w);
double _Complex csqrt(double _Complex z);
double _Complex csin(double _Complex z);
double _Complex ccos(double _Complex z);
double _Complex ctan(double _Complex z);
double _Complex casin(double _Complex z);
double _Complex cacos(double _Complex z);
double _Complex catan(double _Complex z);
double _Complex csinh(double _Complex z);
double _Complex ccosh(double _Complex z);
double _Complex ctanh(double _Complex z);
double _Complex casinh(double _Complex z);
double _Complex cacosh(double _Complex z);
double _Complex catanh(double _Complex z);

float crealf(float _Complex z);
float cimagf(float _Complex z);
float cabsf(float _Complex z);
float cargf(float _Complex z);
float _Complex conjf(float _Complex z);
float _Complex cprojf(float _Complex z);
float _Complex cexpf(float _Complex z);
float _Complex clogf(float _Complex z);
float _Complex cpowf(float _Complex z, float _Complex w);
float _Complex csqrtf(float _Complex z);
float _Complex csinf(float _Complex z);
float _Complex ccosf(float _Complex z);
float _Complex ctanf(float _Complex z);
float _Complex casinf(float _Complex z);
float _Complex cacosf(float _Complex z);
float _Complex catanf(float _Complex z);
float _Complex csinhf(float _Complex z);
float _Complex ccoshf(float _Complex z);
float _Complex ctanhf(float _Complex z);
float _Complex casinhf(float _Complex z);
float _Complex cacoshf(float _Complex z);
float _Complex catanhf(float _Complex z);

long double creall(long double _Complex z);
long double cimagl(long double _Complex z);
long double cabsl(long double _Complex z);
long double cargl(long double _Complex z);
long double _Complex conjl(long double _Complex z);
long double _Complex cprojl(long double _Complex z);
long double _Complex cexpl(long double _Complex z);
long double _Complex clogl(long double _Complex z);
long double _Complex cpowl(long double _Complex z, long double _Complex w);
long double _Complex csqrtl(long double _Complex z);
long double _Complex csinl(long double _Complex z);
long double _Complex ccosl(long double _Complex z);
long double _Complex ctanl(long double _Complex z);
long double _Complex casinl(long double _Complex z);
long double _Complex cacosl(long double _Complex z);
long double _Complex catanl(long double _Complex z);
long double _Complex csinhl(long double _Complex z);
long double _Complex ccoshl(long double _Complex z);
long double _Complex ctanhl(long double _Complex z);
long double _Complex casinhl(long double _Complex z);
long double _Complex cacoshl(long double _Complex z);
long double _Complex catanhl(long double _Complex z);

/* The compiler's helpers for * and / of complex numbers (C's Annex G:
 * an infinite operand gives an infinite result, NaN parts or not); called
 * by compiled code, not by programs. The long double pairs are x86-64's
 * (xc3) and AArch64's (tc3) names; both are in the library. */
double _Complex __muldc3(double a, double b, double c, double d);
double _Complex __divdc3(double a, double b, double c, double d);
float _Complex __mulsc3(float a, float b, float c, float d);
float _Complex __divsc3(float a, float b, float c, float d);
long double _Complex __mulxc3(long double a, long double b, long double c, long double d);
long double _Complex __divxc3(long double a, long double b, long double c, long double d);
long double _Complex __multc3(long double a, long double b, long double c, long double d);
long double _Complex __divtc3(long double a, long double b, long double c, long double d);
```

<a id="threads-h"></a>

### threads.h

`user/include/threads.h` (in RitterOS's tree) -- C11's threads, mutexes, condition variables, call_once, thread-specific storage; for apps.

threads.h (step 22y): C11's threads, mutexes, condition variables, call_once and thread-specific storage. Threads are thread.h's (up to 256 per program, the main one included; thrd_t is the thread's number); waiting goes through the kernel's futex (SYS_FUTEX), so a blocked thread uses no processor time. Timed calls take an absolute TIME_UTC time (timespec_get), measured in the kernel's 10 ms ticks. Mutexes, condition variables and once flags are a program's own (not shared between programs). thread_local is C's _Thread_local (per-thread data, 22i).

```c
#define thread_local _Thread_local
#define ONCE_FLAG_INIT { 0 }
#define TSS_DTOR_ITERATIONS 4

typedef int thrd_t;
typedef int (*thrd_start_t)(void *arg);

typedef struct {
    int state;                          /* 0 free, 1 taken, 2 taken with waiters */
    int type;                           /* mtx_plain, mtx_timed, | mtx_recursive */
    int owner;                          /* recursive: the holder's number + 1 */
    unsigned int count;                 /* recursive: times it is held */
} mtx_t;

typedef struct {
    unsigned int sequence;              /* counts signals; waiters sleep on it */
} cnd_t;

typedef struct {
    int state;                          /* 0 not yet, 1 running, 2 done */
} once_flag;

typedef unsigned int tss_t;
typedef void (*tss_dtor_t)(void *value);

enum { thrd_success = 0, thrd_busy = 1, thrd_error = 2, thrd_nomem = 3, thrd_timedout = 4 };
enum { mtx_plain = 0, mtx_recursive = 1, mtx_timed = 2 };

/* Threads. thrd_create: func(arg) in a new thread (thrd_error when all 16
 * are taken: a detached thread's number is free again once it ended).
 * thrd_join: wait for it, its result into *result. thrd_detach: nobody
 * will join it. thrd_exit: end the calling thread (thread-specific values'
 * destructors run first, also when func returns). thrd_sleep: 0, -1 if a
 * signal ended it early (the rest into *remaining). */
int thrd_create(thrd_t *thr, thrd_start_t func, void *arg);
int thrd_join(thrd_t thr, int *result);
int thrd_detach(thrd_t thr);
thrd_t thrd_current(void);
int thrd_equal(thrd_t a, thrd_t b);
__attribute__((noreturn)) void thrd_exit(int result);
int thrd_sleep(const struct timespec *duration, struct timespec *remaining);
void thrd_yield(void);

/* Mutexes: a recursive one may be locked again by its holder (and is free
 * once unlocked as often). */
int mtx_init(mtx_t *m, int type);
int mtx_lock(mtx_t *m);
int mtx_timedlock(mtx_t *m, const struct timespec *until);
int mtx_trylock(mtx_t *m);
int mtx_unlock(mtx_t *m);
void mtx_destroy(mtx_t *m);

/* Condition variables: cnd_wait unlocks m, waits for a signal and locks m
 * again (it may also return without one: check the condition in a loop). */
int cnd_init(cnd_t *c);
int cnd_signal(cnd_t *c);
int cnd_broadcast(cnd_t *c);
int cnd_wait(cnd_t *c, mtx_t *m);
int cnd_timedwait(cnd_t *c, mtx_t *m, const struct timespec *until);
void cnd_destroy(cnd_t *c);

/* func runs once for flag, whichever threads call; the others wait for it. */
void call_once(once_flag *flag, void (*func)(void));

/* Thread-specific storage: a value per thread for each key (64 keys); a
 * destructor runs on a thread's non-null value when the thread ends. */
int tss_create(tss_t *key, tss_dtor_t dtor);
void *tss_get(tss_t key);
int tss_set(tss_t key, void *value);
void tss_delete(tss_t key);
```

<a id="setjmp-h"></a>

### setjmp.h

`user/include/setjmp.h` (in RitterOS's tree) -- setjmp and longjmp: jumping back out of deep calls; for apps.

Non-local jumps (step 22n): setjmp keeps the place of a call, longjmp goes back to it from deeper calls (setjmp then returns longjmp's value, 1 for 0). The function that called setjmp must not have returned; local variables it changed after setjmp should be volatile. setjmp keeps no signal mask, and there is no sigsetjmp.

```c
/* The registers a call keeps (x86-64: 8 words; AArch64: 21), with room. */
typedef long jmp_buf[22];

__attribute__((returns_twice)) int setjmp(jmp_buf env);
__attribute__((noreturn)) void longjmp(jmp_buf env, int value);
```

<a id="locale-h"></a>

### locale.h

`user/include/locale.h` (in RitterOS's tree) -- the C locale (the only one); for apps.

Locales (step 22n): RitterOS has one, the "C" locale (also called "POSIX"): ASCII, a '.' before decimals, no grouping. setlocale takes it by those names or "" (the system's: the same) and refuses others (0); setlocale(category, 0) says which is in use.

```c
#define LC_CTYPE    0
#define LC_NUMERIC  1
#define LC_TIME     2
#define LC_COLLATE  3
#define LC_MONETARY 4
#define LC_MESSAGES 5
#define LC_ALL      6

/* How numbers and money are written (C's struct; the "C" locale's values:
 * "." and empty texts, CHAR_MAX (127) for "not given"). */
struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    char int_p_cs_precedes;
    char int_p_sep_by_space;
    char int_n_cs_precedes;
    char int_n_sep_by_space;
    char int_p_sign_posn;
    char int_n_sign_posn;
};

char *setlocale(int category, const char *locale);
struct lconv *localeconv(void);
```

<a id="time-h"></a>

### time.h

`user/include/time.h` (in RitterOS's tree) -- the clock, CPU time, dates and their text; for apps.

Time (step 22o): the clock (seconds since 1970-01-01 UTC), the CPU time a program used, dates in the calendar and dates as text. Local time is UTC plus the fixed offset in /system/config/time ("zone = -05:00"; none: UTC); there are no daylight saving rules, so tm_isdst is always 0.

```c
typedef int64_t time_t;
typedef long clock_t;

#define CLOCKS_PER_SEC 1000000L

/* A date: as C's struct, with the two fields BSD and Linux add. */
struct tm {
    int tm_sec;                 /* 0-60 */
    int tm_min;                 /* 0-59 */
    int tm_hour;                /* 0-23 */
    int tm_mday;                /* 1-31 */
    int tm_mon;                 /* 0-11 */
    int tm_year;                /* years since 1900 */
    int tm_wday;                /* 0-6, Sunday 0 */
    int tm_yday;                /* 0-365 */
    int tm_isdst;               /* 0 (no daylight saving here) */
    long tm_gmtoff;             /* seconds east of UTC */
    const char *tm_zone;        /* "UTC" or "UTC-05:00" */
};

/* Seconds since 1970 (also into *t), or -1 when the machine's clock is
 * unknown. */
time_t time(time_t *t);
/* The CPU time the program has used, in CLOCKS_PER_SEC (all its threads,
 * the kernel's work for it included); -1 if not known. */
clock_t clock(void);
double difftime(time_t end, time_t start);
/* A local date into seconds: out-of-range fields are carried (minute 75
 * is 1:15), and *tm is filled in again with the date found. */
time_t mktime(struct tm *tm);
/* Seconds into a date: in UTC or local time. The plain forms share one
 * result (until the next call); the _r forms fill in result. */
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *result);
struct tm *localtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *result);
/* A date as text into s (max bytes with the 0), by format's conversions
 * (C99's, the "C" locale's names: %a %A %b %B %c %C %d %D %e %F %g %G %h
 * %H %I %j %m %M %n %p %r %R %S %t %T %u %U %V %w %W %x %X %y %Y %z %Z
 * %%; E and O are ignored): the length, or 0 if it did not fit. */
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);
/* "Sun Sep 16 01:03:52 1973\n" (a result of their own, until the next call). */
char *asctime(const struct tm *tm);
char *ctime(const time_t *t);

/* (22r) Seconds and nanoseconds. */
struct timespec {
    time_t tv_sec;
    long tv_nsec;
};
/* C11's: the time now in base TIME_UTC (since 1970); TIME_UTC, or 0. */
#define TIME_UTC 1
int timespec_get(struct timespec *ts, int base);
/* POSIX's (22r): CLOCK_REALTIME the date's clock, CLOCK_MONOTONIC one that
 * never goes back (the CPU's counter since the start), and the CPU time the
 * program has used; 0, or -1 (EINVAL: another clock). nanosleep waits at
 * least req (in the timer's ticks of 10 ms); interrupted by a signal: -1,
 * EINTR, and what was left into *rem. */
typedef int clockid_t;
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
int clock_gettime(clockid_t clock, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);
```

<a id="inttypes-h"></a>

### inttypes.h

`user/include/inttypes.h` (in RitterOS's tree) -- printf conversions for stdint.h's types; for apps.

inttypes.h (22m): printf and scanf conversions for the integer types of stdint.h (64-bit programs: long is 64 bits, so PRId64 is "ld").

```c
#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "ld"
#define PRIi8  "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIi64 "li"
#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "lu"
#define PRIx8  "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 "lx"
#define PRIX8  "X"
#define PRIX16 "X"
#define PRIX32 "X"
#define PRIX64 "lX"
#define PRIo8  "o"
#define PRIo16 "o"
#define PRIo32 "o"
#define PRIo64 "lo"
#define PRIdMAX "ld"
#define PRIuMAX "lu"
#define PRIxMAX "lx"
#define PRIdPTR "ld"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#define SCNd32 "d"
#define SCNd64 "ld"
#define SCNu32 "u"
#define SCNu64 "lu"
#define SCNx32 "x"
#define SCNx64 "lx"
```

<a id="unistd-h"></a>

### unistd.h

`user/include/unistd.h` (in RitterOS's tree) -- files, processes, sleeping; for apps.

Reading, writing and closing file descriptors.

```c
#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

/* Bytes transferred (read: 0 at the end), or -1 with errno set. */
ssize_t read(int fd, void *buffer, size_t size);
ssize_t write(int fd, const void *buffer, size_t size);
int close(int fd);

/* (52a) Another descriptor for fd's open file (the two share its position):
 * dup takes the lowest free number, dup2 newfd (closing what was open
 * there; newfd == fd does nothing). The new descriptor, or -1 with errno
 * set. */
int dup(int fd);
int dup2(int fd, int newfd);

/* (52a) A pipe: fds[0] reads what fds[1] writes (a read gives 0 once every
 * write end is closed, a write fails with EPIPE once every read end is).
 * 0, or -1 with errno set. */
int pipe(int fds[2]);

/* Move a file's position (SEEK_SET: from the start, SEEK_CUR, SEEK_END);
 * the new position, or -1 with errno set. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
off_t lseek(int fd, off_t offset, int whence);

pid_t getpid(void);

/* The program's user and group (0: root); root may change them (setuid
 * sets both users). getuid is the real user, who started it; geteuid the
 * effective one, whose rights apply: they differ only in a setuid program
 * (a root-owned one with mode 4755 runs as root for anyone). */
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);

/* Pause the program; other programs run meanwhile. */
unsigned int sleep(unsigned int seconds);
int usleep(useconds_t microseconds);

/* RitterOS: start the program at path with the arguments argv (argv[0]:
 * its name; a null pointer ends the list) as a child process. Returns its
 * ID, or -1 with errno set. The child shares standard input and output and
 * the current directory, and joins the caller's process group;
 * spawn_new_group makes it lead a new one (a job, for a shell). */
/* (22t) The program's environment: "NAME=value" strings, ended by a null
 * pointer (environ is a call underneath: a library exports no data). */
char ***__ritteros_environ(void);
#define environ (*__ritteros_environ())

pid_t spawn(const char *path, char *const argv[]);

/* (55b) Run another program in this process: same process number,
 * parent, user, folder, umask and open descriptors (FD_CLOEXEC ones
 * close), the blocked and ignored signals kept, handled ones back to
 * SIG_DFL, the other threads ended. They return only on an error (-1,
 * errno set; the program goes on). execve: envp the environment; execv
 * and execl: this one's; execvp and execlp look a name without '/' up in
 * PATH (none: /system/<arch>/bin, then /apps) and run a script without #!
 * through sh; execle takes the environment after the list's null pointer. */
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *name, char *const argv[]);
int execl(const char *path, const char *arg, ...);
int execlp(const char *name, const char *arg, ...);
int execle(const char *path, const char *arg, ...);
/* (56a) A new process that is a copy of this one: its memory (copied when
 * either writes a page; MAP_SHARED memory stays shared), open descriptors
 * (the same open files), folder, umask, signal handlers and blocked set,
 * but only the calling thread, and no signals pending. The child's process
 * number, and 0 in the child; -1 (errno: ENOMEM) if it cannot be made.
 * pthread_atfork's handlers run around it. */
pid_t fork(void);
/* (56a) End at once, as C's _Exit: no atexit functions, no flushing (a
 * forked child that must not write its parent's buffered output again). */
__attribute__((noreturn)) void _exit(int code);
pid_t spawn_new_group(const char *path, char *const argv[]);

int chdir(const char *path);

/* Remove an empty directory, or a file; 0, or -1 with errno set. */
int rmdir(const char *path);
int unlink(const char *path);
/* (57a) Links, on RitterFS (EPERM on FAT and exFAT). symlink: a symbolic
 * link at path naming target (kept as given: relative to the link's
 * folder when used; 1-255 bytes); paths through it lead to target (40 at
 * most in one path, then ELOOP); unlink and rename act on the link itself.
 * readlink: the bytes it names, not zero-terminated, at most size; how
 * many (EINVAL: not a link). link: a second name to for the file at from
 * (a symbolic link itself, not what it names), on the same volume (EXDEV),
 * not for a folder (EPERM); the file goes when its last name does. */
int symlink(const char *target, const char *path);
ssize_t readlink(const char *path, char *buffer, size_t size);
int link(const char *from, const char *to);
char *getcwd(char *buffer, size_t size);

/* 1 if fd is a terminal (a console or pseudo-terminal), else 0. */
int isatty(int fd);

/* Process groups and the terminal's foreground job (fd: a terminal). */
pid_t getpgrp(void);
/* (56b) The parent's process number (0: started by the kernel, or its
 * parent has ended). setpgid: pid (0: this one, or a child that has not
 * run another program: EACCES) joins group (0: its own number, a new group)
 * of this session (EPERM otherwise, and for a session's leader); getpgid:
 * a process's group (ESRCH: none); setsid: a new session and group led by
 * this process, with no terminal (EPERM if it leads a group already). */
pid_t getppid(void);
int setpgid(pid_t pid, pid_t group);
pid_t getpgid(pid_t pid);
pid_t setsid(void);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t group);
```

<a id="fcntl-h"></a>

### fcntl.h

`user/include/fcntl.h` (in RitterOS's tree) -- opening files; for apps.

Opening files.

```c
#define O_RDONLY        OPEN_READ
#define O_WRONLY        OPEN_WRITE
#define O_RDWR          OPEN_READ_WRITE
#define O_CREAT         OPEN_CREATE
#define O_TRUNC         OPEN_TRUNCATE
#define O_APPEND        OPEN_APPEND
#define O_EXCL          OPEN_EXCLUSIVE  /* (52j) with O_CREAT: fail (EEXIST) if it exists */
#define O_NONBLOCK      OPEN_NONBLOCK   /* (53b) calls on it never wait: EAGAIN */
#define O_ACCMODE       OPEN_ACCESS

/* (53b) fcntl's commands: F_DUPFD (the lowest free descriptor from arg
 * on), F_DUPFD_CLOEXEC, F_GETFD/F_SETFD (FD_CLOEXEC: kept, though no
 * program yet replaces itself), F_GETFL (the access mode, O_APPEND,
 * O_NONBLOCK), F_SETFL (O_APPEND, O_NONBLOCK: for the open file, which
 * dup'd descriptors and started programs share). With O_NONBLOCK, reads,
 * writes, accept and connect never wait: EAGAIN, or what was written so
 * far; connect EINPROGRESS (poll for POLLOUT, then SO_ERROR); a line read
 * at the terminal still waits for its Enter once a key is typed. */
#define F_DUPFD         FCNTL_DUPFD
#define F_GETFD         FCNTL_GETFD
#define F_SETFD         FCNTL_SETFD
#define F_GETFL         FCNTL_GETFL
#define F_SETFL         FCNTL_SETFL
#define F_DUPFD_CLOEXEC FCNTL_DUPFD_CLOEXEC
#define FD_CLOEXEC      FCNTL_CLOEXEC
int fcntl(int fd, int command, ...);

/* (63m) For utimensat: the current folder (relative paths start there;
 * other folders' descriptors are not taken yet: ENOTSUP), and a symbolic
 * link itself instead of what it names. */
#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100

/* A new file descriptor for path, or -1 with errno set. A file written
 * through it is written to the disk when it is closed (or the program
 * ends). (52b) With O_CREAT a third argument is the new file's mode
 * (e.g. 0666; the umask's bits are taken away, 022 at the start), as POSIX's. */
int open(const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
```

<a id="errno-h"></a>

### errno.h

`user/include/errno.h` (in RitterOS's tree) -- error numbers; for apps.

Error numbers: set by the system call wrappers when they return -1.

```c
/* errno is the place __errno() returns: a function rather than a variable
 * since step 22c, because a shared library exports functions only (a
 * variable's layout would be copied into every program). Use errno as ever. */
int *__errno(void);
#define errno (*__errno())

#define EPERM           ERR_PERM
#define ENOENT          ERR_NOENT
#define EACCES          ERR_ACCES
#define ESRCH           ERR_SRCH
#define EAGAIN          ERR_AGAIN
#define EINTR           ERR_INTR
#define EIO             ERR_IO
#define E2BIG           ERR_2BIG
#define ENOEXEC         ERR_NOEXEC
#define EBADF           ERR_BADF
#define ECHILD          ERR_CHILD
#define ENOMEM          ERR_NOMEM
#define EFAULT          ERR_FAULT
#define EEXIST          ERR_EXIST
#define ENODEV          ERR_NODEV
#define ENXIO           ERR_NXIO
#define ENOTTY          ERR_NOTTY
#define ENOTSOCK        ERR_NOTSOCK     /* (53a: the sockets') */
#define ELOOP           ERR_LOOP        /* (57a) too many symbolic links in a path */
#define EDESTADDRREQ    ERR_DESTADDRREQ
#define EMSGSIZE        ERR_MSGSIZE
#define ENOPROTOOPT     ERR_NOPROTOOPT
#define EPROTONOSUPPORT ERR_PROTONOSUPPORT
#define ESOCKTNOSUPPORT ERR_SOCKTNOSUPPORT
#define EAFNOSUPPORT    ERR_AFNOSUPPORT
#define EADDRNOTAVAIL   ERR_ADDRNOTAVAIL
#define ECONNABORTED    ERR_CONNABORTED
#define EISCONN         ERR_ISCONN
#define EALREADY        ERR_ALREADY     /* (53b) */
#define EDEADLK         ERR_DEADLK      /* (54a) */
#define EINPROGRESS     ERR_INPROGRESS
#define EOPNOTSUPP      ENOTSUP
#define EWOULDBLOCK     EAGAIN
#define ENOTDIR         ERR_NOTDIR
#define EISDIR          ERR_ISDIR
#define EINVAL          ERR_INVAL
#define EMFILE          ERR_MFILE
#define EROFS           ERR_ROFS
#define ENAMETOOLONG    ERR_NAMETOOLONG
#define ENOSYS          ERR_NOSYS
#define ENOTEMPTY       ERR_NOTEMPTY
#define ENOSPC          ERR_NOSPC
#define ENODATA         ERR_NODATA
#define EBUSY           ERR_BUSY
#define EXDEV           ERR_XDEV
#define ENOTSUP         ERR_NOTSUP
#define ENETUNREACH     ERR_NETUNREACH
#define ETIMEDOUT       ERR_TIMEDOUT
#define EHOSTUNREACH    ERR_HOSTUNREACH
#define EPIPE           ERR_PIPE
#define EADDRINUSE      ERR_ADDRINUSE
#define ECONNRESET      ERR_CONNRESET
#define ENOTCONN        ERR_NOTCONN
#define ECONNREFUSED    ERR_CONNREFUSED
#define ENONAME         ERR_NONAME
#define ERANGE          34              /* a number out of range (strtol, 22m; the library's own) */
#define EILSEQ          84      /* (22p: a byte or character no character set has; Linux's number) */
```

<a id="sys-types-h"></a>

### sys/types.h

`user/include/sys/types.h` (in RitterOS's tree) -- basic types; for apps.

```c
typedef int pid_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int mode_t;
typedef int64_t ssize_t;
typedef unsigned int useconds_t;
typedef int64_t off_t;
```

<a id="sys-stat-h"></a>

### sys/stat.h

`user/include/sys/stat.h` (in RitterOS's tree) -- file status, directories, permissions; for apps.

File information.

```c
#define S_IFMT          0170000
#define S_IFDIR         0040000
#define S_IFREG         0100000
#define S_IFBLK         0060000         /* a disk or partition (/dev) */
#define S_IFCHR         0020000         /* (52b) a terminal, a sound device */
#define S_IFIFO         0010000         /* (52b) a pipe */
#define S_IFSOCK        0140000         /* (52b) a socket */
#define S_IFLNK         0120000         /* (57a) a symbolic link (lstat) */
#define S_ISDIR(mode)   (((mode) & S_IFMT) == S_IFDIR)
#define S_ISREG(mode)   (((mode) & S_IFMT) == S_IFREG)
#define S_ISBLK(mode)   (((mode) & S_IFMT) == S_IFBLK)
#define S_ISCHR(mode)   (((mode) & S_IFMT) == S_IFCHR)
#define S_ISFIFO(mode)  (((mode) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(mode)  (((mode) & S_IFMT) == S_IFSOCK)
#define S_ISLNK(mode)   (((mode) & S_IFMT) == S_IFLNK)
#define S_ISUID         0004000         /* setuid: the program runs as its file's owner (50b) */
#define S_ISVTX         0001000         /* (57b) sticky: in this folder only an entry's owner
                                         * (or the folder's) removes or renames it */

struct stat {
    uint32_t st_dev;            /* the file system (a mount point differs from its parent) */
    uint64_t st_ino;            /* RitterFS object ID */
    unsigned int st_mode;       /* S_IFDIR or S_IFREG, and the rwxrwxrwx bits */
    uid_t st_uid;               /* owner */
    gid_t st_gid;
    unsigned int st_nlink;
    int64_t st_size;
    int64_t st_ctime, st_mtime; /* seconds since 1970 (RitterFS: created, modified) */
};

/* Information about the file at path; 0, or -1 with errno set. */
int stat(const char *path, struct stat *st);
/* (52b) The same for an open descriptor (a file as it is now; a terminal,
 * pipe or socket by its kind: S_ISCHR, S_ISFIFO, S_ISSOCK). */
int fstat(int fd, struct stat *st);
/* (57a) stat of the name itself: a symbolic link is S_ISLNK, its size the
 * length of the path it names, its mode lrwxrwxrwx; st_nlink counts a
 * file's names (hard links). */
int lstat(const char *path, struct stat *st);

/* Create a directory with these permissions (e.g. 0755); 0, or -1 with
 * errno set. */
int mkdir(const char *path, unsigned int mode);

/* (52h) The bits taken off the modes of the files and directories this
 * program makes (open with O_CREAT, mkdir): 022 at the start, so 0666
 * gives 0644; passed on to the programs it starts. Sets mask, returns the
 * one before. */
mode_t umask(mode_t mask);

/* Permissions (the owner or root; S_ISUID too) and owner (root only; it
 * clears S_ISUID) of a file or directory; 0, or -1 with errno set. */
int chmod(const char *path, mode_t mode);
int chown(const char *path, uid_t uid, gid_t gid);

/* (63m) A file's times: [0] the last access (taken, not kept: RitterOS
 * keeps none), [1] the last modification; tv_nsec UTIME_NOW for now,
 * UTIME_OMIT to leave it; times 0: both now. utimensat names the file by
 * path (dirfd AT_FDCWD, or any with an absolute path; flags 0 or
 * AT_SYMLINK_NOFOLLOW: a link itself), futimens by an open descriptor.
 * Now needs the owner, write permission or root (EACCES); any other time
 * the owner or root (EPERM). RitterFS keeps nanoseconds, exFAT 10 ms, FAT
 * 2 seconds. 0, or -1 with errno set. */
#define UTIME_NOW       ((1L << 30) - 1)
#define UTIME_OMIT      ((1L << 30) - 2)
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags);
int futimens(int fd, const struct timespec times[2]);
```

<a id="poll-h"></a>

### poll.h

`user/include/poll.h` (in RitterOS's tree) -- waiting for several descriptors at once (poll); for apps.

Waiting for several descriptors at once (52d).

```c
struct pollfd {
    int fd;                     /* a negative one is skipped (revents 0) */
    short events;               /* what to wait for: POLLIN, POLLOUT */
    short revents;              /* what was found; POLLERR, POLLHUP and POLLNVAL always */
};

typedef unsigned long nfds_t;

/* How many of the count descriptors have something to report, waiting up
 * to timeout milliseconds for the first (-1: as long as it takes, 0: not
 * at all); -1 with errno set (EINTR: a signal came, EINVAL: more than 256).
 * Pipes, pseudo-terminals, the terminal (a byte typed: a line read still
 * waits for its Enter) and sockets can wait; files and devices never do. */
int poll(struct pollfd *fds, nfds_t count, int timeout);
```

<a id="sys-socket-h"></a>

### sys/socket.h

`user/include/sys/socket.h` (in RitterOS's tree) -- BSD sockets: TCP, UDP and local connections by address; for apps.

BSD sockets (53a): connections and datagrams by address. AF_INET is IPv4 over TCP (SOCK_STREAM) and UDP (SOCK_DGRAM), AF_INET6 (64d) IPv6 and IPv4 (netinet/in.h); AF_UNIX the local stream sockets, named by sun_path in a kernel registry (no file is made). Binding takes a port; the socket answers on every address the machine has. Calls return -1 with errno set on errors.

```c
typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

/* Room for any address (sockaddr_in, sockaddr_in6, sockaddr_un). */
struct sockaddr_storage {
    sa_family_t ss_family;
    char ss_padding[118];
    uint64_t ss_align;
};

#define AF_UNSPEC       0
#define AF_UNIX         1
#define AF_LOCAL        AF_UNIX
#define AF_INET         2
#define AF_INET6        10
#define PF_UNSPEC       AF_UNSPEC
#define PF_UNIX         AF_UNIX
#define PF_LOCAL        AF_UNIX
#define PF_INET         AF_INET
#define PF_INET6        AF_INET6

#define SOCK_STREAM     1
#define SOCK_DGRAM      2

#define SOMAXCONN       128

/* setsockopt/getsockopt at SOL_SOCKET. SO_RCVTIMEO and SO_SNDTIMEO (a
 * struct timeval) end a wait with EAGAIN; SO_REUSEADDR, SO_KEEPALIVE and
 * SO_BROADCAST are kept and change nothing (a port is only taken while
 * something listens on it; broadcasts are allowed); SO_ERROR reads 0,
 * SO_TYPE the SOCK_* kind, the buffers 65536 (setting them changes
 * nothing). Values are ints unless said. */
#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21

/* send/recv flags: MSG_NOSIGNAL is taken (no signal is ever sent for a
 * closed connection: the call fails with EPIPE); MSG_DONTWAIT (53b) makes
 * this call not wait (EAGAIN), as O_NONBLOCK does; MSG_PEEK is EOPNOTSUPP. */
#define MSG_PEEK        0x2
#define MSG_DONTWAIT    0x40
#define MSG_WAITALL     0x100
#define MSG_NOSIGNAL    0x4000

#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

int socket(int family, int type, int protocol);
int bind(int fd, const struct sockaddr *address, socklen_t length);
int connect(int fd, const struct sockaddr *address, socklen_t length);
int listen(int fd, int backlog);
/* A new descriptor for the next connection; its peer's address into
 * address (if not 0; *length says its room and gets the size). */
int accept(int fd, struct sockaddr *address, socklen_t *length);
ssize_t send(int fd, const void *data, size_t size, int flags);
ssize_t recv(int fd, void *data, size_t size, int flags);
ssize_t sendto(int fd, const void *data, size_t size, int flags, const struct sockaddr *to,
               socklen_t length);
ssize_t recvfrom(int fd, void *data, size_t size, int flags, struct sockaddr *from,
                 socklen_t *length);
/* End a connection's receiving side (SHUT_RD: reads give the end), its
 * sending side (SHUT_WR: the peer reads the end; this side reads on), or
 * both; the descriptor stays until close. */
int shutdown(int fd, int how);
/* The socket's own address, and its peer's (ENOTCONN: none). */
int getsockname(int fd, struct sockaddr *address, socklen_t *length);
int getpeername(int fd, struct sockaddr *address, socklen_t *length);
int setsockopt(int fd, int level, int option, const void *value, socklen_t length);
int getsockopt(int fd, int level, int option, void *value, socklen_t *length);
```

<a id="netinet-in-h"></a>

### netinet/in.h

`user/include/netinet/in.h` (in RitterOS's tree) -- IPv4 addresses (sockaddr_in) and byte order; for apps.

Internet addresses (53a): IPv4's sockaddr_in, and (64a, 64d) IPv6's in6_addr (16 bytes as on the wire) and sockaddr_in6. Addresses and ports are in network byte order (big-endian) in them: htonl, htons. An AF_INET6 socket talks to IPv4 peers too, as ::ffff:a.b.c.d, unless IPV6_V6ONLY (level IPPROTO_IPV6) is set before it is bound.

```c
typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr;
};

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in {
    sa_family_t sin_family;     /* AF_INET */
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

struct sockaddr_in6 {
    sa_family_t sin6_family;    /* AF_INET6 */
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;     /* (not used) */
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;     /* a link-local address's card: its place in net's list + 1 */
};

/* :: and ::1 (calls: no data leaves the library). */
const struct in6_addr *__ritteros_in6addr_any(void);
const struct in6_addr *__ritteros_in6addr_loopback(void);
#define in6addr_any         (*__ritteros_in6addr_any())
#define in6addr_loopback    (*__ritteros_in6addr_loopback())
#define IN6ADDR_ANY_INIT        { { 0 } }
#define IN6ADDR_LOOPBACK_INIT   { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } }

#define IN6_IS_ADDR_UNSPECIFIED(a) \
    (((const uint32_t *)(a)->s6_addr)[0] == 0 && ((const uint32_t *)(a)->s6_addr)[1] == 0 && \
     ((const uint32_t *)(a)->s6_addr)[2] == 0 && ((const uint32_t *)(a)->s6_addr)[3] == 0)
#define IN6_IS_ADDR_LOOPBACK(a) \
    (((const uint32_t *)(a)->s6_addr)[0] == 0 && ((const uint32_t *)(a)->s6_addr)[1] == 0 && \
     ((const uint32_t *)(a)->s6_addr)[2] == 0 && ((const uint32_t *)(a)->s6_addr)[3] == htonl(1))
#define IN6_IS_ADDR_V4MAPPED(a) \
    (((const uint32_t *)(a)->s6_addr)[0] == 0 && ((const uint32_t *)(a)->s6_addr)[1] == 0 && \
     ((const uint32_t *)(a)->s6_addr)[2] == htonl(0xFFFF))
#define IN6_IS_ADDR_MULTICAST(a)    ((a)->s6_addr[0] == 0xFF)
#define IN6_IS_ADDR_LINKLOCAL(a)    ((a)->s6_addr[0] == 0xFE && ((a)->s6_addr[1] & 0xC0) == 0x80)
#define IN6_IS_ADDR_SITELOCAL(a)    ((a)->s6_addr[0] == 0xFE && ((a)->s6_addr[1] & 0xC0) == 0xC0)

#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define IPPROTO_IPV6    41
#define IPPROTO_ICMPV6  58

#define IPV6_V6ONLY     26      /* (level IPPROTO_IPV6; an int) */

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK  ((in_addr_t)0x7F000001)     /* (host order: htonl it) */
#define INADDR_BROADCAST ((in_addr_t)0xFFFFFFFF)
#define INADDR_NONE      ((in_addr_t)0xFFFFFFFF)

#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46

/* Host to network byte order and back, 32 and 16 bits. */
uint32_t htonl(uint32_t v);
uint16_t htons(uint16_t v);
uint32_t ntohl(uint32_t v);
uint16_t ntohs(uint16_t v);
```

<a id="netinet-tcp-h"></a>

### netinet/tcp.h

`user/include/netinet/tcp.h` (in RitterOS's tree) -- TCP's socket option; for apps.

TCP's socket options (53a): TCP_NODELAY at IPPROTO_TCP is kept and changes nothing (RitterOS sends a segment at once anyway).

```c
#define TCP_NODELAY     1
```

<a id="arpa-inet-h"></a>

### arpa/inet.h

`user/include/arpa/inet.h` (in RitterOS's tree) -- IPv4 addresses as text; for apps.

Internet addresses as text (53a; IPv6's since 64a).

```c
/* "a.b.c.d" (AF_INET) or an IPv6 address (AF_INET6: "fe80::1", "::ffff:
 * 10.0.2.15"; RFC 4291's forms, no "%card") into *out (network order, 4
 * or 16 bytes): 1, 0 if it is not one, -1 with EAFNOSUPPORT for another
 * family. */
int inet_pton(int family, const char *text, void *out);
/* An address (network order) as "a.b.c.d" or as IPv6's shortest form
 * (RFC 5952) into out (size at least INET_ADDRSTRLEN or INET6_ADDRSTRLEN);
 * out, or 0 with errno set (ENOSPC, EAFNOSUPPORT). */
const char *inet_ntop(int family, const void *address, char *out, socklen_t size);
/* The older forms: inet_aton 1 or 0 (also a.b.c, a.b and a, as BSD's),
 * inet_addr the address or INADDR_NONE, inet_ntoa into a buffer of its
 * own (the next call reuses it). */
int inet_aton(const char *text, struct in_addr *out);
in_addr_t inet_addr(const char *text);
char *inet_ntoa(struct in_addr address);
```

<a id="sys-un-h"></a>

### sys/un.h

`user/include/sys/un.h` (in RitterOS's tree) -- local socket addresses (sockaddr_un); for apps.

Local (Unix-domain) socket addresses (53a): a name of up to 107 bytes in RitterOS's registry of listeners (no file is made).

```c
struct sockaddr_un {
    sa_family_t sun_family;     /* AF_UNIX */
    char sun_path[108];
};
```

<a id="sys-time-h"></a>

### sys/time.h

`user/include/sys/time.h` (in RitterOS's tree) -- struct timeval, gettimeofday, utimes; for apps.

Times of day in seconds and microseconds (53a): what socket timeouts and select take.

```c
typedef long suseconds_t;

struct timeval {
    time_t tv_sec;
    suseconds_t tv_usec;        /* 0 to 999999 */
};

/* The time now since 1970 (UTC); zone must be 0. 0, or -1 with errno set. */
int gettimeofday(struct timeval *tv, void *zone);

/* (63m) A file's times in microseconds ([0] access, [1] modification; 0:
 * both now): utimensat's older form (sys/stat.h). */
int utimes(const char *path, const struct timeval times[2]);
```

<a id="sys-utsname-h"></a>

### sys/utsname.h

`user/include/sys/utsname.h` (in RitterOS's tree) -- the system's names: uname (release, commit, machine); for apps.

The system's names (68i, POSIX): uname fills a struct utsname, whose layout is the kernel's struct sys_utsname.

```c
#define _UTSNAME_LENGTH 65

struct utsname {
    char sysname[_UTSNAME_LENGTH];      /* "RitterOS" */
    char nodename[_UTSNAME_LENGTH];     /* "ritteros" (no host names yet) */
    char release[_UTSNAME_LENGTH];      /* the release, year.month: "2026.11" */
    char version[_UTSNAME_LENGTH];      /* the commit it was built from ("513e2a4", + when
                                           changed), or "unknown" */
    char machine[_UTSNAME_LENGTH];      /* "x86_64" or "aarch64" */
};

/* Fills *name: 0, or -1 with errno EFAULT. */
int uname(struct utsname *name);
```

<a id="utime-h"></a>

### utime.h

`user/include/utime.h` (in RitterOS's tree) -- a file's times in seconds (utime); for apps.

A file's times in seconds (63m): utimensat's oldest form (sys/stat.h).

```c
struct utimbuf {
    time_t actime;              /* the last access (taken, not kept) */
    time_t modtime;             /* the last modification */
};

/* times 0: both now. 0, or -1 with errno set. */
int utime(const char *path, const struct utimbuf *times);
```

<a id="sys-select-h"></a>

### sys/select.h

`user/include/sys/select.h` (in RitterOS's tree) -- select: waiting for descriptors by sets; for apps.

select (53c): waiting for descriptors by sets, over poll. fd_set holds FD_SETSIZE (256: a program's descriptors) bits. A descriptor is readable with data, the end, or an error; writable with room or an error; the exceptions set never fills (there is no out-of-band data).

```c
#define FD_SETSIZE      FD_MAX

typedef struct {
    uint64_t bits[FD_SETSIZE / 64];
} fd_set;

#define FD_ZERO(set)    do { for (int i_ = 0; i_ < FD_SETSIZE / 64; i_++) (set)->bits[i_] = 0; } while (0)
#define FD_SET(fd, set) ((set)->bits[(fd) / 64] |= (uint64_t)1 << ((fd) % 64))
#define FD_CLR(fd, set) ((set)->bits[(fd) / 64] &= ~((uint64_t)1 << ((fd) % 64)))
#define FD_ISSET(fd, set) (((set)->bits[(fd) / 64] >> ((fd) % 64)) & 1)

/* How many descriptors below count are ready (each set keeps only those),
 * waiting up to timeout (0: as long as it takes; it is left as it was);
 * -1 with errno set (EBADF: one is not open, EINVAL, EINTR). */
int select(int count, fd_set *read, fd_set *write, fd_set *except, struct timeval *timeout);
```

<a id="pthread-h"></a>

### pthread.h

`user/include/pthread.h` (in RitterOS's tree) -- POSIX threads, mutexes, condition variables, keys; for apps.

POSIX threads (54a), over threads.h's C11 threads and the kernel's futex: a waiting thread uses no processor time. Up to 256 threads a program, the main one included (EAGAIN beyond); each has a stack of 8 MiB, or the size its attributes ask (PTHREAD_STACK_MIN to 63 MiB), whose pages are made only when the thread first touches them (54d). Mutexes, condition variables and once controls are a program's own (PTHREAD_PROCESS_PRIVATE). Calls return 0 or an error number (they do not set errno).

```c
typedef unsigned long pthread_t;        /* the thread's number (thread.h) */
typedef unsigned int pthread_key_t;

typedef struct {
    int detached;                       /* PTHREAD_CREATE_* */
    size_t stack_size;                  /* the thread's stack (54d) */
} pthread_attr_t;

typedef struct {
    mtx_t m;
    int kind;                           /* PTHREAD_MUTEX_* */
} pthread_mutex_t;

typedef struct {
    int kind;
} pthread_mutexattr_t;

typedef struct {
    cnd_t c;
    int clock;                          /* the clock of timedwait's time */
} pthread_cond_t;

typedef struct {
    int clock;
} pthread_condattr_t;

typedef struct {
    int state;                          /* 0 not yet, 1 running, 2 done */
} pthread_once_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL

#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED  1       /* (not offered: ENOTSUP) */

#define PTHREAD_MUTEX_INITIALIZER { { 0, 0, 0, 0 }, PTHREAD_MUTEX_NORMAL }
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP { { 0, mtx_recursive, 0, 0 }, PTHREAD_MUTEX_RECURSIVE }
#define PTHREAD_COND_INITIALIZER  { { 0 }, CLOCK_REALTIME }
#define PTHREAD_ONCE_INIT         { 0 }

#define PTHREAD_KEYS_MAX              64
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#define PTHREAD_THREADS_MAX           256
#define PTHREAD_STACK_MIN             65536

/* Threads: start(arg) in a new thread; its result for pthread_join (a
 * detached one is nobody's to join). pthread_exit ends the calling thread
 * with value (keys' destructors run first, as when start returns); the
 * program ends when its last thread has. */
int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg);
int pthread_join(pthread_t thread, void **value);
int pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
__attribute__((noreturn)) void pthread_exit(void *value);

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_setdetachstate(pthread_attr_t *attr, int state);
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *state);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size);
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size);

/* Mutexes. NORMAL: locking it again deadlocks; RECURSIVE: its holder may
 * lock it again (free once unlocked as often); ERRORCHECK: locking it
 * again is EDEADLK, unlocking another's EPERM. trylock: EBUSY;
 * timedlock: ETIMEDOUT past an absolute CLOCK_REALTIME time. */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *until);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int kind);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *kind);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int shared);

/* Condition variables: wait unlocks m, waits for a signal and locks m
 * again (it may return without one: check in a loop); timedwait returns
 * ETIMEDOUT past an absolute time of the condition's clock
 * (CLOCK_REALTIME, or CLOCK_MONOTONIC by pthread_condattr_setclock). */
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *until);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_condattr_init(pthread_condattr_t *attr);
int pthread_condattr_destroy(pthread_condattr_t *attr);
int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock);
int pthread_condattr_getclock(const pthread_condattr_t *attr, clockid_t *clock);

/* init runs once for the control, whichever threads call. */
int pthread_once(pthread_once_t *once, void (*init)(void));

/* Keys: a value per thread (0 at first); a thread's non-zero values go to
 * the key's destructor when it ends (up to 4 rounds). */
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *value));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);

/* (56a) Handlers fork runs: prepare before it (the last registered first),
 * parent and child after it, in their process (in the order registered);
 * any may be 0. At most 32 sets (ENOMEM). */
int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));

/* The calling thread's blocked signals (sigprocmask's, as an error number). */
int pthread_sigmask(int how, const sigset_t *set, sigset_t *old);

/* (54c) Cancellation: pthread_cancel asks a thread to end. A deferred one
 * (the default) ends at its next cancellation point -- read, write, open,
 * close, poll, select, sleep, usleep, nanosleep, wait, waitpid,
 * sigsuspend, accept, connect, send, recv and the like, pthread_join,
 * pthread_cond_wait and _timedwait, pthread_testcancel -- also when it is
 * waiting in one; an asynchronous one (PTHREAD_CANCEL_ASYNCHRONOUS) at
 * once, wherever it is (only for code that holds nothing, as POSIX says).
 * It runs the thread's cleanup handlers (last pushed first), then its
 * keys' destructors; pthread_join then gives PTHREAD_CANCELED. A thread
 * with cancellation disabled (pthread_setcancelstate) keeps the request
 * until it enables it. */
#define PTHREAD_CANCELED        ((void *)-1)
#define PTHREAD_CANCEL_ENABLE   0
#define PTHREAD_CANCEL_DISABLE  1
#define PTHREAD_CANCEL_DEFERRED     0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1

int pthread_cancel(pthread_t thread);
int pthread_setcancelstate(int state, int *old);
int pthread_setcanceltype(int type, int *old);
void pthread_testcancel(void);

/* Cleanup handlers: push and pop pair up in one block (they are macros);
 * pop(1) runs the handler, pop(0) only drops it; a cancel or pthread_exit
 * runs those still pushed. */
struct __pthread_cleanup {
    void (*fn)(void *);
    void *arg;
    struct __pthread_cleanup *next;
};
void __pthread_cleanup_push(struct __pthread_cleanup *c, void (*fn)(void *), void *arg);
void __pthread_cleanup_pop(struct __pthread_cleanup *c, int execute);
#define pthread_cleanup_push(fn, arg) \
    { struct __pthread_cleanup cleanup_; __pthread_cleanup_push(&cleanup_, (fn), (arg));
#define pthread_cleanup_pop(execute) \
    __pthread_cleanup_pop(&cleanup_, (execute)); }

/* (54b) Read-write locks: many readers or one writer; a waiting writer
 * goes before new readers (no writer waits for ever). A writer locking it
 * again, or a reader taking it for writing, is EDEADLK; try: EBUSY;
 * timed: ETIMEDOUT past an absolute CLOCK_REALTIME time. */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t readers, writers;
    int reading;                        /* readers holding it */
    int writer;                         /* the writing thread's number + 1, or 0 */
    int waiting_writers;
} pthread_rwlock_t;

typedef struct {
    int shared;
} pthread_rwlockattr_t;

#define PTHREAD_RWLOCK_INITIALIZER \
    { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0 }

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *attr);
int pthread_rwlock_destroy(pthread_rwlock_t *l);
int pthread_rwlock_rdlock(pthread_rwlock_t *l);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *l);
int pthread_rwlock_timedrdlock(pthread_rwlock_t *l, const struct timespec *until);
int pthread_rwlock_wrlock(pthread_rwlock_t *l);
int pthread_rwlock_trywrlock(pthread_rwlock_t *l);
int pthread_rwlock_timedwrlock(pthread_rwlock_t *l, const struct timespec *until);
int pthread_rwlock_unlock(pthread_rwlock_t *l);
int pthread_rwlockattr_init(pthread_rwlockattr_t *attr);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr);

/* (54b) Barriers: count threads wait in pthread_barrier_wait until the
 * last arrives; one of them gets PTHREAD_BARRIER_SERIAL_THREAD, the others
 * 0, and the barrier is ready for the next round. */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    unsigned count, arrived, round;
} pthread_barrier_t;

typedef struct {
    int shared;
} pthread_barrierattr_t;

#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *attr, unsigned count);
int pthread_barrier_destroy(pthread_barrier_t *b);
int pthread_barrier_wait(pthread_barrier_t *b);
int pthread_barrierattr_init(pthread_barrierattr_t *attr);
int pthread_barrierattr_destroy(pthread_barrierattr_t *attr);

/* (54b) Spin locks: for very short holds. One processor: a waiter gives
 * the processor away instead of spinning. trylock: EBUSY. */
typedef int pthread_spinlock_t;

int pthread_spin_init(pthread_spinlock_t *s, int shared);
int pthread_spin_destroy(pthread_spinlock_t *s);
int pthread_spin_lock(pthread_spinlock_t *s);
int pthread_spin_trylock(pthread_spinlock_t *s);
int pthread_spin_unlock(pthread_spinlock_t *s);
```

<a id="sched-h"></a>

### sched.h

`user/include/sched.h` (in RitterOS's tree) -- sched_yield; for apps.

(54a) Giving the processor away.

```c
/* Let other threads and programs run first; 0. */
int sched_yield(void);
```

<a id="spawn-h"></a>

### spawn.h

`user/include/spawn.h` (in RitterOS's tree) -- posix_spawn: starting a program with file actions and attributes; for apps.

posix_spawn (55c): start a program in a new process, with file actions and attributes applied on its way. The calls return 0 or an error number (they do not set errno); *pid gets the child's number.  Since 56b as POSIX describes it, by fork and exec: the child has every descriptor of the caller not marked FD_CLOEXEC, then the file actions done in order (on any descriptor); signals the caller ignores stay ignored unless named by POSIX_SPAWN_SETSIGDEF, handled ones go back to their default; it takes the caller's blocked signals, or the attributes' with POSIX_SPAWN_SETSIGMASK. A failed action or exec is posix_spawn's error (the child it made has ended then).

```c
#define POSIX_SPAWN_RESETIDS    1   /* the child runs as the real user (giving up another: root only) */
#define POSIX_SPAWN_SETPGROUP   2   /* the child joins process group pgroup, 0: leads a new one */
#define POSIX_SPAWN_SETSIGDEF   4   /* the signals in sigdefault take their default action */
#define POSIX_SPAWN_SETSIGMASK  8   /* the child's blocked signals are sigmask */

struct __spawn_action;

typedef struct {
    int count, room;
    struct __spawn_action *actions;     /* (the library's: done in order) */
} posix_spawn_file_actions_t;

typedef struct {
    short flags;                        /* POSIX_SPAWN_* */
    pid_t pgroup;
    sigset_t sigmask, sigdefault;
} posix_spawnattr_t;

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attr, char *const argv[], char *const envp[]);
/* As posix_spawn, with a name without '/' looked up as execvp does (PATH,
 * else the system's bin and apps); a file without #! that is no binary
 * runs through sh. */
int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attr, char *const argv[], char *const envp[]);

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions);
/* In the child: fd becomes PATH opened with oflag (and mode with O_CREAT). */
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions, int fd,
                                     const char *path, int oflag, mode_t mode);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions, int fd, int newfd);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions, int fd);

int posix_spawnattr_init(posix_spawnattr_t *attr);
int posix_spawnattr_destroy(posix_spawnattr_t *attr);
int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags);
int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags);  /* EINVAL: unknown flags */
int posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup);
int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *mask);
int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *mask);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr, sigset_t *signals);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *signals);
```

<a id="regex-h"></a>

### regex.h

`user/include/regex.h` (in RitterOS's tree) -- regular expressions: regcomp, regexec (POSIX basic and extended); for apps.

Regular expressions (58a), POSIX's: basic (BRE: grep, sed) and, with REG_EXTENDED, extended syntax (ERE: grep -E, awk). regcomp compiles a pattern, regexec finds its leftmost-longest match in a string, with the groups' places in pmatch (-1 for a group that took no part); regfree gives the memory back, regerror describes an error code.  Both syntaxes: . [set] [^set] (ranges, [:alpha:] and the other classes, [=c=], [.c.]) * ^ $ \c; BRE \( \) \{m,n\} \1-\9; ERE ( ) | + ? {m,n} (and \1-\9, an extension as on BSD). In the "C" locale. Groups take, inside the longest match, the greedy leftmost-first choice: POSIX's rule for ambiguous groups ((a|ab)(c|bcd) on "abcd") may choose otherwise.

```c
typedef ssize_t regoff_t;

typedef struct {
    size_t re_nsub;                 /* the number of groups */
    void *re_program;               /* (the library's) */
} regex_t;

typedef struct {
    regoff_t rm_so, rm_eo;          /* the match's start and end, or -1 */
} regmatch_t;

/* regcomp's flags */
#define REG_EXTENDED    1           /* extended syntax (ERE) */
#define REG_ICASE       2           /* letters match either case */
#define REG_NOSUB       4           /* only whether it matches (pmatch is not filled) */
#define REG_NEWLINE     8           /* . and [^...] never match a line break; ^ and $ also
                                     * match after and before one */
/* regexec's flags */
#define REG_NOTBOL      1           /* the string's start is no line's start (^ fails) */
#define REG_NOTEOL      2           /* nor is its end a line's end ($ fails) */

/* Errors (regcomp, regexec) */
#define REG_NOMATCH     1           /* regexec: no match */
#define REG_BADPAT      2           /* an invalid pattern */
#define REG_ECOLLATE    3           /* an unknown collating element */
#define REG_ECTYPE      4           /* an unknown character class */
#define REG_EESCAPE     5           /* a \ at the end */
#define REG_ESUBREG     6           /* \N for a group that is not there */
#define REG_EBRACK      7           /* [ without ] */
#define REG_EPAREN      8           /* ( or \( without its end, or the other way */
#define REG_EBRACE      9           /* { or \{ without its end */
#define REG_BADBR       10          /* an invalid {m,n} */
#define REG_ERANGE      11          /* an invalid range (z-a) */
#define REG_ESPACE      12          /* out of memory, or too much work */
#define REG_BADRPT      13          /* * + ? or { with nothing before it */

#define RE_DUP_MAX      255         /* the largest m and n of {m,n} */

int regcomp(regex_t *re, const char *pattern, int flags);
int regexec(const regex_t *re, const char *string, size_t nmatch, regmatch_t pmatch[],
            int flags);
size_t regerror(int error, const regex_t *re, char *buffer, size_t size);
void regfree(regex_t *re);
```

<a id="netdb-h"></a>

### netdb.h

`user/include/netdb.h` (in RitterOS's tree) -- names and services: getaddrinfo, getnameinfo, gethostbyname; for apps.

Names and services (53d): getaddrinfo and the older calls. Names are looked up by the elf dnsd (DNS: A records, and since 64d AAAA records for getaddrinfo when the machine has an IPv6 address beyond the link, IPv6 then first; gethostbyname stays IPv4), "localhost" is 127.0.0.1, an address written out is taken as it is. Service names come from /system/config/services ("http = 80" lines, the same port for TCP and UDP). There is no reverse lookup: getnameinfo gives addresses as numbers.

```c
#define SERVICES_FILE   "/system/config/services"

struct addrinfo {
    int ai_flags;
    int ai_family;              /* AF_INET, AF_INET6 */
    int ai_socktype;            /* SOCK_STREAM, SOCK_DGRAM */
    int ai_protocol;            /* IPPROTO_TCP, IPPROTO_UDP */
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;         /* AI_CANONNAME: the name as given (no CNAMEs) */
    struct addrinfo *ai_next;
};

/* hints' ai_flags */
#define AI_PASSIVE      0x0001  /* no node: INADDR_ANY (for bind), not 127.0.0.1 */
#define AI_CANONNAME    0x0002
#define AI_NUMERICHOST  0x0004  /* the node must be an address: no lookup */
#define AI_V4MAPPED     0x0008  /* AF_INET6: IPv4 addresses as ::ffff:a.b.c.d when there is no IPv6 one */
#define AI_ALL          0x0010  /* with AI_V4MAPPED: the IPv4 ones too */
#define AI_ADDRCONFIG   0x0020  /* (always so: AAAA only with an IPv6 address beyond the link) */
#define AI_NUMERICSERV  0x0400  /* the service must be a number */

/* getaddrinfo's and getnameinfo's results (gai_strerror's texts) */
#define EAI_BADFLAGS    (-1)
#define EAI_NONAME      (-2)
#define EAI_AGAIN       (-3)
#define EAI_FAIL        (-4)
#define EAI_FAMILY      (-6)
#define EAI_SOCKTYPE    (-7)
#define EAI_SERVICE     (-8)
#define EAI_MEMORY      (-10)
#define EAI_SYSTEM      (-11)
#define EAI_OVERFLOW    (-12)

/* getnameinfo's flags */
#define NI_NUMERICHOST  0x01
#define NI_NUMERICSERV  0x02
#define NI_NOFQDN       0x04
#define NI_NAMEREQD     0x08    /* a name or nothing: always EAI_NONAME (no reverse lookup) */
#define NI_DGRAM        0x10
#define NI_MAXHOST      1025
#define NI_MAXSERV      32

/* The addresses for node (a name, an address, or 0) and service (a name,
 * a number, or 0), as a list to free with freeaddrinfo: one entry per
 * socket type asked (hints' ai_socktype 0: SOCK_STREAM, then SOCK_DGRAM).
 * 0, or an EAI_* (EAI_SYSTEM: errno says). */
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **result);
void freeaddrinfo(struct addrinfo *list);
const char *gai_strerror(int error);
/* An address and port as text: the address as a number, the port by its
 * name from the services file unless NI_NUMERICSERV. 0 or an EAI_*. */
int getnameinfo(const struct sockaddr *address, socklen_t length, char *host, socklen_t host_size,
                char *service, socklen_t service_size, int flags);

/* The older calls: a static entry each, which the next call reuses. */
struct hostent {
    char *h_name;
    char **h_aliases;
    int h_addrtype;             /* AF_INET */
    int h_length;               /* 4 */
    char **h_addr_list;
};
#define h_addr h_addr_list[0]

struct servent {
    char *s_name;
    char **s_aliases;
    int s_port;                 /* network byte order */
    char *s_proto;
};

struct hostent *gethostbyname(const char *name);
struct servent *getservbyname(const char *name, const char *protocol);
struct servent *getservbyport(int port, const char *protocol);

/* gethostbyname's reason for a 0 (a call underneath: libraries export no
 * data). */
int *__h_errno(void);
#define h_errno (*__h_errno())
#define HOST_NOT_FOUND  1
#define TRY_AGAIN       2
#define NO_RECOVERY     3
#define NO_DATA         4
```

<a id="termios-h"></a>

### termios.h

`user/include/termios.h` (in RitterOS's tree) -- a terminal's modes: raw input, echo, Ctrl-C (tcgetattr, tcsetattr); for apps.

A terminal's modes (52e): POSIX's termios. RitterOS acts on ICANON (off: bytes as typed, no line editing and no echo; VMIN and VTIME then say how a read waits), ECHO (lines only), ISIG (Ctrl-C a signal, else a byte 3) and ICRNL (without ICANON: Enter read as \n); the other flags are kept as set but change nothing. A program that changed the modes leaves the terminal as it found it when it ends.

```c
typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS            TERMIOS_NCCS

struct termios {
    tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
    cc_t c_cc[NCCS];
    speed_t c_ispeed, c_ospeed;
};

/* c_lflag */
#define ISIG            TERMIOS_ISIG
#define ICANON          TERMIOS_ICANON
#define ECHO            TERMIOS_ECHO
#define ECHOE           TERMIOS_ECHOE
#define ECHOK           TERMIOS_ECHOK
#define ECHONL          TERMIOS_ECHONL
#define NOFLSH          TERMIOS_NOFLSH
#define TOSTOP          TERMIOS_TOSTOP
#define IEXTEN          TERMIOS_IEXTEN
/* c_iflag */
#define BRKINT          TERMIOS_BRKINT
#define INPCK           TERMIOS_INPCK
#define ISTRIP          TERMIOS_ISTRIP
#define INLCR           TERMIOS_INLCR
#define IGNCR           TERMIOS_IGNCR
#define ICRNL           TERMIOS_ICRNL
#define IXON            TERMIOS_IXON
/* c_oflag */
#define OPOST           TERMIOS_OPOST
#define ONLCR           TERMIOS_ONLCR
/* c_cflag */
#define CSIZE           TERMIOS_CSIZE
#define CS8             TERMIOS_CS8
#define CREAD           TERMIOS_CREAD
#define PARENB          TERMIOS_PARENB
/* c_cc */
#define VINTR           TERMIOS_VINTR
#define VQUIT           TERMIOS_VQUIT
#define VERASE          TERMIOS_VERASE
#define VKILL           TERMIOS_VKILL
#define VEOF            TERMIOS_VEOF
#define VTIME           TERMIOS_VTIME
#define VMIN            TERMIOS_VMIN
#define VSTART          TERMIOS_VSTART
#define VSTOP           TERMIOS_VSTOP
#define VSUSP           TERMIOS_VSUSP

/* tcsetattr's when (TCSADRAIN as TCSANOW: output never waits) */
#define TCSANOW         0
#define TCSADRAIN       1
#define TCSAFLUSH       2
/* tcflush's which (only typed input waits to be dropped) */
#define TCIFLUSH        0
#define TCOFLUSH        1
#define TCIOFLUSH       2

/* Speeds are plain numbers (BSD's); they change nothing. */
#define B0              0
#define B9600           9600
#define B19200          19200
#define B38400          38400
#define B57600          57600
#define B115200         115200

/* 0, or -1 with errno set (ENOTTY: not a terminal, EPERM: changing a
 * terminal whose foreground job is another). */
int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int when, const struct termios *t);
int tcflush(int fd, int which);
int tcdrain(int fd);
/* Raw: no ICANON, ECHO, ISIG or input translation, 8 bits, one byte a read. */
void cfmakeraw(struct termios *t);
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
int cfsetispeed(struct termios *t, speed_t speed);
int cfsetospeed(struct termios *t, speed_t speed);
```

<a id="sys-wait-h"></a>

### sys/wait.h

`user/include/sys/wait.h` (in RitterOS's tree) -- how a child ended; for apps.

Waiting for child processes, and what their status means.

```c
#define WNOHANG                 1       /* return 0 at once if none has ended */

#define WIFEXITED(status)       (((status) & 0x7F) == 0)
#define WEXITSTATUS(status)     (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status)     (((status) & 0x7F) != 0)
#define WTERMSIG(status)        ((status) & 0x7F)

/* Wait for any child / for child pid (-1: any) to end; its ID, or -1 with
 * errno set (ECHILD: no such child). flags: 0 or WNOHANG. */
pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int flags);
```

<a id="sys-mman-h"></a>

### sys/mman.h

`user/include/sys/mman.h` (in RitterOS's tree) -- memory mappings: anonymous memory, files shared between programs; for apps.

Memory mappings (step 22a): mmap and munmap as on Unix, a subset. Anonymous memory (MAP_ANONYMOUS: zeroed, the program's own) and files: a read-only mapping of a RitterFS file shares one copy of its pages with every program that maps the same version of the file (a file rewritten later is a new version; old mappings keep the old pages), a writable MAP_PRIVATE mapping is the program's own copy -- since 22e made a page at a time, at the page's first write; until then it shares the file's pages. Since 22j a MAP_SHARED mapping of a RitterFS file open for writing may be writable: every program mapping that file shares the same pages and sees the others' writes at once; msync (and munmap, and the program's end) writes them to the file. Other file systems: ENOTSUP; a descriptor open for reading only: EACCES. read() sees the writes once they are written back. Anonymous memory is made when first touched, so a large mapping costs nothing until used; a file's pages are read when it is mapped. A mapping may reach past its file's end; touching a page there ends the program with SIGBUS. Never writable and executable at once (ENOTSUP).

```c
#define PROT_NONE       0               /* a reservation: nothing mapped, every access faults */
#define PROT_READ       MMAP_READ
#define PROT_WRITE      MMAP_WRITE      /* (reading too) */
#define PROT_EXEC       MMAP_EXEC

#define MAP_SHARED      MMAP_SHARED
#define MAP_PRIVATE     MMAP_PRIVATE
#define MAP_FIXED       MMAP_FIXED      /* exactly at addr (a page boundary in the mmap window),
                                         * replacing the mappings there */
#define MAP_ANONYMOUS   MMAP_ANONYMOUS  /* zeroed memory: fd and offset are not used */
#define MAP_ANON        MAP_ANONYMOUS

#define MAP_FAILED      ((void *)-1)

#define MS_ASYNC        MSYNC_ASYNC     /* msync flags (as on Unix; all write back at once) */
#define MS_INVALIDATE   MSYNC_INVALIDATE
#define MS_SYNC         MSYNC_SYNC

/* Map length bytes (rounded up to whole 4096-byte pages) where the system
 * chooses, or exactly at addr with MAP_FIXED; a file from offset (a
 * multiple of 4096). The address, or MAP_FAILED with errno set. */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);

/* Remove the mappings in [addr, addr + length), or parts of them (addr on a
 * page boundary); parts of the range with nothing mapped are fine. 0, or -1
 * with errno set. */
int munmap(void *addr, size_t length);

/* Give the pages in [addr, addr + length) (addr on a page boundary, all of
 * it mapped: else ENOMEM) the access prot; they keep their contents, also
 * through PROT_NONE. 0, or -1 with errno set: ENOTSUP for writable and
 * executable at once, for writing a shared file mapping, and for giving
 * access to a file mapping made PROT_NONE (step 22f). */
int mprotect(void *addr, size_t length, int prot);

/* Write what was written through the writable shared mappings in
 * [addr, addr + length) (addr on a page boundary) to their files (22j). 0, or
 * -1 with errno set (EIO if a file could not be written). */
int msync(void *addr, size_t length, int flags);
```

<a id="dlfcn-h"></a>

### dlfcn.h

`user/include/dlfcn.h` (in RitterOS's tree) -- loading shared libraries while running (dlopen, dlsym); for apps.

Loading shared libraries while a program runs (step 22k), as on Unix: dlopen maps a library (a name in /system/<arch>/lib, or a path) and the libraries it needs, links them and runs their initializers; dlsym finds a function or variable in it (per-thread variables: the calling thread's); dlclose gives it back -- it stays loaded until the program ends, since other code may already use it; dlerror says what the last failure in this thread was. The dynamic loader ld.so does the work, so a static program has none of it: dlopen fails there.

```c
#define RTLD_LAZY       1               /* (all are resolved at once, either way) */
#define RTLD_NOW        2
#define RTLD_LOCAL      0               /* (every library's symbols are visible to all) */
#define RTLD_GLOBAL     0x100
#define RTLD_DEFAULT    ((void *)0)     /* dlsym: everything loaded, the program first */

/* The library's handle, or 0 (dlerror says why). dlopen(0, ...) is the
 * program itself. A library already loaded gives the same handle. */
void *dlopen(const char *file, int mode);

/* The address of `name` in the library (then in everything loaded), or 0. */
void *dlsym(void *handle, const char *name);

/* 0, or -1 for something dlopen did not give. */
int dlclose(void *handle);

/* The last failure's message in this thread, once; 0 if none since. */
char *dlerror(void);
```

<a id="dirent-h"></a>

### dirent.h

`user/include/dirent.h` (in RitterOS's tree) -- reading directories; for apps.

Reading directories: opendir, then readdir until it returns 0.

```c
#define DT_BLK  6
#define DT_DIR  4
#define DT_REG  8
#define DT_LNK  10              /* (57a) a symbolic link */

struct dirent {
    uint64_t d_ino;             /* RitterFS object ID */
    unsigned char d_type;       /* DT_DIR, DT_REG, DT_BLK or DT_LNK */
    char d_name[256];
};

typedef struct DIR DIR;         /* (as many open as descriptors: 63k) */

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);       /* 0 at the end (or on an error: errno) */
int closedir(DIR *dir);
```

<a id="signal-h"></a>

### signal.h

`user/include/signal.h` (in RitterOS's tree) -- signals; for apps.

Signals. A signal ends the process it is sent to, unless the process ignores it (SIG_IGN) or handles it (since 22r): a handler is a function the kernel calls on the program's way back from the kernel (a system call, an interrupt, a fault), on its own stack; when it returns, the program goes on where it was. A waiting system call such as read that a signal interrupts returns -1 with errno EINTR (it is not restarted), so a shell notices Ctrl-C (SIGINT) at its prompt. Faults (SIGSEGV, SIGBUS, SIGILL, SIGFPE) go to a handler too; one that returns runs the faulting instruction again, so such a handler usually ends the program or jumps away with longjmp. SIGKILL can be neither handled nor ignored.

```c
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

/* What a handler may change and the program read safely (22r). */
typedef int sig_atomic_t;

int kill(pid_t pid, int signal);
/* (22r) The handler (or SIG_DFL, SIG_IGN) for a signal, as glibc's and the
 * BSDs' signal: it stays set, and its signal waits while it runs. The old
 * one, or SIG_ERR. */
sighandler_t signal(int signal, sighandler_t action);
/* (22r) Send a signal to this program: its handler has run when raise
 * returns. */
int raise(int signal);

/* (22r) Sets of signals (bit n: signal n), and sigaction: a handler with
 * the signals it holds back while it runs (sa_mask, besides its own unless
 * SA_NODEFER) and SA_RESETHAND (back to SIG_DFL once called). SA_RESTART is
 * taken and does nothing (interrupted calls are never restarted here).
 * There is no SA_SIGINFO. */
typedef unsigned long sigset_t;
#define SA_RESETHAND 0x80000000
#define SA_NODEFER   0x40000000
#define SA_RESTART   0x10000000
struct sigaction {
    sighandler_t sa_handler;
    sigset_t sa_mask;
    int sa_flags;
};
int sigaction(int signal, const struct sigaction *action, struct sigaction *old);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signal);
int sigdelset(sigset_t *set, int signal);
int sigismember(const sigset_t *set, int signal);

/* (52f) The calling thread's blocked signals: SIG_BLOCK adds set, SIG_UNBLOCK
 * takes it away, SIG_SETMASK makes it the set (set 0: only ask; old, if
 * not 0, gets the set before). A blocked signal waits until a thread lets
 * it through, then is handled or ends the program; it does not interrupt
 * a blocking thread's waiting call. SIGKILL is never blocked. New threads
 * start with their creator's set, and so do programs started by spawn
 * (52i, as posix_spawn); signals waiting are not passed on.
 * 0, or -1 with errno set (EINVAL: how). */
#define SIG_BLOCK       SIGMASK_BLOCK
#define SIG_UNBLOCK     SIGMASK_UNBLOCK
#define SIG_SETMASK     SIGMASK_SET
int sigprocmask(int how, const sigset_t *set, sigset_t *old);

/* (52i) The signals waiting that the calling thread blocks; 0. */
int sigpending(sigset_t *set);
/* (52i) Block mask instead until a signal it lets through comes (ignored
 * ones do not count): its handler runs, then the set from before is back
 * and sigsuspend returns -1 with errno EINTR. Or the program ends. */
int sigsuspend(const sigset_t *mask);
```

<a id="pwd-h"></a>

### pwd.h

`user/include/pwd.h` (in RitterOS's tree) -- users; for apps.

Users: /system/config/users holds one line per user, name:uid:gid:home (readable by everyone; passwords are elsewhere, see ritteros.h).

```c
#define USERS_FILE "/system/config/users"

struct passwd {
    char *pw_name;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_dir;               /* the home directory */
};

/* The user with that name or ID, or 0 (errno set only on a read error).
 * The result lives until the next call. */
struct passwd *getpwnam(const char *name);
struct passwd *getpwuid(uid_t uid);
```

<a id="thread-h"></a>

### thread.h

`user/include/thread.h` (in RitterOS's tree) -- threads; for apps.

Threads: several flows of one program that share its memory and files, each with its own stack (8 MiB, its pages made when first touched: 54d) and registers, the floating point ones included. Up to 256 per program; number 0 is the one that runs main. exit() from any thread ends the whole program; when the last thread ends with thread_exit, the program ends with exit code 0.

```c
/* Start fn(arg) in a new thread; its number, or -1 (errno: EAGAIN when all
 * 256 numbers are taken). */
int thread_create(int (*fn)(void *arg), void *arg);

/* Wait until thread `number` has ended; its result (the value fn returned
 * or thread_exit was given) goes to *result. -1 (errno ESRCH) if there is
 * no such thread or it was joined already. */
int thread_join(int number, int *result);

/* End the calling thread with a result. */
__attribute__((noreturn)) void thread_exit(int result);

/* The calling thread's number. */
int thread_self(void);
```

<a id="ritteros-h"></a>

### ritteros.h

`user/include/ritteros.h` (in RitterOS's tree) -- RitterOS's own calls; for apps; parts are the system's own.

RitterOS-specific parts of the programs' library. Programs normally use the standard headers (stdio.h, string.h, unistd.h, fcntl.h, stdlib.h, errno.h); system calls themselves are listed in common/syscalls.h.

```c
/* A raw system call: number and up to six arguments; returns the kernel's
 * result (negative: -ERR_*). user/arch/<arch>/start.S */
int64_t syscall6(int64_t number, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                 int64_t f);

/* Per-thread data (22i): `__thread` variables, a copy for each thread. The
 * system sets it up -- ld.so for a dynamic program, the start code for a
 * static one, thread_create for each new thread -- so programs call none of
 * these. A module (the program or a library) with per-thread data: its
 * initial data (image, filesz bytes; the rest of memsz zeroed), alignment,
 * and its block's offset from the thread pointer. */
struct ritteros_tls_module {
    const void *image;
    size_t filesz, memsz, align;
    long tpoff;
};
int ritteros_set_thread_pointer(void *tp);
int __ritteros_tls_setup(const struct ritteros_tls_module *modules, int count);
/* A module dlopen loaded (22k): into the reserve of every thread's area; 0, or
 * -1 if it does not fit. */
int __ritteros_tls_add(const struct ritteros_tls_module *module);
/* ld.so's dlopen, dlsym, dlclose (22k), handed over at the start: dlfcn.h's
 * functions call them, one at a time. open and symbol put a message into
 * error when they fail (0). */
struct ritteros_dl_loader {
    void *(*open)(const char *file, int flags, char *error, size_t size);
    void *(*symbol)(void *handle, const char *name, char *error, size_t size);
    int (*close)(void *handle);
};
int __ritteros_dl_setup(const struct ritteros_dl_loader *loader);
void __ritteros_start(long *sp);
void *__tls_get_addr(void *index);

/* Uptime and memory (struct sys_info); 0, or -1 with errno set. */
int ritteros_sysinfo(struct sys_info *info);

/* (35a) Swap: SWAP_STATUS fills status; SWAP_ON and SWAP_OFF (root's) turn
 * it on with /system/swap or off. 0, or -1 with errno set. */
int ritteros_swap(int op, struct sys_swap *status);
/* (37b-1) ACPI table number index (the DSDT, the FACS, then the XSDT's
 * list), at most size bytes copied: its full length; -1 with errno ENOENT
 * past the last, EPERM for anyone but root. */
int64_t ritteros_power_table(int index, void *buffer, size_t size);
/* (37b-2) The AML namespace's objects, one a line ("\\_SB_.PCI0 Device"),
 * at most size bytes with a 0: the full length; -1 with errno EPERM for
 * anyone but root. */
int64_t ritteros_power_names(char *buffer, size_t size);
/* (37b-3) The AML object at path ("\\_SB.PCI0._CRS") evaluated, its value as
 * text lines ("\\_SB_.PCI0._CRS: Buffer 238 bytes", ...), at most size bytes
 * with a 0: the full length; -1 with errno ENOENT for no such object, EIO
 * when the evaluation failed, EPERM for anyone but root. */
int64_t ritteros_power_eval(const char *path, char *buffer, size_t size);

/* (37g-4) Sleep (ACPI S3) for seconds (1 to a day), woken by the RTC's
 * alarm; (37g-7) 0: until the power button (or the firmware's own wake,
 * a laptop's lid) wakes it: 0 once awake again; -1 with errno EINVAL for
 * more than a day, EIO
 * when the machine did not sleep (the kernel's log says why), EPERM for
 * anyone but root. */
int ritteros_power_sleep(unsigned seconds);

/* Switch the machine off or restart it (POWER_OFF, POWER_REBOOT); returns
 * only if that did not work. */
int ritteros_power(int how);

/* A kernel report (REPORT_PROCESSES, _THREADS, _PCI, _USB) as text in
 * buffer (zero-terminated, cut short if needed); returns its full length,
 * or -1 with errno set. */
int64_t ritteros_report(int which, char *buffer, size_t size);

/* RitterFS snapshots (SNAPSHOT_CREATE, _DELETE with a name; SNAPSHOT_LIST
 * into buffer); the kernel's result, or -1 with errno set. */
int64_t ritteros_snapshot(int op, const char *name, struct sys_snapshot *buffer, size_t size);

/* spawn with the child's standard input, output and error taken from the
 * caller's descriptors fds[0..2] (-1: the caller's own 0, 1 or 2); flags
 * as SYS_SPAWN's (SPAWN_NEW_GROUP). The child's ID, or -1 with errno set. */
int ritteros_spawn(const char *path, char *const argv[], int flags, const int fds[3]);
/* (22t) The same with an environment of the caller's choice (envp: "NAME=value"
 * strings ended by a null pointer; ritteros_spawn passes the caller's own). */
int ritteros_spawn_env(const char *path, char *const argv[], char *const envp[], int flags,
                       const int fds[3]);

/* The same with the child running as user uid, group gid (root only:
 * login starts a user's shell this way). */
int ritteros_spawn_as(const char *path, char *const argv[], int flags, const int fds[3],
                      uid_t uid, gid_t gid);

/* An instant copy of a file or a directory tree: the copy shares the data
 * blocks until one side changes; 0, or -1 with errno set. */
int ritteros_copy(const char *from, const char *to);

/* Attributes of path (ATTR_GET, _SET, _REMOVE, _LIST); the kernel's result,
 * or -1 with errno set. */
int64_t ritteros_attr(int op, const char *path, const char *key, void *buffer, size_t size);

/* Hashes (user/lib/crypto.c): SHA-256, HMAC-SHA256, and PBKDF2-HMAC-SHA256
 * for passwords (salt up to PBKDF2_SALT_MAX bytes). */
#define SHA256_SIZE 32
#define PBKDF2_SALT_MAX 64
struct sha256 {
    uint32_t h[8];
    uint64_t length;
    uint8_t block[64];
    size_t used;
};
void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t n);
void sha256_final(struct sha256 *s, uint8_t out[SHA256_SIZE]);
void sha256(const void *data, size_t n, uint8_t out[SHA256_SIZE]);
void hmac_sha256(const void *key, size_t key_length, const void *data, size_t n,
                 uint8_t out[SHA256_SIZE]);
void pbkdf2_sha256(const void *password, size_t password_length, const uint8_t *salt,
                   size_t salt_length, uint32_t iterations, uint8_t *out, size_t out_length);

/* CRC-32 (IEEE 802.3, as GPT and zlib use it) and CRC-32C (Castagnoli,
 * as RitterFS uses it); common/crc32.c. */
uint32_t crc32(const void *data, size_t size);
uint32_t crc32c(const void *data, size_t size);

/* The time: nanoseconds since 1970-01-01 UTC; 0 if the machine has no
 * clock. */
uint64_t ritteros_time(void);

/* Random bytes from the kernel (at most 256 per call); n, or -1. */
int64_t ritteros_random(void *buffer, size_t n);

/* Whether the terminal shows what is typed (off for passwords; the kernel
 * switches it on again when the program ends); 0, or -1. */
int ritteros_echo(bool on);

/* Raw terminal input for full-screen programs (see SYS_TERMINAL): read()
 * returns bytes as typed, at least one, without echo; the kernel switches
 * back to lines when the program ends. 0, or -1. */
int ritteros_terminal_raw(bool on);
/* Raw input for a program that edits its own line (the shell): the console
 * shows that line again after a message. ritteros_terminal_raw(false) ends it. */
int ritteros_terminal_line_editor(void);

/* Mount a RitterFS partition (by its name in /dev) at path, or with path
 * 0 at /volumes/<label>; the path used goes into buffer. Unmount by the
 * mount's path or the device's name. Root only; 0, or -1 (EBUSY: mounted
 * already, or in use). */
int ritteros_mount(const char *device, const char *path, char *buffer, size_t size);
int ritteros_unmount(const char *path_or_device);

/* Copy from (a file, or a directory with everything in it) to a new path
 * byte by byte, for another file system (where ritteros_copy says EXDEV);
 * remove a file or a directory with everything in it. 0, or -1 with errno
 * (and the path that failed in failed). */
int ritteros_copy_bytes(const char *from, const char *to, char *failed, size_t size);
int ritteros_remove_tree(const char *path);

/* A volume is mounted at path (its file system differs from its parent's;
 * rm and mv refuse to empty or move one). */
bool ritteros_mount_point(const char *path);

/* Entry number index of the device list (disks, each followed by its
 * partitions, as /dev lists them): 1, or 0 past the last. */
int ritteros_device(int index, struct sys_device *out);

/* (68j) The RitterFS mounted on partition (its name in /dev) takes the
 * room its partition grew by (disks grow): its block count of 4 KiB now
 * (unchanged without new room). Root only; -1 with errno (EINVAL: no
 * RitterFS mounted there). */
int64_t ritteros_grow(const char *partition);

/* The terminal's text area in characters (80x24 without a screen); 0. */
int ritteros_terminal_size(int *rows, int *cols);

/* A pseudo-terminal (sshd): fds[0] its master side (read: what is shown,
 * write: typing), fds[1] its slave side, to give to a new session's
 * programs (spawn with SPAWN_NEW_SESSION: it becomes their terminal);
 * its size in characters. 0, or -1. */
int ritteros_pty_open(int fds[2]);
int ritteros_pty_size(int fd, int rows, int cols);

/* A pipe: fds[0] the read end, fds[1] the write end. A read gives 0 once
 * every write end is closed; a write fails (errno EPIPE) once every read
 * end is closed. Binary-transparent. 0, or -1. */
int ritteros_pipe(int fds[2]);

/* The network (IPv4, host byte order). ip_parse reads "10.0.2.15" or
 * "10.0.2.15/24" (prefix may be 0: then no "/N" is allowed) and returns
 * whether it was one; ip_format writes one. ritteros_netset sets a card's
 * settings (root only); ritteros_ping sends one ICMP echo and returns the
 * round trip in microseconds, or -1 (errno: ETIMEDOUT, ENETUNREACH,
 * EHOSTUNREACH, EINTR). (user/lib/net.c) */
bool ip_parse(const char *text, uint32_t *ip, uint32_t *prefix);
void ip_format(uint32_t ip, char *out, size_t size);
int ritteros_netset(const struct sys_netset *settings);
/* A card's IPv6 settings (64b; root only): NETSET6_AUTO (from the
 * network's routers), NETSET6_OFF, NETSET6_FIXED (address and prefix, a
 * gateway); a DNS server instead of the router's. */
int ritteros_netset6(const struct sys_netset6 *settings);
/* A card's MAC address, link and settings (name filled in); 0 or -1. */
int ritteros_netinfo(struct sys_netinfo *info);
/* (64d) A card's IPv6 side: its addresses, router, DNS server and MTU, by
 * name or (an empty name) by its place in the list, as ritteros_netinfo. */
int ritteros_netinfo6(struct sys_netinfo6 *info);
/* (64g) A DHCPv6 lease for a card (root; the elf dhclient): its address and
 * DNS server, or NETLEASE6_DROP to end it. */
int ritteros_netlease6(const struct sys_netlease6 *lease);
/* The same for the card at place index in the kernel's list (0 first, the
 * name filled in): loop until -1 with errno ENODEV to see every card,
 * whatever its kind or name. 0 or -1. */
int ritteros_netcard(int index, struct sys_netinfo *info);
/* A wireless card's radio and link, for the elf wifid (root only; 24c):
 * op is a WLAN_* constant, card its name ("wlan0"), buffer/length and value
 * as that op wants (common/syscalls.h). The op's result, or -1 with errno. */
int64_t ritteros_wlan(int op, const char *card, void *buffer, size_t length, uint64_t value);
/* A UDP socket of one card only, before ritteros_bind (root only): for a
 * DHCP client, which sends broadcasts out of the card before it has an
 * address. 0 or -1. */
int ritteros_bindcard(int fd, const char *card);
/* Forwarding between cards: FORWARD_ASK, FORWARD_ON or FORWARD_OFF (root
 * only); 1 (on), 0 (off) or -1. */
int ritteros_forward(int mode);
/* Masquerade (source NAT, 49i): MASQ_RESET clears the private cards,
 * MASQ_ADD marks the card named `card` private (root). */
int ritteros_masquerade(int op, const char *card);
/* Virtual network cards (38a, root only): TAP_ADD makes a tapN whose
 * /dev/tapN is user `value`'s (and root's), giving N; TAP_REMOVE removes
 * tap number `value` (EBUSY while a program has it open). -1 on errors. */
int ritteros_tap(int op, unsigned value);
/* Bridges (38b, root only): BRIDGE_ADD makes the card named `card` a port
 * of bridge brN (bridge -1: of a new one), giving N; BRIDGE_REMOVE takes
 * it out (card 0: the whole bridge goes). -1 on errors (ENOTSUP: a card
 * that cannot take other machines' frames, a Wi-Fi card). */
int ritteros_bridge(int op, int bridge, const char *card);
/* The default route's cards (50g): DEFAULT_ROUTE_RESET clears the order,
 * DEFAULT_ROUTE_ADD puts the card named `card` (ethN, wlanN, tapN or brN,
 * there or not) next (root). */
int ritteros_default_route(int op, const char *card);
int64_t ritteros_ping(uint32_t ip, uint16_t id, uint16_t seq, size_t size, uint32_t timeout_ms);
/* The same to any address (64a): IPv6 (a link-local or multicast one with
 * its card in `scope`: EINVAL without), or IPv4 as ::ffff:a.b.c.d. */
int64_t ritteros_ping6(const struct sys_inaddr *to, uint16_t id, uint16_t seq, size_t size,
                       uint32_t timeout_ms);

/* A name's IPv4 address: an address written out is taken as it is,
 * localhost is 127.0.0.1, any other name is asked of the elf dnsd (DNS,
 * on this machine's port 53). 0, or -1 (errno: ENONAME, ETIMEDOUT,
 * ENETUNREACH: no DNS server known). */
int ritteros_resolve(const char *name, uint32_t *ip);
/* (64d) A name's IPv6 address (16 bytes): one written out is taken as it is
 * (no %card), localhost is ::1, any other name is asked of dnsd for its
 * AAAA record. 0, or -1 with errno as ritteros_resolve's. */
int ritteros_resolve6(const char *name, uint8_t address[16]);

/* DNS messages (RFC 1035; user/lib/dns.c), for dnsd and the resolver.
 * dns_query: a question for name's A record (its length, 0 if the name
 * cannot be one); dns_answer: the first A record of an answer, 0 or an
 * errno (ENONAME, ENETUNREACH: refused, ETIMEDOUT: server failure,
 * EINVAL); dns_question: the name a simple question (one, type A) asks
 * for; dns_reply: the answer to a question, one A record; dns_failure:
 * an answer without records, with rcode (2: server failure, 5: refused). */
#define DNS_NAME_MAX 253
size_t dns_query(const char *name, uint16_t id, uint8_t *out, size_t size);
int dns_answer(const uint8_t *m, size_t length, uint32_t *address, uint32_t *ttl);
bool dns_question(const uint8_t *m, size_t length, char *name, size_t size);
size_t dns_reply(const uint8_t *query, size_t length, uint32_t address, uint32_t ttl,
                 uint8_t *out, size_t size);
size_t dns_failure(const uint8_t *query, size_t length, int rcode, uint8_t *out, size_t size);
/* (64d) Any record type: dns_query_type asks for `type` (1 A, 28 AAAA);
 * dns_answer_aaaa takes the first AAAA record (errors as dns_answer);
 * dns_question_type gives a simple question's name and type; dns_reply_aaaa
 * answers with one AAAA record. */
size_t dns_query_type(const char *name, uint16_t id, uint16_t type, uint8_t *out, size_t size);
int dns_answer_aaaa(const uint8_t *m, size_t length, uint8_t address[16], uint32_t *ttl);
bool dns_question_type(const uint8_t *m, size_t length, char *name, size_t size, uint16_t *type);
size_t dns_reply_aaaa(const uint8_t *query, size_t length, const uint8_t address[16], uint32_t ttl,
                      uint8_t *out, size_t size);

/* Sockets (SOCKET_UDP, SOCKET_TCP): descriptors that read() and write()
 * work on once connected. bind returns the port used; recvfrom stores the
 * sender's address and port (either may be 0) and waits at most
 * timeout_ms (0: as long as it takes); accept the peer's. -1 and errno on
 * errors. */
int ritteros_socket(int type);
int ritteros_bind(int fd, uint16_t port);
int ritteros_connect(int fd, uint32_t ip, uint16_t port);
ssize_t ritteros_sendto(int fd, const void *data, size_t size, uint32_t ip, uint16_t port);
ssize_t ritteros_recvfrom(int fd, void *data, size_t size, uint32_t *ip, uint16_t *port,
                          uint32_t timeout_ms);
int ritteros_listen(int fd, int backlog);
int ritteros_accept(int fd, uint32_t *ip, uint16_t *port);
/* Local (Unix-domain) stream sockets: two programs on one machine talk over
 * a named connection (SOCKET_LOCAL). The name is a path-like string.
 * bind_local/connect_local act on a socket made with ritteros_socket;
 * local_listen and local_connect make the socket too (a listening or a
 * connected descriptor). accept on a local listener returns a new
 * descriptor (its address is 0). read/write/close as usual. */
int ritteros_bind_local(int fd, const char *path);
int ritteros_connect_local(int fd, const char *path);
int ritteros_local_listen(const char *path, int backlog);
int ritteros_local_connect(const char *path);
/* The user at the other end of a connected local socket: the client's user
 * for a server, the user who bound the name for a client (a name says
 * nothing about whose it is). The user ID, or -1 with errno (ENOTCONN). */
int ritteros_peer_uid(int fd);
/* Shared buffers (SYS_SHARE): create one (a descriptor), map it into this
 * program (its address, or 0 on error), ask its size, and pass it to / take
 * it from another program over a local socket. The buffer lives until every
 * descriptor that holds it is closed. */
int ritteros_share_create(size_t size);
void *ritteros_share_map(int fd);
long ritteros_share_size(int fd);
int ritteros_share_send(int socket_fd, int buffer_fd);
int ritteros_share_recv(int socket_fd);
/* The screen (SYS_DISPLAY), for the window server windowd: take over the
 * screen (parks the text console, root only), give it back, ask its size and
 * format, and copy a rectangle of native pixels to it. */
int ritteros_display_acquire(void);
int ritteros_display_release(void);
int ritteros_display_info(struct sys_display *info);
int ritteros_display_flush(const void *pixels, uint32_t stride, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h);
int ritteros_display_event(struct sys_input_event *event, uint32_t timeout_ms);
/* (65e-34) The screen's mode and refresh, the monitor (its EDID) and the
 * modes the firmware can set; anyone may ask. */
int ritteros_display_modes(struct sys_display_modes *modes);
/* (66b-2) Show output's monitor at width x height (refresh in millihertz,
 * 0: the monitor's usual rate for that size), a mode it lists; root or the
 * user at the machine's screen, not while a program owns the screen
 * (EBUSY). said (said_size bytes, may be 0) gets what was set or why not. */
int ritteros_display_set_mode(const char *output, uint32_t width, uint32_t height,
                              uint32_t refresh_mhz, char *said, size_t said_size);
/* (66c-3) Wait (at most timeout_ms; 0: a second) for the screen's next
 * vertical blank: 1 with its frame number in *frame (may be 0), 0 if none
 * came, -1 (ENODEV) where no graphics driver tells it. */
int ritteros_display_vblank(uint64_t *frame, uint32_t timeout_ms);
/* (66d-1) Show screen buffer `buffer` (0: the console's) from the next
 * vertical blank: 1 once it shows (its frame in *frame, may be 0), 0 if it
 * did not in time, -1 with errno (ENODEV: no driver flips; EPERM: not the
 * screen's owner; EINVAL: no such buffer). */
int ritteros_display_flip(int buffer, uint64_t *frame);
/* (66d-2) The screen buffers mapped into the screen's owner (write only:
 * reading them is very slow), their count, size and stride; the same
 * places after a mode change (ask again for the size). -1 with errno
 * (ENODEV: no driver has them; EPERM: not the screen's owner). */
int ritteros_display_buffers(struct sys_display_buffers *buffers);
#define NETWORK_CONFIG "/system/config/network"
/* The firewall (user/lib/guard.c): its rules as text, one per line of
 * GUARD_FILE (read top to bottom, "#" starts a comment):
 *   in = drop | allow        what comes in when no rule matches
 *   out = drop | allow       what goes out when none does
 *   forward = drop | allow   what is forwarded between cards when none does
 *   ACTION DIRECTION [PROTOCOL] [PORT | PORT-PORT] [from ADDRESS[/N]] [to ADDRESS[/N]] [on ethN]
 *   limit DIRECTION ... N/minute
 * ACTION allow or drop (limit: allow at most N new connections a minute,
 * its rate the last word); DIRECTION in, out or forward; PROTOCOL tcp,
 * udp or icmp (a port needs tcp or udp: coming in the local one, going out
 * or forwarded the remote one). A forward rule names its cards with
 * "in ethN" (incoming) and "out ethN" (outgoing) instead of "on ethN".
 * guard_parse adds one line to g (rules, defaults): 0, or
 * -1 with the reason in why; guard_read a whole file (missing defaults:
 * in = drop, out = allow, forward = drop): 0, or -1 with "line N: reason" in why (errno:
 * the file could not be read); guard_format writes a rule back in words.
 * ritteros_guard: the system call SYS_GUARD (GUARD_LOAD, GUARD_OFF,
 * GUARD_STATUS). */
#define GUARD_FILE "/system/config/guard"
int guard_parse(const char *line, struct sys_guard *g, char *why, size_t size);
int guard_read(const char *path, struct sys_guard *g, char *why, size_t size);
void guard_format(const struct sys_guard_rule *rule, char *out, size_t size);
int ritteros_guard(int op, struct sys_guard *g);
/* (64e) Both families: guard_read6 reads the file into g (IPv4's table:
 * the rules without an address and those with an IPv4 one) and g6
 * (IPv6's: without one or with an IPv6 one, each with its number in the
 * file); guard_format6 writes an IPv6 rule back in words; ritteros_guard6
 * is SYS_GUARD on IPv6's table (GUARD_SET6, GUARD_GET6). guard_parse
 * checks an IPv6 rule but adds it nowhere. */
int guard_read6(const char *path, struct sys_guard *g, struct sys_guard6 *g6, char *why, size_t size);
void guard_format6(const struct sys_guard6_rule *rule, char *out, size_t size);
int ritteros_guard6(int op, struct sys_guard6 *g);

/* Logs (SYS_LOG, root only; user/lib/syscalls.c): the log this process's
 * console output (and its children's) also goes to ("": none); a text
 * added to a log; what the logs hold, taken (struct sys_log_record + text,
 * one after another): the bytes, or -1. */
#define LOGS_DIR "/system/logs"
int ritteros_log_as(const char *name);
/* A firmware variable (SYS_EFIVAR: EFIVAR_GET, EFIVAR_SET; root only). */
int ritteros_efivar(int op, struct sys_efivar *v);
int ritteros_log_write(const char *name, const char *text);
int ritteros_log_take(void *buffer, size_t size);

/* The elves: programs elvesd keeps running, one file each (user/bin/elvesd.c). */
#define ELVES_DIR "/system/config/elves"
#define WIFI_DIR "/system/config/wifi"               /* the wireless link's settings (24e) */
#define WIFI_NETWORKS "/system/config/wifi/networks" /* one file per saved network (root) */
#define WIFI_RADIO "/system/config/wifi/radio"       /* the radio itself: country (24f) */
/* elvesd's process ID, or 0 if it does not run; elvesd_start starts it
 * from bin_dir (/system/<arch>/bin) if it does not run: its process ID, or
 * -1 (user/lib/elves.c). */
int elvesd_pid(void);
int elvesd_start(const char *bin_dir);
/* The process ID of the first running process with that program name, or
 * 0; logs_flush asks logd to write what it keeps and waits (up to 3 s)
 * until it has (poweroff, reboot). */
int process_named(const char *name);
void logs_flush(void);

/* The system's settings: one file per topic in CONFIG_DIR, written as
 * "key = value" lines (spaces around both ignored; "#" starts a comment
 * line). config_get puts key's value from the file at path into value:
 * 1, 0 if the file has no such key, -1 (errno set) if it cannot be read
 * (user/lib/config.c). */
#define CONFIG_DIR "/system/config"
int config_get(const char *path, const char *key, char *value, size_t size);

/* Users (user/lib/users.c, with <pwd.h>). Names: 1-31 of a-z 0-9 _ -,
 * not starting with a digit or '-'. Passwords are stored in
 * PASSWORDS_FILE (root only) as PBKDF2-HMAC-SHA256 with a random salt. */
#define USER_NAME_MAX 31
#define PASSWORDS_FILE "/system/config/passwords"
#define PASSWORD_SALT 16
#define PASSWORD_ITERATIONS 100000
bool user_name_valid(const char *name);
/* Replace (line 0: remove) user's line in one of the two files, which then
 * gets the given mode; 0, or -1 with errno set. */
int user_file_set(const char *path, const char *user, const char *line, unsigned mode);
int password_set(const char *user, const char *password);
/* 1: the password is right, 0: wrong (or none set), -1: cannot read. It
 * takes as long for a user without a password. */
int password_check(const char *user, const char *password);
bool password_exists(const char *user);
/* (68e) sudo's memory (SYS_SESSION_STAMP) on this session's leader, for
 * the caller's real user: STAMP_AGE its age in seconds, STAMP_SET now
 * (root only), STAMP_FORGET gone; -1 with errno (ENOENT: none, EPERM). */
int ritteros_session_stamp(int op);

/*
 * The program's entry in the shell's help: PROGRAM_HELP("ls [PATH]...",
 * "list directories") once in its source. It goes into the ELF section
 * .ritteros.help (a tab between the two texts), which is kept in the file
 * but not loaded (user/user.ld); sh reads it from there.
 */
#define PROGRAM_HELP(usage, description)                                        \
    __attribute__((section(".ritteros.help"), used))                           \
    static const char ritteros_program_help[] = usage "\t" description
```

<a id="window-h"></a>

### window.h

`user/include/window.h` (in RitterOS's tree) -- windowed apps (libwindow); for apps.

libwindow (step 15f): the toolkit for graphical programs. It wraps the window protocol (windowproto.h) so a program need not speak it directly: open a window, draw into its pixels (fill rectangles, draw text in the shared font), show the result, and read pointer and keyboard events. The same drawing a program uses is the drawing every program uses, so windows look alike.

```c
struct window;

/* Events a window receives. */
#define WINDOW_NONE     0
#define WINDOW_POINTER  1       /* the pointer moved or a button changed */
#define WINDOW_KEY      2       /* a key: `key` is the byte (as the terminal gets) */
#define WINDOW_CLOSED   3       /* the window was closed (the close box, or windowd went away) */
#define WINDOW_RESIZE   4       /* the user resized the window: x, y is the new content size;
                                 * call window_resize to adopt it, then redraw */
#define WINDOW_MENU     5       /* the user chose a menu item: `menu` is its id */
#define WINDOW_DOUBLE_CLICK 6   /* the left button pressed twice in a row at (x, y), within the
                                 * user's double_click time and 4 pixels (after the second
                                 * press's WINDOW_POINTER); the same rule for every program */
#define WINDOW_SCROLL   7       /* the mouse wheel turned over the window: y = steps, negative
                                 * up (away from the user); a list moves 3 rows a step (15o) */
#define WINDOW_KEY_DOWN 8       /* a physical key pressed: `key` is its WKEY_* number (the USB
                                 * HID usage ID), modifiers too; once, not repeated (15p) */
#define WINDOW_KEY_UP   9       /* that key released (also when the window loses the front) */
#define WINDOW_TIMER    10      /* (41a) a timer of window_timer came due: `key` is its id */
#define WINDOW_FOCUS    11      /* (41f-1) the window got the keyboard (`key` 1) or lost it (0);
                                 * also once right after window_open */
#define WINDOW_ENTER    12      /* (41f-3) the pointer came over the window's content */
#define WINDOW_LEAVE    13      /* (41f-3) it left it: for another window, the frame, the desktop
                                 * or a menu (hover highlights end) */
#define WINDOW_TEXT     14      /* (41f-4) a typed character, whole: `key` its Unicode code point
                                 * (after the WINDOW_KEY bytes; never control bytes or a key's
                                 * escape sequence) */
#define WINDOW_DROP     15      /* (41f-6) a drag was let go over the window at (x, y): its text
                                 * is window_drop_text's */

/* Physical keys (WINDOW_KEY_DOWN/UP): the USB HID usage IDs, the same for
 * every keyboard and layout (where the key is, not what it types: WKEY_A
 * is the key labelled A on a US keyboard). Others by number. */
#define WKEY_A          0x04    /* ... WKEY_Z 0x1D, in order */
#define WKEY_B          0x05
#define WKEY_C          0x06
#define WKEY_D          0x07
#define WKEY_E          0x08
#define WKEY_F          0x09
#define WKEY_G          0x0A
#define WKEY_H          0x0B
#define WKEY_I          0x0C
#define WKEY_J          0x0D
#define WKEY_K          0x0E
#define WKEY_L          0x0F
#define WKEY_M          0x10
#define WKEY_N          0x11
#define WKEY_O          0x12
#define WKEY_P          0x13
#define WKEY_Q          0x14
#define WKEY_R          0x15
#define WKEY_S          0x16
#define WKEY_T          0x17
#define WKEY_U          0x18
#define WKEY_V          0x19
#define WKEY_W          0x1A
#define WKEY_X          0x1B
#define WKEY_Y          0x1C
#define WKEY_Z          0x1D
#define WKEY_1          0x1E    /* ... WKEY_9 0x26, then WKEY_0 */
#define WKEY_2          0x1F
#define WKEY_3          0x20
#define WKEY_4          0x21
#define WKEY_5          0x22
#define WKEY_6          0x23
#define WKEY_7          0x24
#define WKEY_8          0x25
#define WKEY_9          0x26
#define WKEY_0          0x27
#define WKEY_ENTER      0x28
#define WKEY_ESCAPE     0x29
#define WKEY_BACKSPACE  0x2A
#define WKEY_TAB        0x2B
#define WKEY_SPACE      0x2C
#define WKEY_MINUS      0x2D
#define WKEY_EQUAL      0x2E
#define WKEY_LBRACKET   0x2F
#define WKEY_RBRACKET   0x30
#define WKEY_BACKSLASH  0x31
#define WKEY_SEMICOLON  0x33
#define WKEY_APOSTROPHE 0x34
#define WKEY_GRAVE      0x35
#define WKEY_COMMA      0x36
#define WKEY_PERIOD     0x37
#define WKEY_SLASH      0x38
#define WKEY_CAPSLOCK   0x39
#define WKEY_F1         0x3A    /* ... WKEY_F12 0x45, in order */
#define WKEY_F2         0x3B
#define WKEY_F3         0x3C
#define WKEY_F4         0x3D
#define WKEY_F5         0x3E
#define WKEY_F6         0x3F
#define WKEY_F7         0x40
#define WKEY_F8         0x41
#define WKEY_F9         0x42
#define WKEY_F10        0x43
#define WKEY_F11        0x44
#define WKEY_F12        0x45
#define WKEY_PRINTSCREEN 0x46
#define WKEY_SCROLLLOCK 0x47
#define WKEY_PAUSE      0x48
#define WKEY_INSERT     0x49
#define WKEY_HOME       0x4A
#define WKEY_PAGEUP     0x4B
#define WKEY_DELETE     0x4C
#define WKEY_END        0x4D
#define WKEY_PAGEDOWN   0x4E
#define WKEY_RIGHT      0x4F
#define WKEY_LEFT       0x50
#define WKEY_DOWN       0x51
#define WKEY_UP         0x52
#define WKEY_LCTRL      0xE0
#define WKEY_LSHIFT     0xE1
#define WKEY_LALT       0xE2
#define WKEY_LGUI       0xE3    /* the Windows/Command key */
#define WKEY_RCTRL      0xE4
#define WKEY_RSHIFT     0xE5
#define WKEY_RALT       0xE6    /* AltGr on the US International layout */
#define WKEY_RGUI       0xE7

struct window_event {
    int type;
    int x, y;                   /* WINDOW_POINTER: position in the window; WINDOW_RESIZE: new size */
    unsigned buttons;           /* WINDOW_POINTER: buttons held */
    int key;                    /* WINDOW_KEY: the byte; WINDOW_KEY_DOWN/UP: WKEY_*;
                                 * WINDOW_TIMER: the timer's id; WINDOW_FOCUS: 1 or 0 */
    int menu;                   /* WINDOW_MENU: the chosen item's id */
};

/* A menu the program declares; windowd draws it and reports the chosen id.
 * A menu bar is an array of these (each a dropdown); a context menu is one
 * list of items. */
struct window_menu_item { int id; const char *label; };
struct window_menu { const char *title; const struct window_menu_item *items; int count; };

/* Set the window's menu bar (drawn below the title bar); `count` 0 clears it. */
int window_set_menu(struct window *win, const struct window_menu *menus, int count);

/* Pop up a context menu of `items` at (x, y) in the window (e.g. on a
 * right-click the program received). The chosen id comes back as WINDOW_MENU. */
int window_popup_menu(struct window *win, int x, int y,
                      const struct window_menu_item *items, int count);

/* Open a window of w x h pixels with a title. 0 if windowd is unreachable or
 * out of memory, or (errno EACCES) if the windowd running is another user's
 * (only its own user's and root's programs may use it). (41h-1) The
 * windowd is the one listening at /run/windowd, or where the environment
 * variable WINDOW_SOCKET says (a headless windowd's, for tests). */
struct window *window_open(int w, int h, const char *title);
void window_close(struct window *win);

/* (41f-5) A dialog: a window tied to `parent`, opened centred over it and
 * in front; while it is open the parent takes no input (a click on it
 * brings the dialog forward), and when it closes the parent is in front
 * again. As window_open otherwise. */
struct window *window_dialog_open(struct window *parent, int w, int h, const char *title);

int window_width(struct window *win);
int window_height(struct window *win);

/* Scroll bars (15o), the same in every app. window_scrollbar draws one at
 * (x, y), WINDOW_SCROLLBAR_W wide and h high, for a list of `total` rows of
 * which `shown` fit, starting at row `first`: a trough and a thumb as long
 * as the shown part (nothing to scroll: the trough alone).
 * window_scrollbar_hit gives the first row for a click or a drag at py (a y
 * in the window) on such a bar: the thumb centred there. */
#define WINDOW_SCROLLBAR_W 12
void window_scrollbar(struct window *win, int x, int y, int h, int first, int shown, int total);
int window_scrollbar_hit(int y, int h, int py, int shown, int total);

/* Adopt a new size after a WINDOW_RESIZE event: makes a new pixel buffer of
 * w x h, hands it to windowd, and replaces the window's. The new pixels are
 * blank, so redraw and window_flush after. 0 on success, -1 on failure (the
 * old buffer is kept). */
int window_resize(struct window *win, int w, int h);

/* A native pixel value for the (r, g, b) color. */
uint32_t window_rgb(struct window *win, int r, int g, int b);

/* Drawing into the window's pixels (clipped to the window). */
void window_fill(struct window *win, int x, int y, int w, int h, uint32_t color);
void window_text(struct window *win, int x, int y, const char *utf8, uint32_t fg, uint32_t bg);
/* The same with a font of wfont.h (wfont_find; 0: the default). */
struct font;
void window_text_font(struct window *win, const struct font *font, int x, int y, const char *utf8,
                      uint32_t fg, uint32_t bg);
#define WINDOW_TRANSPARENT 0xFFFFFFFFu  /* bg that leaves the pixels behind the text */

/* Copy a w x h block of native pixels (as window_rgb makes) into the window
 * at (x, y), clipped. For images and video frames (libimage/libvideo). */
void window_blit(struct window *win, int x, int y, int w, int h, const uint32_t *src);

/* The window's native pixel layout: the left shifts of the red, green and
 * blue channels, so a program can build native pixels itself in bulk. */
void window_shifts(struct window *win, int *red, int *green, int *blue);

/* (41b) The window's own pixels (window_width x window_height, rows that
 * wide, in window_shifts' layout): a libdraw canvas draws on them in
 * place. They move with window_resize: ask again after it. */
uint32_t *window_pixels(struct window *win);

/* Show what was drawn (windowd recomposites). */
void window_flush(struct window *win);

/* Wait for the next event; 1 with *ev filled, or 0 when the window closed. */
int window_wait(struct window *win, struct window_event *ev);

/* (41a) The event loop beside other work. window_wait_timeout waits at
 * most ms milliseconds (0: not at all; negative: as long as it takes): 1
 * with *ev filled, 0 when none came in time (ev->type WINDOW_NONE), -1
 * when the window closed (WINDOW_CLOSED). window_timer makes timer `id`
 * come due every ms milliseconds as a WINDOW_TIMER event, from now on,
 * until it is set again (ms 0 removes it); a late one is not caught up
 * (at most one event each time it is looked at); 0, or -1 when the
 * window has 8 timers already. Both waits hand out timers that are due.
 * window_fd is the window's connection to windowd, readable when windowd
 * sent something: a program waiting on its own descriptors puts it in its
 * poll (POLLIN), and after any wake-up takes every event with
 * window_wait_timeout(win, &ev, 0) until that returns 0 (events read early
 * by window_clipboard_get are kept there, not in the descriptor). Timers
 * do not make it readable: such a poll waits no longer than the program's
 * next timer. Never read or write the descriptor itself. */
int window_wait_timeout(struct window *win, struct window_event *ev, int ms);
int window_timer(struct window *win, int ms, int id);
int window_fd(struct window *win);

/* (41f-4) The pointer's shape while it is over the window's content. */
#define WINDOW_CURSOR_ARROW 0
#define WINDOW_CURSOR_TEXT  1       /* an I-beam: over text that can be edited */
#define WINDOW_CURSOR_HAND  2       /* a hand: over a link */
void window_set_cursor(struct window *win, int shape);

/* (41f-2) The sizes (content, pixels) the user's resizing and maximizing
 * keep to: at least min_w x min_h, at most max_w x max_h (0: no limit
 * that way). The window's present size is not changed. */
void window_set_limits(struct window *win, int min_w, int min_h, int max_w, int max_h);

/* (41f-6) window_fullscreen(win, 1): the window fills the screen, without
 * its frame and title bar, in front of the other windows and the taskbar
 * (and follows a change of the screen's mode); window_fullscreen(win, 0)
 * puts it back where and as large as it was. The new size comes as a
 * WINDOW_RESIZE (answer it with window_resize, as a resize by the user);
 * the limits of window_set_limits do not apply. Leaving full screen is the
 * program's to offer (a key, a button): windowd has none.
 * window_minimize: the window goes to a taskbar button, as by its - box;
 * the user brings it back. */
void window_fullscreen(struct window *win, int on);
void window_minimize(struct window *win);

/* (41f-6) Drag and drop between programs. window_drag, called while the
 * left button pressed in the window is still held (on the pointer's move
 * that starts the drag), makes n bytes of text (up to 64 KiB) what the
 * pointer carries; windowd shows a page beside it, and when the button is
 * let go over a window's content, that window gets WINDOW_DROP with where
 * it fell (its own window too). Called after the button is let go, it does
 * nothing. Files are dragged as their full paths, one per line.
 * window_drop_text is the text of the last WINDOW_DROP (NUL-ended; "" if
 * none), kept until the next one. */
int window_drag(struct window *win, const char *text, size_t n);
const char *window_drop_text(struct window *win);

/* (41g-2) The program a double click starts for the file `path`, by its
 * ending (in any case): (42a) the user's ~/.config/filetypes (written with
 * its defaults when missing), else /system/config/filetypes. Its NAME goes
 * into buf; the result is 0 for an app, (42b) a bundle in
 * /system/<arch>/apps (run NAME PATH: its window), 1 for a program in bin
 * (term NAME PATH), -1 when none is set or the program is in neither. */
int window_app_for(const char *path, char *buf, size_t size);

/* (41g-4) A notification: a box at the top right of the screen, over
 * everything, for 5 seconds, with the title (bold; 0 or "" for none) and
 * the text below it, wrapped (a newline starts a line; up to 511 bytes,
 * the rest cut). Up to four show at once, the newest on top. win may be 0
 * (a program without a window: it reaches windowd by a connection of its
 * own). 0, or -1 when windowd could not be reached. */
int window_notify(struct window *win, const char *title, const char *text);

/* (41g-5) The user's colours, as 0xRRGGBB (window_rgb takes them apart):
 * `theme = dark` (the default) or `light`, and `accent = RRGGBB`, in
 * ~/.config/windows, else /system/config/windows; read once. The accent,
 * when set, is both ACCENT and SELECTION; otherwise each is the theme's.
 * An unknown `which` gives the theme's TEXT. */
#define WINDOW_COLOR_BACKGROUND   0     /* a window's background */
#define WINDOW_COLOR_PANEL        1     /* an area set apart on it (a list, a field) */
#define WINDOW_COLOR_FACE         2     /* a button */
#define WINDOW_COLOR_FACE_PRESSED 3     /* a button held down */
#define WINDOW_COLOR_HOVER        4     /* a button under the pointer */
#define WINDOW_COLOR_TEXT         5     /* text on any of them */
#define WINDOW_COLOR_EDGE         6     /* lines: edges, boxes, separators */
#define WINDOW_COLOR_ACCENT       7     /* focus, progress, a slider's part done */
#define WINDOW_COLOR_SELECTION    8     /* what is chosen: rows, title bars of the window in front */
uint32_t window_color(int which);

/* The clipboard (15w), windowd's, shared by the screen's user's programs:
 * set it to n bytes of UTF-8 text (up to 64 KiB), or get its text into buf
 * (NUL-ended, cut to size - 1): its length, or -1. Get from the thread
 * that calls window_wait (events arriving meanwhile are kept for it). */
int window_clipboard_set(struct window *win, const char *utf8, size_t n);
int window_clipboard_get(struct window *win, char *buf, size_t size);

/* A one-line text field (15w), the same in every app: at (x, y), w pixels
 * wide (its height: window_field_height). Give it every event of the
 * window: window_field_event returns -1 for one not its own (the app's),
 * 0 for one it took, 1 when Enter was pressed in it. A click focuses it, a
 * drag or Shift selects, a double click selects a word; Ctrl+A, Ctrl+C,
 * Ctrl+X and Ctrl+V select all, copy, cut and paste through the
 * clipboard. It draws and shows itself after a change it took; the app
 * calls window_field_draw when it redraws its window. window_field_focus
 * gives it the keyboard (with all its text selected) or takes it away. */
struct window_field;
struct window_field *window_field_new(struct window *win, int x, int y, int w);
void window_field_free(struct window_field *f);
void window_field_place(struct window_field *f, int x, int y, int w);
int window_field_height(const struct window_field *f);
void window_field_draw(struct window_field *f);
int window_field_event(struct window_field *f, const struct window_event *ev);
const char *window_field_text(const struct window_field *f);
void window_field_set_text(struct window_field *f, const char *utf8);
void window_field_focus(struct window_field *f, int on);
int window_field_focused(const struct window_field *f);

/* The "Open file" dialog every app shares: a window of its own listing
 * `folder` (0 or not a folder: "/"), titled `title` (0: "Open"). The user
 * picks a file with clicks (a second click on a row opens it), the arrow keys
 * and Enter, or types a name or path; Backspace with nothing typed goes up.
 * It returns when the user is done: 1 with the file's full path in `path`
 * (`size` bytes), 0 if cancelled (Cancel, the close box), -1 if no window
 * could open. Modal: `parent` (may be 0) gets no events meanwhile. */
int window_choose_file(struct window *parent, const char *title, const char *folder,
                       char *path, int size);

/* (42c) The same dialog, choosing what `accept(path, data)` says may be
 * chosen (non-zero): every other file is shown greyed and cannot be
 * picked, and an app bundle (a folder NAME.program) is one item, shown as
 * NAME, chosen or greyed as accept says (never entered); plain folders are
 * browsed. accept 0 is window_choose_file. The chosen path (a bundle's
 * too, without a slash at its end) goes into `path`. */
int window_choose(struct window *parent, const char *title, const char *folder,
                  int (*accept)(const char *path, void *data), void *data, char *path, int size);

/* (42c) Can the app bundle at `bundle` (a folder NAME.program, holding
 * its program NAME) open `file`: does its info's `types` list the file's
 * ending (any case)? 1 or 0. */
int window_app_opens(const char *bundle, const char *file);
```

<a id="media-h"></a>

### media.h

`user/include/media.h` (in RitterOS's tree) -- the media libraries' types; for apps.

media.h: the data types shared by the media libraries -- libimage and libvideo pass frames as a `struct image`, libaudio and libvideo pass sound as a `struct audio_buffer`. Both hold decoded data in memory the caller frees. Kept free of any display or hardware dependency on purpose, so a GPU or a sound card later changes drivers, not these types.

```c
/* A pixel's byte layout in `struct image`. libimage decodes to RGBA8888
 * (bytes R, G, B, A); the others are for conversions. */
enum pixel_format {
    PIXEL_RGBA8888,             /* 4 bytes per pixel: R, G, B, A */
    PIXEL_RGB888,               /* 3 bytes per pixel: R, G, B */
    PIXEL_GRAY8                 /* 1 byte per pixel */
};

/* A decoded image (or one video frame). `pixels` is malloc'd; `image_free`
 * releases it. `stride` is the bytes per row (>= w * bytes-per-pixel). */
struct image {
    int w, h;
    int stride;
    enum pixel_format format;
    uint8_t *pixels;
};

/* A sample's type in `struct audio_buffer`. */
enum sample_format {
    SAMPLE_S16,                 /* signed 16-bit */
    SAMPLE_F32                  /* 32-bit float, -1.0 .. 1.0 */
};

/* Decoded PCM audio, interleaved by channel. `samples` is malloc'd;
 * `audio_free` releases it. */
struct audio_buffer {
    int rate;                   /* samples per second per channel, e.g. 44100 */
    int channels;               /* 1 mono, 2 stereo */
    enum sample_format format;
    long frames;                /* samples per channel */
    void *samples;              /* frames * channels * bytes-per-sample */
};
```

<a id="image-h"></a>

### image.h

`user/include/image.h` (in RitterOS's tree) -- images (libimage); for apps.

libimage (user/lib/image.c): decode and encode still images, to and from a `struct image` of RGBA8888 pixels in memory. Formats are written from their specifications (PPM, BMP, PNG and JPEG so far). All colour conversion, scaling and blitting is here too, so libvideo and viewers share it; those are where SIMD or a GPU would plug in later, behind these same signatures.

```c
/* Decode a file (or a buffer in memory) into *out (RGBA8888; pixels malloc'd).
 * 0, or -1 with errno set (ENOMEM, or EINVAL for a format we do not read). */
int image_load(const char *path, struct image *out);
int image_load_memory(const void *data, long n, struct image *out);

/* Fill *out's w/h/format/stride without decoding the pixels (pixels = 0). */
int image_info(const char *path, struct image *out);

/* Encode *img to a file, by the path's extension: .png (41h-2; RGBA, or RGB
 * when every pixel is opaque), .bmp, else PPM. */
int image_save(const char *path, const struct image *img);

/* Release img->pixels (and zero the struct). Safe on a zero struct. */
void image_free(struct image *img);

/* A scaled copy (nearest-neighbour) of w x h into *out. 0 or -1/errno. */
int image_scale(const struct image *src, int w, int h, struct image *out);
```

<a id="audio-h"></a>

### audio.h

`user/include/audio.h` (in RitterOS's tree) -- sound (libaudio); for apps.

libaudio (user/lib/audio.c): decode and encode sound to and from a `struct audio_buffer` of PCM samples in memory, and the sample operations (resample, mix) that a player and libvideo share. WAV (PCM) so far. This library ends at PCM in RAM; playing it needs the sound device /dev/pcm0.

```c
/* Decode a file (or a buffer) into *out (samples malloc'd). 0, or -1 with
 * errno (EINVAL for a format or encoding we do not read). */
int audio_load(const char *path, struct audio_buffer *out);
int audio_load_memory(const void *data, long n, struct audio_buffer *out);

/* Fill *out's rate/channels/format/frames without the samples (samples = 0). */
int audio_info(const char *path, struct audio_buffer *out);

/* Encode *buf to a WAV file. */
int audio_save(const char *path, const struct audio_buffer *buf);

/* Release buf->samples (and zero the struct). Safe on a zero struct. */
void audio_free(struct audio_buffer *buf);

/* A copy resampled to `rate` (linear) into *out. 0 or -1/errno. */
int audio_resample(const struct audio_buffer *src, int rate, struct audio_buffer *out);

/* Add src into dst (same rate/format/channels) scaled by gain, clamped. */
int audio_mix(struct audio_buffer *dst, const struct audio_buffer *src, float gain);
```

<a id="video-h"></a>

### video.h

`user/include/video.h` (in RitterOS's tree) -- video (libvideo); for apps.

libvideo (user/lib/video.c): read a video as a stream of frames (each a `struct image`) plus an optional audio track (`struct audio_buffer`). The container so far is RVID, a simple own format (a header, the audio PCM, then raw RGB frames) -- real codecs like H.264 are a much larger undertaking; the interface is shaped so they slot in behind it later.

```c
struct video;                           /* opaque: the open file and decode state */

/* Open a video file. 0 with *v set, or -1 with errno (EINVAL: not a video). */
int video_open(const char *path, struct video **v);

/* Its dimensions, frames per second, and whether it has an audio track. */
void video_info(struct video *v, int *w, int *h, int *fps, int *has_audio);

/* Decode the next frame into *frame (RGBA8888, pixels malloc'd); its time in
 * nanoseconds in *pts_ns. 1 with a frame, 0 at the end, -1 on error. */
int video_read_frame(struct video *v, struct image *frame, int64_t *pts_ns);

/* The whole audio track as one buffer (samples malloc'd), if any. 0 or -1. */
int video_audio(struct video *v, struct audio_buffer *out);

void video_close(struct video *v);
```

<a id="draw-h"></a>

### draw.h

`user/include/draw.h` (in RitterOS's tree) -- 2D drawing, anti-aliased, on any pixel buffer (libdraw); for apps.

libdraw (41b): 2D drawing on any pixel buffer: a window's (window_pixels), an image's or one of the program's own, described by a struct draw_canvas. Every shape is anti-aliased: a pixel takes the shape's colour in proportion to how much of its square the shape covers, by area, and every colour has an alpha (0 transparent, 255 opaque) blended over what is there. Coordinates are floats in pixels: pixel (i, j) is the square from (i, j) to (i + 1, j + 1), so its centre is (i + 0.5, j + 0.5).  A `line` of 0 fills a shape; above 0 it is the width of the shape's outline: a rectangle's, a circle's or an ellipse's lies inside the shape (as a CSS border, so a 1-pixel one on whole coordinates is sharp), a polygon's is centred on its edges (bevelled corners), as is a line's. Curves are drawn as straight pieces within 1/1024 of a pixel of them.

```c
/* A pixel buffer: w x h pixels of 32 bits, rows `stride` pixels apart.
 * Each 8-bit channel lies at its shift (a window's: window_shifts; an
 * RGBA8888 image's in memory: red 0, green 8, blue 16); `opaque`'s bits
 * are set in every pixel written (0xFF000000 for an RGBA8888 image's
 * alpha, 0 for a window). Fill in the first eight fields; draw_clip sets
 * the rest (all 0: the whole canvas). */
struct draw_canvas {
    uint32_t *pixels;
    int w, h, stride;
    int red_shift, green_shift, blue_shift;
    uint32_t opaque;
    int clipped;                /* 1: only inside the clip rectangle */
    int clip_x, clip_y, clip_w, clip_h;
};

/* A colour: 8 bits each of red, green, blue and alpha (as 0xAARRGGBB). */
uint32_t draw_rgba(int r, int g, int b, int a);

/* Draw only inside x, y, w, h from now on (a part of it outside the canvas
 * is left out); draw_clip(c, 0, 0, c->w, c->h) draws everywhere again. */
void draw_clip(struct draw_canvas *c, int x, int y, int w, int h);

/* A rectangle from (x, y), w x h: filled (line 0), or its outline `line`
 * wide inside it. */
void draw_rect(struct draw_canvas *c, float x, float y, float w, float h, float line, uint32_t color);

/* A line from (x0, y0) to (x1, y1), `width` wide, its ends cut square at
 * the two points. */
void draw_line(struct draw_canvas *c, float x0, float y0, float x1, float y1, float width,
               uint32_t color);

/* A polygon of n points (xy: x0, y0, x1, y1, ...; the last joined to the
 * first): filled (line 0; where its edges cross themselves, a part wound
 * round twice is filled once), or its outline `line` wide. */
void draw_polygon(struct draw_canvas *c, const float *xy, int n, float line, uint32_t color);

/* (41b-2) A circle around (cx, cy) of radius r, an ellipse of radii rx
 * (across) and ry (down), and a rectangle whose corners are quarter
 * circles of `radius` (at most half its width and height): filled, or
 * their outline `line` wide inside them (an ellipse's inner edge is the
 * ellipse of radii rx - line, ry - line; a rounded rectangle's has corners
 * of radius - line). */
void draw_circle(struct draw_canvas *c, float cx, float cy, float r, float line, uint32_t color);
void draw_ellipse(struct draw_canvas *c, float cx, float cy, float rx, float ry, float line,
                  uint32_t color);
void draw_round_rect(struct draw_canvas *c, float x, float y, float w, float h, float radius,
                     float line, uint32_t color);

/* (41b-3) The rectangle x, y, w, h filled with a gradient: `from` at
 * (x0, y0) to `to` at (x1, y1) along the line through them (each pixel by
 * where its centre falls on that line; before the first point `from`,
 * past the second `to`), alpha included. */
void draw_gradient(struct draw_canvas *c, float x, float y, float w, float h, float x0, float y0,
                   uint32_t from, float x1, float y1, uint32_t to);

/* An image (libimage's struct image: RGBA8888, RGB888 or GRAY8) scaled to
 * w x h at x, y and turned `angle` degrees clockwise about that
 * rectangle's centre; its alpha blends it over what is there. Between its
 * pixels it is read linearly (shrunk: averaged over each canvas pixel);
 * drawn at its own size on whole coordinates, or turned by right angles,
 * it is copied exactly. */
void draw_image(struct draw_canvas *c, const struct image *img, float x, float y, float w, float h,
                float angle);
```

<a id="widget-h"></a>

### widget.h

`user/include/widget.h` (in RitterOS's tree) -- controls for windowed programs: buttons, check boxes, layouts (libwidget); for apps.

libwidget (41e): controls for windowed programs, the same in every one. A program builds a tree of widgets -- rows, columns and grids holding labels, buttons, check boxes, radio buttons... -- and hands it with its window to widget_run, which lays it out to the window (again after every resize), draws it (libdraw on the window's own pixels, FreeSans at the user's text_size), and runs the window's events: a click or Space/Enter presses the control with the keyboard, Tab and Shift+Tab move the keyboard between controls, and each change calls the function given with widget_on. Strings are copied. Not thread-safe: one thread builds and runs a tree (its callbacks may change it).

```c
struct widget;
struct window;

/* Layouts. A row puts its children left to right, a column top to bottom,
 * a grid in `columns` columns (row by row), each child at the size it
 * needs and 8 pixels apart (the window's outermost one 8 from its edges);
 * a row's or column's spare length goes to the children with a stretch
 * weight (widget_stretch), shared by their weights. Children are added in
 * order with widget_add (to a row, column, grid or tabs; others ignore it). */
struct widget *widget_row(void);
struct widget *widget_column(void);
struct widget *widget_grid(int columns);

/* (41e-2) Tabs: its children are pages, one shown at a time below a row
 * of titles (each page's text, widget_set_text); its value is the open
 * page (0 the first), changed by a click on a title or by Left/Right
 * while it has the keyboard; it is as large as its largest page. */
struct widget *widget_tabs(void);
void widget_add(struct widget *parent, struct widget *child);
void widget_stretch(struct widget *w, int weight);

/* A label: text (wrapped at 400 pixels, and at newlines); a button: text
 * on a face, pressed by a click or by Space/Enter; a check box (value 0
 * or 1); a radio button (value 1 when it is its group's chosen one): `group`
 * 0 starts a new group, else the new one joins group's (choosing one
 * clears the others; on 1 makes it the chosen one). 0 without memory. */
struct widget *widget_label(const char *utf8);
struct widget *widget_button(const char *utf8);
struct widget *widget_check(const char *utf8, int on);
struct widget *widget_radio(const char *utf8, struct widget *group, int on);

/* (41e-2) A slider: a value from min to max, dragged with the pointer or
 * moved by the arrow keys (Page Up/Down a tenth, Home/End the ends); a
 * progress bar: value of max shown filled (no input). */
struct widget *widget_slider(int min, int max, int value);
struct widget *widget_progress(int max, int value);

/* (41e-3) Rows to choose from, given as text: a list, a line a row; a
 * table of `columns` columns, a line a row and a tab between cells, its
 * first line the header; a tree, a line a node, its leading tabs its level
 * (a node with deeper lines after it is a branch, closed at first: its
 * triangle, Right/Left or Space/Enter open and close it). widget_value is
 * the chosen row (a table's counted under its header, a tree's line
 * among all of them; -1: none) and widget_text its text (a table's with
 * its tabs); widget_set_text gives new rows (none chosen). A click or
 * Up/Down, Page Up/Down, Home/End choose; the wheel and the scroll bar
 * scroll; the row under the pointer is highlighted. */
struct widget *widget_list(const char *lines);
struct widget *widget_table(int columns, const char *rows);
struct widget *widget_tree(const char *lines);

/* (41e-4) An editor: text over `lines` lines (wrapped at its width,
 * scrolled with a scroll bar and the wheel), or with 1 a one-line entry
 * (scrolled sideways; newlines become spaces). Typing (WINDOW_TEXT) and
 * the arrows, Home/End (Ctrl: the text's ends), Page Up/Down, Backspace,
 * Delete and Enter edit; Shift and a drag select, a double click selects
 * a word; Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V go through windowd's clipboard;
 * the pointer is an I-beam over it, its cursor blinks. widget_text is its
 * text, widget_value its cursor (a byte of the text); widget_set_text puts
 * the cursor at the end; its function is called at every change. */
struct widget *widget_editor(const char *utf8, int lines);

/* (41e-5) Dialogs over a program's window (window_dialog_open: the window
 * takes no input meanwhile), sized to their text, run until answered (they
 * may be called from a callback): a message with an OK button; a question
 * with the buttons yes and no (0: "Yes", "No"), 1 for yes, 0 for no,
 * Escape or the dialog closed. The first button has the keyboard (Enter
 * presses it); Escape presses the last. */
void widget_message(struct window *parent, const char *title, const char *text);
int widget_question(struct window *parent, const char *title, const char *text, const char *yes, const char *no);

/* What to call when a widget is used: a button pressed, a check box
 * changed, a radio button chosen, a slider moved, another tab opened,
 * another row chosen, an editor's text changed (fn(w, data)). */
void widget_on(struct widget *w, void (*fn)(struct widget *w, void *data), void *data);

/* A widget's value (a check box's 0/1, a radio button's: chosen or not,
 * a slider's, a progress bar's, the open tab; kept within their range)
 * and text, read and changed (a change is drawn; it calls no function). */
int widget_value(const struct widget *w);
void widget_set_value(struct widget *w, int value);
const char *widget_text(const struct widget *w);
void widget_set_text(struct widget *w, const char *utf8);

/* Run the window's events for the tree root until the window is closed (0)
 * or widget_quit(root) is called, from a callback (1). The window stays
 * open; the tree stays the program's (widget_free gives back all of it).
 * (41f-2) The window cannot be resized below what the tree needs, and one
 * opened smaller grows to it. */
int widget_run(struct window *win, struct widget *root);
void widget_quit(struct widget *root);
void widget_free(struct widget *root);
```

<a id="wfont-h"></a>

### wfont.h

`user/include/wfont.h` (in RitterOS's tree) -- the windows' font (cell sizes for laying out text); for apps.

Text in user space for the window system (step 15f; fonts from files since step 22c): glyphs drawn into a 32-bit pixel buffer. The default face, 9x15 (and its bold, 9x15B), is compiled into the library -- the same public-domain X11 misc-fixed glyphs the console uses -- and always there; the other fonts are the BDF files in /system/fonts, read when a program first asks for one. A font is opaque: programs hold a pointer and ask through these functions, so its layout can change (wider glyphs, proportional faces) without breaking them. Used by libwindow and windowd. (41c-3) Scalable fonts from TrueType and OpenType files: wfont_open.

```c
struct font;

#define WFONT_TRANSPARENT 0xFFFFFFFFu        /* leave the pixels behind the text */
#define WFONT_DIR "/system/fonts"            /* the font files: NAME.bdf */

/* The compiled-in default face, 9x15, and its bold, 9x15B (whose missing
 * glyphs come from the regular one): always there, and the fallback when a
 * font file is missing or cannot be read. Functions, not variables: a shared
 * library exports functions only (22c). */
const struct font *wfont_regular(void);
const struct font *wfont_bold(void);

/* The font NAME from WFONT_DIR/NAME.bdf ("10x20", "9x18B", "helvR12", ...):
 * a bitmap BDF font with glyphs up to 64 pixels wide, fixed-width or
 * proportional, read the first time it is asked for, then kept for the
 * program's life. 0 if there is no such file, it
 * cannot be read or is not such a font: use wfont_regular() then. A bold
 * face (NAMEB) takes missing glyphs from NAME. Not thread-safe (the first
 * call per font should come from one thread). */
const struct font *wfont_find(const char *name);

/* (41c-3) A TrueType or OpenType font file (.ttf, .otf: TrueType or CFF
 * outlines; the FreeFont faces in WFONT_DIR, FreeSans.ttf ...) at `size`
 * pixels an em: `path` a file's path, or a bare file name in WFONT_DIR.
 * Its glyphs are drawn smooth (each pixel by how much of it the glyph
 * covers, blended over what is there) at any size from 4 to 1000, the pen
 * moving by the exact advances (a glyph at the nearest quarter pixel);
 * wfont_width is its widest advance, wfont_height its ascent, descent and
 * line gap. Read the first time it is asked for at that size, then kept for
 * the program's life; 0 when there is no such file or it is not a font this
 * reader knows. Its functions may be called from several threads. (41d) A
 * size of 0 is the user's: text_size in ~/.config/windows (else
 * /system/config/windows; 8-64, 15 if not set). */
const struct font *wfont_open(const char *path, int size);

/* A font's character cell in pixels: every glyph of a fixed-width font is
 * this wide (a proportional font's widest; measure text with
 * wfont_text_width), and every line this tall. */
int wfont_width(const struct font *font);
int wfont_height(const struct font *font);

/* The width in pixels wfont_text takes for utf8 with this font. */
int wfont_text_width(const struct font *font, const char *utf8);

/* (41d) Text over several lines, with any font. wfont_wrap gives how many
 * bytes of utf8 its first line takes when lines are at most `width` pixels
 * wide: a line breaks after the spaces following a word (spaces at its end
 * do not count to its width), inside a word only when the word alone is
 * wider than a line, and after a newline (its byte counted); at least one
 * character. The next line starts that many bytes on. wfont_layout draws
 * utf8 so wrapped at x, y, a line wfont_height below the one before (as
 * wfont_text, newlines not drawn), and returns its height; buf 0 only
 * measures it. */
int wfont_wrap(const struct font *font, const char *utf8, int width);
int wfont_layout(uint32_t *buf, uint32_t stride, uint32_t bw, uint32_t bh, const struct font *font, int x,
                 int y, int width, const char *utf8, uint32_t fg, uint32_t bg);

/* Decode one UTF-8 character at *s and advance past it (0 at the end). */
uint32_t utf8_next(const char **s);

/* Draw a UTF-8 string into the pixel buffer `buf` (bw x bh pixels, `stride`
 * pixels per row) with its top-left at (x, y). Colors are native pixels; bg
 * WFONT_TRANSPARENT leaves the background. Returns the x after the text. */
int wfont_text(uint32_t *buf, uint32_t stride, uint32_t bw, uint32_t bh,
               const struct font *font, int x, int y, const char *utf8,
               uint32_t fg, uint32_t bg);
```

<a id="tls-h"></a>

### tls.h

`user/include/tls.h` (in RitterOS's tree) -- TLS 1.3 connections, OpenBSD's libtls interface (libtls); for apps.

TLS (step 23d): connections protected by TLS 1.3, as OpenBSD's libtls offers them (a subset, the same names and behaviour), in libtls.so.1. A client checks the server's certificate chain against the trusted roots (by default /system/certs/cacert.pem), its host name and the dates. Our own code (RFC 8446): TLS 1.3 only, X25519, AES-128/256-GCM and ChaCha20-Poly1305, RSA and ECDSA (P-256, P-384) certificates.  struct tls *t = tls_client(); tls_configure(t, 0);                 // (0: the defaults) if (tls_connect(t, "example.com", "443") < 0) puts(tls_error(t)); tls_write(t, request, n); n = tls_read(t, buf, sizeof(buf)); tls_close(t); tls_free(t);  Calls return 0 (or a byte count), -1 on failure (tls_error says why). Everything blocks: TLS_WANT_POLLIN/POLLOUT are never returned here.

```c
#define TLS_API             20200120    /* (OpenBSD's version of this interface) */
#define TLS_WANT_POLLIN     (-2)
#define TLS_WANT_POLLOUT    (-3)
#define TLS_DEFAULT_CA_FILE "/system/certs/cacert.pem"

struct tls;
struct tls_config;

int tls_init(void);

/* A configuration: which roots to trust, what to check. */
struct tls_config *tls_config_new(void);
void tls_config_free(struct tls_config *config);
const char *tls_config_error(struct tls_config *config);
const char *tls_default_ca_cert_file(void);
int tls_config_set_ca_file(struct tls_config *config, const char *file);       /* PEM */
int tls_config_set_ca_mem(struct tls_config *config, const uint8_t *pem, size_t length);
void tls_config_insecure_noverifycert(struct tls_config *config);     /* no chain check */
void tls_config_insecure_noverifyname(struct tls_config *config);     /* any host name */
void tls_config_insecure_noverifytime(struct tls_config *config);     /* any dates */
void tls_config_verify(struct tls_config *config);                     /* all of them again */

/* A client connection. */
struct tls *tls_client(void);
int tls_configure(struct tls *ctx, struct tls_config *config);
void tls_reset(struct tls *ctx);
void tls_free(struct tls *ctx);
int tls_connect(struct tls *ctx, const char *host, const char *port);
int tls_connect_servername(struct tls *ctx, const char *host, const char *port,
                           const char *servername);
int tls_connect_socket(struct tls *ctx, int socket, const char *servername);
int tls_handshake(struct tls *ctx);
ssize_t tls_read(struct tls *ctx, void *buf, size_t length);
ssize_t tls_write(struct tls *ctx, const void *buf, size_t length);
/* tls_read returns 0 at the end, also when the server just closed the
 * connection; tls_close then fails ("...without close_notify"): the data
 * may have been cut short (as OpenBSD's). */
int tls_close(struct tls *ctx);
const char *tls_error(struct tls *ctx);

/* After the handshake: "TLSv1.3"; the cipher suite's name; the host name. */
const char *tls_conn_version(struct tls *ctx);
const char *tls_conn_cipher(struct tls *ctx);
const char *tls_conn_servername(struct tls *ctx);
```

<a id="crypto-h"></a>

### crypto.h

`user/include/crypto.h` (in RitterOS's tree) -- SHA-512, X25519, Ed25519, ChaCha20-Poly1305 (written for SSH); AES-GCM, SHA-384, HKDF, RSA and ECDSA checks (for TLS); for apps.

Cryptography for SSH (user/lib/sha512.c, chacha.c, c25519.c), TLS (aes.c, hkdf.c, verify.c; item 23) and Wi-Fi (sha1.c, wpacrypto.c; item 24), our own code from the RFCs, IEEE 802.11 and FIPS standards, checked against their test vectors (scripts/crypto_ref.py, user/test/tlscrypto.c). SHA-256, HMAC-SHA256 and PBKDF2 are in ritteros.h. Secret-dependent work takes the same time for every secret (no branches or table lookups on secret data: AES is bitsliced); checking signatures works on public data only and need not.

```c
/* SHA-512 (FIPS 180-4). */
#define SHA512_SIZE 64
struct sha512 {
    uint64_t h[8];
    uint64_t length;                    /* bytes so far */
    uint8_t block[128];
    size_t used;
};
void sha512_init(struct sha512 *s);
void sha512_update(struct sha512 *s, const void *data, size_t n);
void sha512_final(struct sha512 *s, uint8_t out[SHA512_SIZE]);
void sha512(const void *data, size_t n, uint8_t out[SHA512_SIZE]);

/* ChaCha20 (RFC 8439): data XOR the key stream, from the 16 bytes that are
 * the state's last four words: a 64-bit block counter (little-endian;
 * RFC 8439's 32-bit counter and 96-bit nonce fit the same bytes) and the
 * nonce. in and out may be the same. */
void chacha20(const uint8_t key[32], const uint8_t block[16], const void *in, void *out,
              size_t n);

/* Poly1305 (RFC 8439): a 16-byte tag of a message under a one-time key. */
void poly1305(const uint8_t key[32], const void *message, size_t n, uint8_t tag[16]);

/* Equal without a branch on the bytes (for tags and hashes). */
bool crypto_equal(const void *a, const void *b, size_t n);

/* X25519 (RFC 7748): out = scalar * point (32 bytes each, little-endian);
 * the public key of a secret is x25519_base(secret). */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* Ed25519 (RFC 8032): a 32-byte secret seed, its 32-byte public key,
 * 64-byte signatures. */
void ed25519_public(uint8_t public_key[32], const uint8_t seed[32]);
void ed25519_sign(uint8_t signature[64], const uint8_t seed[32], const uint8_t public_key[32],
                  const void *message, size_t n);
bool ed25519_verify(const uint8_t signature[64], const uint8_t public_key[32],
                    const void *message, size_t n);

/* AES (FIPS 197) and GCM (NIST SP 800-38D), for TLS (23a). A key of 16
 * (AES-128) or 32 bytes (AES-256); aes_encrypt4 does four blocks at once.
 * In common/aes.c with CCM (shared with the kernel; common/aes.h declares
 * the same: keep them in step). */
struct aes {
    uint64_t key[15][8];                /* each round key, bitsliced */
    int rounds;
};
void aes_init(struct aes *a, const uint8_t *key, size_t length);
void aes_encrypt(const struct aes *a, const uint8_t in[16], uint8_t out[16]);
void aes_encrypt4(const struct aes *a, const uint8_t in[64], uint8_t out[64]);
/* The inverse cipher (24a: WPA's key unwrap). */
void aes_decrypt(const struct aes *a, const uint8_t in[16], uint8_t out[16]);
void aes_decrypt4(const struct aes *a, const uint8_t in[64], uint8_t out[64]);
/* GCM with a 12-byte IV and a 16-byte tag; open checks the tag first and
 * decrypts nothing if it is wrong (false). in and out may be the same. */
void aes_gcm_seal(const struct aes *a, const uint8_t iv[12], const void *aad, size_t aad_n,
                  const void *in, void *out, size_t n, uint8_t tag[16]);
bool aes_gcm_open(const struct aes *a, const uint8_t iv[12], const void *aad, size_t aad_n,
                  const void *in, void *out, size_t n, const uint8_t tag[16]);

/* SHA-384 (FIPS 180-4: SHA-512 with other initial values, 48 bytes), its
 * HMAC, and HKDF (RFC 5869) with SHA-256 or SHA-384 (size 32 or 48): the
 * key schedule of TLS 1.3 (23a). */
#define SHA384_SIZE 48
#define HKDF_INFO_MAX 512
void sha384_init(struct sha512 *s);
void sha384_final(struct sha512 *s, uint8_t out[SHA384_SIZE]);
void sha384(const void *data, size_t n, uint8_t out[SHA384_SIZE]);
void hmac_sha384(const void *key, size_t key_length, const void *data, size_t n,
                 uint8_t out[SHA384_SIZE]);
void hkdf_extract(size_t size, const void *salt, size_t salt_length, const void *ikm,
                  size_t ikm_length, uint8_t *prk);
int hkdf_expand(size_t size, const uint8_t *prk, const void *info, size_t info_length,
                uint8_t *out, size_t length);

/* Checking signatures (23b), public data only: RSA PKCS #1 v1.5 and PSS
 * (salt as long as the hash; RFC 8017) with a SHA-256, -384 or -512 digest,
 * the modulus and exponent as big-endian bytes; ECDSA on P-256 or P-384
 * (bits) with an uncompressed public point (04 x y) and r, s as big-endian
 * bytes. True for a good signature. */
bool rsa_verify_pkcs1(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                      const uint8_t *digest, size_t dlen, const uint8_t *sig, size_t slen);
bool rsa_verify_pss(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                    const uint8_t *digest, size_t dlen, const uint8_t *sig, size_t slen);
bool ecdsa_verify(int bits, const uint8_t *pub, size_t pub_len, const uint8_t *digest,
                  size_t dlen, const uint8_t *r, size_t rlen, const uint8_t *s, size_t slen);

/* --- Wi-Fi (item 24) --- */

/* SHA-1 (FIPS 180-4), HMAC-SHA1 (RFC 2104), PBKDF2-HMAC-SHA1 (RFC 8018;
 * salt up to PBKDF2_SHA1_SALT_MAX bytes): WPA2's passphrase to key, with
 * the network's name as the salt and 4,096 rounds. */
#define SHA1_SIZE 20
#define PBKDF2_SHA1_SALT_MAX 64
struct sha1 {
    uint32_t h[5];
    uint64_t length;                    /* bytes so far */
    uint8_t block[64];
    size_t used;
};
void sha1_init(struct sha1 *s);
void sha1_update(struct sha1 *s, const void *data, size_t n);
void sha1_final(struct sha1 *s, uint8_t out[SHA1_SIZE]);
void sha1(const void *data, size_t n, uint8_t out[SHA1_SIZE]);
void hmac_sha1(const void *key, size_t key_length, const void *data, size_t n,
               uint8_t out[SHA1_SIZE]);
void pbkdf2_sha1(const void *password, size_t password_length, const uint8_t *salt,
                 size_t salt_length, uint32_t iterations, uint8_t *out, size_t out_length);

/* IEEE 802.11's key derivations: the PRF (12.7.1.2: HMAC-SHA1 over the
 * label, a zero byte, the data and a counter; WPA2's keys) and KDF-SHA256
 * (12.7.1.7.2: HMAC-SHA256 over a counter, the label, the context and the
 * length in bits, both 16-bit little-endian; WPA3's). out_length bytes, at
 * most IEEE80211_KDF_MAX; data/context at most IEEE80211_DATA_MAX bytes. */
#define IEEE80211_KDF_MAX 128
#define IEEE80211_DATA_MAX 256
void ieee80211_prf(const void *key, size_t key_length, const char *label, const void *data,
                   size_t n, uint8_t *out, size_t out_length);
void ieee80211_kdf_sha256(const void *key, size_t key_length, const char *label,
                          const void *context, size_t n, uint8_t *out, size_t out_length);

/* AES key wrap (RFC 3394): n bytes of key data (a multiple of 8, at least
 * 16, at most AES_WRAP_MAX) into n + 8 bytes; unwrap takes the n + 8 and
 * gives n back, or false if the check value is wrong (then out is zeroed). */
#define AES_WRAP_MAX 256
void aes_key_wrap(const struct aes *kek, const uint8_t *in, size_t n, uint8_t *out);
bool aes_key_unwrap(const struct aes *kek, const uint8_t *in, size_t n, uint8_t *out);

/* AES-CMAC (RFC 4493): a 16-byte tag. WPA3's handshake check and the
 * protection of management frames (BIP) use it. */
void aes_cmac(const struct aes *a, const void *data, size_t n, uint8_t tag[16]);

/* AES-CCM (RFC 3610) with a 13-byte nonce (L = 2: messages up to 65,535
 * bytes) and a tag of tag_length bytes (4 to 16, even): CCMP, Wi-Fi's frame
 * protection, uses 8. open gives out only if the tag is right; if not
 * (false), out is zeroed. in and out may be the same. */
void aes_ccm_seal(const struct aes *a, const uint8_t nonce[13], const void *aad, size_t aad_n,
                  const void *in, void *out, size_t n, uint8_t *tag, size_t tag_length);
bool aes_ccm_open(const struct aes *a, const uint8_t nonce[13], const void *aad, size_t aad_n,
                  const void *in, void *out, size_t n, const uint8_t *tag, size_t tag_length);

/* SAE, WPA3-Personal's key exchange (802.11-2020 12.4, sae.c, 24b), group
 * 19 (P-256), constant-time where the password is involved. The password
 * element (PWE) by hunting and pecking (both stations' MAC addresses and the
 * password, SAE_HUNT_ROUNDS rounds always run) or by hash to element (sae_pt
 * once per network: its name, the password and an optional identifier;
 * then sae_pwe_from_pt per peer). sae_commit makes our scalar and element
 * (random rand and mask, or the given ones: tests); sae_process takes the
 * peer's (refused: a scalar outside (1, r), an element off the curve, our
 * own commit reflected) and makes KCK, PMK and PMKID. Points are x || y,
 * numbers big-endian. */
#define SAE_PASSWORD_MAX 256
#define SAE_HUNT_ROUNDS 40
struct sae {
    uint8_t pwe[64];
    uint8_t rand[32], scalar[32], element[64];
    uint8_t peer_scalar[32], peer_element[64];
    uint8_t kck[32], pmk[32], pmkid[16];
};
bool sae_pwe_hunt(struct sae *s, const uint8_t mac_a[6], const uint8_t mac_b[6],
                  const void *password, size_t n);
void sae_pt(uint8_t pt[64], const void *ssid, size_t ssid_n, const void *password, size_t n,
            const void *id, size_t id_n);
bool sae_pwe_from_pt(struct sae *s, const uint8_t pt[64], const uint8_t mac_a[6],
                     const uint8_t mac_b[6]);
bool sae_commit(struct sae *s, const uint8_t *rand, const uint8_t *mask);
bool sae_process(struct sae *s, const uint8_t peer_scalar[32], const uint8_t peer_element[64]);
/* Our confirm (send-confirm, our commit, then the peer's), and the check of
 * the peer's (its counter, its commit, then ours). */
void sae_confirm(const struct sae *s, uint16_t send_confirm, uint8_t out[32]);
bool sae_check_confirm(const struct sae *s, uint16_t peer_send_confirm, const uint8_t confirm[32]);
/* For tests (RFC 9380's vectors): SSWU of u (a number below p) and a sum. */
void p256_sswu(const uint8_t u[32], uint8_t point[64]);
bool p256_add(const uint8_t a[64], const uint8_t b[64], uint8_t sum[64]);
```

<a id="x509-h"></a>

### x509.h

`user/include/x509.h` (in RitterOS's tree) -- X.509 certificates: reading, chains to trusted roots, host names (libtls uses it); for the system's own.

X.509 certificates for TLS (step 23c; x509.c): what a certificate says, checked signatures, host names, chains to trusted roots, PEM bundles.

```c
enum { X509_KEY_RSA = 1, X509_KEY_P256, X509_KEY_P384 };
enum { X509_SIG_RSA_SHA256 = 1, X509_SIG_RSA_SHA384, X509_SIG_RSA_SHA512,
       X509_SIG_ECDSA_SHA256, X509_SIG_ECDSA_SHA384, X509_SIG_ECDSA_SHA512 };
enum { X509_OK = 0, X509_ERR_EMPTY, X509_ERR_NAME, X509_ERR_TIME, X509_ERR_NOT_CA,
       X509_ERR_CHAIN, X509_ERR_SIGNATURE, X509_ERR_UNTRUSTED };

/* One certificate; the pointers point into its DER bytes (kept by the caller). */
struct x509 {
    const uint8_t *der, *tbs, *sig, *issuer, *subject, *san;
    size_t der_len, tbs_len, sig_len, issuer_len, subject_len, san_len;
    int sig_alg, key_type;
    const uint8_t *rsa_n, *rsa_e, *ec_point;
    size_t rsa_n_len, rsa_e_len, ec_point_len;
    int64_t not_before, not_after;              /* seconds since 1970 */
    int is_ca, path_len, name_constraints;
};

int x509_parse(struct x509 *c, const uint8_t *der, size_t len);         /* 1, or 0: refused */
int x509_signed_by(const struct x509 *child, const struct x509 *parent);
int x509_host_matches(const struct x509 *c, const char *host);
/* The server's chain (its own first), the trusted roots, the host name and
 * the time now (host 0: any name; now 0: any time): X509_OK or what is
 * wrong (x509_error). */
int x509_verify_chain(const struct x509 *chain, int count, const struct x509 *roots, int nroots,
                      const char *host, int64_t now);
const char *x509_error(int e);
/* The next certificate of a PEM text (from *text, which moves on): its DER
 * bytes in out (at most size), their length; 0 at the end or if too long. */
size_t pem_next(const char **text, const char *end, uint8_t *out, size_t size);
```

<a id="ssh-h"></a>

### ssh.h

`user/include/ssh.h` (in RitterOS's tree) -- the SSH protocol (sshd, ssh); for the system's own.

SSH's transport (user/lib/ssh.c, RFC 4253), for sshd and ssh: the version exchange, binary packets, the key exchange curve25519-sha256 (RFC 8731) with an ssh-ed25519 host key, and the cipher chacha20-poly1305@openssh.com. Plus the wire format's pieces: numbers big-endian, strings with a 32-bit length in front.

```c
/* Message numbers (RFC 4250). */
enum {
    SSH_DISCONNECT = 1, SSH_IGNORE = 2, SSH_UNIMPLEMENTED = 3, SSH_DEBUG = 4,
    SSH_SERVICE_REQUEST = 5, SSH_SERVICE_ACCEPT = 6, SSH_EXT_INFO = 7,
    SSH_KEXINIT = 20, SSH_NEWKEYS = 21, SSH_KEX_ECDH_INIT = 30, SSH_KEX_ECDH_REPLY = 31,
    SSH_USERAUTH_REQUEST = 50, SSH_USERAUTH_FAILURE = 51, SSH_USERAUTH_SUCCESS = 52,
    SSH_USERAUTH_BANNER = 53, SSH_USERAUTH_PK_OK = 60,
    SSH_GLOBAL_REQUEST = 80, SSH_REQUEST_SUCCESS = 81, SSH_REQUEST_FAILURE = 82,
    SSH_CHANNEL_OPEN = 90, SSH_CHANNEL_OPEN_CONFIRMATION = 91, SSH_CHANNEL_OPEN_FAILURE = 92,
    SSH_CHANNEL_WINDOW_ADJUST = 93, SSH_CHANNEL_DATA = 94, SSH_CHANNEL_EXTENDED_DATA = 95,
    SSH_CHANNEL_EOF = 96, SSH_CHANNEL_CLOSE = 97, SSH_CHANNEL_REQUEST = 98,
    SSH_CHANNEL_SUCCESS = 99, SSH_CHANNEL_FAILURE = 100,
};

#define SSH_PACKET_MAX 35000            /* RFC 4253 6.1: at least 35000 */

/* A message being built. */
struct ssh_buf {
    uint8_t data[SSH_PACKET_MAX];
    size_t length;
};
void ssh_buf_init(struct ssh_buf *b, uint8_t type);
void ssh_put_u8(struct ssh_buf *b, uint8_t v);
void ssh_put_bool(struct ssh_buf *b, bool v);
void ssh_put_u32(struct ssh_buf *b, uint32_t v);
void ssh_put_string(struct ssh_buf *b, const void *data, size_t n);
void ssh_put_text(struct ssh_buf *b, const char *s);
void ssh_put_mpint(struct ssh_buf *b, const uint8_t *big_endian, size_t n);

/* A message being read: every get fails (and keeps failing: bad) at the
 * end of the data. */
struct ssh_reader {
    const uint8_t *data;
    size_t length, at;
    bool bad;
};
void ssh_reader_init(struct ssh_reader *r, const uint8_t *data, size_t n);
uint8_t ssh_get_u8(struct ssh_reader *r);
bool ssh_get_bool(struct ssh_reader *r);
uint32_t ssh_get_u32(struct ssh_reader *r);
/* A string: where it starts in the message, its length in *n. */
const uint8_t *ssh_get_string(struct ssh_reader *r, size_t *n);
/* A string as text (NUL-terminated, cut to size); false if too long. */
bool ssh_get_text(struct ssh_reader *r, char *out, size_t size);
/* Is the string equal to text? */
bool ssh_string_is(const uint8_t *s, size_t n, const char *text);

/* One side of a connection. */
struct ssh {
    int fd;                             /* the TCP socket */
    bool server;
    char ours[64], theirs[256];         /* version strings, without CR LF */
    uint8_t session_id[32];
    bool keyed;                         /* the first key exchange is done */
    bool encrypt_out, encrypt_in;
    uint8_t key_out[64], key_in[64];    /* chacha20-poly1305: main key, then header key */
    uint32_t seq_out, seq_in;
    bool strict;                        /* strict key exchange (OpenSSH, the Terrapin fix) */
    uint8_t host_seed[32], host_public[32];   /* the server's host key */
    uint8_t server_key[32];             /* the client: the host key the server showed */
    uint8_t in[16384];                  /* bytes received, not used yet */
    size_t in_at, in_length;
    uint8_t packet[SSH_PACKET_MAX + 64];   /* the last message received */
    size_t packet_length;
    volatile int send_lock;             /* two threads send (sshd: input and output) */
    char error[160];                    /* why it failed */
};

/* Exchange versions, then keys: the server signs with its host key, the
 * client checks the signature (and then compares server_key with what
 * it knows). 0, or -1 with s->error set. */
int ssh_start(struct ssh *s, int fd, bool server);

/* Send one message (encrypted once keys are there). 0 or -1. */
int ssh_send(struct ssh *s, const struct ssh_buf *b);

/* The next message into s->packet (IGNORE and DEBUG are skipped); its
 * type, or -1 (the connection ended or broke: s->error). */
int ssh_receive(struct ssh *s);

/* Send DISCONNECT with a reason (2: protocol error, 11: by application). */
void ssh_disconnect(struct ssh *s, uint32_t reason, const char *text);

/* "SHA256:" and the unpadded base64 of SHA-256 over a key's blob, as
 * OpenSSH shows fingerprints. */
void ssh_fingerprint(const uint8_t public_key[32], char *out, size_t size);
/* The blob of an Ed25519 public key: string "ssh-ed25519", string key. */
size_t ssh_key_blob(const uint8_t public_key[32], uint8_t out[51]);

/* Base64 (RFC 4648): the encoded length, or for decoding the decoded
 * length (-1 if it is not base64). */
size_t base64_encode(const uint8_t *in, size_t n, char *out, bool padding);
long base64_decode(const char *in, size_t n, uint8_t *out, size_t size);
```

<a id="gpt-h"></a>

### gpt.h

`user/include/gpt.h` (in RitterOS's tree) -- GPT partition tables (disks, osinstall); for the system's own.

GPT partition tables (user/lib/gpt.c), for programs that write them through /dev (disks, osinstall). Sector 0 a protective MBR, sector 1 the header, then 128 entries of 128 bytes; a copy of both at the end of the disk. Header and entries carry CRC-32 checksums. GUIDs are kept in the GPT's byte order (the first three groups little-endian).

```c
#define GPT_ENTRIES         128             /* a new table's entries */
#define GPT_ENTRY_SIZE      128
#define GPT_NAME_CHARS      36              /* UTF-16 characters in an entry's name */
#define GPT_TYPE_RITTEROS   "B5CA1B3E-6278-48F0-BBEF-87907326A8B3"
#define GPT_TYPE_EFI        "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"

struct gpt_header {
    char signature[8];                  /* "EFI PART" */
    uint32_t revision, header_size, header_crc, reserved;
    uint64_t my_lba, alternate_lba, first_usable, last_usable;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count, entry_size, entries_crc;
} __attribute__((packed));

struct gpt_entry {
    uint8_t type_guid[16], unique_guid[16];
    uint64_t first_lba, last_lba, attributes;
    uint16_t name[GPT_NAME_CHARS];
} __attribute__((packed));

/* A table being changed, as read from the disk (or made new). */
struct gpt_table {
    struct gpt_header h;
    uint8_t *entries;
    size_t entries_bytes;
    uint64_t entries_lba;               /* the first copy's entries (2 if made here) */
};

/* The disk's GPT (the copy at the end if the first is damaged); false if
 * neither is whole. */
bool gpt_read(int fd, const struct sys_device *disk, struct gpt_table *t);
/* A new, empty table that fills the disk, with a new random disk GUID. */
void gpt_empty(const struct sys_device *disk, struct gpt_table *t);
/* Write the table: the copy at the end first (entries, then header), then
 * the first one. */
bool gpt_write(int fd, const struct sys_device *disk, struct gpt_table *t);
/* The protective MBR: one partition of type 0xEE over the whole disk. */
bool gpt_write_protective_mbr(int fd, const struct sys_device *disk);
/* A protective MBR's partition over the whole disk again (after the GPT
 * was fitted to it); other MBRs are left alone. */
bool gpt_fit_protective_mbr(int fd, const struct sys_device *disk);

struct gpt_entry *gpt_entry_at(struct gpt_table *t, uint32_t i);
bool gpt_unused(const struct gpt_entry *e);
void gpt_set_name(struct gpt_entry *e, const char *name);   /* ASCII */
/* A random GUID (version 4). */
void gpt_new_guid(uint8_t *g);
/* "B5CA1B3E-6278-48F0-BBEF-87907326A8B3" to the GPT's bytes; false if it
 * is not one. */
bool gpt_parse_guid(const char *s, uint8_t *g);
```

<a id="mkfs-h"></a>

### mkfs.h

`user/include/mkfs.h` (in RitterOS's tree) -- making file systems (format, osinstall); for the system's own.

Making file systems on a device opened for writing (/dev/NAME): the writers that format and osinstall share (user/lib/mkfs*.c). Each takes the open device, writes everything and returns false (errno set) when a write fails. Sizes are the device's bytes; hidden is where the volume begins on its disk, in the disk's blocks (FAT and exFAT keep it).

```c
#define MKFS_CLEAR          (64 * 1024)     /* cleared first: other file systems' marks */
#define EXFAT_LABEL_MAX     11              /* UTF-16 units */

/* RitterFS: 0 if one fits bytes, else why not ("is too small ..."). */
const char *mkfs_ritterfs_check(uint64_t bytes);
bool mkfs_ritterfs(int fd, const char *label, bool whole_disk, uint64_t device_bytes,
                   uint64_t now);

/* FAT32. */
struct fat_layout {
    uint32_t sector, cluster_sectors;   /* bytes per sector; sectors per cluster */
    uint64_t sectors;                   /* the whole volume */
    uint32_t fat_sectors, clusters;
};
/* The layout for bytes with sectors of sector bytes; false if FAT32 does
 * not fit (too small or too large). */
bool mkfs_fat_plan(uint64_t bytes, uint32_t sector, struct fat_layout *l);
/* Up to 11 letters, digits, spaces or !#$%&'()-@^_`{}~ (kept in upper case). */
bool mkfs_fat_label_valid(const char *label);
bool mkfs_fat32(int fd, const struct fat_layout *l, const char *label, uint64_t hidden,
                bool whole_disk, uint64_t device_bytes, uint64_t now);

/* exFAT. */
struct exfat_layout {
    uint32_t sector, sector_shift, cluster_shift, cluster;
    uint64_t sectors;                       /* the volume's length */
    uint32_t fat_offset, fat_length, heap_offset, clusters;
    uint32_t bitmap_clusters, upcase_clusters;  /* from cluster 2: bitmap, table, root */
};
bool mkfs_exfat_plan(uint64_t bytes, uint32_t sector, struct exfat_layout *l);
/* A label in UTF-16 (up to 11 characters of the Basic Multilingual Plane,
 * none of "*:<>?\|/ or control characters): the units, or -1. */
int mkfs_exfat_label(const char *label, uint16_t *out);
bool mkfs_exfat(int fd, const struct exfat_layout *l, const uint16_t *label, int label_units,
                uint64_t hidden, bool whole_disk, uint64_t device_bytes);

/* What the writers share. */
bool mkfs_write(int fd, uint64_t offset, const void *data, size_t n);
bool mkfs_zero(int fd, uint64_t offset, uint64_t n);
void mkfs_put16(uint8_t *p, uint32_t v);
void mkfs_put32(uint8_t *p, uint32_t v);
void mkfs_put64(uint8_t *p, uint64_t v);
```

<a id="windowproto-h"></a>

### windowproto.h

`user/include/windowproto.h` (in RitterOS's tree) -- the window server's protocol (window.h speaks it); for the system's own.

The window protocol between windowd and its clients (step 15e). Clients connect to the local socket WINDOW_SOCKET and exchange fixed-size messages; a window's pixels travel as a shared buffer passed over the same socket. libwindow (15f) will wrap this; for now clients use it directly.  Creating a window: the client makes a shared buffer of width*height*4 native pixels, sends it with ritteros_share_send, then sends WMSG_CREATE with the size. windowd receives the buffer, shows the window, and replies WMSG_CREATED with its id. windowd then sends WMSG_POINTER and WMSG_KEY as input arrives for the window.

```c
#define WINDOW_SOCKET   "/run/windowd"

/* client -> windowd */
#define WMSG_CREATE     1       /* a: width, b: height, c: title length (a shared buffer was
                                 * just sent; c bytes of title follow this message) 
                                 * (41f-5) | (the parent's id + 1) << 16 for a dialog
                                 * (41g-3) | 1 << 31: after the title a uint8 length
                                 * and the program's name (getprogname: its app) */
#define WMSG_CREATE_APP (1u << 31)
#define WMSG_DAMAGE     5       /* the window's pixels changed: please recomposite */
#define WMSG_BUFFER     7       /* a: width, b: height; a new buffer (sent just before) replaces
                                 * the window's, at the new size (the client's reply to RESIZE) */
#define WMSG_SET_MENU   9       /* c: length of a menu-bar blob that follows (see below); sets
                                 * the window's menu bar (c = 0 clears it) */
#define WMSG_POPUP      11      /* a: x, b: y (in the window), c: length of an item blob that
                                 * follows; windowd pops up a context menu there */
#define WMSG_CLIP_SET   15      /* c: length of the text that follows (UTF-8, up to
                                 * WINDOW_CLIPBOARD_MAX): the clipboard now holds it (15w) */
#define WMSG_CLIP_GET   16      /* please send the clipboard (WMSG_CLIP_TEXT) */
/* windowd -> client */
#define WMSG_CREATED    2       /* a: the window id */
#define WMSG_POINTER    3       /* a: x in the window, b: y in the window, c: buttons */
#define WMSG_KEY        4       /* a: the byte */
#define WMSG_CLOSE      6       /* the user clicked the close box: please close the window */
#define WMSG_RESIZE     8       /* a: width, b: height; please become this content size
                                 * (reply with a new buffer via WMSG_BUFFER, or ignore) */
#define WMSG_MENU       10      /* a: the id of the menu item the user chose */
#define WMSG_KEYCODE    14      /* a: a physical key's USB HID usage ID, b: 1 pressed, 0 released
                                 * (15p; beside WMSG_KEY's bytes) */
#define WMSG_SCROLL     13      /* a: wheel steps (int32, negative up), b, c: the pointer in the
                                 * window: the wheel turned over it (15o) */
#define WMSG_DOUBLE_CLICK 12    /* a: x, b: y in the window: the left button's second press
                                 * there within the double_click time (windowd decides, 15n) */
#define WMSG_CLIP_TEXT  17      /* c: length of the clipboard's text that follows (the answer
                                 * to WMSG_CLIP_GET; 0: empty) */
#define WMSG_FOCUS      18      /* (41f-1) a: 1 the window got the keyboard (it is the front one),
                                 * 0 it lost it; also once when the window is made */
#define WMSG_LIMITS     19      /* (41f-2) a: the content's least width | height << 16, b: its
                                 * greatest (0: none); a resize or a maximize keeps to them */
#define WMSG_HOVER      20      /* (41f-3) a: 1 the pointer came over the window's content, 0 it
                                 * left it (for another window, a frame, the desktop, a menu) */
#define WMSG_CURSOR     21      /* (41f-4) a: the pointer's shape over the content (WINDOW_CURSOR_*) */
#define WMSG_TEXT       22      /* (41f-4) a: a typed character (Unicode), sent after its WMSG_KEY bytes */
#define WMSG_FULLSCREEN 23      /* (41f-6) a: 1 the whole screen without a frame, 0 back (a
                                 * WMSG_RESIZE follows, as for the user's resize) */
#define WMSG_MINIMIZE   24      /* (41f-6) off to a taskbar button (the user brings it back) */
#define WMSG_DRAG       25      /* (41f-6) c: length of the text that follows (up to
                                 * WINDOW_CLIPBOARD_MAX): a drag starts, while the left button is
                                 * held (else ignored) */
#define WMSG_DROP       26      /* (41f-6) windowd -> client: a: x, b: y in the window where a
                                 * drag was let go, c: length of its text that follows */
#define WMSG_NOTIFY     27      /* (41g-4) a: title length (< WINDOW_TITLE_MAX), b: text length
                                 * (< WINDOW_NOTIFY_MAX), the title and text follow: a
                                 * notification, shown at the top right for 5 s; also the one
                                 * message of a connection without a window (`notify`) */
#define WMSG_SHOT       28      /* (41h-2) the one message of a connection without a window
                                 * (`wshot`): windowd answers WMSG_SHOT (a: width, b: height,
                                 * c: red_shift | green_shift << 8 | blue_shift << 16), then
                                 * sends a shared buffer holding a copy of the whole screen
                                 * (width x height pixels, rows width apart) */
#define WMSG_INPUT      29      /* (41h-3) a connection without a window (`wsend`) sends these
                                 * one after another, each a keyboard or pointer event (a
                                 * struct sys_input_event: a: type | buttons << 8 | key << 16 |
                                 * modifiers << 24, b: x | y << 16) windowd handles as from the
                                 * devices; it answers each with WMSG_INPUT, a: 1 taken, 0
                                 * refused (only a headless windowd takes them) */
#define WMSG_WINDOWS    30      /* (41h-6) the one message of a connection without a window
                                 * (`wsend wait TITLE`): windowd answers WMSG_WINDOWS (a: how
                                 * many windows, c: the length of the text that follows), then
                                 * their titles, each ending in a newline */

/* A menu-bar blob (WMSG_SET_MENU): uint8 n_menus, then per menu
 *   uint8 title_len, title bytes, uint8 n_items, then per item
 *     uint16 id (little-endian), uint8 label_len, label bytes.
 * A pop-up blob (WMSG_POPUP) is one menu's items: uint8 n_items, then the items.
 * Ids are the client's own; windowd sends the chosen one back in WMSG_MENU. */
#define WINDOW_MENU_MAX   6     /* top-level menus per window */
#define WINDOW_MENU_ITEMS 16    /* items per menu */
#define WINDOW_LABEL_MAX  32    /* a menu label, with its NUL */

#define WINDOW_TITLE_MAX 64     /* a title, with its NUL */
#define WINDOW_CLIPBOARD_MAX 65536  /* the clipboard's text, bytes (15w) */
#define WINDOW_NOTIFY_MAX 512   /* a notification's text, with its NUL (41g-4) */

struct wmsg {
    uint32_t type;
    uint32_t a, b, c;
};
```

<a id="common-syscalls-h"></a>

### common/syscalls.h

`common/syscalls.h` (in RitterOS's tree) -- system call numbers, flags and structures, shared with the kernel; for apps.

System calls: how programs ask the kernel for something. Shared by the kernel (kernel/process.c) and the programs' library (user/lib).  x86-64:  number in RAX, arguments in RDI, RSI, RDX, R10, R8, R9; instruction "syscall"; result in RAX (RCX and R11 are lost) AArch64: number in X8, arguments in X0-X5; instruction "svc #0"; result in X0  The meaning follows Unix: a negative result is an error, -ERR_*. The numbers are RitterOS's own, the same on both architectures.

```c
#define SYS_EXIT        0       /* exit(code): end the program; code 0-255 */
#define SYS_WRITE       1       /* write(fd, buffer, size): bytes written */
#define SYS_READ        2       /* read(fd, buffer, size): bytes read, 0 at the end */
#define SYS_OPEN        3       /* open(path, flags, mode): a new file descriptor; mode (with
                                 * OPEN_MODE in flags, 52b) a created file's permissions,
                                 * less the process's umask (52h); without it 0644 */
#define SYS_CLOSE       4       /* close(fd): 0 */
#define SYS_SPAWN       5       /* spawn(path, argv, flags, fds, ids): the new child's ID */
#define SYS_WAIT        6       /* wait(pid or -1, &status, flags): the ended child's ID */
#define SYS_KILL        7       /* kill(pid, signal): 0 */
#define SYS_GETPID      8       /* getpid(): the caller's process ID */
#define SYS_SLEEP       9       /* sleep(milliseconds): 0 */
#define SYS_STAT        10      /* stat(path, struct sys_stat *): 0 */
#define SYS_READDIR     11      /* readdir(fd, struct sys_dirent *): 1, or 0 at the end */
#define SYS_CHDIR       12      /* chdir(path): 0 */
#define SYS_GETCWD      13      /* getcwd(buffer, size): length */
#define SYS_GETPGRP     14      /* getpgrp(): the caller's process group */
#define SYS_FOREGROUND  15      /* foreground(group or 0): the terminal's foreground group */
#define SYS_SIGNAL      16      /* signal(signal, action): 0 */
#define SYS_SYSINFO     17      /* sysinfo(struct sys_info *): 0 */
#define SYS_POWER       18      /* power(POWER_OFF or POWER_REBOOT): returns only if it failed;
                                   power(POWER_TABLE, index, buffer, size) (37b-1),
                                   power(POWER_NAMES, buffer, size) (37b-2),
                                   power(POWER_EVAL, path, buffer, size) (37b-3) */
#define SYS_REPORT      19      /* report(REPORT_*, buffer, size): the report's full length */
#define SYS_SEEK        20      /* seek(fd, offset, SEEK_*): the new position */
#define SYS_THREAD_CREATE 21    /* thread_create(start, fn, arg, stack size or 0): the new
                                 * thread's number (54d: the size, up to USER_STACK_MAX
                                 * bytes, 0 for USER_STACK_SIZE) */
#define SYS_THREAD_EXIT 22      /* thread_exit(value): end the calling thread */
#define SYS_THREAD_JOIN 23      /* thread_join(number, &value): 0 once it has ended */
#define SYS_THREAD_SELF 24      /* thread_self(): the caller's thread number (0: main) */
#define SYS_SNAPSHOT    25      /* snapshot(SNAPSHOT_*, name, buffer, size): see below */
#define SYS_ATTR        26      /* attr(ATTR_*, path, key, buffer, size): see below */
/* Changing the file system: each call is written to the disk at once
 * (one commit), all or nothing. Paths in /.snapshots are read-only. */
#define SYS_MKDIR       27      /* mkdir(path, mode): 0; mode: its rwxrwxrwx bits */
#define SYS_RMDIR       28      /* rmdir(path): 0; the directory must be empty */
#define SYS_UNLINK      29      /* unlink(path): 0; a file, not a directory */
#define SYS_RENAME      30      /* rename(from, to): 0; to must not exist */
#define SYS_COPY        31      /* copy(from, to): 0; an instant copy (shares the data) */
/* Users: every process has a user and a group (IDs; 0 is root, who may do
 * everything). Files have an owner, a group and rwxrwxrwx permissions. */
#define SYS_GETUID      32      /* getuid(): the caller's (real) user ID: who started it */
#define SYS_GETGID      33      /* getgid(): its group ID */
#define SYS_SETUID      34      /* setuid(uid): 0; root only (or its own ID) */
#define SYS_SETGID      35      /* setgid(gid): 0; root only (or its own ID) */
#define SYS_CHMOD       36      /* chmod(path, mode): 0; the owner or root */
#define SYS_CHOWN       37      /* chown(path, uid, gid): 0; root only */
#define SYS_RANDOM      38      /* random(buffer, size): size (at most 256) random bytes */
#define SYS_ECHO        39      /* echo(on): 0; the terminal shows typed text, or not
                                 * (passwords); the foreground job only; on again
                                 * when the program ends */
#define SYS_TERMINAL    40      /* terminal(TERMINAL_*, argument): see below */

/*
 * The terminal for full-screen programs (an editor):
 *   TERMINAL_RAW   argument 1: reads return the bytes as typed, at least
 *                  one, without echo or line editing (keys like the arrows
 *                  arrive as VT100 escape sequences, ESC [ A); 2: the same
 *                  for a program that edits a line itself (the shell): the
 *                  console shows its line again after a message, as it
 *                  does its own; 0: lines again. The foreground job only;
 *                  lines again when the program ends.
 *   TERMINAL_SIZE  the text area: rows << 16 | columns (80x24 without a
 *                  screen)
 * Output understands a few VT100 sequences: ESC [ row ; col H, ESC [ J
 * (0: to the end of the screen, 2: all), ESC [ K (0: to the end of the
 * line, 2: all of it), ESC [ A/B/C/D (cursor moves), ESC [ m (0 normal,
 * 1 bold, 7 reverse, 22, 27, 30-37 and 90-97 colors, 39), ESC [ ? 25 l/h
 * (cursor hidden/shown).
 */
#define TERMINAL_RAW    1
#define TERMINAL_SIZE   2
#define TERMINAL_ISATTY 3       /* terminal(TERMINAL_ISATTY, fd): 1 if fd is a terminal, else 0 */

/*
 * Devices: disks and their partitions, as /dev lists them. /dev/NAME is
 * root's only, read and written as raw bytes (a byte position like a
 * file's). Writing is refused where a mounted file system lies (EBUSY: a
 * mounted partition cannot be opened for writing, its blocks on the disk
 * not written); closing a written device reads the disk's partition table
 * again (new RitterFS partitions are mounted).
 */
#define SYS_DEVICE      41      /* device(index, struct sys_device *): 1, or 0 past the last */
#define SYS_MOUNT       42      /* mount(device, path or 0, buffer, size): 0; the path used
                                 * (0: /volumes/<label>) into buffer; root only */
#define SYS_UNMOUNT     43      /* unmount(path or device): 0; root only; EBUSY while a
                                 * program has a file open there or its directory there */
#define SYS_TIME        44      /* time(): nanoseconds since 1970-01-01 UTC (0: unknown,
                                 * e.g. with noruntime) */
/*
 * The network. Addresses are IPv4, in host byte order (10.0.2.15 is
 * 0x0A00020F).
 */
#define SYS_NETSET      45      /* netset(struct sys_netset *): 0; the card's IPv4
                                 * settings (address 0: none); root only */
#define SYS_PING        46      /* ping(address, id << 16 | seq, size, timeout ms): the
                                 * round trip of an ICMP echo in microseconds */
/* 47 was resolve(name): DNS is the elf dnsd since step 44c */
/* Sockets: file descriptors for network conversations (read and write
 * work on connected ones: TCP's stream, UDP's datagrams to and from the
 * peer; close ends them). Ports and addresses as numbers, no structs. */
#define SYS_SOCKET      48      /* socket(SOCKET_UDP, SOCKET_TCP or SOCKET_LOCAL; | SOCKET_INET6):
                                 * a descriptor */
#define SYS_BIND        49      /* bind(fd, port or 0): 0; the port used. A local socket:
                                 * bind(fd, name, length) names the listener (0 or ERR_*) */
#define SYS_CONNECT     50      /* connect(fd, address, port): 0 (TCP: when connected). A local
                                 * socket: connect(fd, name, length) to a listener's name */
#define SYS_SENDTO      51      /* sendto(fd, buffer, size, address, port): bytes sent */
#define SYS_RECVFROM    52      /* recvfrom(fd, buffer, size, &from, timeout ms or 0):
                                 * bytes; from: address << 16 | port (a uint64_t) */
#define SYS_LISTEN      53      /* listen(fd, backlog): 0 (TCP) */
#define SYS_ACCEPT      54      /* accept(fd, &from): a new descriptor for a connection */

#define SYS_PTY         55      /* a pseudo-terminal: PTY_OPEN (int fds[2]: master, slave),
                                   PTY_SIZE (fd, rows, cols) */
#define PTY_OPEN        1
#define PTY_SIZE        2
#define SYS_BINDCARD    57      /* bindcard(fd, "eth0"): 0; a UDP socket of that card
                                 * only, before bind: it receives what arrives on
                                 * the card, and sends broadcasts out of it (also
                                 * without an address); root only */
#define SYS_NETINFO     58      /* netinfo(struct sys_netinfo *): 0; a card's MAC,
                                 * link and settings */
#define SYS_GUARD       59      /* guard(GUARD_*, struct sys_guard *): the firewall,
                                 * see below */
#define SYS_LOG         60      /* log(LOG_*, ...): the logs, see below; root only */
#define SYS_EFIVAR      61      /* efivar(EFIVAR_GET|EFIVAR_SET, struct sys_efivar *): a
                                 * firmware variable, through UEFI's runtime services
                                 * (boot entries); root only, see below */
#define SYS_FORWARD     56      /* forward(FORWARD_*): forwarding packets between network
                                   cards; ON and OFF root only; the state (1: on) */
#define FORWARD_OFF     0
#define FORWARD_ON      1
#define FORWARD_ASK     2
#define SYS_MASQUERADE  62      /* masquerade(MASQ_*, char *name): mark the private cards for
                                   source NAT (49i); root only (net shows the state) */
#define MASQ_RESET      0       /* clear the private set */
#define MASQ_ADD        1       /* mark the card named *name private */
#define SYS_DEFAULT_ROUTE 102   /* default_route(DEFAULT_ROUTE_*, char *name) (50g): the cards
                                   whose gateway carries the default route, in order (the
                                   first with a link, an address and a gateway; none of them:
                                   the first other card with a gateway); root only (net shows
                                   them). A name may be a card not there now */
#define DEFAULT_ROUTE_RESET 0   /* no preference: the first card with a gateway */
#define DEFAULT_ROUTE_ADD   1   /* the card named *name next in the order */
/* IPv6's socket calls (64c): as their IPv4 forms, with struct sys_inaddr
 * (either family) for the address. A socket made with SOCKET_INET6 talks
 * to IPv6 and IPv4 peers (::ffff:a.b.c.d); the IPv4 calls on it see IPv4
 * peers' addresses and 0 for IPv6 ones. */
#define SYS_CONNECT6    103     /* connect6(fd, struct sys_inaddr *): 0 (TCP: when connected) */
#define SYS_SENDTO6     104     /* sendto6(fd, buffer, size, struct sys_inaddr * or 0 (the
                                   connected peer), 0, flags): bytes sent */
#define SYS_RECVFROM6   105     /* recvfrom6(fd, buffer, size, struct sys_inaddr * (out) or 0,
                                   timeout ms or 0, flags): bytes */
#define SYS_ACCEPT6     106     /* accept6(fd, struct sys_inaddr * (out) or 0): a descriptor */
#define SYS_SOCKNAME6   107     /* sockname6(fd, SOCKNAME_OWN or _PEER, struct sys_inaddr *):
                                   0; an IPv6 socket's own "any" address is :: */
#define SYS_NETSET6     109     /* netset6(struct sys_netset6 *): 0; a card's IPv6 settings
                                   (64b: address6, gateway6, dns6); root only. The same
                                   settings again change nothing (the addresses stay) */
#define NETSET6_AUTO    0       /* addresses from the network's routers (SLAAC); a card's
                                   start */
#define NETSET6_OFF     1       /* no IPv6 on the card */
#define NETSET6_FIXED   2       /* the settings' address (and gateway); routers ignored */
#define NETSET6_DHCP    3       /* (64g) addresses from DHCPv6 (the elf dhclient), none from
                                   the routers' prefixes; their router, routes, MTU kept */
#define SYS_NETLEASE6   111     /* netlease6(struct sys_netlease6 *): 0; (64g) an address and
                                   DNS server DHCPv6 gave a card (dhclient), or with
                                   NETLEASE6_DROP its end; root only. Checked for duplicates
                                   as any address; a card set to OFF or FIXED takes none */
#define NETLEASE6_DROP  1       /* the lease is over: its address and DNS server go */
#define SYS_SWAP        112     /* swap(SWAP_*, struct sys_swap *): 0 (35a) */
#define SWAP_STATUS     0       /* fill the struct: on, its pages, used, written, read */
#define SWAP_ON         1       /* turn swap on with /system/swap (made a fixed file first);
                                   root only; ERR_NOENT without the file, ERR_INVAL if it has
                                   holes, a part block or blocks shared with a snapshot */
#define SWAP_OFF        2       /* turn it off, reading the pages in it back first (35b);
                                   root only; ERR_NOMEM when they do not fit in memory */
#define SYS_TAP         113     /* tap(TAP_*, value) (38a): virtual network cards; root only */
#define TAP_ADD         0       /* a new tapN for user `value` (below 2^24; its /dev/tapN is
                                   that user's and root's): N; ERR_NOSPC when every
                                   tap0..tap63 is taken. (38c) With (N + 1) << 24 in `value`
                                   tapN itself (ERR_EXIST: it is there) */
#define TAP_REMOVE      1       /* remove tap number `value`: 0; ERR_NOENT, or ERR_BUSY while
                                   a program has its /dev/tapN open */
#define SYS_BRIDGE      114     /* bridge(BRIDGE_*, number, char *card) (38b): bridges; root only */
#define BRIDGE_ADD      0       /* the card named *card becomes a port of bridge brN (number
                                   -1: of a new bridge, with the card's MAC; (38c) -2 - N: of
                                   a new bridge brN itself, ERR_EXIST if it is there): N; ERR_NOENT (no
                                   such bridge or card), ERR_BUSY (a port already), ERR_INVAL
                                   (a bridge, lo), ERR_NOTSUP (it cannot take other machines'
                                   frames: Wi-Fi), ERR_NOSPC (br0..br7 or 16 ports taken) */
#define BRIDGE_REMOVE   1       /* the card leaves bridge brN; card 0: the bridge goes, every
                                   port leaving: 0; ERR_NOENT */
#define SYS_SESSION_STAMP 115   /* session_stamp(STAMP_*) (68e): sudo's memory, on the caller's
                                   session leader, for the caller's real user */
#define STAMP_AGE       0       /* its age in seconds; ERR_NOENT when there is none */
#define STAMP_SET       1       /* now (root only: sudo is setuid root): 0 */
#define STAMP_FORGET    2       /* gone (sudo -k): 0 */
#define SYS_GROW        117     /* grow(char *partition) (68j): the RitterFS mounted on the
                                   partition takes the room its partition grew by (disks
                                   grow): its block count of 4 KiB now (unchanged without
                                   new room); root only; ERR_NOENT (no such device),
                                   ERR_INVAL (no RitterFS mounted there), ERR_NOSPC, ERR_ROFS,
                                   ERR_IO */
#define SYS_UNAME       116     /* uname(struct sys_utsname *) (68i): 0; the system's name,
                                   release (VERSION), commit and machine */
#define SYS_NETINFO6    110     /* netinfo6(struct sys_netinfo6 *): 0; a card's IPv6
                                   addresses, router, DNS server and MTU (64d), by name
                                   or by place as netinfo */
#define SYS_PING6       108     /* ping6(struct sys_inaddr *, id << 16 | seq, size, timeout ms)
                                   (64a): as SYS_PING, to an IPv6 address (an IPv4 one as
                                   ::ffff:a.b.c.d too); a link-local or multicast one needs
                                   its card in `scope` (ERR_INVAL without) */
#define SYS_SHARE       63      /* shared memory (window system): share(SHARE_*, ...) */
#define SHARE_CREATE    0       /* share(SHARE_CREATE, size): a new buffer; its descriptor */
#define SHARE_MAP       1       /* share(SHARE_MAP, fd): map it into this program; the address */
#define SHARE_SIZE      2       /* share(SHARE_SIZE, fd): the size it was made with */
#define SHARE_SEND      3       /* share(SHARE_SEND, socket_fd, buffer_fd): pass it over a
                                 * local socket (fd-passing); 0 */
#define SHARE_RECV      4       /* share(SHARE_RECV, socket_fd): take a passed buffer; its
                                 * descriptor, or -ERR_AGAIN if none waits */
#define SHARE_MAX       (16 * 1024 * 1024)      /* a shared buffer's largest size */
#define SYS_DISPLAY     64      /* the screen (window system): display(DISPLAY_*, ...) */
#define DISPLAY_ACQUIRE 0       /* display(DISPLAY_ACQUIRE): park the console; this program owns
                                 * the screen (root, or the user at the console; one at a
                                 * time); 0, ERR_PERM or ERR_BUSY */
#define DISPLAY_RELEASE 1       /* display(DISPLAY_RELEASE): give the screen back to the console */
#define DISPLAY_INFO    2       /* display(DISPLAY_INFO, struct sys_display *): the size and format */
#define DISPLAY_FLUSH   3       /* display(DISPLAY_FLUSH, struct sys_display_flush *): copy a
                                 * rectangle of native pixels to the screen (owner only) */
#define DISPLAY_EVENT   4       /* display(DISPLAY_EVENT, struct sys_input_event *, timeout_ms):
                                 * the next pointer or keyboard event for the display owner
                                 * (1: one, 0: timed out, -ERR_*). While the display is owned,
                                 * the keyboard goes here instead of to the terminal. */
#define DISPLAY_MODES   5       /* (65e-34) display(DISPLAY_MODES, struct sys_display_modes *):
                                 * the screen's mode and refresh, the monitor (its EDID) and the
                                 * modes the firmware can set; anyone may ask */
#define DISPLAY_SET_MODE 6      /* (66b-2) display(DISPLAY_SET_MODE, struct sys_display_set_mode *):
                                 * show an output's monitor in a mode it lists; root or the user
                                 * at the machine's screen; a program owning the screen gets
                                 * INPUT_SCREEN (66b-5). `said` gets what was set or why not */
#define DISPLAY_VBLANK  7       /* (66c-3) display(DISPLAY_VBLANK, uint64_t *frame, timeout_ms):
                                 * wait for the screen's next vertical blank (1, the frame
                                 * number in *frame; 0: timed out; -ERR_NODEV: no driver tells
                                 * it); anyone may */
#define DISPLAY_BUFFERS 8       /* (66d-2) display(DISPLAY_BUFFERS, struct sys_display_buffers *):
                                 * the screen buffers mapped into the screen's owner (write-
                                 * combining: write, never read), how many, their size and
                                 * stride; the same places after a mode change (ask again for the
                                 * size); gone when it releases the screen. -ERR_NODEV where no
                                 * driver has them */
#define DISPLAY_FLIP    9       /* (66d-1) display(DISPLAY_FLIP, buffer, uint64_t *frame): show
                                 * screen buffer N (0: the console's) from the next vertical
                                 * blank; returns once it shows (1; its frame in *frame; 0: it
                                 * did not within 100 ms). The screen's owner only; -ERR_NODEV
                                 * where no driver flips. DISPLAY_BUFFERS (8) maps them (66d-2) */
#define SYS_PIPE        65      /* pipe(int fds[2]): a pipe; fds[0] reads, fds[1] writes. A read
                                 * gives 0 once every write end is closed, a write -ERR_PIPE once
                                 * every read end is closed. Binary-transparent. */
#define SYS_MMAP        66      /* mmap(addr, length, MMAP_* access, MMAP_* flags, fd, offset):
                                 * the mapping's address (see below) */
#define SYS_MUNMAP      67      /* munmap(addr, length): 0; the pages of mappings in the range go */
#define SYS_MPROTECT    68      /* mprotect(addr, length, MMAP_* access): 0; the mappings in the
                                 * range (all of it mapped) get that access (22f) */
#define SYS_MSYNC       69      /* msync(addr, length, MSYNC_* flags): 0; what was written through
                                 * the writable shared mappings in the range goes to the files
                                 * (22j; munmap and the end of the program do it too) */
#define MSYNC_ASYNC      1      /* (all three write back at once) */
#define MSYNC_INVALIDATE 2
#define MSYNC_SYNC       4
#define SYS_THREAD_POINTER 70   /* thread_pointer(address): 0; the calling thread's thread
                                 * pointer (x86 FS base, ARM TPIDR_EL0), where its per-thread
                                 * data is found (22i) */
#define SYS_WLAN        71      /* wlan(WLAN_* op, card name, buffer, length, value): a
                                 * wireless card's radio and link, for the elf wifid (24c;
                                 * root only): the result of the op, or -ERR_* */
#define SYS_PEER_UID    72      /* peer_uid(fd): the user at the other end of a connected
                                 * local socket (ERR_NOTCONN otherwise) */
#define SYS_GETEUID     73      /* geteuid(): the effective user ID, whose rights apply (a
                                 * setuid program's: its file's owner; 50b) */
#define SYS_SPAWN_ENV   74      /* spawn_env(path, argv, envp, flags, fds, ids): SYS_SPAWN
                                 * with an environment (22t: an array of "NAME=value"
                                 * strings ended by a null pointer, on the child's stack
                                 * after its arguments) */
#define SYS_CPUTIME     75      /* cputime(): the CPU time the calling program has run, all
                                 * its threads, in nanoseconds (22o: C's clock) */
#define SYS_SIGACTION   76      /* sigaction(signal, struct sys_sigaction *new or 0, *old or 0,
                                 * restorer): a signal's handler (22r; restorer: where a
                                 * handler returns to, the library's call of SYS_SIGRETURN) */
#define SYS_SIGRETURN   77      /* sigreturn(context): back from a handler to where the
                                 * program was (the registers the kernel kept at context) */
#define SYS_FUTEX       78      /* futex(address, FUTEX_* op, value, timeout_ns): the C
                                 * library's mutexes and condition variables (22y). WAIT:
                                 * if the 32-bit word at address holds value, sleep until
                                 * woken (0), timeout_ns passed (0: no limit; -ERR_TIMEDOUT)
                                 * or a signal (-ERR_INTR); -ERR_AGAIN if it held another.
                                 * WAKE: wake up to value of the program's threads waiting
                                 * there; how many. Words are the program's own (its
                                 * threads share them; never across programs). */

#define SYS_EXECVE      92      /* execve(path, argv, envp): the calling process runs another
                                 * program (55b): same process number, parent, group, session,
                                 * user, folder, umask and descriptors (FD_CLOEXEC ones closed),
                                 * the blocked and ignored signals kept, handled ones back to
                                 * the default, its other threads ended. Returns only on an
                                 * error, the old program untouched; one later (no memory)
                                 * ends the process (SIGKILL) */
#define SYS_FORK        93      /* fork() (56a): a new process, the caller's child, that is a
                                 * copy of it -- its memory (copy-on-write; MAP_SHARED memory
                                 * stays shared), descriptors (the same open files), folder,
                                 * umask, users, group, session, signal handlers and the
                                 * calling thread's blocked set; no pending signals, and only
                                 * the calling thread (same number). The child's ID, and 0 in
                                 * the child; shared buffers (SYS_SHARE) are not passed on */
#define SYS_GETPPID     94      /* getppid() (56b): the parent's process ID (0: the kernel's
                                 * child, or an orphan) */
#define SYS_SETPGID     99      /* setpgid(pid, group) (56b): pid (0: the caller) joins group
                                 * (0: its own ID, a new group) of its session; the caller or
                                 * a child that has not run execve (EACCES), never a session
                                 * leader (EPERM). group -1: the process's group (getpgid) */
#define SYS_SETSID      100     /* setsid() (56b): a new session and group led by the caller,
                                 * no terminal; EPERM if it leads a group. Its ID */
#define SYS_UTIMENS     101     /* utimens(path or 0, fd, struct sys_timespec times[2] or 0,
                                 * flags) (63m): a file's times, [0] the last access (taken,
                                 * not kept: RitterFS has none), [1] the last change; the path
                                 * (UTIMENS_NOFOLLOW: a link itself) or, path 0, the open fd.
                                 * nsec UTIMENS_NOW: now, UTIMENS_OMIT: left as it is; times 0:
                                 * both now. Now needs the owner, write permission or root
                                 * (EACCES); any other time the owner or root (EPERM) */
#define SYS_SYMLINK     95      /* symlink(target, path) (57a): a symbolic link at path (the
                                 * name itself, never followed) naming target, 1-255 bytes,
                                 * kept as given; EPERM where there are none (FAT, exFAT) */
#define SYS_READLINK    96      /* readlink(path, buffer, size): the bytes a symbolic link
                                 * names, not zero-terminated, at most size; how many.
                                 * EINVAL for anything but a link */
#define SYS_LSTAT       97      /* lstat(path, struct sys_stat *): stat of the name itself
                                 * (a link: FILE_TYPE_SYMLINK, its target's length) */
#define SYS_LINK        98      /* link(from, to): a second name for the object at from (a
                                 * link itself, not what it names), on the same volume (EXDEV);
                                 * not a folder (EPERM); the caller owns it, or may read and
                                 * write it, or is root (EPERM) */
#define SYS_THREAD_INTERRUPT 91 /* thread_interrupt(number, THREAD_INTERRUPT_*, entry): 0, ESRCH
                                 * (54c, pthread_cancel): a thread of the calling program.
                                 * WAKE: its waiting call returns ERR_INTR. ENTER: as well, on
                                 * its next way back to the program it runs entry(0) as if a
                                 * signal handler (its own stack, all signals blocked) */
#define THREAD_INTERRUPT_WAKE  0
#define THREAD_INTERRUPT_ENTER 1
#define SYS_FCNTL       90      /* fcntl(fd, FCNTL_*, arg): a descriptor's flags (53b): see below */
#define SYS_SHUTDOWN    87      /* shutdown(fd, SHUTDOWN_*): 0; a connection's sending or
                                 * receiving side ended, the descriptor kept (53a) */
#define SYS_SOCKNAME    88      /* sockname(fd, SOCKNAME_OWN or _PEER, struct sys_sockname *):
                                 * 0; a socket's own or its peer's address (53a) */
#define SYS_SOCKOPT     89      /* sockopt(fd, SOCKOPT_GET or _SET, SOCKOPT_* option,
                                 * uint64_t *value): 0 (53a) */
#define SYS_SIGPENDING  85      /* sigpending(uint64_t *out): the signals waiting that the
                                 * calling thread blocks (52i): 0 */
#define SYS_SIGSUSPEND  86      /* sigsuspend(const uint64_t *mask): block mask instead until a
                                 * signal it lets through comes (52i): -ERR_INTR, after that
                                 * signal's handler ran (then the old set again) */
#define SYS_UMASK       84      /* umask(mask): the old one; mask's bits (0777) are taken off
                                 * the modes of files and folders the process makes (52h);
                                 * passed on to the programs it starts, 022 at the start */
#define SYS_SIGPROCMASK 83      /* sigprocmask(SIGMASK_*, const uint64_t *set or 0, uint64_t *old
                                 * or 0): the calling thread's blocked signals (52f): 0. A
                                 * signal every thread blocks waits until one lets it through */
#define SYS_TERMIOS     82      /* termios(fd, TERMIOS_*, struct sys_termios *): a terminal's
                                 * modes (52e; tcgetattr, tcsetattr, tcflush): 0, -ERR_NOTTY
                                 * (not a terminal), -ERR_PERM (setting: not its foreground) */
#define SYS_POLL        81      /* poll(struct sys_pollfd *, count, timeout_ms): how many
                                 * descriptors have something to report (52d), waiting up to
                                 * timeout_ms (-1: no limit, 0: not at all) for the first;
                                 * -ERR_INTR on a signal, -ERR_INVAL past POLL_MAX */
#define SYS_FSTAT       80      /* fstat(fd, struct sys_stat *): 0; stat of an open descriptor
                                 * (52b): a file's as stat gives it, now; a terminal, pipe or
                                 * socket by its type (FILE_TYPE_CHAR, _FIFO, _SOCKET) */
#define SYS_DUP2        79      /* dup2(old, new): descriptor new refers to old's open file
                                 * too (one position, as POSIX's dup2; 52a), new closed first if
                                 * it was open; new -1: the lowest free descriptor (dup). The
                                 * new descriptor, -ERR_BADF (old not open, new out of range)
                                 * or -ERR_MFILE (none free). old == new: new, nothing done. */

/* termios (52e, termios.h). The kernel keeps what a program set and acts
 * on: ICANON (off: bytes as typed, no line editing, no echo; VMIN and
 * VTIME then say how a read waits), ECHO (lines only), ISIG (Ctrl-C a
 * signal, else a byte), ICRNL (without ICANON: Enter read as \n). The
 * other flags are kept but change nothing. Numbers are Linux's, speeds
 * plain numbers (BSD's). A process that changed ICANON, ECHO or ISIG gets
 * the old ones back when it ends. */
#define TERMIOS_GET     0
#define TERMIOS_SET     1       /* at once (TCSANOW; TCSADRAIN alike: output never waits) */
#define TERMIOS_SET_FLUSH 2     /* TCSAFLUSH: typed input not yet read is dropped first */
#define TERMIOS_FLUSH   3       /* tcflush: drop typed input not yet read */
#define TERMIOS_ISIG    0000001 /* c_lflag */
#define TERMIOS_ICANON  0000002
#define TERMIOS_ECHO    0000010
#define TERMIOS_ECHOE   0000020
#define TERMIOS_ECHOK   0000040
#define TERMIOS_ECHONL  0000100
#define TERMIOS_NOFLSH  0000200
#define TERMIOS_TOSTOP  0000400
#define TERMIOS_IEXTEN  0100000
#define TERMIOS_BRKINT  0000002 /* c_iflag */
#define TERMIOS_INPCK   0000020
#define TERMIOS_ISTRIP  0000040
#define TERMIOS_INLCR   0000100
#define TERMIOS_IGNCR   0000200
#define TERMIOS_ICRNL   0000400
#define TERMIOS_IXON    0002000
#define TERMIOS_OPOST   0000001 /* c_oflag */
#define TERMIOS_ONLCR   0000004
#define TERMIOS_CSIZE   0000060 /* c_cflag */
#define TERMIOS_CS8     0000060
#define TERMIOS_CREAD   0000200
#define TERMIOS_PARENB  0000400
#define TERMIOS_VINTR   0       /* c_cc */
#define TERMIOS_VQUIT   1
#define TERMIOS_VERASE  2
#define TERMIOS_VKILL   3
#define TERMIOS_VEOF    4
#define TERMIOS_VTIME   5       /* tenths of a second */
#define TERMIOS_VMIN    6
#define TERMIOS_VSTART  8
#define TERMIOS_VSTOP   9
#define TERMIOS_VSUSP   10
#define TERMIOS_NCCS    32

struct sys_termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t cc[TERMIOS_NCCS];
    uint32_t ispeed, ospeed;
};

/* poll (52d, poll.h): events asked for and found per descriptor, Linux's
 * numbers. POLLERR, POLLHUP and POLLNVAL (not an open descriptor) are
 * reported whether asked for or not; a negative fd is skipped. */
#define POLLIN          0x001   /* reading would not wait (data, or the end) */
#define POLLPRI         0x002   /* (never reported) */
#define POLLOUT         0x004   /* writing would not wait */
#define POLLERR         0x008   /* an error: writing to a pipe nobody reads */
#define POLLHUP         0x010   /* the other side is gone */
#define POLLNVAL        0x020
#define POLLRDNORM      0x040   /* as POLLIN */
#define POLLWRNORM      0x100   /* as POLLOUT */
#define POLL_MAX        256     /* descriptors in one call (52k: as many as a program has) */

struct sys_pollfd {
    int32_t fd;
    int16_t events, revents;
};

#define FUTEX_WAIT      0
#define FUTEX_WAKE      1

/* mmap (22a, sys/mman.h): access is MMAP_READ, _WRITE, _EXEC (never write
 * and execute at once; 0: a reservation, nothing mapped); flags are
 * MMAP_SHARED or MMAP_PRIVATE, with MMAP_ANONYMOUS (zeroed memory, no file)
 * and MMAP_FIXED (exactly at addr, replacing mappings there; addr then must
 * be in the mmap window). A read-only mapping of a RitterFS file shares one
 * copy of its pages with every other mapping of the same version of the
 * file; a private writable one shares them until a page's first write
 * copies it (22e). Anonymous pages are made when first touched; a file's
 * pages are read when mapped; touching a page past the file's end is
 * SIGBUS. Writable shared file mappings are not there yet (-ERR_NOTSUP). */
#define MMAP_READ       1
#define MMAP_WRITE      2
#define MMAP_EXEC       4
#define MMAP_SHARED     0x01
#define MMAP_PRIVATE    0x02
#define MMAP_FIXED      0x10
#define MMAP_ANONYMOUS  0x20
#define MMAP_WINDOW_BASE 0x0000300000000000ULL  /* mappings go here, above the shared buffers */
#define MMAP_WINDOW_TOP  0x0000700000000000ULL  /* and below the stacks */

/* An input event delivered to the display owner (DISPLAY_EVENT). */
#define INPUT_POINTER   1       /* the pointer moved or a button changed */
#define INPUT_KEY       2       /* a key: `key` is the byte the terminal would have received */
#define INPUT_WHEEL     3       /* the mouse wheel turned: `key` holds the steps as a signed byte,
                                 * negative up (away from the user); x, y the pointer (15o) */
#define INPUT_KEYCODE   4       /* a physical key pressed (buttons 1) or released (buttons 0):
                                 * `key` is its USB HID usage ID, modifiers too (15p) */
#define INPUT_SCREEN    5       /* (66b-5) the screen has a new mode (DISPLAY_SET_MODE): x, y
                                 * its width and height; DISPLAY_INFO gives the rest */
#define INPUT_BLANK     6       /* (37e) `key` 1: the screen goes off after a while without
                                 * input (draw nothing until 0); 0: it is on again */
struct sys_input_event {
    uint8_t type;               /* INPUT_POINTER or INPUT_KEY */
    uint8_t buttons;            /* INPUT_POINTER: buttons held (MOUSE_* bits) */
    uint8_t key;                /* INPUT_KEY: the byte */
    uint8_t modifiers;          /* INPUT_POINTER: keys held (INPUT_CTRL...) on any keyboard */
    uint16_t x, y;              /* INPUT_POINTER: the pointer, in pixels */
};
#define MOUSE_LEFT      0x01    /* buttons bits, as a USB/PS2 mouse reports them */
#define MOUSE_RIGHT     0x02
#define MOUSE_MIDDLE    0x04
#define INPUT_CTRL      0x01    /* modifiers bits: left or right Ctrl, Shift, Alt */
#define INPUT_SHIFT     0x02
#define INPUT_ALT       0x04

/* The screen's size and pixel format (DISPLAY_INFO). Pixels are 32-bit; a
 * color is (r << red_shift) | (g << green_shift) | (b << blue_shift). */
struct sys_display {
    uint32_t width, height;     /* visible pixels */
    uint32_t stride;            /* pixels from one row to the next */
    uint8_t bytes_per_pixel;    /* always 4 */
    uint8_t red_shift, green_shift, blue_shift;
};

/* A rectangle to copy to the screen (DISPLAY_FLUSH): `w` x `h` native pixels
 * at (x, y) on the screen, read from the program's buffer at `pixels`, whose
 * rows are `stride` bytes apart. */
struct sys_display_flush {
    uint64_t pixels;
    uint32_t stride;
    uint32_t x, y, w, h;
};

/* (65e-34) The screen as the firmware and the monitor describe it
 * (DISPLAY_MODES). Refresh rates in millihertz (59950 is 59.95 Hz), 0
 * where the monitor's EDID does not give one; without an EDID only `now`'s
 * size and the firmware's modes are known. */
#define DISPLAY_MODES_MAX 32
#define DISPLAY_EDID_MAX  512
struct sys_display_mode {
    uint32_t width, height, refresh_mhz;
};
struct sys_display_modes {
    struct sys_display_mode now;        /* the screen's mode, its refresh as the EDID lists it */
    struct sys_display_mode native;     /* the monitor's preferred mode (its first timing) */
    char name[14];                      /* the monitor's name, "" if unknown */
    uint16_t edid_size;                 /* bytes in edid; 0: the firmware gave none */
    uint32_t mode_count;                /* the modes the firmware can set */
    struct sys_display_mode modes[DISPLAY_MODES_MAX];   /* refresh as the EDID lists the size */
    uint8_t edid[DISPLAY_EDID_MAX];     /* as the firmware gave it (base and extension blocks) */
};

/* (66d-2) DISPLAY_BUFFERS' answer: buffer k at address[k], width x height
 * pixels in the screen's format (DISPLAY_INFO), rows `stride` bytes apart. */
#define DISPLAY_BUFFERS_MAX 2
struct sys_display_buffers {
    uint32_t count;
    uint32_t width, height, stride;
    uint64_t address[DISPLAY_BUFFERS_MAX];
};

/* (66b-2) DISPLAY_SET_MODE's request and answer. */
struct sys_display_set_mode {
    char output[8];                     /* "hdmi0" (displays lists them) */
    uint32_t width, height;
    uint32_t refresh_mhz;               /* 0: the monitor's usual rate for that size */
    char said[128];                     /* (out) "hdmi0 now 1920x1080 at 60.000 Hz ...", or why not */
};

/* (53a) shutdown's sides, sockname's which, socket options (SOCKOPT_*:
 * the value a uint64_t; times in milliseconds, 0: no limit). A receive
 * or send that runs past its time is ERR_AGAIN. REUSEADDR, KEEPALIVE,
 * NODELAY and BROADCAST are kept and change nothing (a port is only
 * taken by a listener; segments go out at once; broadcasts are allowed);
 * ERROR reads 0, TYPE the SOCKET_* kind, the buffers 65536. */
/* (53b) fcntl: FCNTL_DUPFD (the lowest free descriptor from arg on;
 * _CLOEXEC marks it), GETFD/SETFD (FD_CLOEXEC: kept, nothing to do it
 * yet: spawn passes only 0-2 on), GETFL (OPEN_READ, _WRITE or _READ_WRITE,
 * OPEN_APPEND, OPEN_NONBLOCK), SETFL (OPEN_APPEND and OPEN_NONBLOCK; the
 * open file's, shared by its descriptors). Linux's numbers. sendto's and
 * recvfrom's last argument may hold SOCKET_DONTWAIT (MSG_DONTWAIT). */
#define FCNTL_DUPFD     0
#define FCNTL_GETFD     1
#define FCNTL_SETFD     2
#define FCNTL_GETFL     3
#define FCNTL_SETFL     4
#define FCNTL_DUPFD_CLOEXEC 1030
#define FCNTL_CLOEXEC   1
#define SOCKET_DONTWAIT 1
#define SHUTDOWN_READ   0
#define SHUTDOWN_WRITE  1
#define SHUTDOWN_BOTH   2
#define SOCKNAME_OWN    0
#define SOCKNAME_PEER   1
#define SOCKOPT_GET     0
#define SOCKOPT_SET     1
#define SOCKOPT_RCVTIMEO 1
#define SOCKOPT_SNDTIMEO 2
#define SOCKOPT_REUSEADDR 3
#define SOCKOPT_KEEPALIVE 4
#define SOCKOPT_NODELAY 5
#define SOCKOPT_BROADCAST 6
#define SOCKOPT_ERROR   7
#define SOCKOPT_TYPE    8
#define SOCKOPT_RCVBUF  9
#define SOCKOPT_SNDBUF  10
#define SOCKOPT_V6ONLY  11      /* (64d) an IPv6 socket that takes IPv6 peers only */
#define SOCKET_UDP      1
#define SOCKET_TCP      2
#define SOCKET_INET6    0x100   /* (64c) with SOCKET_UDP or SOCKET_TCP: an IPv6 socket */
#define SOCKET_LOCAL    3       /* a local (Unix-domain) stream socket, named by a path-like
                                 * string in a kernel registry (not a file on the disk) */
#define LOCAL_PATH_MAX  108     /* a local socket's name, with its NUL (like Unix sun_path) */

struct sys_sockname {           /* (53a) sockname's answer */
    uint32_t type;              /* SOCKET_* */
    uint32_t address;           /* IPv4, host order (0: any, or none yet) */
    uint16_t port;
    uint16_t reserved;
    char name[LOCAL_PATH_MAX];  /* a local socket's: its listener's name ("": none) */
};

#define NETSET_LEASED   1       /* netset flags: the address came from DHCP (lease) */

struct sys_netset {
    char name[8];               /* "eth0" */
    uint32_t address, prefix;   /* 10.0.2.15 and 24 */
    uint32_t gateway, dns;      /* 0: none */
    uint32_t dns_port;          /* 0: 53 (another for tests) */
    uint32_t flags;             /* NETSET_* */
    uint32_t lease;             /* NETSET_LEASED: the lease's seconds (shown by net) */
};

/*
 * The firewall (guard; roadmap item 12). The kernel checks every packet
 * that arrives for the machine on a card or leaves it on one (lo never;
 * forwarded packets not yet) against a table of rules: the first rule
 * that matches decides, else the default of its direction. Packets of a
 * connection a rule allowed (both ways, also its replies) pass without
 * the rules. The program guardd loads the table from /system/config/guard;
 * until then (GUARD_STATE_BOOT) what comes in is dropped.
 *   GUARD_LOAD    the table in *g (state, in_default, out_default, count,
 *                 rules); root only; its counters start at 0
 *   GUARD_OFF     no checks at all (everything passes); root only
 *   GUARD_STATUS  the table as loaded, with its counters and the last
 *                 packets dropped (newest first), into *g
 *   GUARD_SET6    (64e) IPv6's own table (struct sys_guard6: count, rules);
 *                 root only; the defaults are GUARD_LOAD's. Until it is
 *                 set, IPv6 packets are judged by the IPv4 table's rules
 *                 that name no address (64c's interim)
 *   GUARD_GET6    IPv6's table as set, its counters and drops, into *g6
 * The rules file holds both families' rules: one without addresses goes
 * into both tables, one with an IPv4 or IPv6 address into its family's.
 * Neighbour discovery, router advertisements, MLD and ICMPv6's errors
 * always pass; ICMPv6 counts as icmp.
 */
#define GUARD_LOAD      1
#define GUARD_OFF       2
#define GUARD_STATUS    3
#define GUARD_SET6      4
#define GUARD_GET6      5

#define GUARD_ALLOW     1       /* actions */
#define GUARD_DROP      2
#define GUARD_LIMIT     3       /* allow, at most limit new connections a minute */
#define GUARD_IN        1       /* directions */
#define GUARD_OUT       2
#define GUARD_FORWARD   3       /* a packet passing between two cards (49h) */
#define GUARD_ANY_CARD  0xFF
#define GUARD_WLAN_CARD 0x80    /* (50k) a card byte with this bit: wlanN (N in the low bits), else ethN */
/* (38b) What kind of card a card byte names (a rule's kinds[], a drop's
 * card_kind): */
#define GUARD_KIND_ETH  0       /* ethN or wlanN, as the card byte says (as before 38b) */
#define GUARD_KIND_TAP  1       /* tapN, N the card byte */
#define GUARD_KIND_BRIDGE 2     /* brN, N the card byte */
#define GUARD_RULES     64
#define GUARD_DROPS     20

#define GUARD_STATE_OFF     0   /* no checks */
#define GUARD_STATE_ON      1   /* the loaded table */
#define GUARD_STATE_BOOT    2   /* none loaded yet: what comes in is dropped */

struct sys_guard_rule {
    uint8_t action, direction;
    uint8_t protocol;           /* 0: any; 1 ICMP, 6 TCP, 17 UDP */
    uint8_t card;               /* N of ethN, GUARD_WLAN_CARD | N of wlanN, or GUARD_ANY_CARD
                                 * (forward: the incoming card) */
    uint16_t port_low, port_high;   /* the local port (in) or the remote one (out); 0, 0: any */
    uint32_t from, from_prefix; /* the sender's address and prefix length (0: any) */
    uint32_t to, to_prefix;     /* the receiver's */
    uint32_t limit;             /* GUARD_LIMIT: new connections a minute */
    uint8_t out_card;           /* forward: the outgoing card, or GUARD_ANY_CARD */
    uint8_t kinds[3];           /* (38b) GUARD_KIND_* of card ([0]) and out_card ([1]); [2] 0 */
    uint64_t packets;           /* status: packets it decided */
};

struct sys_guard_drop {
    uint64_t time;              /* nanoseconds since 1970 (0: the clock is unknown) */
    uint32_t from, to;
    uint16_t port;              /* the local port (in) or the remote one (out) */
    uint8_t direction, protocol, card;
    uint8_t rule;               /* 1-based, or 0: the default */
    uint16_t card_kind;         /* (38b) GUARD_KIND_* of card */
};

/* (64e) IPv6's rules: as sys_guard_rule's, with 16-byte addresses. */
struct sys_guard6_rule {
    uint8_t action, direction;
    uint8_t protocol;           /* 0: any; 1 ICMP (ICMPv6), 6 TCP, 17 UDP */
    uint8_t card;               /* as sys_guard_rule's */
    uint16_t port_low, port_high;
    uint8_t from[16], to[16];   /* network order */
    uint8_t from_prefix, to_prefix;     /* 0-128; 0: any */
    uint8_t out_card;
    uint8_t number;             /* its place among the file's rules, 1 the first */
    uint8_t both;               /* 1: it names no address, so the IPv4 table has it too */
    uint8_t kinds[3];           /* (38b) as sys_guard_rule's */
    uint32_t limit;
    uint64_t packets;           /* status: IPv6 packets it decided */
};

struct sys_guard6_drop {
    uint64_t time;
    uint8_t from[16], to[16];
    uint16_t port;
    uint8_t direction, protocol, card;
    uint8_t rule;               /* the rule's number in the file, or 0: the default */
    uint16_t card_kind;         /* (38b) GUARD_KIND_* of card */
};

struct sys_guard6 {
    uint16_t count;             /* rules */
    uint16_t reserved;
    uint32_t drop_count;        /* status: entries in drops */
    uint64_t in_allowed, in_dropped, out_allowed, out_dropped;  /* status: by the defaults */
    struct sys_guard6_rule rules[GUARD_RULES];
    struct sys_guard6_drop drops[GUARD_DROPS];
};

struct sys_guard {
    uint32_t state;             /* GUARD_STATE_* (status) */
    uint8_t in_default, out_default;    /* GUARD_ALLOW or GUARD_DROP */
    uint16_t count;             /* rules */
    uint64_t in_allowed, in_dropped, out_allowed, out_dropped;  /* status: by the defaults */
    uint64_t connection_packets;    /* status: passed as part of an allowed connection */
    uint64_t fwd_allowed, fwd_dropped;  /* status: forwarded packets by the default (49h) */
    uint32_t drop_count;        /* status: entries in drops */
    uint8_t forward_default;    /* GUARD_ALLOW or GUARD_DROP, for forwarded packets (49h) */
    uint8_t reserved[3];
    struct sys_guard_rule rules[GUARD_RULES];
    struct sys_guard_drop drops[GUARD_DROPS];
};

/*
 * Logs: the kernel keeps lines in memory, one ring per log, each line
 * with its time in front, until the elf logd takes them and writes them
 * to /system/logs/NAME.log. "kernel" holds the kernel's own messages.
 *   LOG_AS     log(LOG_AS, name): what the caller (and the children it
 *              starts from now on) print on the console also goes to the
 *              log name ("": none). elvesd names its elves' logs so.
 *   LOG_WRITE  log(LOG_WRITE, name, text): add text to the log name
 *   LOG_TAKE   log(LOG_TAKE, buffer, size): what the logs hold, taken, as
 *              struct sys_log_record + its text, one after another; the
 *              bytes used
 */
#define LOG_AS          1
#define LOG_WRITE       2
#define LOG_TAKE        3
#define LOG_NAME_SIZE   16

struct sys_log_record {
    char name[LOG_NAME_SIZE];
    uint32_t length;            /* bytes of text that follow */
    uint32_t lost;              /* bytes dropped before them (the log was full) */
};

/*
 * Firmware variables (SYS_EFIVAR, 48c): GET fills attributes, size and data
 * (ERR_NOENT: none of that name; ERR_2BIG: more than EFIVAR_DATA_MAX bytes,
 * size says how many); SET writes them (size 0 deletes the variable).
 * ERR_NOSYS without runtime services (noruntime, or the firmware refused
 * SetVirtualAddressMap), ERR_IO for the firmware's other errors.
 */
#define EFIVAR_GET          1
#define EFIVAR_SET          2
#define EFIVAR_DATA_MAX     4096
#define EFIVAR_NON_VOLATILE 0x1     /* attributes: kept across restarts */
#define EFIVAR_BOOT_ACCESS  0x2     /* seen by the firmware's boot manager */
#define EFIVAR_RUNTIME      0x4     /* seen after ExitBootServices */

struct sys_efivar {
    char name[64];              /* ASCII: "BootOrder", "Boot0003" */
    uint8_t guid[16];           /* the vendor's GUID, as the firmware keeps it */
    uint32_t attributes;
    uint32_t size;              /* bytes of data */
    uint8_t data[EFIVAR_DATA_MAX];
};

/* An Internet address and port (64a; roadmap item 29, IPv6): the system
 * calls that came with IPv6 take this for both families, an IPv4 address
 * as ::ffff:a.b.c.d (family 2 or 10, as AF_INET and AF_INET6). */
struct sys_inaddr {
    uint16_t family;            /* 2 IPv4, 10 IPv6 */
    uint16_t port;              /* host byte order; 0 where there is none (ping) */
    uint32_t scope;             /* the card for a link-local or multicast address: its
                                 * index in the kernel's list + 1 (netinfo's index + 1,
                                 * as BSD's interface index); 0: none */
    uint8_t address[16];        /* network order, as on the wire */
};

struct sys_netset6 {
    char name[8];               /* "eth0" */
    uint32_t mode;              /* NETSET6_* */
    uint32_t prefix;            /* NETSET6_FIXED: the address's prefix length (1-128) */
    uint8_t address[16];        /* NETSET6_FIXED's address */
    uint8_t gateway[16];        /* NETSET6_FIXED's router (0: none) */
    uint8_t dns[16];            /* a DNS server instead of the router's (0: the router's) */
    uint32_t dns_port;          /* 0: 53 */
};

/* SYS_UNAME's answer (68i): POSIX's struct utsname, the same layout
 * (sys/utsname.h). Each field is a string. */
#define SYS_UTSNAME_LENGTH 65
struct sys_utsname {
    char sysname[SYS_UTSNAME_LENGTH];   /* "RitterOS" */
    char nodename[SYS_UTSNAME_LENGTH];  /* "ritteros" (no host names yet) */
    char release[SYS_UTSNAME_LENGTH];   /* the VERSION file's: "2026.11" */
    char version[SYS_UTSNAME_LENGTH];   /* the commit built from ("513e2a4", + changed), or "unknown" */
    char machine[SYS_UTSNAME_LENGTH];   /* "x86_64" or "aarch64" */
};

struct sys_netinfo6 {
    char name[8];               /* "eth0" (in); empty: the card at `index` (filled in) */
    uint16_t index;             /* (in) as sys_netinfo's; ERR_NODEV past the last */
    uint16_t mode;              /* NETSET6_* */
    uint32_t count;             /* addresses in use or being checked below */
    struct {
        uint8_t address[16];
        uint8_t prefix;
        uint8_t state;          /* 1 being checked for duplicates, 2 in use, 3 a duplicate */
        uint8_t origin;         /* 1 link-local, 2 from the router, 3 fixed */
        uint8_t reserved;
        uint32_t valid, preferred;  /* seconds left; 0xFFFFFFFF: for ever */
    } addresses[4];
    uint8_t router[16];         /* the default router (a router's or the gateway; 0: none) */
    uint8_t dns[16];            /* the DNS server: the settings', else the router's (0: none) */
    uint32_t dns_port;          /* 0: 53 */
    uint32_t mtu;               /* the router's (0: the card's) */
};

/* (64g) sys_netinfo6's mode also holds the router's flags (its last
 * advertisement): NETINFO6_MANAGED, addresses from DHCPv6; NETINFO6_OTHER,
 * other settings (the DNS server) from DHCPv6. */
#define NETINFO6_MANAGED    0x100
#define NETINFO6_OTHER      0x200

struct sys_netlease6 {
    char name[8];               /* "eth0" */
    uint32_t flags;             /* NETLEASE6_DROP */
    uint32_t valid, preferred;  /* the address's lifetimes in seconds (0xFFFFFFFF: for ever) */
    uint8_t address[16];        /* the leased address (0: only the DNS server: stateless) */
    uint8_t dns[16];            /* the DNS server DHCPv6 named (0: none) */
};

struct sys_netinfo {
    char name[8];               /* "eth0" (in); empty: the card at `index` (filled in) */
    uint8_t mac[6];
    uint16_t index;             /* with an empty name: the card's place in the kernel's
                                 * list, 0 first (in); ERR_NODEV past the last */
    uint32_t link_speed;        /* Mbit/s; 0: no link */
    uint32_t address, prefix, gateway, dns, dns_port;
    uint32_t lease;             /* seconds; 0: a fixed address (or none) */
};

/* Wireless cards (24c, kernel/wlan.c): the elf wifid scans, joins and
 * runs WPA's handshake through SYS_WLAN; the kernel carries the data (802.11
 * data frames to and from Ethernet frames, CCMP with the keys wifid sets)
 * and gives wifid the management frames and the EAPOL frames it receives. */
#define WLAN_INFO       1       /* buffer: struct sys_wlan_info (out) */
#define WLAN_CHANNEL    2       /* value: the channel to listen and send on */
#define WLAN_SEND       3       /* buffer: an 802.11 management frame (no FCS); the kernel
                                 * gives it its sequence number */
#define WLAN_RECEIVE    4       /* buffer: a struct sys_wlan_frame, then the bytes (out);
                                 * value: how long to wait (ms); its length, or -ERR_TIMEDOUT */
#define WLAN_JOIN       5       /* buffer: struct sys_wlan_join: the access point's frames are
                                 * taken (the link is up, or with WLAN_JOIN_SECURED once the
                                 * pairwise key is set); aid 0 before associating, then again
                                 * with the AID once associated (the card's firmware is told) */
#define WLAN_LEAVE      6       /* the link is down, the keys are gone */
#define WLAN_KEY        7       /* buffer: struct sys_wlan_key */
#define WLAN_EAPOL      8       /* buffer: the destination (6 bytes), then an EAPOL frame,
                                 * sent as a data frame (protected once a pairwise key is set) */
#define WLAN_COUNTRY    10      /* buffer: a two-letter country code (the firmware then
                                 * picks the channels and power it may use); -ERR_NOTSUP
                                 * when the card has no such setting */
#define WLAN_SCAN       9       /* start a full scan on a card whose driver scans in its
                                 * firmware (the MT7921): the beacons it hears arrive through
                                 * WLAN_RECEIVE like any management frame. 0, or -ERR_NOTSUP
                                 * when the driver has no scan of its own (wifid then tunes the
                                 * channels and listens itself, as for the test card).
                                 * buffer (optional): a network's name (up to 32 bytes) to ask
                                 * for by name (an active scan: hidden networks answer it) */
#define WLAN_SIGNAL     11      /* the joined access point's signal, averaged over the frames
                                 * it sent us, as -dBm (55: -55 dBm), 0 if not known (wifid
                                 * looks for a better access point when it is weak) */

#define WLAN_FRAME_MAX      2400    /* an 802.11 frame's bytes, at most */
#define WLAN_CHANNELS_MAX   64

struct sys_wlan_info {
    uint8_t mac[6];
    uint8_t joined;             /* 1: the link is up */
    uint8_t channel;            /* the channel now */
    uint8_t bssid[6];           /* the access point joined */
    uint16_t aid;
    uint8_t channels;           /* how many the card has, in channel_list */
    uint8_t reserved[3];
    uint8_t channel_list[WLAN_CHANNELS_MAX];
    uint32_t rx_management, rx_eapol, rx_data, rx_replayed, rx_bad_mic, rx_dropped;
    uint32_t tx_data, tx_management;
};

#define WLAN_FRAME_MANAGEMENT 1 /* an 802.11 management frame as received */
#define WLAN_FRAME_EAPOL      2 /* an EAPOL frame's bytes (from source) */
#define WLAN_FRAME_SCAN_DONE  3 /* a WLAN_SCAN has ended (no bytes) */
struct sys_wlan_frame {
    uint16_t kind;
    uint16_t length;            /* the bytes after this header */
    uint8_t channel;            /* where it was received */
    int8_t signal;              /* dBm, if the card says (else 0) */
    uint8_t source[6];          /* (EAPOL: the sender) */
    uint8_t protected_frame;    /* (management: it came protected and was opened) */
    uint8_t reserved[3];
};

#define WLAN_JOIN_MFP     1     /* management frames are protected (WPA3, 802.11w) */
#define WLAN_JOIN_SECURED 2     /* WPA: the link is up only once the pairwise key is set */
#define WLAN_JOIN_POWERSAVE 4   /* the card may doze between beacons (powersave = on in
                                 * /system/config/wifi/radio) */
struct sys_wlan_join {
    uint8_t bssid[6];
    uint16_t aid;
    uint8_t channel;
    uint8_t flags;              /* WLAN_JOIN_* */
    uint8_t reserved[2];
};

#define WLAN_KEY_PAIRWISE 1     /* CCMP-128, for frames to and from the access point */
#define WLAN_KEY_GROUP    2     /* CCMP-128, for group frames from it (index 1-3) */
#define WLAN_KEY_IGTK     3     /* BIP-CMAC-128, group management frames (index 4-5) */
struct sys_wlan_key {
    uint8_t kind;               /* WLAN_KEY_* */
    uint8_t index;
    uint8_t length;             /* 16 */
    uint8_t reserved;
    uint8_t rsc[8];             /* the receive counter to start from (PN, little-endian) */
    uint8_t key[32];
};

#define DEVICE_PARTITION 1      /* flags: a partition (else a disk) */
#define DEVICE_SYSTEM    2      /* the disk with the system partition, or that partition */
#define DEVICE_EMPTY     4      /* its first 12 KiB are all zero */
#define DEVICE_READ_ONLY 8      /* mounted, but read only (its check found problems) */

#define DEVICE_TABLE_NONE 0     /* a disk's partition table */
#define DEVICE_TABLE_GPT  1
#define DEVICE_TABLE_MBR  2

struct sys_device {
    char name[32];              /* "nvme0n1", "usbdisk0p2", ... (/dev/NAME) */
    char disk[32];              /* a partition's disk; "" for a disk */
    char kind[16];              /* "NVMe", "SATA", "USB", "partition" */
    char model[48];             /* a disk's own name for itself */
    uint64_t block_size, block_count;
    uint32_t flags;             /* DEVICE_* */
    uint32_t number;            /* a partition's entry in the table (1: the first) */
    uint32_t table;             /* a disk's DEVICE_TABLE_* */
    uint32_t partitions;        /* a disk's partitions */
    uint64_t first_usable;      /* a disk's blocks that partitions may use (GPT: */
    uint64_t last_usable;       /* from its header; MBR: 1 to the end) */
    uint64_t first_block;       /* a partition's first block on its disk */
    uint8_t type_guid[16];      /* a GPT partition's type */
    uint32_t mbr_type;          /* an MBR partition's type byte */
    char type_name[32];         /* the type's name if known, else "" */
    char part_name[40];         /* a GPT partition's name */
    char filesystem[16];        /* "RitterFS", "FAT32", "FAT16", "FAT12", "exFAT", "NTFS",
                                 * "ext", "APFS", or "" (none found) */
    char label[65];             /* the file system's label */
    uint64_t generation;        /* RitterFS: the newest valid superblock's */
    char mounted_at[64];        /* "/" for the system partition; "" if not mounted */
};

/*
 * Threads of a program: up to PROCESS_THREADS share its memory and files;
 * number 0 is the one that ran main. thread_create starts a thread at the
 * library's `start` routine with the stack pointer at a 16-byte frame
 * holding fn and arg (fn first); it calls fn(arg) and thread_exit with the
 * result. exit() from any thread, a fault or a signal ends all of them;
 * the last thread's thread_exit ends the program with exit code 0.
 */
#define PROCESS_THREADS 256

/* Reports: the kernel's text, as its rescue console prints it. */
#define REPORT_PROCESSES 1
#define REPORT_THREADS   2
#define REPORT_PCI       3
#define REPORT_USB       4
#define REPORT_DISKS     5      /* what the program disks prints */
#define REPORT_NET       6      /* what the program net prints */
#define REPORT_MEMORY    7      /* the page cache (22g): mem's second line */
#define REPORT_CPUS      8      /* (65b) the processor's cores: what the program cpus prints */
#define REPORT_DISPLAYS  9      /* (66a) the screen, graphics devices, outputs, monitors: displays */
#define REPORT_DRIVERS   10     /* (67a) the kernel's modules, loaded or not and why: drivers */
#define REPORT_POWER     11     /* (37a) what the firmware offers for power management: power */
#define REPORT_DISPLAY_SLEEP 12 /* (37g-5c-2) what the graphics drivers kept over the last sleep:
                                 * displays sleep */

/*
 * RitterFS snapshots of the file system the program runs from. Each one is
 * readable (read-only) at /.snapshots/<name>/...
 *   SNAPSHOT_CREATE: keep the current state under name; returns the
 *                    generation (commit) it shows
 *   SNAPSHOT_DELETE: delete it; returns the blocks that became free
 *   SNAPSHOT_LIST:   fill buffer with struct sys_snapshot entries (as many
 *                    as fit in size bytes); returns how many there are
 * Errors: ERR_EXIST, ERR_NOENT, ERR_INVAL (a bad name), ERR_ROFS, ERR_IO.
 */
#define SNAPSHOT_CREATE 1
#define SNAPSHOT_DELETE 2
#define SNAPSHOT_LIST   3

struct sys_snapshot {
    char name[64];              /* zero-terminated */
    uint64_t generation;
    uint64_t created;           /* nanoseconds since 1970 */
};

/*
 * Attributes of a file or directory: key=value pairs (keys 1-63 bytes
 * without '=', values up to 1024 bytes of any content).
 *   ATTR_GET:    the value into buffer (at most size bytes); returns its
 *                full length
 *   ATTR_SET:    set key to buffer[0..size); 0. Written to the disk at once
 *   ATTR_REMOVE: remove key; 0. Written to the disk at once
 *   ATTR_LIST:   the keys into buffer, each followed by a NUL (as many as
 *                fit); returns the bytes all of them need (key unused)
 * Errors: ERR_NOENT (no such file), ERR_NODATA (no such attribute),
 * ERR_INVAL (a bad key or a too long value), ERR_ROFS (in a snapshot).
 */
#define ATTR_GET        1
#define ATTR_SET        2
#define ATTR_REMOVE     3
#define ATTR_LIST       4
#define ATTR_KEY_MAX    63
#define ATTR_VALUE_MAX  1024

#define SEEK_SET        0       /* seek: from the start */
#define SEEK_CUR        1       /* from the current position */
#define SEEK_END        2       /* from the end */

#define SPAWN_NEW_GROUP 1       /* spawn flag: the child leads a new process group */
#define SPAWN_FOREGROUND 2      /* with SPAWN_NEW_GROUP: its group gets the terminal
                                 * before it starts (as foreground() would allow) */
#define SPAWN_NEW_SESSION 4     /* with SPAWN_NEW_GROUP: the child leads a new session
                                 * (it may hand the terminal to its jobs) */
#define SPAWN_AS        8       /* root only: the child runs as the user and group in
                                 * spawn's 5th argument (uid | gid << 32) */
#define SPAWN_JOIN_GROUP 16     /* (51a) the child joins the process group in the flags'
                                 * upper 32 bits (flags | (uint64_t)group << 32): one
                                 * of the caller's session, so a pipeline is one job
                                 * (Ctrl-C reaches all its commands); not with
                                 * SPAWN_NEW_GROUP */
/* spawn's fds: 0, or the caller's descriptors that become the child's
 * standard input, output and error (int32 each; -1: the caller's own fd
 * 0, 1 or 2). Without them the child gets the caller's 0, 1 and 2. */
#define WAIT_NO_HANG    1       /* wait flag: return 0 at once if no child has ended */

/* signal() actions (SYS_SIGNAL, the call before 22r's SYS_SIGACTION; kept
 * for older programs). SIGNAL_IGNORE (SIGINT and SIGTERM only there): the
 * signal does not end the process; it only interrupts a waiting system
 * call (-ERR_INTR). */
#define SIGNAL_DEFAULT  0
#define SIGNAL_IGNORE   1

#define POWER_OFF       0
#define POWER_REBOOT    1
#define POWER_TABLE     2       /* (37b-1) ACPI table number index (the DSDT, the FACS, then
                                   the XSDT's list) copied whole or in part to buffer: its full
                                   length, ERR_NOENT past the last; root only */
#define POWER_NAMES     3       /* (37b-2) the AML namespace's objects as text lines (power
                                   names) into buffer, at most size bytes with a 0: the full
                                   length; root only */
#define POWER_EVAL      4       /* (37b-3) the AML object at the path (a string) evaluated, its
                                   value as text lines (power eval) into buffer: the full
                                   length; ERR_NOENT for no such object, ERR_IO when it failed;
                                   root only */
#define POWER_SLEEP     5       /* (37g-4) sleep (ACPI S3) for args[1] seconds (1 to a day),
                                   woken by the RTC's alarm; (37g-7) 0: until woken (the power
                                   button): 0 once awake again, ERR_INVAL for more than a day, ERR_IO when it did not sleep (the kernel's
                                   log says why: a driver that cannot); root only */

/*
 * foreground(group): make group (of the caller's session) the terminal's
 * foreground job, the one that reads input and gets Ctrl-C. Allowed for the
 * session's leader (the shell) and for the foreground job itself.
 * foreground(0) only returns the current foreground group.
 */

#define FILE_TYPE_FILE  1
#define FILE_TYPE_DIR   2
#define FILE_TYPE_BLOCK 3       /* a disk or partition in /dev */
#define FILE_TYPE_CHAR  4       /* (52b, fstat) a terminal, a pseudo-terminal, a sound device */
#define FILE_TYPE_FIFO  5       /* (52b) a pipe's end */
#define FILE_TYPE_SOCKET 6      /* (52b) a socket */
#define FILE_TYPE_SYMLINK 7     /* (57a) a symbolic link (lstat, readdir) */

struct sys_stat {
    uint64_t id;                /* RitterFS object ID */
    uint64_t size;              /* bytes */
    uint32_t type;              /* FILE_TYPE_* */
    uint32_t links;
    uint64_t created, modified; /* nanoseconds since 1970 */
    uint32_t uid, gid;          /* owner */
    uint32_t mode;              /* permissions: rwxrwxrwx bits */
    uint32_t device;            /* which file system: 1 the system's, then each volume
                                 * (a mount point differs from its parent); 0 for /dev */
};

/* (63m) SYS_UTIMENS's times. */
struct sys_timespec {
    int64_t sec;                /* since 1970 */
    int64_t nsec;               /* 0-999999999, or: */
};
#define UTIMENS_NOW     ((1L << 30) - 1)
#define UTIMENS_OMIT    ((1L << 30) - 2)
#define UTIMENS_NOFOLLOW 1      /* flags: a symbolic link itself */

struct sys_dirent {
    uint64_t id;
    uint32_t type;              /* FILE_TYPE_* */
    uint32_t name_length;
    char name[256];             /* UTF-8, zero-terminated */
};

/* (35a) Swap: SYS_SWAP's SWAP_STATUS. Pages since the boot. */
struct sys_swap {
    uint64_t on;                /* 1: on */
    uint64_t pages;             /* slots in the swap file (pages of 4 KiB) */
    uint64_t used;              /* slots holding a page */
    uint64_t written, read;     /* pages written to it and read from it */
};

struct sys_info {
    uint64_t ticks;             /* timer ticks since the start */
    uint64_t ticks_per_second;
    uint64_t free_pages, usable_pages, page_size;
};

/*
 * How a process ended (wait's status), as on Unix: exit(code) gives
 * code << 8; a process stopped by a signal has the signal's number in the
 * low 7 bits. Faults are signals too: page faults and protection errors
 * SIGSEGV, undefined or privileged instructions SIGILL, division SIGFPE.
 */
#define SIGHUP          1       /* the terminal is gone (a pseudo-terminal's master closed) */
#define SIGINT          2       /* Ctrl-C */
#define SIGQUIT         3       /* (22r: these and the others below, Linux's numbers) */
#define SIGILL          4
#define SIGTRAP         5
#define SIGABRT         6       /* abort */
#define SIGBUS          7       /* touching a mapped file past its end (22e) */
#define SIGFPE          8
#define SIGKILL         9       /* cannot be handled or ignored */
#define SIGUSR1         10
#define SIGSEGV         11
#define SIGUSR2         12
#define SIGPIPE         13
#define SIGALRM         14
#define SIGTERM         15
#define SIGCHLD         17
#define SIGNALS         32      /* signals are 1 to 31 */
#define SIGMASK_BLOCK   0       /* sigprocmask (52f): add set, take it away, or set it */
#define SIGMASK_UNBLOCK 1
#define SIGMASK_SET     2

/* A signal's action for SYS_SIGACTION (22r): handler SIGNAL_DEFAULT (the
 * program ends), SIGNAL_IGNORE, or a function's address; mask: the signals
 * (bit n: signal n) blocked while it runs, besides the signal itself unless
 * SIGACTION_NODEFER; SIGACTION_RESETHAND: back to the default once used. */
struct sys_sigaction {
    uint64_t handler;
    uint64_t mask;
    uint32_t flags;
    uint32_t reserved;
};
#define SIGACTION_RESETHAND 1
#define SIGACTION_NODEFER   2
#define STATUS_EXITED(code)     (((code) & 0xFF) << 8)
#define STATUS_SIGNALED(sig)    ((sig) & 0x7F)

/* open() flags: one of the three access modes, and any of the others. A
 * file that was written is committed when it is closed (also at a
 * program's exit), and in between every 8 MiB. */
#define OPEN_READ       0
#define OPEN_WRITE      1
#define OPEN_READ_WRITE 2
#define OPEN_ACCESS     3       /* mask of the three above */
#define OPEN_CREATE     4       /* create the file if it does not exist */
#define OPEN_TRUNCATE   8       /* cut it to 0 bytes (when opened for writing) */
#define OPEN_APPEND     16      /* every write goes to the end */
#define OPEN_NONBLOCK   128     /* (53b) reads, writes, accept and connect never wait: EAGAIN
                                 * (connect: EINPROGRESS, then poll for POLLOUT) */
#define OPEN_EXCLUSIVE  64      /* (52j) with OPEN_CREATE: ERR_EXIST if the name is taken (O_EXCL) */
#define OPEN_MODE       32      /* (52b) the third argument is a created file's mode (open's
                                 * own: O_CREAT's mode argument); programs built before it
                                 * passed 0 there and get 0644 */

/* File descriptors every program starts with: the terminal. Reading it
 * gives a line at a time (edited and echoed by the kernel), including its
 * "\n"; Ctrl-D on an empty line is the end (read returns 0), Ctrl-C stops
 * the program. */
#define FD_STDIN        0
#define FD_STDOUT       1
#define FD_STDERR       2
#define FD_MAX          256     /* file descriptors per program (16 until 52k) */

/* Error numbers (as in Unix). */
#define ERR_NOENT       2       /* no such file or directory */
#define ERR_SRCH        3       /* no such process */
#define ERR_INTR        4       /* interrupted: the process is being stopped */
#define ERR_IO          5       /* input/output error (e.g. a damaged block) */
#define ERR_NXIO        6       /* (52c) no such device: /dev/tty without a terminal */
#define ERR_NOTTY       25      /* (52e) not a terminal (termios on a file) */
#define ERR_DEADLK      35      /* (54a, the library's) a thread would wait for itself */
#define ERR_LOOP        40      /* (57a) more than 40 symbolic links in one path */
#define ERR_NOTSOCK     88      /* (53a) a socket call on another kind of descriptor */
#define ERR_DESTADDRREQ 89      /* (53a, the library's) a datagram without an address */
#define ERR_MSGSIZE     90      /* (53a) a datagram too long */
#define ERR_NOPROTOOPT  92      /* (53a) no such socket option */
#define ERR_PROTONOSUPPORT 93   /* (53a) socket(): no such protocol for the family */
#define ERR_SOCKTNOSUPPORT 94   /* (53a) socket(): no such type */
#define ERR_AFNOSUPPORT 97      /* (53a) an address family RitterOS has not (AF_INET6) */
#define ERR_ADDRNOTAVAIL 99     /* (53a) an address that is not the machine's */
#define ERR_CONNABORTED 103     /* (53a) */
#define ERR_ISCONN      106     /* (53a) sendto with an address on a stream */
#define ERR_ALREADY     114     /* (53b) a connect still under way */
#define ERR_INPROGRESS  115     /* (53b) a connect without waiting: started */
#define ERR_2BIG        7       /* argument list too long */
#define ERR_NOEXEC      8       /* not a program for this CPU */
#define ERR_BADF        9       /* not an open file descriptor */
#define ERR_CHILD       10      /* no child to wait for */
#define ERR_AGAIN       11      /* try again: e.g. no free thread number */
#define ERR_NOMEM       12      /* out of memory */
#define ERR_ACCES       13      /* permission denied (by a file's permissions) */
#define ERR_FAULT       14      /* a pointer outside the program's memory */
#define ERR_EXIST       17      /* the name is already taken */
#define ERR_NODEV       19      /* no such device (e.g. no card at that place in the list) */
#define ERR_NOTDIR      20      /* a path component is not a directory */
#define ERR_ISDIR       21      /* reading a directory as a file */
#define ERR_PERM        1       /* not allowed */
#define ERR_INVAL       22      /* an invalid argument */
#define ERR_MFILE       24      /* too many open files */
#define ERR_ROFS        30      /* the file system is read-only (for programs, so far) */
#define ERR_NAMETOOLONG 36      /* a path that is too long */
#define ERR_NOSYS       38      /* no such system call */
#define ERR_NOTEMPTY    39      /* removing a directory that is not empty */
#define ERR_NOSPC       28      /* no space left on the disk */
#define ERR_NODATA      61      /* no such attribute */
#define ERR_BUSY        16      /* in use: a mounted device, a volume someone is in */
#define ERR_XDEV        18      /* on another file system (rename, instant copy) */
#define ERR_NOTSUP      95      /* the file system cannot do that (attributes on FAT) */
#define ERR_PIPE        32      /* writing to a connection the other side closed */
#define ERR_ADDRINUSE   98      /* the port is taken */
#define ERR_NETUNREACH  101     /* no route: no address, no link or no gateway */
#define ERR_CONNRESET   104     /* the other side reset the connection */
#define ERR_NOTCONN     107     /* not connected */
#define ERR_CONNREFUSED 111     /* nobody listens on that port */
#define ERR_NONAME      150     /* DNS: no such name (RitterOS's own number) */
#define ERR_TIMEDOUT    110     /* no answer in time */
#define ERR_HOSTUNREACH 113     /* no answer to ARP: the neighbour is not there */
```
