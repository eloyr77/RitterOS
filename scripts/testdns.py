#!/usr/bin/env python3
"""A tiny DNS server for the tests (answers only what it is told).

Usage: testdns.py PORT [--delay=SECONDS] NAME=ADDRESS... [ALIAS>NAME...]

Listens on UDP 127.0.0.1:PORT (QEMU's user network takes the guest's
packets to 10.0.2.2:PORT there). A question of type A for NAME gets
ADDRESS; (64d) an ADDRESS with a ':' is an IPv6 one, given for type AAAA
(a name may have one of each: NAME=A NAME=AAAA); for ALIAS, a CNAME to
NAME and NAME's A record; a known name without the type asked for "no
records" (NOERROR, no answers, as real servers say), any other name "no
such name" (NXDOMAIN). Every question is printed to stdout. Runs until
killed. (64j) --delay=SECONDS waits that long before each answer (a slow
upstream server: dnsd's own queue fills meanwhile); since 64q each answer
waits on a thread of its own, so questions wait together. (64q) A NAME
"*.ZONE" answers for every name in ZONE.
"""
import socket
import struct
import sys
import threading


def read_name(msg, at):
    labels = []
    while msg[at]:
        n = msg[at]
        labels.append(msg[at + 1:at + 1 + n].decode())
        at += 1 + n
    return ".".join(labels), at + 1


def encode(name):
    return b"".join(bytes([len(p)]) + p.encode() for p in name.split(".")) + b"\0"


def answer(query, names, aliases, names6={}):
    ident, flags, qd = struct.unpack_from(">HHH", query, 0)
    name, at = read_name(query, 12)
    qtype, qclass = struct.unpack_from(">HH", query, at)
    question = query[12:at + 4]
    records = b""
    count = 0
    key = name.lower()
    # (64q) "*.zone" names every name in the zone (many names in one test)
    wild = "*." + key.split(".", 1)[1] if "." in key else ""
    if key not in names and key not in names6 and key not in aliases:
        if wild in names or wild in names6:
            names = dict(names, **({key: names[wild]} if wild in names else {}))
            names6 = dict(names6, **({key: names6[wild]} if wild in names6 else {}))
    if qtype == 1 and key in aliases:
        target = aliases[key]
        records += b"\xc0\x0c" + struct.pack(">HHIH", 5, 1, 60, len(encode(target))) + encode(target)
        records += encode(target) + struct.pack(">HHIH", 1, 1, 60, 4) + \
            socket.inet_aton(names[target.lower()])
        count = 2
    elif qtype == 1 and key in names:
        records = b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 60, 4) + socket.inet_aton(names[key])
        count = 1
    elif qtype == 28 and key in names6:
        records = b"\xc0\x0c" + struct.pack(">HHIH", 28, 1, 60, 16) + \
            socket.inet_pton(socket.AF_INET6, names6[key])
        count = 1
    known = key in names or key in names6 or key in aliases
    rcode = 0 if count or known else 3
    header = struct.pack(">HHHHHH", ident, 0x8180 | rcode, 1, count, 0, 0)
    said = "answered" if count else "no records" if known else "no such name"
    print(f"question {name} type {qtype}: {said}", flush=True)
    return header + question + records


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    port = int(sys.argv[1])
    names, aliases, names6 = {}, {}, {}
    delay = 0.0
    for arg in sys.argv[2:]:
        if arg.startswith("--delay="):
            delay = float(arg.split("=", 1)[1])
        elif "=" in arg:
            k, v = arg.split("=", 1)
            (names6 if ":" in v else names)[k.lower()] = v
        elif ">" in arg:
            k, v = arg.split(">", 1)
            aliases[k.lower()] = v
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    print(f"listening on 127.0.0.1:{port}", flush=True)
    while True:
        data, peer = s.recvfrom(512)
        if delay:
            # (64q) each answer after its own delay: several questions wait
            # at once, as on a real server
            threading.Timer(delay, reply, (s, data, peer, names, aliases, names6)).start()
        else:
            reply(s, data, peer, names, aliases, names6)


def reply(s, data, peer, names, aliases, names6):
    try:
        s.sendto(answer(data, names, aliases, names6), peer)
    except (IndexError, struct.error, UnicodeDecodeError):
        pass


if __name__ == "__main__":
    main()
