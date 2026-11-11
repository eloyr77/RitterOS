#!/usr/bin/env python3
"""Two hosts on the Mac for the routing test: the guest's cards eth0 and
attack: send malformed packets to the guest (the security test, 49a).
eth1 are on two QEMU networks of their own (`-netdev dgram`: every
Ethernet frame is one UDP datagram between QEMU and this script), and on
each network this script plays one host:

  A  192.168.71.2 on eth0's network (192.168.71.0/24)
  B  192.168.72.2 on eth1's network (192.168.72.0/24); B is also the
     guest's gateway there: it answers echo requests for any address
     that reaches it (as if the internet were behind it)

Both answer ARP for their own address and echo requests (ping). What the
guest forwards is checked as B (or A) sees it: its TTL and header checksum.
Each can also be a DHCP server (off at the start): it offers .50 of its
network, with itself as gateway and DNS server, for an hour.

Usage:
  testroute.py serve CONTROL A_HOST A_GUEST B_HOST B_GUEST
      run until killed: the hosts receive on 127.0.0.1:A_HOST and B_HOST,
      QEMU's networks on A_GUEST and B_GUEST; commands come on the TCP
      port CONTROL (one line, one answer line)
  testroute.py ask CONTROL COMMAND...
      send a command, print the answer:
      ping FROM DST [TTL]   host FROM (A or B) sends an echo request to the
                            address DST through the guest (its address on
                            FROM's network is the router) and reports what
                            came back within 2 seconds
      dhcp HOST on|off      HOST (A or B) answers DHCP requests or not
      ping6 FROM DST        (64a) host FROM sends an ICMPv6 echo request to the
                            IPv6 address DST on its network (a neighbour
                            solicitation to DST's group first, unless DST is
                            a group) and reports the answer within 2 seconds
      claim6 HOST ADDRESS   HOST answers duplicate checks for ADDRESS as its
                            owner would (the guest must not use it then)
      mldquery HOST         HOST asks the link which multicast groups are
                            wanted (MLD) and reports the answers in 2 seconds
      router6 HOST PREFIX/LEN LIFETIME [dns ADDRESS] [mtu N]
                            (64b) HOST is the link's router: it answers router
                            solicitations (and sends one advertisement now)
                            with the prefix (on the link, for addresses;
                            valid a day, preferred four hours), its router
                            lifetime, a DNS server and an MTU; it takes the
                            prefix and its interface ID as an address and,
                            as a gateway, answers echo requests to any address
                            [flags M|O|MO] (64g): DHCPv6's "managed", "other"
      router6 HOST off      one advertisement with router lifetime 0, then quiet
      dhcp6 HOST on PREFIX [dns ADDRESS] [t1 S] [valid S]
                            (64g) HOST is a DHCPv6 server: PREFIX::1:N for the Nth
                            client (by its DUID), the DNS server, T1 (valid / 2
                            unless given), the valid lifetime (an hour unless
                            given; preferred the same); Information-request
                            gets the DNS server alone
      dhcp6 HOST off        it answers no more
      ra6 HOST PREFIX/LEN VALID PREFERRED
                            one advertisement with one more prefix and these
                            lifetimes (seconds)
      addr6 HOST ADDRESS    HOST has ADDRESS too (neighbour solicitations, echo)
      frag6 HOST DST PORT SIZE [overlap]
                            (64c) HOST sends a UDP datagram of SIZE bytes (i % 251)
                            from its port 5555 to DST:PORT in pieces of 1232 bytes
                            (overlap: the second piece starts 8 bytes early) and
                            reports the answer that comes to port 5555 in 2 seconds
      udp HOST DST PORT TEXT [FROM]
                            (64e) HOST sends TEXT in one UDP datagram from its
                            port FROM (5555) to DST (IPv4 or IPv6) and reports
                            the answer that comes to that port in 2 seconds
      (65e) Each host also sends on TCP port 8889 (IPv4): a connection gets
                            BULK_BYTES of the tests' pattern (byte i = i % 251),
                            in the window the guest announces (scaled when both
                            sides offered window scaling, RFC 7323: our shift
                            is 7), then a FIN; the log says the guest's shift,
                            its largest window, the most bytes in flight.
                            With SACK offered (RFC 2018) it leaves out every
                            50th segment of the first 4 MB once and resends
                            only what the guest's SACK blocks show missing
                            (after 0.3 s without progress: everything from the
                            first unacknowledged byte the blocks do not cover);
                            the log says how many ACKs carried blocks, whether
                            each was right, what was resent
      bulkto HOST GUEST PORT
                            (65e-25) HOST opens the connection instead: a SYN
                            from its port 8889 to GUEST:PORT offering window
                            scaling (shift 7) and SACK, then the same sending
                            as port 8889's (the log line says what the guest's
                            SYN-ACK offered)
      (65e) Port 8890 (IPv4) receives: the guest's upload (the pattern) with
                            window scaling and SACK if it offers them; the
                            first copy of its data segments 40, 42, 44 and 46
                            is left out (close together: one flight), every segment acknowledged (with SACK
                            blocks, newest first) 100 ms after it came (a
                            round trip); the log says what came, whether right,
                            and how long the four holes took to be filled. Only
                            the first connection gets holes; each says the most
                            bytes it had in flight (65e-20: a send buffer past
                            64 KiB)
      (65e-29) Port 8891 (IPv4) sends 201 full segments of the pattern,
                            one every 15 ms (the last 200 ms after the others,
                            alone), then a FIN; the log says how many
                            ACKs the guest sent and how long the last segment's
                            took (delayed ACKs: about one per two segments)
      tcp6 HOST on          HOST answers TCP on port 8888 of its link-local
                            address: the first segment too big for a 1280-byte
                            link gets "packet too big" instead of an ACK; at
                            the guest's FIN it prints the bytes and the largest
                            segments before and after

IPv6 (64a): each host also has its link-local address (fe80:: and its MAC
as an EUI-64: A fe80::ff:fe00:7102, B fe80::ff:fe00:7202); it answers
neighbour solicitations and echo requests for it (and echo requests to
ff02::1), and prints the guest's duplicate checks and MLD reports.

Everything the hosts see is printed to stdout, one line each.
"""
import socket
import struct
import sys
import threading
import time

ROUTER = {"A": "192.168.71.1", "B": "192.168.72.1"}
ADDRESS = {"A": "192.168.71.2", "B": "192.168.72.2"}
MAC = {"A": bytes.fromhex("020000007102"), "B": bytes.fromhex("020000007202")}
BROADCAST = b"\xff" * 6

lock = threading.Lock()
events = []                     # (host, kind, details) as they come
events6 = []                    # (host, ICMPv6 type, source, destination, hop limit, body)

UNSPECIFIED6 = bytes(16)
ALL_NODES6 = bytes.fromhex("ff020000000000000000000000000001")


def link_local6(mac):
    return bytes.fromhex("fe80000000000000") + bytes([mac[0] ^ 2, mac[1], mac[2], 0xFF, 0xFE,
                                                      mac[3], mac[4], mac[5]])


def text6(a):
    return socket.inet_ntop(socket.AF_INET6, a)


def solicited6(a):
    return bytes.fromhex("ff0200000000000000000001ff") + a[13:]


def icmp6(src, dst, kind, code, rest):
    body = bytes([kind, code, 0, 0]) + rest
    pseudo = src + dst + struct.pack("!I", len(body)) + b"\0\0\0\x3a"
    return body[:2] + struct.pack("!H", checksum(pseudo + body)) + body[4:]


def ip6_packet(src, dst, next_header, payload, hops=64, alert=False):
    if alert:                   # a hop-by-hop header with Router Alert (MLD)
        payload = bytes([next_header, 0, 5, 2, 0, 0, 1, 0]) + payload
        next_header = 0
    return struct.pack("!IHBB", 0x60000000, len(payload), next_header, hops) + src + dst + payload


def nd_option(options, kind):
    """The link-layer address in an ND option of this kind, or None."""
    while len(options) >= 8 and options[1]:
        if options[0] == kind:
            return options[2:8]
        options = options[options[1] * 8:]
    return None


def say(text):
    print(text, flush=True)


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def ip_packet(src, dst, proto, payload, ttl=64):
    header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 0x1234, 0, ttl, proto, 0,
                         socket.inet_aton(src), socket.inet_aton(dst))
    header = header[:10] + struct.pack("!H", checksum(header)) + header[12:]
    return header + payload


def icmp(kind, code, rest):
    m = struct.pack("!BBH", kind, code, 0) + rest
    return m[:2] + struct.pack("!H", checksum(m)) + m[4:]


BULK_BYTES = 16000000                   # (65e) what port 8889 sends a connection (38d-3: 8 MB
                                        # left the guest's buffer growth too little room)
PACED_SEGMENTS = 201                    # (65e-29) what port 8891 sends, one at a time
_PERIOD = bytes(i % 251 for i in range(251)) * 300


def pattern(n, start=0):
    """(65e) The tests' pattern (byte i = i % 251), cut from a precomputed run."""
    out = bytearray()
    while n > 0:
        o = start % 251
        k = min(n, len(_PERIOD) - o)
        out += _PERIOD[o:o + k]
        n -= k
        start += k
    return bytes(out)


def syn_options(body, hlen):
    """(65e) A SYN's MSS (536 without one), window shift (None) and SACK
    permitted."""
    mss, shift, sack, i = 536, None, False, 20
    while i + 1 < hlen:
        kind = body[i]
        if kind == 0:
            break
        if kind == 1:
            i += 1
            continue
        if kind == 2 and body[i + 1] == 4:
            mss = struct.unpack("!H", body[i + 2:i + 4])[0]
        if kind == 3 and body[i + 1] == 3:
            shift = min(body[i + 2], 14)
        if kind == 4 and body[i + 1] == 2:
            sack = True
        i += max(body[i + 1], 2)
    return mss, shift, sack


def tcp_segment(sport, dport, seq, ack, flags, window, src, dst, options=b"", data=b""):
    while len(options) % 4:
        options += b"\0"
    offset = (5 + len(options) // 4) << 4
    seg = struct.pack("!HHIIBBHHH", sport, dport, seq, ack, offset, flags, window, 0, 0) + options + data
    pseudo = socket.inet_aton(src) + socket.inet_aton(dst) + struct.pack("!BBH", 0, 6, len(seg))
    return seg[:16] + struct.pack("!H", checksum(pseudo + seg)) + seg[18:]


def attack(hosts, who, guest):
    """Malformed packets a LAN attacker could send (49a): the machine must
    not hang or crash."""
    h = hosts[who]
    mac = h.resolve(guest)
    if not mac:
        return "no ARP answer from the guest"
    sent = []
    # A SYN flood: 40 SYNs with no final ACK. With SYN cookies the machine
    # keeps no state for them (49f), so none become half-open connections.
    for sp in range(41000, 41040):
        s = tcp_segment(sp, 22, 7000 + sp, 0, 0x02, 8192, h.ip, guest, bytes([2, 4, 0x05, 0xb4]))
        h.send(mac, 0x0800, ip_packet(h.ip, guest, 6, s))
    sent.append("SYN flood (40 SYNs, no ACK) to tcp 22")
    time.sleep(0.3)
    # A full handshake to port 22 with an MSS option of 0, so sshd accepts
    # and writes its banner: a zero segment size looped the sender forever.
    with lock:
        h.tcp = []
    seg = tcp_segment(40000, 22, 1000, 0, 0x02, 8192, h.ip, guest, bytes([2, 4, 0, 0]))
    h.send(mac, 0x0800, ip_packet(h.ip, guest, 6, seg))
    guest_seq = None
    deadline = time.time() + 2
    while time.time() < deadline and guest_seq is None:
        time.sleep(0.05)
        with lock:
            for src, body in h.tcp:
                sport, dport, seq, ack, off, flags = struct.unpack("!HHIIBB", body[:14])
                if sport == 22 and dport == 40000 and (flags & 0x12) == 0x12:
                    guest_seq = seq
    if guest_seq is not None:
        ackseg = tcp_segment(40000, 22, 1001, (guest_seq + 1) & 0xFFFFFFFF, 0x10, 8192, h.ip, guest)
        h.send(mac, 0x0800, ip_packet(h.ip, guest, 6, ackseg))
        sent.append("SYN+ACK mss=0 handshake to tcp 22")
        # RFC 5961 (49e): a data segment on the real four-tuple, with an
        # in-window sequence (our next byte, 1001) but an ACK far beyond
        # anything we sent. The guest must challenge-ACK and NOT take the
        # data, so its next-expected byte stays 1001; if it took our 8 bytes
        # it would acknowledge 1009.
        with lock:
            before_inj = len(h.tcp)
        bogus = (guest_seq + 1 + 1000000) & 0xFFFFFFFF
        inj = tcp_segment(40000, 22, 1001, bogus, 0x18, 8192, h.ip, guest, data=b"INJECT!!")
        h.send(mac, 0x0800, ip_packet(h.ip, guest, 6, inj))
        time.sleep(0.6)
        accepted = False
        with lock:
            for _src, b in h.tcp[before_inj:]:
                if len(b) >= 12 and struct.unpack("!I", b[8:12])[0] == (1001 + 8) & 0xFFFFFFFF:
                    accepted = True
        sent.append("bad-ACK data injection on tcp 22: " + ("ACCEPTED" if accepted else "rejected"))
    else:
        sent.append("SYN mss=0 to tcp 22 (no SYN-ACK seen)")
    # A UDP datagram to port 53 whose source is the guest itself (a martian
    # source: the kernel must drop it, so dnsd does not answer itself forever).
    dns = struct.pack("!HHHHHH", 0x4321, 0x0100, 1, 0, 0, 0) + b"\x07example\x03com\x00" + struct.pack("!HH", 1, 1)
    h.send(mac, 0x0800, ip_packet(guest, guest, 17, struct.pack("!HHHH", 5353, 53, 8 + len(dns), 0) + dns))
    sent.append("UDP src=guest to udp 53")
    # A DNS response (QR=1) to port 53 (dnsd must ignore it, not answer it).
    resp = struct.pack("!HHHHHH", 0x4321, 0x8180, 1, 0, 0, 0) + b"\x07example\x03com\x00" + struct.pack("!HH", 1, 1)
    h.send(mac, 0x0800, ip_packet(h.ip, guest, 17, struct.pack("!HHHH", 5353, 53, 8 + len(resp), 0) + resp))
    sent.append("DNS response to udp 53")
    # An echo request to the guest from an on-link source we never answer ARP
    # for (49c): the guest must learn the sender's MAC from this frame and
    # reply at once, not block its receive thread waiting for an ARP answer.
    # The reply (to the spoofed source, through our MAC) is captured in events.
    spoof = h.ip.rsplit(".", 1)[0] + ".123"
    with lock:
        start = len(events)
    req = icmp(8, 0, struct.pack("!HH", 0x5151, 1) + b"onlink")
    h.send(mac, 0x0800, ip_packet(spoof, guest, 1, req))
    replied = False
    deadline = time.time() + 1.5
    while time.time() < deadline and not replied:
        time.sleep(0.05)
        with lock:
            replied = any(ev[1] == 0 and ev[4] == spoof for ev in events[start:])
    sent.append("on-link spoofed-source ping to the guest: " +
                ("answered" if replied else "NOT answered"))
    # An echo request to a broadcast address (49g, anti-smurf): the guest
    # must not answer it (an amplifier).
    bcast = h.ip.rsplit(".", 1)[0] + ".255"
    with lock:
        start_b = len(events)
    breq = icmp(8, 0, struct.pack("!HH", 0x6161, 1) + b"smurf")
    h.send(mac, 0x0800, ip_packet(h.ip, bcast, 1, breq))
    time.sleep(0.6)
    with lock:
        answered_b = any(ev[1] == 0 and ev[4] == h.ip for ev in events[start_b:])
    sent.append("broadcast ping to %s: %s" % (bcast, "ANSWERED" if answered_b else "not answered"))
    # A spoofed gratuitous ARP claiming our address at a bogus MAC (49g), sent
    # LAST so nothing re-learns our real MAC after it: the guest already
    # learned our real MAC (from our who-has at the start), and sticky ARP
    # must keep it, not take the bogus one.
    bogus = bytes.fromhex("deadbeef0001")
    arp_spoof = struct.pack("!HHBBH6s4s6s4s", 1, 0x0800, 6, 4, 2, bogus,
                            socket.inet_aton(h.ip), mac, socket.inet_aton(guest))
    h.send(mac, 0x0806, arp_spoof)
    sent.append("spoofed gratuitous ARP for %s at de:ad:be:ef:00:01" % h.ip)
    time.sleep(0.5)
    return "sent: " + ", ".join(sent)


class Host:
    def __init__(self, name, port, guest_port):
        self.name = name
        self.ip = ADDRESS[name]
        self.mac = MAC[name]
        self.guest = ("127.0.0.1", guest_port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", port))
        self.arp = {}
        self.dhcp = False
        self.tcp = []
        self.conns = {}                 # TCP responder state per (ip, port), for port 8888
        self.bulk = {}                  # (65e) the port 8889 sender's state per (ip, port)
        self.bulk_to = {}               # (65e-25) bulkto's SYNs waiting for an answer
        self.sink = {}                  # (65e) the port 8890 receiver's state per (ip, port)
        self.paced = {}                 # (65e-29) the port 8891 sender's state per (ip, port)
        self.sinks = 0                  # (65e) connections to it so far
        self.ip6 = link_local6(self.mac)
        self.nd = {}                    # IPv6 neighbours: address -> MAC
        self.claims = set()             # addresses it defends (claim6)
        self.globals6 = set()           # its other IPv6 addresses (addr6, router6)
        self.tcp6 = False               # the port 8888 responder (tcp6)
        self.tcp6_conns = {}            # (address, port) -> state
        self.router6 = None             # (prefix, length, lifetime, dns, mtu) as the link's router
        self.router6_flags = 0          # its M (0x80) and O (0x40) flags
        self.dhcp6 = None               # (prefix, dns, t1, valid) as a DHCPv6 server
        self.dhcp6_clients = {}         # DUID -> its address

    def send(self, to_mac, kind, payload):
        self.sock.sendto(to_mac + self.mac + struct.pack("!H", kind) + payload, self.guest)

    def send_ip(self, packet, to_ip):
        mac = self.resolve(to_ip)
        if mac:
            self.send(mac, 0x0800, packet)

    def resolve(self, ip):
        for _ in range(20):
            with lock:
                if ip in self.arp:
                    return self.arp[ip]
            self.send(BROADCAST, 0x0806, struct.pack("!HHBBH6s4s6s4s", 1, 0x0800, 6, 4, 1, self.mac,
                                                     socket.inet_aton(self.ip), b"\0" * 6,
                                                     socket.inet_aton(ip)))
            time.sleep(0.1)
        return None

    def run(self):
        while True:
            frame, _ = self.sock.recvfrom(2048)
            if len(frame) < 14:
                continue
            kind = struct.unpack("!H", frame[12:14])[0]
            if kind == 0x0806:
                self.arp_input(frame[14:])
            elif kind == 0x0800 and frame[0:6] in (self.mac, BROADCAST):
                self.ip_input(frame[6:12], frame[14:])
            elif kind == 0x86DD and (frame[0:6] == self.mac or frame[0:2] == b"\x33\x33"):
                self.ip6_input(frame[6:12], frame[14:])

    def send6(self, packet, dst):
        """Send an IPv6 packet: to a group's MAC, else to the neighbour's
        (asked first); False if nobody answered for it."""
        if dst[0] == 0xFF:
            self.send(b"\x33\x33" + dst[12:], 0x86DD, packet)
            return True
        mac = self.resolve6(dst)
        if mac:
            self.send(mac, 0x86DD, packet)
        return mac is not None

    def resolve6(self, ip):
        for _ in range(20):
            with lock:
                if ip in self.nd:
                    return self.nd[ip]
            group = solicited6(ip)
            ns = icmp6(self.ip6, group, 135, 0, bytes(4) + ip + bytes([1, 1]) + self.mac)
            self.send(b"\x33\x33" + group[12:], 0x86DD, ip6_packet(self.ip6, group, 58, ns, 255))
            time.sleep(0.1)
        return None

    def advertise6(self, target, to, to_mac, solicited):
        flags = (0x40 if solicited else 0) | 0x20
        na = icmp6(target, to, 136, 0, bytes([flags, 0, 0, 0]) + target + bytes([2, 1]) + self.mac)
        self.send(to_mac, 0x86DD, ip6_packet(target, to, 58, na, 255))

    def ip6_input(self, from_mac, p):
        if len(p) < 40:
            return
        length, next_header, hops = struct.unpack("!HBB", p[4:8])
        src, dst, body = p[8:24], p[24:40], p[40:40 + length]
        if next_header == 0 and len(body) >= 8:             # hop-by-hop options (MLD)
            next_header, body = body[0], body[(body[1] + 1) * 8:]
        if next_header == 17 and len(body) >= 8 and struct.unpack("!H", body[2:4])[0] == 547:
            if self.dhcp6:
                self.dhcp6_input(from_mac, src, struct.unpack("!H", body[0:2])[0], body[8:])
            return
        if next_header == 17 and len(body) >= 8:
            sport, dport = struct.unpack("!HH", body[:4])
            with lock:
                events6.append((self.name, "udp", src, dst, dport, body[8:]))
            say(f"{self.name}: UDP6 from {text6(src)} port {sport} to port {dport}: {body[8:].decode(errors='replace')}")
            return
        if next_header == 6 and len(body) >= 20 and self.tcp6 and dst == self.ip6:
            self.tcp6_respond(from_mac, src, p[:40 + length], body)
            return
        if next_header != 58 or len(body) < 4:
            say(f"{self.name}: IPv6 from {text6(src)} to {text6(dst)}: next header {next_header}")
            return
        pseudo = src + dst + struct.pack("!I", len(body)) + b"\0\0\0\x3a"
        if checksum(pseudo + body) != 0:
            say(f"{self.name}: ICMPv6 {body[0]} from {text6(src)}: checksum WRONG")
            return
        kind = body[0]
        with lock:
            events6.append((self.name, kind, src, dst, hops, body))
        if kind == 135 and len(body) >= 24:                 # neighbour solicitation
            target, mac = body[8:24], nd_option(body[24:], 1)
            if src == UNSPECIFIED6:
                say(f"{self.name}: NDP: duplicate check for {text6(target)}, hop limit {hops}")
                if target in self.claims:
                    self.advertise6(target, ALL_NODES6, b"\x33\x33" + ALL_NODES6[12:], False)
                    say(f"{self.name}: NDP: {text6(target)} is ours: told everyone")
                return
            say(f"{self.name}: NDP: who has {text6(target)}, asked by {text6(src)}"
                f"{' with its MAC' if mac else ''}, hop limit {hops}")
            if mac:
                with lock:
                    self.nd[src] = mac
            if target == self.ip6 or target in self.claims or target in self.globals6:
                self.advertise6(target, src, mac or from_mac, True)
        elif kind == 136 and len(body) >= 24:               # neighbour advertisement
            target, mac = body[8:24], nd_option(body[24:], 2)
            with lock:
                self.nd[target] = mac or from_mac
            say(f"{self.name}: NDP: {text6(target)} is at {(mac or from_mac).hex(':')}, "
                f"{'an answer' if body[4] & 0x40 else 'news'}, hop limit {hops}")
        elif kind == 143 and len(body) >= 8:                # MLDv2 report
            groups = [text6(body[8 + 20 * i + 4:8 + 20 * i + 20])
                      for i in range(struct.unpack("!H", body[6:8])[0])]
            say(f"{self.name}: MLD report from {text6(src)}, hop limit {hops}: {', '.join(groups)}")
        elif kind == 133:                                   # router solicitation
            say(f"{self.name}: NDP: router solicitation from {text6(src)}, hop limit {hops}")
            if self.router6:
                self.advertise_router()
        elif kind == 128 and len(body) >= 8 and (dst in (self.ip6, ALL_NODES6) or
                                                 dst in self.globals6 or self.router6):
            # (as the router, a gateway: any address is answered, from itself)
            me = self.ip6 if dst == ALL_NODES6 else dst
            reply = icmp6(me, src, 129, 0, body[4:])
            self.send(from_mac, 0x86DD, ip6_packet(me, src, 58, reply))
            say(f"{self.name}: ICMPv6 echo request from {text6(src)} to {text6(dst)}: answered")

    def tcp6_respond(self, from_mac, src, packet, body):
        """The port 8888 responder (tcp6): a handshake, ACKs in order, one
        "packet too big" (MTU 1280) for the first segment that would not
        fit, a FIN for the guest's."""
        sport, dport, seq, ack, off, flags = struct.unpack("!HHIIBB", body[:14])
        if dport != 8888:
            return
        data = body[(off >> 4) * 4:]
        key = (src, sport)

        def out(myseq, myack, fl, options=b""):
            hlen = 20 + len(options)
            seg = struct.pack("!HHIIBBHHH", 8888, sport, myseq, myack, (hlen // 4) << 4, fl, 65535, 0, 0) + options
            pseudo = self.ip6 + src + struct.pack("!I", len(seg)) + b"\0\0\0\x06"
            seg = seg[:16] + struct.pack("!H", checksum(pseudo + seg)) + seg[18:]
            self.send(from_mac, 0x86DD, ip6_packet(self.ip6, src, 6, seg))

        if (flags & 0x02) and not (flags & 0x10):
            self.tcp6_conns[key] = {"next": (seq + 1) & 0xFFFFFFFF, "bytes": 0, "before": 0,
                                    "after": 0, "too_big": False}
            out(7000, (seq + 1) & 0xFFFFFFFF, 0x12, struct.pack("!BBH", 2, 4, 1440))
            return
        c = self.tcp6_conns.get(key)
        if not c:
            return
        if data and len(packet) > 1280 and not c["too_big"]:
            c["too_big"] = True
            quoted = packet[:1280 - 48]
            ptb = icmp6(self.ip6, src, 2, 0, struct.pack("!I", 1280) + quoted)
            self.send(from_mac, 0x86DD, ip6_packet(self.ip6, src, 58, ptb))
            say(f"{self.name}: tcp6 8888: a {len(packet)}-byte packet: packet too big (1280)")
            return
        if data and seq == c["next"]:
            c["next"] = (c["next"] + len(data)) & 0xFFFFFFFF
            c["bytes"] += len(data)
            which = "after" if c["too_big"] else "before"
            c[which] = max(c[which], len(data))
        if flags & 0x01:
            c["next"] = (c["next"] + 1) & 0xFFFFFFFF
            out(7001, c["next"], 0x11)              # FIN + ACK
            say(f"{self.name}: tcp6 8888: {c['bytes']} bytes, segments of at most {c['before']} "
                f"before packet too big, {c['after']} after")
            return
        if data:
            out(7001, c["next"], 0x10)

    def dhcp6_input(self, from_mac, src, sport, m):
        """A DHCPv6 client's message (to ff02::1:2 or us, port 547)."""
        if len(m) < 4:
            return
        kind, xid = m[0], m[1:4]
        options, at = {}, 4
        while at + 4 <= len(m):
            code, length = struct.unpack("!HH", m[at:at + 4])
            options[code] = m[at + 4:at + 4 + length]
            at += 4 + length
        client = options.get(1)
        if client is None:
            return
        prefix, dns, t1, valid = self.dhcp6
        names = {1: "Solicit", 3: "Request", 5: "Renew", 6: "Rebind", 11: "Information-request"}
        if kind not in names:
            return
        server_duid = struct.pack("!HH", 3, 1) + self.mac
        if kind in (3, 5) and options.get(2) != server_duid:
            return                                  # (another server's)

        def opt(code, data):
            return struct.pack("!HH", code, len(data)) + data
        reply = bytes([2 if kind == 1 else 7]) + xid + opt(1, client) + opt(2, server_duid)
        if dns:
            reply += opt(23, dns)
        said = f"{self.name}: DHCPv6 {names[kind]} from {text6(src)}"
        if kind != 11:
            if client not in self.dhcp6_clients:
                self.dhcp6_clients[client] = prefix[:8] + bytes(5) + bytes([1, 0, len(self.dhcp6_clients) + 1])
            address = self.dhcp6_clients[client]
            iaid = options.get(3, bytes(12))[:4]
            ia = iaid + struct.pack("!II", t1, valid * 4 // 5) + opt(5, address + struct.pack("!II", valid, valid))
            reply += opt(3, ia)
            said += f": {'advertised' if kind == 1 else 'gave'} {text6(address)}"
        udp = struct.pack("!HHHH", 547, sport, 8 + len(reply), 0) + reply
        pseudo = self.ip6 + src + struct.pack("!I", len(udp)) + b"\0\0\0\x11"
        udp = udp[:6] + struct.pack("!H", checksum(pseudo + udp) or 0xFFFF) + udp[8:]
        self.send(from_mac, 0x86DD, ip6_packet(self.ip6, src, 17, udp))
        say(said)

    def advertise_router(self, lifetime=None, extra=None):
        """A router advertisement to ff02::1: our MAC, the prefix, MTU and DNS
        server as router6 set them; extra: (prefix, length, valid, preferred)."""
        prefix, length, router_lifetime, dns, mtu = self.router6
        if lifetime is not None:
            router_lifetime = lifetime
        options = bytes([1, 1]) + self.mac
        for (p, n, valid, preferred) in [(prefix, length, 86400, 14400)] + ([extra] if extra else []):
            options += struct.pack("!BBBBIII", 3, 4, n, 0xC0, valid, preferred, 0) + p
        if mtu:
            options += struct.pack("!BBHI", 5, 1, 0, mtu)
        if dns:
            options += struct.pack("!BBHI", 25, 3, 0, 1200) + dns
        ra = icmp6(self.ip6, ALL_NODES6, 134, 0, struct.pack("!BBHII", 64, self.router6_flags,
                                                             router_lifetime, 0, 0) + options)
        self.send(b"\x33\x33" + ALL_NODES6[12:], 0x86DD, ip6_packet(self.ip6, ALL_NODES6, 58, ra, 255))
        say(f"{self.name}: NDP: router advertisement, lifetime {router_lifetime}")

    def arp_input(self, p):
        if len(p) < 28:
            return
        op, smac, sip, tip = struct.unpack("!6xH6s4s6x4s", p[:28])
        sip, tip = socket.inet_ntoa(sip), socket.inet_ntoa(tip)
        with lock:
            self.arp[sip] = smac
        if op == 1 and tip == self.ip:
            say(f"{self.name}: ARP: who has {tip}, asked by {sip}: answered")
            self.send(smac, 0x0806, struct.pack("!HHBBH6s4s6s4s", 1, 0x0800, 6, 4, 2, self.mac,
                                                socket.inet_aton(self.ip), smac,
                                                socket.inet_aton(sip)))

    def ip_input(self, from_mac, p):
        if len(p) < 20:
            return
        length = struct.unpack("!H", p[2:4])[0]
        ttl, proto = p[8], p[9]
        src, dst = socket.inet_ntoa(p[12:16]), socket.inet_ntoa(p[16:20])
        good = checksum(p[:20]) == 0
        body = p[20:length]
        if proto == 17 and len(body) >= 8 and struct.unpack("!H", body[2:4])[0] == 67:
            self.dhcp_input(body[8:])
            return
        if proto == 17 and len(body) >= 8 and struct.unpack("!H", body[2:4])[0] == 5555:
            with lock:
                events6.append((self.name, "udp", socket.inet_aton(src), socket.inet_aton(dst), 5555,
                                body[8:]))
            return
        if proto == 17 and len(body) >= 8 and struct.unpack("!H", body[2:4])[0] == 9999:
            self.flood_then_reply(from_mac, src, struct.unpack("!H", body[0:2])[0])
            return
        if proto == 6 and len(body) >= 20:
            if struct.unpack("!H", body[2:4])[0] == 8888:
                self.tcp_respond(from_mac, src, body)
                return
            if struct.unpack("!H", body[2:4])[0] == 8889:
                self.tcp_bulk(from_mac, src, body)
                return
            if struct.unpack("!H", body[2:4])[0] == 8890:
                self.tcp_sink(from_mac, src, body)
                return
            if struct.unpack("!H", body[2:4])[0] == 8891:
                self.tcp_paced(from_mac, src, body)
                return
            with lock:
                self.tcp.append((src, body))
            return
        if proto != 1 or len(body) < 8:
            say(f"{self.name}: from {src} to {dst}: protocol {proto}, ttl {ttl}")
            return
        kind, code = body[0], body[1]
        say(f"{self.name}: ICMP {kind}/{code} from {src} to {dst}, ttl {ttl}, "
            f"header checksum {'ok' if good else 'WRONG'}")
        with lock:
            events.append((self.name, kind, code, src, dst, ttl, good, body))
        if kind == 8 and (dst == self.ip or self.name == "B"):
            # An echo request: answer it (B for any address: it is the gateway).
            reply = icmp(0, 0, body[4:])
            self.sock.sendto(from_mac + self.mac + b"\x08\x00" + ip_packet(dst, src, 1, reply),
                             self.guest)


    def tcp_respond(self, from_mac, src, body):
        """A minimal TCP server on port 8888 for the FIN_WAIT_2 test (49e):
        it completes a handshake, acknowledges data and acknowledges the
        guest's FIN, but never sends its own FIN. So a guest that connects,
        sends and closes is left in FIN_WAIT_2, waiting for a FIN that never
        comes; the kernel must free that slot after a timeout."""
        sport, dport, seq, ack, off, flags = struct.unpack("!HHIIBB", body[:14])
        data = body[(off >> 4) * 4:]
        key = (src, sport)

        def out(myseq, myack, fl):
            seg = tcp_segment(8888, sport, myseq, myack, fl, 65535, self.ip, src)
            self.sock.sendto(from_mac + self.mac + b"\x08\x00" + ip_packet(self.ip, src, 6, seg),
                             self.guest)

        if (flags & 0x02) and not (flags & 0x10):           # SYN: answer SYN+ACK
            self.conns[key] = (seq + 1) & 0xFFFFFFFF        # their next sequence number
            out(9000, (seq + 1) & 0xFFFFFFFF, 0x12)
            return
        if key not in self.conns:
            return
        theirs = self.conns[key]
        if len(data) > 0 and seq == theirs:
            theirs = (theirs + len(data)) & 0xFFFFFFFF
        if flags & 0x01:                                    # FIN: count it, ack it, no FIN of ours
            theirs = (theirs + 1) & 0xFFFFFFFF
            say(f"{self.name}: tcp 8888: acked the guest's FIN, sending none (FIN_WAIT_2)")
        self.conns[key] = theirs
        if len(data) > 0 or (flags & 0x01):                 # acknowledge data or the FIN
            out(9001, theirs, 0x10)

    def tcp_bulk(self, from_mac, src, body):
        """(65e) The port 8889 sender for the window scaling test: a
        handshake (with a window scale option of ours if the guest's SYN
        offered one), then BULK_BYTES in what the guest's window and a
        congestion window allow (slow start from 10 segments, halved on a
        loss), going back to the first unacknowledged byte after three
        duplicate ACKs or 0.3 s without progress (a burst can overflow the
        card's ring: the guest keeps what came after a gap)."""
        sport, dport, seq, ack, off, flags, window = struct.unpack("!HHIIBBH", body[:16])
        hlen = (off >> 4) * 4
        key = (src, sport)
        if (flags & 0x02) and not (flags & 0x10):           # SYN: its options, our SYN+ACK
            mss, shift, sack = syn_options(body, hlen)
            c = self.bulk_new(key, from_mac, seq, mss, shift, sack)
            opts = struct.pack("!BBH", 2, 4, 1460) + (bytes([1, 3, 3, 7]) if shift is not None else b"")
            opts += bytes([1, 1, 4, 2]) if sack else b""
            self.bulk_out(c, 0x12, 50000, opts=opts)
            return
        if (flags & 0x12) == 0x12 and key in self.bulk_to:  # (65e-25) the answer to bulkto's SYN
            del self.bulk_to[key]
            mss, shift, sack = syn_options(body, hlen)
            c = self.bulk_new(key, from_mac, seq, mss, shift, sack)
            with c["lock"]:
                c["win"] = window                           # (a SYN's window is never scaled)
                self.bulk_out(c, 0x10, 50001)
                self.bulk_send(c)
            return
        c = self.bulk.get(key)
        if c is None:
            return
        with c["lock"]:
            if len(body) > hlen and seq == c["theirs"]:
                c["theirs"] = (c["theirs"] + len(body) - hlen) & 0xFFFFFFFF
            if flags & 0x01:                                # the guest's FIN: acknowledge it
                c["theirs"] = (c["theirs"] + 1) & 0xFFFFFFFF
                self.bulk_out(c, 0x10, c["end"] + 1)
                return
            if not (flags & 0x10):
                return
            if c["sack"]:
                self.bulk_sack(c, body, hlen, ack)
            c["win"] = window << c["shift"] if c["shift"] is not None else window
            c["maxwin"] = max(c["maxwin"], c["win"])
            if ack == c["end"] + 1 and c["fin"]:
                c["fin_acked"] = True                       # (65e-25) the FIN arrived
            if c["una"] < ack <= c["end"] + 1:
                c["cwnd"] = min(c["cwnd"] + ack - c["una"], 1 << 20)   # slow start, at most 1 MiB
                c["una"] = ack
                c["dups"] = 0
                c["progress"] = time.time()
                if c["nxt"] < c["una"]:
                    c["nxt"] = c["una"]
            elif ack == c["una"] and c["nxt"] > c["una"] and len(body) == hlen:
                c["dups"] += 1
                if c["dups"] == 3 and not c["sack"]:        # a loss: go back
                    self.bulk_back(c)
            if c["sack"]:
                self.bulk_repair(c)
            self.bulk_send(c)

    def bulk_new(self, key, from_mac, seq, mss, shift, sack):
        """(65e) A port 8889 connection's state; its timer starts."""
        c = self.bulk[key] = dict(theirs=(seq + 1) & 0xFFFFFFFF, una=50001, nxt=50001,
                                  end=50001 + BULK_BYTES, mss=min(mss, 1460), shift=shift,
                                  win=0, maxwin=0, maxfly=0, fin=False, dups=0,
                                  cwnd=10 * min(mss, 1460), progress=time.time(),
                                  mac=from_mac, src=key[0], sport=key[1], lock=threading.Lock(),
                                  resent=0, sack=sack, holes=[], left_out=0, board=[],
                                  sack_acks=0, most_blocks=0, wrong=None, resent_bytes=0,
                                  repaired=set(), segs=0, sent_max=50001, resent_at=set(),
                                  fin_acked=False)
        threading.Thread(target=self.bulk_timer, args=(c,), daemon=True).start()
        return c

    def bulk_connect(self, guest_ip, port):
        """(65e-25) bulkto: a SYN from port 8889 to the guest's PORT with
        our MSS, window scale (7) and SACK permitted; its answer comes to
        tcp_bulk, which sends as to a connection the guest opened."""
        mac = self.resolve(guest_ip)
        if mac is None:
            return f"{self.name}: no ARP answer from {guest_ip}"
        self.bulk_to[(guest_ip, port)] = True
        opts = struct.pack("!BBH", 2, 4, 1460) + bytes([1, 3, 3, 7, 1, 1, 4, 2])
        seg = tcp_segment(8889, port, 50000, 0, 0x02, 65535, self.ip, guest_ip, opts)
        self.sock.sendto(mac + self.mac + b"\x08\x00" + ip_packet(self.ip, guest_ip, 6, seg), self.guest)
        return f"{self.name}: SYN sent to {guest_ip} port {port}"

    def tcp_paced(self, from_mac, src, body):
        """(65e-29) Port 8891: PACED_SEGMENTS full segments of the pattern,
        one every 15 ms (each in a card's burst of its own), then a FIN; it
        counts the guest's ACKs and how long the last segment's took."""
        sport, dport, seq, ack, off, flags = struct.unpack("!HHIIBB", body[:14])
        hlen = (off >> 4) * 4
        key = (src, sport)

        def out(myseq, fl, data=b"", opts=b""):
            seg = tcp_segment(8891, sport, myseq, (seq + 1) & 0xFFFFFFFF if fl == 0x12 else
                              self.paced[key]["theirs"], fl, 65535, self.ip, src, opts, data)
            self.sock.sendto(from_mac + self.mac + b"\x08\x00" + ip_packet(self.ip, src, 6, seg),
                             self.guest)

        if (flags & 0x02) and not (flags & 0x10):
            self.paced[key] = dict(theirs=(seq + 1) & 0xFFFFFFFF, acks=0, una=90001, started=False,
                                   last_sent=None, last_acked=None)
            out(90000, 0x12, opts=struct.pack("!BBH", 2, 4, 1460))
            return
        p = self.paced.get(key)
        if p is None:
            return
        if len(body) > hlen and seq == p["theirs"]:
            p["theirs"] = (p["theirs"] + len(body) - hlen) & 0xFFFFFFFF
        if flags & 0x01:
            p["theirs"] = (p["theirs"] + 1) & 0xFFFFFFFF
            out(90001 + PACED_SEGMENTS * 1460 + 1, 0x10)
            return
        if not (flags & 0x10):
            return
        end = 90001 + PACED_SEGMENTS * 1460
        if p["started"] and len(body) == hlen and ack > p["una"]:
            p["acks"] += 1
            p["una"] = ack
            if ack == end and p["last_acked"] is None:
                p["last_acked"] = time.time()
        if not p["started"]:
            p["started"] = True

            def send_all():
                for k in range(PACED_SEGMENTS):
                    at = 90001 + k * 1460
                    if k == PACED_SEGMENTS - 1:
                        time.sleep(0.2)                     # (any held ACK has gone: the last
                                                            # comes alone, its ACK the timer's)
                    out(at, 0x18, data=pattern(1460, at - 90001))
                    if k == PACED_SEGMENTS - 1:
                        p["last_sent"] = time.time()
                    time.sleep(0.015)
                for _ in range(100):
                    if p["last_acked"] is not None:
                        break
                    time.sleep(0.01)
                took = int((p["last_acked"] - p["last_sent"]) * 1000) if p["last_acked"] else -1
                say(f"{self.name}: tcp 8891: {PACED_SEGMENTS} segments one at a time, every 15 ms: "
                    f"{p['acks']} ACKs; the last acknowledged after {took} ms")
                out(end, 0x11)

            threading.Thread(target=send_all, daemon=True).start()

    def tcp_sink(self, from_mac, src, body):
        """(65e) The port 8890 receiver for the sending side's SACK test."""
        sport, dport, seq, ack, off, flags, window = struct.unpack("!HHIIBBH", body[:16])
        hlen = (off >> 4) * 4
        key = (src, sport)

        def out(r, fl, myseq, opts=b""):
            seg = tcp_segment(8890, sport, myseq, r["nxt"], fl, 65535, self.ip, src, opts)
            self.sock.sendto(from_mac + self.mac + b"\x08\x00" + ip_packet(self.ip, src, 6, seg),
                             self.guest)

        if (flags & 0x02) and not (flags & 0x10):           # SYN
            shift = sack = None
            i = 20
            while i + 1 < hlen:
                kind = body[i]
                if kind == 0:
                    break
                if kind == 1:
                    i += 1
                    continue
                if kind == 3 and body[i + 1] == 3:
                    shift = body[i + 2]
                if kind == 4 and body[i + 1] == 2:
                    sack = True
                i += max(body[i + 1], 2)
            self.sinks += 1
            r = self.sink[key] = dict(isn=(seq + 1) & 0xFFFFFFFF, nxt=(seq + 1) & 0xFFFFFFFF,
                                      ranges=[], data={}, segs=0, dropped=[], first_drop=None,
                                      filled=None, sack=bool(sack), resent=0, fin=False,
                                      lock=threading.Lock(), holes=self.sinks == 1,
                                      history=[], top=0, flight=0, number=self.sinks)
            opts = struct.pack("!BBH", 2, 4, 1460)
            if shift is not None:
                opts += bytes([1, 3, 3, 7])
            if sack:
                opts += bytes([1, 1, 4, 2])
            out(r, 0x12, 70000, opts)
            return
        r = self.sink.get(key)
        if r is None:
            return
        with r["lock"]:
            data = body[hlen:]
            if data:
                start = (seq - r["isn"]) & 0xFFFFFFFF
                end = start + len(data)
                if start in r["data"] or start < r["nxt"] - r["isn"]:
                    r["resent"] += len(data)
                first_copy = start not in r["data"] and start not in [d for d, _ in r["dropped"]]
                if first_copy:
                    r["segs"] += 1
                if r["holes"] and first_copy and r["segs"] in (40, 42, 44, 46):   # (one flight)
                    r["dropped"].append((start, end))   # left out once
                    if r["first_drop"] is None:
                        r["first_drop"] = time.time()
                else:
                    r["data"][start] = bytes(data)
                    self.sink_merge(r, start, end)
                # In flight as the guest sees it: the highest byte come, less what
                # was acknowledged a round trip (100 ms) ago (the ACK it just saw).
                now = time.time()
                r["top"] = max(r["top"], end)
                acked = 0
                for when, nxt in r["history"]:
                    if when > now - 0.1:
                        break
                    acked = nxt
                r["flight"] = max(r["flight"], r["top"] - acked)
                r["history"].append((now, (r["nxt"] - r["isn"]) & 0xFFFFFFFF))
                while len(r["history"]) > 1 and r["history"][1][0] <= now - 0.1:
                    r["history"].pop(0)
            if flags & 0x01 and (seq + len(data) - r["isn"]) & 0xFFFFFFFF == r["nxt"] - r["isn"]:
                r["nxt"] = (r["nxt"] + 1) & 0xFFFFFFFF
                r["fin"] = True
                self.sink_report(r)
            if data or (flags & 0x01):
                blocks = b""
                if r["sack"] and r["ranges"]:
                    newest = [x for x in r["ranges"] if x[0] <= start < x[1]] if data else []
                    order = newest + [x for x in r["ranges"] if x not in newest]
                    for a, b in order[:4]:
                        blocks += struct.pack("!II", (r["isn"] + a) & 0xFFFFFFFF,
                                              (r["isn"] + b) & 0xFFFFFFFF)
                    blocks = bytes([1, 1, 5, 2 + len(blocks)]) + blocks
                state = (r["nxt"], blocks)
                threading.Timer(0.1, self.sink_ack, args=(from_mac, src, sport, state)).start()

    def sink_merge(self, r, start, end):
        """In order: move nxt on over what is there; else keep the range."""
        r["ranges"].append((start, end))
        r["ranges"].sort()
        merged = []
        for a, b in r["ranges"]:
            if merged and a <= merged[-1][1]:
                merged[-1] = (merged[-1][0], max(merged[-1][1], b))
            else:
                merged.append((a, b))
        base = r["nxt"] - r["isn"]
        while merged and merged[0][0] <= base:
            base = max(base, merged[0][1])
            merged.pop(0)
        r["nxt"] = (r["isn"] + base) & 0xFFFFFFFF
        r["ranges"] = merged
        if r["dropped"] and r["filled"] is None and len(r["dropped"]) == 4 and \
                base >= max(e for _, e in r["dropped"]):
            r["filled"] = time.time()

    def sink_ack(self, from_mac, src, sport, state):
        nxt, blocks = state
        seg = tcp_segment(8890, sport, 70001, nxt, 0x10, 65535, self.ip, src, blocks)
        self.sock.sendto(from_mac + self.mac + b"\x08\x00" + ip_packet(self.ip, src, 6, seg),
                         self.guest)

    def sink_report(self, r):
        total = (r["nxt"] - r["isn"] - 1) & 0xFFFFFFFF
        whole = bytearray(total)
        for k, piece in r["data"].items():          # each at its offset (resent pieces too)
            whole[k:k + len(piece)] = piece[:max(0, total - k)]
        good = bytes(whole) == pattern(total, 0)
        took = int((r["filled"] - r["first_drop"]) * 1000) if r["filled"] and r["first_drop"] else -1
        say(f"{self.name}: tcp 8890: received {total} bytes, the pattern "
            f"{'right' if good else 'WRONG'}; SACK {'offered' if r['sack'] else 'not offered'}; "
            f"{len(r['dropped'])} segments left out once, filled {took} ms after the first "
            f"(round trips of 100 ms); {r['resent']} bytes came again; at most {r['flight']} bytes "
            f"in flight (connection {r['number']})")

    def bulk_out(self, c, fl, myseq, data=b"", opts=b""):
        seg = tcp_segment(8889, c["sport"], myseq, c["theirs"], fl, 65535, self.ip, c["src"], opts, data)
        self.sock.sendto(c["mac"] + self.mac + b"\x08\x00" + ip_packet(self.ip, c["src"], 6, seg),
                         self.guest)

    def bulk_sack(self, c, body, hlen, ack):
        """(65e) An ACK's SACK blocks onto the scoreboard, each checked: above
        what is acknowledged, inside what was sent, not over a hole left out."""
        i, blocks = 20, []
        while i + 1 < hlen:
            kind = body[i]
            if kind == 0:
                break
            if kind == 1:
                i += 1
                continue
            if kind == 5:
                for k in range(i + 2, i + body[i + 1], 8):
                    blocks.append(struct.unpack("!II", body[k:k + 8]))
            i += max(body[i + 1], 2)
        if not blocks:
            return
        c["sack_acks"] += 1
        c["most_blocks"] = max(c["most_blocks"], len(blocks))
        for start, end in blocks:
            bad = not (ack < start < end <= c["sent_max"]) or any(   # (ever sent: a go-back moves nxt)
                start < h_end and h_start < end and (h_start, h_end) not in c["repaired"]
                for h_start, h_end in c["holes"])
            if bad and c["wrong"] is None:
                c["wrong"] = f"[{start}, {end}) with ACK {ack}"
            c["board"].append((start, end))
        merged = []
        for start, end in sorted(c["board"]):
            if merged and start <= merged[-1][1]:
                merged[-1] = (merged[-1][0], max(merged[-1][1], end))
            else:
                merged.append((start, end))
        c["board"] = [(a, b) for a, b in merged if b > ack]

    def bulk_repair(self, c):
        """(65e) Resend, once each (until the next stall: 65e-25, it was
        once per piece, and a piece changes as the ACKs move), the segments
        between what is acknowledged and the highest SACKed byte that no
        block covers."""
        at = c["una"]
        for start, end in c["board"]:
            p = at
            while p < start:
                n = min(c["mss"], start - p)
                if p not in c["resent_at"]:
                    c["resent_at"].add(p)
                    for h_start, h_end in c["holes"]:
                        if h_start < p + n and p < h_end:
                            c["repaired"].add((h_start, h_end))
                    self.bulk_out(c, 0x10, p, data=pattern(n, p - 50001))
                    c["resent_bytes"] += n
                p += n
            at = max(at, end)

    def bulk_back(self, c):
        c["cwnd"] = max(c["cwnd"] // 2, 10 * c["mss"])
        c["nxt"] = c["una"]
        c["resent"] += 1

    def bulk_send(self, c):
        limit = min(c["win"], c["cwnd"])
        while c["nxt"] < c["end"]:
            n = min(c["mss"], c["end"] - c["nxt"], limit - (c["nxt"] - c["una"]))
            if n <= 0:
                break
            fresh = c["nxt"] >= c["sent_max"]               # (never sent before)
            if not fresh and c["sack"] and any(a <= c["nxt"] and c["nxt"] + n <= b
                                               for a, b in c["board"]):
                c["nxt"] += n                               # (65e-25) back, but not over what it has
                continue
            if not fresh:
                c["resent_bytes"] += n
            if fresh:
                c["segs"] += 1
            if c["sack"] and fresh and c["segs"] % 50 == 0 and c["nxt"] - 50001 < 4000000:
                c["holes"].append((c["nxt"], c["nxt"] + n))   # left out on purpose, once
                c["left_out"] += n
            else:
                self.bulk_out(c, 0x10, c["nxt"], data=pattern(n, c["nxt"] - 50001))
            c["nxt"] += n
            c["sent_max"] = max(c["sent_max"], c["nxt"])
            c["maxfly"] = max(c["maxfly"], c["nxt"] - c["una"])
        if c["una"] >= c["end"] and not c["fin"]:
            c["fin"] = True
            self.bulk_out(c, 0x11, c["end"])
            say(f"{self.name}: tcp 8889: sent {BULK_BYTES} bytes; the guest offered "
                f"{'shift ' + str(c['shift']) if c['shift'] is not None else 'no window scaling'}; "
                f"its largest window {c['maxwin']}; at most {c['maxfly']} bytes in flight; "
                f"went back {c['resent']} time(s)")
            if c["sack"]:
                say(f"{self.name}: tcp 8889: SACK offered; {len(c['holes'])} segments ({c['left_out']} "
                    f"bytes) left out once; {c['sack_acks']} ACKs with SACK blocks (at most "
                    f"{c['most_blocks']} in one), "
                    f"{'every block right' if c['wrong'] is None else 'a wrong block ' + c['wrong']}; "
                    f"resent {c['resent_bytes']} bytes")
            else:
                say(f"{self.name}: tcp 8889: no SACK offered")

    def bulk_timer(self, c):
        """0.3 s without progress: back to the first unacknowledged byte (with
        SACK, 65e-25, skipping what the blocks show came: until then it sent
        the first segment alone, and a burst whose tail the link lost took a
        segment per 0.3 s, a minute in all); a FIN without its ACK again."""
        while not c["fin_acked"]:
            time.sleep(0.1)
            with c["lock"]:
                if time.time() - c["progress"] <= 0.3:
                    continue
                if c["fin"]:
                    if not c["fin_acked"]:
                        self.bulk_out(c, 0x11, c["end"])
                elif c["nxt"] > c["una"]:
                    if c["sack"]:
                        c["repaired"] = set((h_start, h_end) for h_start, h_end in c["holes"])
                        c["resent_at"] = set()
                    self.bulk_back(c)
                    self.bulk_send(c)
                c["progress"] = time.time()

    def flood_then_reply(self, from_mac, guest_ip, guest_port):
        """The guest sent a UDP request to port 9999 (the 49d flow-flood
        test): fill the guard's flow table with inbound ICMP flows (echo
        replies, distinct identifiers, so the guest opens a flow for each but
        answers none), then reply. The reply gets back only if the machine's
        own outbound flow (to us:9999) was kept through the flood."""
        for ident in range(1, 301):
            rep = icmp(0, 0, struct.pack("!HH", ident, 1) + b"flood")
            self.sock.sendto(from_mac + self.mac + b"\x08\x00" + ip_packet(self.ip, guest_ip, 1, rep),
                             self.guest)
        time.sleep(0.4)
        payload = b"flood-survived"
        udp = struct.pack("!HHHH", 9999, guest_port, 8 + len(payload), 0) + payload
        self.sock.sendto(from_mac + self.mac + b"\x08\x00" + ip_packet(self.ip, guest_ip, 17, udp),
                         self.guest)
        say(f"{self.name}: flow-flood (300 inbound) then a reply to {guest_ip}:{guest_port}")

    def dhcp_input(self, m):
        """A DHCP request from the guest: OFFER for DISCOVER, ACK for REQUEST."""
        if not self.dhcp or len(m) < 240 or m[0] != 1:
            return
        kind, i = None, 240
        while i + 1 < len(m) and m[i] != 255:
            if m[i] == 0:
                i += 1
                continue
            if m[i] == 53:
                kind = m[i + 2]
            i += 2 + m[i + 1]
        answer = {1: 2, 3: 5}.get(kind)
        if not answer:
            return
        offered = self.ip.rsplit(".", 1)[0] + ".50"
        say(f"{self.name}: DHCP {'DISCOVER' if kind == 1 else 'REQUEST'}: "
            f"{'offered' if answer == 2 else 'acknowledged'} {offered}")
        me = socket.inet_aton(self.ip)
        reply = bytearray(240)
        reply[0:4] = bytes([2, 1, 6, 0])
        reply[4:8] = m[4:8]                         # xid
        reply[10:12] = m[10:12]                     # flags (broadcast)
        reply[16:20] = socket.inet_aton(offered)
        reply[20:24] = me
        reply[28:44] = m[28:44]                     # the client's MAC
        reply[236:240] = bytes([0x63, 0x82, 0x53, 0x63])
        reply += bytes([53, 1, answer, 54, 4]) + me + bytes([51, 4]) + struct.pack("!I", 3600)
        reply += bytes([1, 4, 255, 255, 255, 0, 3, 4]) + me + bytes([6, 4]) + me + bytes([255])
        udp = struct.pack("!HHHH", 67, 68, 8 + len(reply), 0) + bytes(reply)
        packet = ip_packet(self.ip, "255.255.255.255", 17, udp)
        self.sock.sendto(BROADCAST + self.mac + b"\x08\x00" + packet, self.guest)


def ping(hosts, name, dst, ttl):
    host = hosts[name]
    ident, seq = 0x4242, int(time.time() * 1000) & 0xFFFF
    request = icmp(8, 0, struct.pack("!HH", ident, seq) + b"testroute" * 4)
    packet = ip_packet(host.ip, dst, 1, request, ttl)
    with lock:
        start = len(events)
    host.send_ip(packet, ROUTER[name])
    deadline = time.time() + 2
    seen = ""
    while time.time() < deadline:
        time.sleep(0.05)
        with lock:
            new = events[start:]
        for (at, kind, code, src, to, got_ttl, good, body) in new:
            ours = body[4:8] == struct.pack("!HH", ident, seq)
            if at != name and kind == 8 and ours and not seen:
                seen = f", seen at {at} with ttl {got_ttl}, checksum {'ok' if good else 'WRONG'}"
            if at == name and kind == 0 and ours:
                return f"{name} to {dst}: answered by {src}, ttl {got_ttl}{seen}"
            if at == name and kind in (3, 11):
                quoted = body[8:8 + 28]
                same = (len(quoted) == 28 and quoted[12:20] == packet[12:20] and
                        quoted[24:28] == request[4:8])
                what = "time exceeded" if kind == 11 else f"unreachable (code {code})"
                return (f"{name} to {dst}: {what} from {src}"
                        f"{', quoting our packet' if same else ', quoting something else'}{seen}")
    return f"{name} to {dst}: no answer{seen}"


def ping6(hosts, name, dst_text):
    host = hosts[name]
    dst = socket.inet_pton(socket.AF_INET6, dst_text)
    ident, seq = 0x4343, int(time.time() * 1000) & 0xFFFF
    request = icmp6(host.ip6, dst, 128, 0, struct.pack("!HH", ident, seq) + b"testroute6" * 4)
    with lock:
        start = len(events6)
    if not host.send6(ip6_packet(host.ip6, dst, 58, request), dst):
        return f"{name} to {dst_text}: no neighbour answer"
    deadline = time.time() + 2
    while time.time() < deadline:
        time.sleep(0.05)
        with lock:
            new = events6[start:]
        for (at, kind, src, to, hops, body) in new:
            if at == name and kind == 129 and body[4:8] == struct.pack("!HH", ident, seq):
                return f"{name} to {dst_text}: answered by {text6(src)}, hop limit {hops}"
    return f"{name} to {dst_text}: no answer"


def frag6(hosts, name, dst_text, port, size, overlap):
    host = hosts[name]
    dst = socket.inet_pton(socket.AF_INET6, dst_text.split("%")[0])
    payload = bytes(i % 251 for i in range(size))
    udp = struct.pack("!HHHH", 5555, port, 8 + size, 0) + payload
    pseudo = host.ip6 + dst + struct.pack("!I", len(udp)) + b"\0\0\0\x11"
    udp = udp[:6] + struct.pack("!H", checksum(pseudo + udp) or 0xFFFF) + udp[8:]
    ident = int(time.time() * 1000) & 0xFFFFFFFF
    pieces, at = [], 0
    while at < len(udp):
        n = min(1232, len(udp) - at)
        start = at - 8 if overlap and pieces else at
        more = 1 if at + n < len(udp) else 0
        frag = struct.pack("!BBHI", 17, 0, start | more, ident) + udp[start:at + n]
        pieces.append(frag)
        at += n
    with lock:
        first = len(events6)
    mac = host.resolve6(dst)
    if not mac:
        return f"{name} to {dst_text}: no neighbour answer"
    for frag in pieces:
        host.send(mac, 0x86DD, ip6_packet(host.ip6, dst, 44, frag))
    deadline = time.time() + 2
    while time.time() < deadline:
        time.sleep(0.05)
        with lock:
            new = events6[first:]
        for e in new:
            if e[0] == name and e[1] == "udp" and e[4] == 5555:
                return f"{name}: {len(pieces)} pieces to {dst_text} port {port}: answer '{e[5].decode()}'"
    return f"{name}: {len(pieces)} pieces to {dst_text} port {port}: no answer"


def udp(hosts, name, dst_text, port, text, sport=5555):
    host = hosts[name]
    payload = text.encode()
    with lock:
        first = len(events6)
    if ":" in dst_text:
        dst = socket.inet_pton(socket.AF_INET6, dst_text.split("%")[0])
        d = struct.pack("!HHHH", sport, port, 8 + len(payload), 0) + payload
        pseudo = host.ip6 + dst + struct.pack("!I", len(d)) + b"\0\0\0\x11"
        d = d[:6] + struct.pack("!H", checksum(pseudo + d) or 0xFFFF) + d[8:]
        if not host.send6(ip6_packet(host.ip6, dst, 17, d), dst):
            return f"{name} to {dst_text}: no neighbour answer"
    else:
        d = struct.pack("!HHHH", sport, port, 8 + len(payload), 0) + payload
        host.send_ip(ip_packet(host.ip, dst_text, 17, d), dst_text)
    deadline = time.time() + 2
    while time.time() < deadline:
        time.sleep(0.05)
        with lock:
            new = events6[first:]
        for e in new:
            if e[0] == name and e[1] == "udp" and e[4] == sport:
                return f"{name}: udp to {dst_text} port {port}: answer '{e[5].decode()}'"
    return f"{name}: udp to {dst_text} port {port}: no answer"


def mld_query(hosts, name):
    host = hosts[name]
    # A general query (MLDv2: 28 bytes; maximum response 1000 ms) to ff02::1.
    query = icmp6(host.ip6, ALL_NODES6, 130, 0, struct.pack("!HH", 1000, 0) + bytes(16) + bytes([2, 125, 0, 0]))
    with lock:
        start = len(events6)
    host.send6(ip6_packet(host.ip6, ALL_NODES6, 58, query, 1, alert=True), ALL_NODES6)
    time.sleep(2)
    with lock:
        new = events6[start:]
    reports = []
    for (at, kind, src, to, hops, body) in new:
        if at == name and kind == 143:
            groups = [text6(body[8 + 20 * i + 4:8 + 20 * i + 20])
                      for i in range(struct.unpack("!H", body[6:8])[0])]
            reports.append(f"MLD report from {text6(src)}: {', '.join(groups)}")
    return f"{name}: " + ("; ".join(reports) if reports else "no MLD report")


def serve(control, a_host, a_guest, b_host, b_guest):
    hosts = {"A": Host("A", a_host, a_guest), "B": Host("B", b_host, b_guest)}
    for h in hosts.values():
        threading.Thread(target=h.run, daemon=True).start()
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", control))
    server.listen(4)
    say("testroute: listening")
    while True:
        c, _ = server.accept()
        words = c.makefile().readline().split()
        if len(words) == 3 and words[0] == "dhcp" and words[1] in hosts and words[2] in ("on", "off"):
            hosts[words[1]].dhcp = words[2] == "on"
            answer = f"{words[1]}: DHCP server {words[2]}"
        elif len(words) in (3, 4) and words[0] == "ping" and words[1] in hosts:
            answer = ping(hosts, words[1], words[2], int(words[3]) if len(words) == 4 else 64)
        elif len(words) == 3 and words[0] == "ping6" and words[1] in hosts:
            answer = ping6(hosts, words[1], words[2])
        elif len(words) == 3 and words[0] == "claim6" and words[1] in hosts:
            hosts[words[1]].claims.add(socket.inet_pton(socket.AF_INET6, words[2]))
            answer = f"{words[1]}: defends {words[2]}"
        elif len(words) == 2 and words[0] == "mldquery" and words[1] in hosts:
            answer = mld_query(hosts, words[1])
        elif len(words) == 3 and words[0] == "router6" and words[1] in hosts and words[2] == "off":
            h = hosts[words[1]]
            if h.router6:
                h.advertise_router(lifetime=0)
            h.router6 = None
            answer = f"{words[1]}: no longer a router"
        elif len(words) >= 4 and words[0] == "router6" and words[1] in hosts:
            h = hosts[words[1]]
            text, length = words[2].split("/")
            prefix = socket.inet_pton(socket.AF_INET6, text)
            opts = dict(zip(words[4::2], words[5::2]))
            dns = socket.inet_pton(socket.AF_INET6, opts["dns"]) if "dns" in opts else None
            h.router6 = (prefix, int(length), int(words[3]), dns, int(opts.get("mtu", 0)))
            h.router6_flags = (0x80 if "M" in opts.get("flags", "") else 0) | \
                              (0x40 if "O" in opts.get("flags", "") else 0)
            h.globals6.add(prefix[:8] + h.ip6[8:])
            h.advertise_router()
            answer = f"{words[1]}: the router of {words[2]}"
        elif len(words) == 5 and words[0] == "ra6" and words[1] in hosts and hosts[words[1]].router6:
            text, length = words[2].split("/")
            hosts[words[1]].advertise_router(extra=(socket.inet_pton(socket.AF_INET6, text), int(length),
                                                    int(words[3]), int(words[4])))
            answer = f"{words[1]}: advertised {words[2]} for {words[3]} s"
        elif len(words) in (5, 6) and words[0] == "frag6" and words[1] in hosts:
            answer = frag6(hosts, words[1], words[2], int(words[3]), int(words[4]),
                           len(words) == 6 and words[5] == "overlap")
        elif len(words) in (5, 6) and words[0] == "udp" and words[1] in hosts:
            answer = udp(hosts, words[1], words[2], int(words[3]), words[4],
                         int(words[5]) if len(words) == 6 else 5555)
        elif len(words) == 3 and words[0] == "tcp6" and words[1] in hosts:
            hosts[words[1]].tcp6 = words[2] == "on"
            answer = f"{words[1]}: tcp6 {words[2]}"
        elif len(words) == 3 and words[0] == "dhcp6" and words[1] in hosts and words[2] == "off":
            hosts[words[1]].dhcp6 = None
            answer = f"{words[1]}: no DHCPv6 server"
        elif len(words) >= 4 and words[0] == "dhcp6" and words[1] in hosts and words[2] == "on":
            text, length = words[3].split("/")
            opts = dict(zip(words[4::2], words[5::2]))
            valid = int(opts.get("valid", 3600))
            dns = socket.inet_pton(socket.AF_INET6, opts["dns"]) if "dns" in opts else b""
            hosts[words[1]].dhcp6 = (socket.inet_pton(socket.AF_INET6, text), dns,
                                     int(opts.get("t1", valid // 2)), valid)
            answer = f"{words[1]}: a DHCPv6 server for {words[3]}"
        elif len(words) == 3 and words[0] == "addr6" and words[1] in hosts:
            hosts[words[1]].globals6.add(socket.inet_pton(socket.AF_INET6, words[2]))
            answer = f"{words[1]}: has {words[2]}"
        elif len(words) == 4 and words[0] == "bulkto" and words[1] in hosts:
            answer = hosts[words[1]].bulk_connect(words[2], int(words[3]))
        elif len(words) == 3 and words[0] == "attack" and words[1] in hosts:
            answer = attack(hosts, words[1], words[2])
        else:
            answer = f"testroute: unknown command {' '.join(words)}"
        say(f"testroute: {' '.join(words)}: {answer}")
        c.sendall((answer + "\n").encode())
        c.close()


def ask(control, words):
    c = socket.create_connection(("127.0.0.1", control), timeout=10)
    c.sendall((" ".join(words) + "\n").encode())
    print(c.makefile().readline().rstrip("\n"))


if __name__ == "__main__":
    if len(sys.argv) == 7 and sys.argv[1] == "serve":
        serve(*map(int, sys.argv[2:]))
    elif len(sys.argv) >= 4 and sys.argv[1] == "ask":
        ask(int(sys.argv[2]), sys.argv[3:])
    else:
        print(__doc__)
        sys.exit(2)
