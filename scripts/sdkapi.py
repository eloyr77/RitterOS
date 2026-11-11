#!/usr/bin/env python3
"""
sdkapi.py: write sdk/API.md, the programs' API as the SDK offers it, from the
headers themselves (user/include and common/syscalls.h), so the list cannot
drift from them. Each header is shown as it is (its comments are the
documentation), with an index of every function and function-like macro.

  python3 scripts/sdkapi.py           write sdk/API.md
  python3 scripts/sdkapi.py --check   exit 1 if sdk/API.md is not what the headers give

Every header in user/include must be listed in HEADERS (with what it is and
who it is for): a new header stops this script until it is.
"""
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'sdk', 'API.md')

APPS = 'apps'
MIXED = 'apps; parts are the system\'s own'
SYSTEM = 'the system\'s own'

# (path from ROOT, what it is, who it is for), in the order API.md shows them.
HEADERS = [
    ('user/include/stdio.h', 'formatted output', APPS),
    ('user/include/stdlib.h', 'memory, exit, numbers', APPS),
    ('user/include/string.h', 'strings and memory', APPS),
    ('user/include/strings.h', 'comparing strings without case', APPS),
    ('user/include/ctype.h', 'classes of characters (ASCII)', APPS),
    ('user/include/assert.h', 'assert', APPS),
    ('user/include/math.h', 'C99\'s mathematics: trigonometry, exponentials, logarithms, powers', APPS),
    ('user/include/fenv.h', 'the floating-point environment: rounding directions and exception flags', APPS),
    ('user/include/wchar.h', 'wide characters and strings, wide streams and formatting (the C locale: a byte each)', APPS),
    ('user/include/wctype.h', 'classes of wide characters (ASCII)', APPS),
    ('user/include/uchar.h', 'Unicode characters: UTF-8 always, UTF-16 and code points', APPS),
    ('user/include/complex.h', "complex numbers: C99's functions and the compiler's * and / helpers", APPS),
    ('user/include/threads.h', "C11's threads, mutexes, condition variables, call_once, thread-specific storage", APPS),
    ('user/include/setjmp.h', 'setjmp and longjmp: jumping back out of deep calls', APPS),
    ('user/include/locale.h', 'the C locale (the only one)', APPS),
    ('user/include/time.h', 'the clock, CPU time, dates and their text', APPS),
    ('user/include/inttypes.h', 'printf conversions for stdint.h\'s types', APPS),
    ('user/include/unistd.h', 'files, processes, sleeping', APPS),
    ('user/include/fcntl.h', 'opening files', APPS),
    ('user/include/errno.h', 'error numbers', APPS),
    ('user/include/sys/types.h', 'basic types', APPS),
    ('user/include/sys/stat.h', 'file status, directories, permissions', APPS),
    ('user/include/poll.h', 'waiting for several descriptors at once (poll)', APPS),
    ('user/include/sys/socket.h', 'BSD sockets: TCP, UDP and local connections by address', APPS),
    ('user/include/netinet/in.h', 'IPv4 addresses (sockaddr_in) and byte order', APPS),
    ('user/include/netinet/tcp.h', "TCP's socket option", APPS),
    ('user/include/arpa/inet.h', 'IPv4 addresses as text', APPS),
    ('user/include/sys/un.h', 'local socket addresses (sockaddr_un)', APPS),
    ('user/include/sys/time.h', 'struct timeval, gettimeofday, utimes', APPS),
    ('user/include/sys/utsname.h', "the system's names: uname (release, commit, machine)", APPS),
    ('user/include/utime.h', "a file's times in seconds (utime)", APPS),
    ('user/include/sys/select.h', 'select: waiting for descriptors by sets', APPS),
    ('user/include/pthread.h', 'POSIX threads, mutexes, condition variables, keys', APPS),
    ('user/include/sched.h', 'sched_yield', APPS),
    ('user/include/spawn.h', 'posix_spawn: starting a program with file actions and attributes', APPS),
    ('user/include/regex.h', 'regular expressions: regcomp, regexec (POSIX basic and extended)', APPS),
    ('user/include/netdb.h', 'names and services: getaddrinfo, getnameinfo, gethostbyname', APPS),
    ('user/include/termios.h', "a terminal's modes: raw input, echo, Ctrl-C (tcgetattr, tcsetattr)", APPS),
    ('user/include/sys/wait.h', 'how a child ended', APPS),
    ('user/include/sys/mman.h', 'memory mappings: anonymous memory, files shared between programs', APPS),
    ('user/include/dlfcn.h', 'loading shared libraries while running (dlopen, dlsym)', APPS),
    ('user/include/dirent.h', 'reading directories', APPS),
    ('user/include/signal.h', 'signals', APPS),
    ('user/include/pwd.h', 'users', APPS),
    ('user/include/thread.h', 'threads', APPS),
    ('user/include/ritteros.h', 'RitterOS\'s own calls', MIXED),
    ('user/include/window.h', 'windowed apps (libwindow)', APPS),
    ('user/include/media.h', 'the media libraries\' types', APPS),
    ('user/include/image.h', 'images (libimage)', APPS),
    ('user/include/audio.h', 'sound (libaudio)', APPS),
    ('user/include/video.h', 'video (libvideo)', APPS),
    ('user/include/draw.h', '2D drawing, anti-aliased, on any pixel buffer (libdraw)', APPS),
    ('user/include/widget.h', 'controls for windowed programs: buttons, check boxes, layouts (libwidget)', APPS),
    ('user/include/wfont.h', 'the windows\' font (cell sizes for laying out text)', APPS),
    ('user/include/tls.h', 'TLS 1.3 connections, OpenBSD\'s libtls interface (libtls)', APPS),
    ('user/include/crypto.h', 'SHA-512, X25519, Ed25519, ChaCha20-Poly1305 (written for SSH); AES-GCM, SHA-384, HKDF, RSA and ECDSA checks (for TLS)', APPS),
    ('user/include/x509.h', 'X.509 certificates: reading, chains to trusted roots, host names (libtls uses it)', SYSTEM),
    ('user/include/ssh.h', 'the SSH protocol (sshd, ssh)', SYSTEM),
    ('user/include/gpt.h', 'GPT partition tables (disks, osinstall)', SYSTEM),
    ('user/include/mkfs.h', 'making file systems (format, osinstall)', SYSTEM),
    ('user/include/windowproto.h', 'the window server\'s protocol (window.h speaks it)', SYSTEM),
    ('common/syscalls.h', 'system call numbers, flags and structures, shared with the kernel', APPS),
]

# ritteros.h's functions that are the system's own: for root only or for one
# system program (named), so programs outside the tree do not use them.
SYSTEM_FUNCTIONS = {
    'syscall6',                                                     # raw system calls
    'ritteros_spawn_as',                                            # login, sshd (root)
    'ritteros_power',                                               # poweroff, reboot
    'ritteros_snapshot', 'ritteros_mount', 'ritteros_unmount', 'ritteros_device',
    'ritteros_grow',                                                # disks (68j)
    'ritteros_terminal_line_editor',                                # sh
    'ritteros_netset', 'ritteros_bindcard', 'ritteros_forward', 'ritteros_masquerade',  # net, dhclient
    'ritteros_default_route',  # net (50g)
    'dns_query', 'dns_answer', 'dns_question', 'dns_reply', 'dns_failure',  # dnsd (apps: ritteros_resolve)
    'ritteros_display_acquire', 'ritteros_display_release', 'ritteros_display_info',
    'ritteros_display_flush', 'ritteros_display_event',             # windowd
    'guard_parse', 'guard_read', 'guard_format', 'ritteros_guard',  # the firewall
    'ritteros_log_as', 'ritteros_log_write', 'ritteros_log_take', 'logs_flush',  # logs
    'ritteros_efivar',                                              # osinstall
    'elvesd_pid', 'elvesd_start',                                   # the elves
    'user_name_valid', 'user_file_set', 'password_set', 'password_check', 'password_exists',  # users
}

INTRO = """\
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
"""


def library_of(header):
    """The shared library that holds a header's functions (scripts/libexports.py),
    the static archive for the rest, or - for a header without functions."""
    import libexports
    for library, headers in libexports.LIBRARIES.items():
        if header in headers:
            return '`%s`' % library
    path = header if header.startswith('common/') else 'user/include/' + header
    _, body = split_header(path)
    functions = [d for _, d in declarations(body) if not d.startswith('#define')]
    return '`libritteros.a` only' if functions else '-'


def strip_comments(text):
    """The text with /* ... */ comments removed (newlines kept)."""
    return re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), text, flags=re.S)


def drop_attributes(text):
    """The text without __attribute__((...)) (balanced parentheses)."""
    out, i = [], 0
    while True:
        j = text.find('__attribute__', i)
        if j < 0:
            out.append(text[i:])
            return ''.join(out)
        out.append(text[i:j])
        k = text.find('(', j)
        depth = 0
        while k < len(text):
            if text[k] == '(':
                depth += 1
            elif text[k] == ')':
                depth -= 1
                if depth == 0:
                    break
            k += 1
        i = k + 1


def split_header(path):
    """(description, body): the header's opening comment as text, and the
    rest without its include guard and #include lines."""
    lines = open(os.path.join(ROOT, path)).read().split('\n')
    description = ''
    i = 0
    while i < len(lines) and not lines[i].strip():
        i += 1
    if i < len(lines) and lines[i].lstrip().startswith('/*'):
        start = i
        while '*/' not in lines[i]:
            i += 1
        block = '\n'.join(lines[start:i + 1])
        i += 1
        text = block.strip()[2:-2]
        description = ' '.join(re.sub(r'^\s*\*\s?', '', l).strip()
                               for l in text.split('\n')).strip()
    body = lines[i:]
    guard = None
    kept = []
    for n, line in enumerate(body):
        s = line.strip()
        m = re.match(r'#ifndef\s+(\w+)$', s)
        if guard is None and m and n + 1 < len(body) and body[n + 1].strip() == '#define ' + m.group(1):
            guard = m.group(1)
            continue
        if guard and s == '#define ' + guard:
            continue
        if s.startswith('#include'):
            continue
        kept.append(line)
    if guard:                                   # the guard's #endif: the last one
        for n in range(len(kept) - 1, -1, -1):
            if kept[n].strip().startswith('#endif'):
                del kept[n]
                break
    text = '\n'.join(kept)
    text = re.sub(r'\n{3,}', '\n\n', text).strip('\n')
    return description, text


def declarations(body):
    """(name, declaration) for every function and function-like macro."""
    found = []
    code = strip_comments(body)
    lines = code.split('\n')
    i, statement, depth = 0, '', 0
    while i < len(lines):
        line = lines[i]
        if line.lstrip().startswith('#'):
            macro = line
            while macro.endswith('\\') and i + 1 < len(lines):
                i += 1
                macro = macro[:-1] + ' ' + lines[i]
            m = re.match(r'\s*#define\s+(\w+)\(([^)]*)\)', macro)
            if m:
                found.append((m.group(1), '#define %s(%s)' % (m.group(1), m.group(2).strip())))
            i += 1
            continue
        for ch in line:
            statement += ch
            if ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
            elif ch == ';' and depth == 0:
                s = ' '.join(drop_attributes(statement).split())
                statement = ''
                # Types (typedefs, struct/enum bodies) and variables are not functions.
                if not s or s.startswith('typedef') or '{' in s:
                    continue
                m = re.search(r'(\w+)\s*\(', s)
                if m:
                    found.append((m.group(1), s[:-1].strip()))
        statement += '\n'
        i += 1
    return found


def generate():
    present = sorted(os.path.relpath(p, ROOT)
                     for p in glob.glob(os.path.join(ROOT, 'user/include/**/*.h'), recursive=True))
    listed = [h[0] for h in HEADERS]
    missing = [p for p in present if p not in listed]
    if missing:
        sys.exit('sdkapi.py: %s not in HEADERS: add it (what it is, who it is for)'
                 % ', '.join(missing))
    gone = [p for p in listed if not os.path.exists(os.path.join(ROOT, p))]
    if gone:
        sys.exit('sdkapi.py: %s listed in HEADERS but not there' % ', '.join(gone))

    out = [INTRO]
    parts, index = [], []
    for path, what, who in HEADERS:
        name = path.replace('user/include/', '')
        anchor = re.sub(r'[^a-z0-9]+', '-', name.lower()).strip('-')
        out.append('| [`%s`](#%s) | %s | %s | %s |\n' % (name, anchor, what, who, library_of(name)))
        description, body = split_header(path)
        for fn, decl in declarations(body):
            for_whom = 'system' if who == SYSTEM or fn in SYSTEM_FUNCTIONS else 'apps'
            index.append((fn, name, anchor, decl, for_whom))
        part = ['\n<a id="%s"></a>\n\n### %s\n\n' % (anchor, name)]
        part.append('`%s` (in RitterOS\'s tree) -- %s; for %s.\n\n' % (path, what, who))
        if description:
            part.append(description + '\n\n')
        part.append('```c\n' + body + '\n```\n')
        parts.append(''.join(part))

    out.append('\n## Every function and macro\n\n')
    out.append('%d of them, by name. Each links to its header, where its comment says what '
               'it does. "system": the system\'s own (see above); do not use it outside '
               'RitterOS\'s tree.\n\n' % len(index))
    unknown = SYSTEM_FUNCTIONS - {entry[0] for entry in index}
    if unknown:
        sys.exit('sdkapi.py: SYSTEM_FUNCTIONS names no function: %s' % ', '.join(sorted(unknown)))
    out.append('| Name | For | Header | Declaration |\n|---|---|---|---|\n')
    for fn, name, anchor, decl, for_whom in sorted(index, key=lambda x: (x[0].lower(), x[1])):
        out.append('| `%s` | %s | [`%s`](#%s) | `%s` |\n'
                   % (fn, for_whom, name, anchor, decl.replace('|', '\\|')))
    out.append('\n## The headers, as they are\n')
    out.extend(parts)
    return ''.join(out)


def main():
    text = generate()
    if sys.argv[1:] == ['--check']:
        old = open(OUT).read() if os.path.exists(OUT) else ''
        if old != text:
            sys.exit('sdkapi.py: sdk/API.md does not match the headers: '
                     'run python3 scripts/sdkapi.py and commit it')
        print('sdkapi.py: sdk/API.md matches the headers')
        return
    if sys.argv[1:]:
        sys.exit('usage: sdkapi.py [--check]')
    with open(OUT, 'w') as f:
        f.write(text)
    print('sdkapi.py: wrote sdk/API.md (%d headers)' % len(HEADERS))


if __name__ == '__main__':
    main()
