#!/usr/bin/env python3
"""
libexports.py LIBRARY OUT: the version script of one of RitterOS's shared
libraries (step 22c). A library exports exactly the functions its headers
declare -- its interface, what a program may call -- and keeps everything
else inside (helpers shared between its own files). The linker refuses a
listed function the library does not define, so a header and its library
cannot drift apart. The headers are read with scripts/sdkapi.py's parser,
the one that makes sdk/API.md.

Since 63 the build runs scripts/libexports.c, which writes the same file;
this script stays for sdkapi.py (library_of), and its LIBRARIES must list
the same headers as libexports.c's table.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sdkapi  # noqa: E402

# Each shared library and the headers that declare its functions.
LIBRARIES = {
    'libritteros.so.1': ['stdio.h', 'stdlib.h', 'string.h', 'unistd.h', 'fcntl.h', 'errno.h',
                         'sys/stat.h', 'sys/wait.h', 'sys/mman.h', 'dlfcn.h', 'dirent.h', 'signal.h',
                         'pwd.h', 'thread.h', 'ritteros.h', 'strings.h', 'ctype.h', 'assert.h',
                         'math.h', 'fenv.h', 'setjmp.h', 'locale.h', 'time.h',
                         'wchar.h', 'wctype.h', 'uchar.h', 'complex.h', 'threads.h', 'poll.h', 'termios.h',
                         'sys/socket.h', 'netinet/in.h', 'arpa/inet.h', 'sys/time.h', 'sys/select.h', 'netdb.h', 'pthread.h',
                         'sched.h', 'spawn.h', 'regex.h', 'utime.h', 'sys/utsname.h'],
    'libwindow.so.1': ['window.h', 'wfont.h'],
    'libimage.so.1': ['image.h'],
    'libaudio.so.1': ['audio.h'],
    'libvideo.so.1': ['video.h'],
    'libdraw.so.1': ['draw.h'],
    'libwidget.so.1': ['widget.h'],
    'libtls.so.1': ['tls.h'],
}


def exports(library):
    names = set()
    for header in LIBRARIES[library]:
        _, body = sdkapi.split_header('user/include/' + header)
        for name, decl in sdkapi.declarations(body):
            if not decl.startswith('#define'):          # macros are not in the library
                names.add(name)
    return sorted(names)


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in LIBRARIES:
        sys.exit('usage: libexports.py %s OUT' % '|'.join(sorted(LIBRARIES)))
    library, out = sys.argv[1], sys.argv[2]
    lines = ['/* %s: made by scripts/libexports.py from its headers. */' % library,
             '{', '  global:']
    lines += ['    %s;' % name for name in exports(library)]
    lines += ['  local:', '    *;', '};', '']
    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    with open(out, 'w') as f:
        f.write('\n'.join(lines))


if __name__ == '__main__':
    main()
