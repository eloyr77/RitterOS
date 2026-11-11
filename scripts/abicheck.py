#!/usr/bin/env python3
"""
abicheck.py: keep each shared library's interface compatible (step 22d). A
program built with libimage.so.1 must keep working with every later
libimage.so.1, so what it depends on is recorded, per library, in
sdk/abi/LIBRARY.txt, and checked against the headers:

  function  every function the library exports, with its declaration
  type      every struct, union and enum its headers define, as written
  constant  every constant (#define NAME VALUE) of its headers

Removing or changing any of them breaks programs built before: such a change
needs a new major version (libimage.so.2, a library of its own next to the
old one). Adding is compatible, and is then recorded.

  python3 scripts/abicheck.py             check (exit 1: a change, or something new to record)
  python3 scripts/abicheck.py --update    record what is new (refused for an incompatible change)
  python3 scripts/abicheck.py --selftest  the checker's own test, on made-up changes

(67b) The kernel's driver interface (drivers/include/: what a kernel module
may use) is recorded the same way in drivers/abi/driver-api.N.txt, N its
major version (DRIVER_API_MAJOR): its functions and function-like macros,
types and constants. Adding is compatible but must raise DRIVER_API_MINOR
(a module built for the new minor version is refused by older kernels);
removing or changing needs a new major version (a new record).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import libexports  # noqa: E402
import sdkapi  # noqa: E402

ROOT = sdkapi.ROOT
ABI_DIR = os.path.join(ROOT, 'sdk', 'abi')

# Headers whose types and constants a library's programs see besides the
# ones that declare its functions.
EXTRA = {
    'libritteros.so.1': ['common/syscalls.h'],  # flags, structures, system call numbers
    'libimage.so.1': ['user/include/media.h'],
    'libaudio.so.1': ['user/include/media.h'],
    'libvideo.so.1': ['user/include/media.h'],
    'libdraw.so.1': ['user/include/media.h'],
}


def normal(text):
    return ' '.join(sdkapi.strip_comments(text).split())


def types_of(body):
    """struct/union/enum NAME { ... } definitions, normalized, by name."""
    code = sdkapi.strip_comments(body)
    found = {}
    for m in re.finditer(r'\b(struct|union|enum)\s+(\w+)\s*\{', code):
        depth, i = 0, m.end() - 1
        while i < len(code):
            depth += {'{': 1, '}': -1}.get(code[i], 0)
            if depth == 0:
                break
            i += 1
        found['%s %s' % (m.group(1), m.group(2))] = normal(code[m.start():i + 1])
    return found


def constants_of(body):
    """#define NAME VALUE (not function-like, not an include guard), by name."""
    found = {}
    for line in sdkapi.strip_comments(body).split('\n'):
        m = re.match(r'\s*#define\s+(\w+)\s+(.+?)\s*$', line)
        if m and not line.rstrip().endswith('\\'):
            found[m.group(1)] = ' '.join(m.group(2).split())
    return found


def interface(library):
    """The library's interface now: {key: text}."""
    items = {}
    exported = set(libexports.exports(library))
    for header in libexports.LIBRARIES[library]:
        _, body = sdkapi.split_header('user/include/' + header)
        for name, decl in sdkapi.declarations(body):
            if name in exported and not decl.startswith('#define'):
                items['function ' + name] = decl
    for path in ['user/include/' + h for h in libexports.LIBRARIES[library]] + EXTRA.get(library, []):
        _, body = sdkapi.split_header(path)
        for name, text in types_of(body).items():
            items['type ' + name] = text
        for name, value in constants_of(body).items():
            items['constant ' + name] = value
    return items


def record_path(library):
    return os.path.join(ABI_DIR, library + '.txt')


def read_record(library):
    path = record_path(library)
    if not os.path.exists(path):
        return None
    items = {}
    for line in open(path).read().split('\n'):
        if not line or line.startswith('#'):
            continue
        kind, name, text = line.split('\t', 2)
        items[kind + ' ' + name] = text
    return items


def write_record(library, items):
    os.makedirs(ABI_DIR, exist_ok=True)
    lines = ['# %s: its interface, recorded by scripts/abicheck.py --update. Removing or' % library,
             '# changing a line breaks programs built before: that needs a new major version.']
    for key in sorted(items):
        kind, name = key.split(' ', 1)
        lines.append('%s\t%s\t%s' % (kind, name, items[key]))
    with open(record_path(library), 'w') as f:
        f.write('\n'.join(lines) + '\n')


def reserved_taken(recorded, current):
    """True when the only difference is a field named reserved... that now has
    a name of its own: the same type at the same place, so programs built
    before (which never looked at it) are not affected."""
    a, b = re.findall(r'\w+|\S', recorded), re.findall(r'\w+|\S', current)
    if len(a) != len(b):
        return False
    diff = [(x, y) for x, y in zip(a, b) if x != y]
    return bool(diff) and all(re.fullmatch(r'reserved\w*', x) and re.fullmatch(r'[A-Za-z_]\w*', y)
                              for x, y in diff)


# Changes agreed one by one as compatible (library, key, was, now): open
# became POSIX's variadic open (52b, the user's choice: in libritteros.so.1).
# Its first two arguments are passed as before; only O_CREAT reads a third,
# so a program built before that creates a file gets an unknown mode.
AGREED = {
    ('libritteros.so.1', 'function open', 'int open(const char *path, int flags)',
     'int open(const char *path, int flags, ...)'),
    # 52k (the user's: 256 descriptors a program): larger limits; a program
    # built before keeps within the old ones.
    ('libritteros.so.1', 'constant FD_MAX', '16', '256'),
    ('libritteros.so.1', 'constant POLL_MAX', '64', '256'),
    # 54d (the user's: more threads): a program built before never makes
    # more than 16, so it never sees a thread number past its own tables.
    ('libritteros.so.1', 'constant PROCESS_THREADS', '16', '256'),
    ('libritteros.so.1', 'constant PTHREAD_THREADS_MAX', '16', '256'),
    # 38b (the user's, 2026-11-07: 6.1, recorded as agreed): struct net_card
    # gains set_promiscuous at its end. The kernel allocates every card
    # (net_card_new, zeroed), so a 6.0 module never sees past `gone` and its
    # field is 0: "cannot be a bridge's port", which the bridge refuses.
    ('driver-api.6', 'type struct net_card',
     'struct net_card { char name[8]; uint8_t mac[6]; const char *driver; char model[40]; char where[16]; uint32_t (*link_speed)(struct net_card *card); bool (*send)(struct net_card *card, const void *frame, size_t length); const struct pci_device *pci; void (*details)(struct net_card *card); void (*set_multicast)(struct net_card *card, const uint8_t (*macs)[6], int count); void *priv; struct wlan *wlan; volatile uint64_t rx_frames, rx_bytes, tx_frames, tx_bytes, tx_dropped; volatile bool gone; }',
     'struct net_card { char name[8]; uint8_t mac[6]; const char *driver; char model[40]; char where[16]; uint32_t (*link_speed)(struct net_card *card); bool (*send)(struct net_card *card, const void *frame, size_t length); const struct pci_device *pci; void (*details)(struct net_card *card); void (*set_multicast)(struct net_card *card, const uint8_t (*macs)[6], int count); void *priv; struct wlan *wlan; volatile uint64_t rx_frames, rx_bytes, tx_frames, tx_bytes, tx_dropped; volatile bool gone; void (*set_promiscuous)(struct net_card *card, bool on); }'),
}


def compare(library, recorded, current):
    """(incompatible, added): lists of messages."""
    incompatible, added = [], []
    for key in sorted(recorded):
        if key not in current:
            incompatible.append('%s: %s was removed' % (library, key))
        elif current[key] != recorded[key] and key.startswith('type ') and \
                reserved_taken(recorded[key], current[key]):
            added.append('%s: %s takes a reserved field into use: now "%s"' % (library, key, current[key]))
        elif (library, key, recorded[key], current[key]) in AGREED:
            added.append('%s: %s changed as agreed: now "%s"' % (library, key, current[key]))
        elif current[key] != recorded[key]:
            incompatible.append('%s: %s changed: now "%s", was "%s"' % (library, key, current[key], recorded[key]))
    for key in sorted(set(current) - set(recorded)):
        added.append('%s: %s is new' % (library, key))
    return incompatible, added


def major_hint(library):
    stem, _, major = library.rpartition('.')
    return '%s.%d' % (stem, int(major) + 1) if major.isdigit() else 'a new major version'


DRIVER_HEADERS = ['drivers/include/driver.h', 'drivers/include/driver_net.h', 'drivers/include/driver_wifi.h',
                  'drivers/include/driver_usb.h', 'drivers/include/driver_gpu.h',
                  'drivers/include/driver_sound.h']


def driver_version():
    _, body = sdkapi.split_header(DRIVER_HEADERS[0])
    c = constants_of(body)
    return int(c['DRIVER_API_MAJOR']), int(c['DRIVER_API_MINOR'])


def driver_interface():
    """The driver interface now: {key: text} (its minor version apart)."""
    items = {}
    for path in DRIVER_HEADERS:
        _, body = sdkapi.split_header(path)
        for name, decl in sdkapi.declarations(body):
            items[('macro ' if decl.startswith('#define') else 'function ') + name] = decl
        for name, text in types_of(body).items():
            items['type ' + name] = text
        for name, value in constants_of(body).items():
            if name != 'DRIVER_API_MINOR':
                items['constant ' + name] = value
    return items


def check_drivers(update):
    """The driver interface against its record: True when it failed."""
    major, minor = driver_version()
    name = 'driver-api.%d' % major
    path = os.path.join(ROOT, 'drivers', 'abi', name + '.txt')
    current = driver_interface()
    recorded, recorded_minor = None, None
    if os.path.exists(path):
        recorded = {}
        for line in open(path).read().split('\n'):
            m = re.match(r'# minor version (\d+)$', line)
            if m:
                recorded_minor = int(m.group(1))
            if not line or line.startswith('#'):
                continue
            kind, key, text = line.split('\t', 2)
            recorded[kind + ' ' + key] = text

    def write():
        os.makedirs(os.path.dirname(path), exist_ok=True)
        lines = ['# %s: the kernel\'s driver interface %d.%d (drivers/include/), recorded by' % (name, major, minor),
                 '# scripts/abicheck.py --update. Removing or changing a line breaks modules built',
                 '# before: that needs a new major version. Adding raises DRIVER_API_MINOR.',
                 '# minor version %d' % minor]
        for key in sorted(current):
            kind, rest = key.split(' ', 1)
            lines.append('%s\t%s\t%s' % (kind, rest, current[key]))
        with open(path, 'w') as f:
            f.write('\n'.join(lines) + '\n')

    if recorded is None:
        if update:
            write()
            print('abicheck: %s: recorded (%d items, version %d.%d)' % (name, len(current), major, minor))
            return False
        print('abicheck: %s: nothing recorded yet (python3 scripts/abicheck.py --update)' % name)
        return True
    incompatible, added = compare(name, recorded, current)
    for m in incompatible:
        print('abicheck: %s -- incompatible: it needs driver interface %d.0 (or undo it)' % (m, major + 1))
    if incompatible:
        return True
    if added and (recorded_minor is None or minor <= recorded_minor):
        for m in added:
            print('abicheck: %s -- raise DRIVER_API_MINOR (%d) first' % (m, minor))
        return True
    if added and update:
        write()
        print('abicheck: %s: recorded %d new item(s), version %d.%d' % (name, len(added), major, minor))
        return False
    if added:
        for m in added:
            print('abicheck: %s (compatible: record it with --update)' % m)
        return True
    print('abicheck: %s: compatible (%d items, version %d.%d)' % (name, len(current), major, minor))
    return False


def check(update):
    failed = check_drivers(update)
    for library in sorted(libexports.LIBRARIES):
        current = interface(library)
        recorded = read_record(library)
        if recorded is None:
            if update:
                write_record(library, current)
                print('abicheck: %s: recorded (%d items)' % (library, len(current)))
            else:
                print('abicheck: %s: nothing recorded yet (python3 scripts/abicheck.py --update)' % library)
                failed = True
            continue
        incompatible, added = compare(library, recorded, current)
        for m in incompatible:
            print('abicheck: %s -- incompatible: it needs %s (or undo it)' % (m, major_hint(library)))
        if incompatible:
            failed = True
            continue
        if added and update:
            write_record(library, current)
            print('abicheck: %s: recorded %d new item(s)' % (library, len(added)))
        elif added:
            for m in added:
                print('abicheck: %s (compatible: record it with --update)' % m)
            failed = True
        else:
            print('abicheck: %s: compatible (%d items)' % (library, len(current)))
    return 1 if failed else 0


def selftest():
    """The verdicts on made-up changes to libimage.so.1's interface."""
    base = interface('libimage.so.1')
    fn = next(k for k in sorted(base) if k.startswith('function '))
    ty = next(k for k in sorted(base) if k.startswith('type '))
    cases = [
        ('unchanged', dict(base), 0, 0),
        ('a function removed', {k: v for k, v in base.items() if k != fn}, 1, 0),
        ('a declaration changed', dict(base, **{fn: base[fn].replace(')', ', int more)')}), 1, 0),
        ('a struct changed', dict(base, **{ty: base[ty].replace('}', ' int extra; }')}), 1, 0),
        ('a function added', dict(base, **{'function image_new_one': 'int image_new_one(void)'}), 0, 1),
    ]
    with_reserved = dict(base, **{ty: base[ty].replace('}', ' uint8_t reserved; }')})
    cases += [
        ('a reserved field named', dict(with_reserved, **{ty: base[ty].replace('}', ' uint8_t flags; }')}), 0, 1),
        ('a reserved field retyped', dict(with_reserved, **{ty: base[ty].replace('}', ' uint16_t flags; }')}), 1, 0),
    ]
    ok = True
    for name, current, want_incompatible, want_added in cases:
        recorded = with_reserved if name.startswith('a reserved') else base
        incompatible, added = compare('libimage.so.1', recorded, current)
        good = len(incompatible) == want_incompatible and len(added) == want_added
        ok &= good
        print('abicheck: self-test: %s: %s' % (name, 'ok' if good else 'WRONG'))
    print('abicheck: self-test %s' % ('ok' if ok else 'FAILED'))
    return 0 if ok else 1


def main():
    args = sys.argv[1:]
    if args == ['--selftest']:
        sys.exit(selftest())
    if args not in ([], ['--update']):
        sys.exit(__doc__)
    sys.exit(check(update=args == ['--update']))


if __name__ == '__main__':
    main()
