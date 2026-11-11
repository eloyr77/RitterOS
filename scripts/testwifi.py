#!/usr/bin/env python3
"""
testwifi.py: a test Wi-Fi access point for RitterOS (steps 24d-24e), on the
other end of the ritterwifi card's netdev (-netdev dgram). The wire carries a
4-byte header { u8 channel; s8 signal; u16 rsv } then one 802.11 frame (no
FCS), as scripts/qemu/ritterwifi.c does.

Usage: testwifi.py GUEST_PORT LOCAL_PORT CHANNEL GATEWAY NET...
Each NET is SSID:SECURITY[:PASSPHRASE] (SECURITY = open, wpa2 or wpa3). The AP
beacons each network, answers probe requests, and takes a station through
authentication (open or SAE), association and -- for WPA2/WPA3 -- the 4-way
handshake, then forwards the station's IP traffic: it is the gateway GATEWAY
(a /24), answers ARP and ICMP echo, and is a DHCP server. Frames are
CCMP-protected once the keys are set. Pure-Python crypto (wificrypto.py).
"""
import os
import signal
import socket
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wificrypto as wc  # noqa: E402

BROADCAST = b"\xff\xff\xff\xff\xff\xff"
SNAP = b"\xaa\xaa\x03\x00\x00\x00"


def bssid_for(index):
    return bytes([0x02, 0x00, 0x00, 0x00, 0x00, index + 1])


def rsn_ie(akm):
    def suite(n):
        return b"\x00\x0f\xac" + bytes([n])
    caps = 0x00c0 if akm == 8 else 0x0080
    body = (struct.pack("<H", 1) + suite(4) + struct.pack("<H", 1) + suite(4) +
            struct.pack("<H", 1) + suite(akm) + struct.pack("<H", caps))
    return bytes([48, len(body)]) + body


def ip_checksum(data):
    if len(data) % 2:
        data += b"\x00"
    s = sum(struct.unpack(">%dH" % (len(data) // 2), data))
    s = (s >> 16) + (s & 0xffff)
    s += s >> 16
    return (~s) & 0xffff


class Net:
    def __init__(self, spec, index):
        parts = spec.split(":")
        self.ssid = parts[0]
        self.security = parts[1]
        self.passphrase = parts[2] if len(parts) > 2 else ""
        self.hidden = "hidden" in parts[3:]    # its beacons carry an empty name
        self.bad_m3 = "badm3" in parts[3:]     # a forged message 3 before the real one
        self.signal = -56                       # dBm, on every frame it sends
        self.bssid = bssid_for(index)
        self.beacon_seq = 0
        self.pt = None
        self.pmk = None
        if self.security == "wpa3":
            self.pt = wc.sae_pt(self.ssid.encode(), self.passphrase.encode())

    def privacy(self):
        return self.security != "open"

    def akm(self):
        return 8 if self.security == "wpa3" else 2

    def ies(self, channel, named=True):
        name = self.ssid.encode() if named else b""
        out = bytes([0, len(name)]) + name
        out += bytes([1, 4, 0x82, 0x84, 0x8b, 0x96])
        out += bytes([3, 1, channel])
        if self.security == "wpa2":
            out += rsn_ie(2)
        elif self.security == "wpa3":
            out += rsn_ie(8)
        return out

    def beacon(self, channel, csa=None):
        cap = 0x0001 | (0x0010 if self.privacy() else 0)
        self.beacon_seq = (self.beacon_seq + 1) & 0xfff
        header = struct.pack("<HH", 0x0080, 0) + BROADCAST + self.bssid + self.bssid + \
            struct.pack("<H", self.beacon_seq << 4)
        body = struct.pack("<QHH", 0, 100, cap) + self.ies(channel, not self.hidden)
        if csa:                                 # Channel Switch Announcement: mode 1,
            body += bytes([37, 3, 1, csa[0], csa[1]])  # the new channel, beacons to go
        return header + body


class Station:
    """One associated station, its handshake and keys."""

    def __init__(self, ap, net, mac):
        self.ap = ap
        self.net = net
        self.mac = mac
        self.state = "auth"
        self.sae = None
        self.pmk = net.pmk        # WPA2 PSK (set on the net); WPA3 fills it from SAE
        self.ptk = None
        self.gtk = bytes(range(16))
        self.gtk_index = 1
        self.group_pn = 0
        self.igtk = os.urandom(16)    # WPA3: BIP's key for group management frames
        self.igtk_index = 4
        self.bip_ipn = 0
        self.rekeying = False
        self.new_ptk = None           # a restarted 4-way's key, live after message 4
        self.ptk_rekeying = False
        self.ba_token = 0
        self.qos_seq = 100            # TID 0's sequence numbers (QoS data)
        self.anonce = os.urandom(32)
        self.replay = 0
        self.ip = None
        self.tx_pn = 0
        self.seq = 0

    # --- CCMP ---
    def ccmp_nonce(self, a2, pn, priority=0, mgmt=False):
        return bytes([priority | (0x10 if mgmt else 0)]) + a2 + pn.to_bytes(6, "big")

    def encrypt_data(self, header, body):
        self.tx_pn += 1
        pn = self.tx_pn
        c = bytes([pn & 0xff, (pn >> 8) & 0xff, 0, 0x20,
                   (pn >> 16) & 0xff, (pn >> 24) & 0xff, (pn >> 32) & 0xff, (pn >> 40) & 0xff])
        aad = bytes([header[0] & 0x8f, (header[1] & ~0x38) | 0x40]) + header[4:22] + \
            bytes([header[22] & 0x0f, 0])
        nonce = self.ccmp_nonce(header[10:16], pn)
        tk = self.ptk[32:48]
        enc = wc.ccm_encrypt(tk, nonce, aad, body, 8)
        return header[:1] + bytes([header[1] | 0x40]) + header[2:] + c + enc

    def encrypt_group(self, header, body):
        # A broadcast with the group key: its index in the CCMP header.
        self.group_pn += 1
        pn = self.group_pn
        c = bytes([pn & 0xff, (pn >> 8) & 0xff, 0, 0x20 | (self.gtk_index << 6),
                   (pn >> 16) & 0xff, (pn >> 24) & 0xff, (pn >> 32) & 0xff, (pn >> 40) & 0xff])
        aad = bytes([header[0] & 0x8f, (header[1] & ~0x38) | 0x40]) + header[4:22] + \
            bytes([header[22] & 0x0f, 0])
        nonce = self.ccmp_nonce(header[10:16], pn)
        enc = wc.ccm_encrypt(self.gtk, nonce, aad, body, 8)
        return header[:1] + bytes([header[1] | 0x40]) + header[2:] + c + enc

    def gtk_kde(self):
        # GTK KDE: OUI and type (4), key ID and reserved (2), the key; its
        # length byte counts those (22), as a real access point sends it;
        # padded to a multiple of 8 for key wrap
        kde = b"\xdd" + bytes([4 + 2 + len(self.gtk)]) + b"\x00\x0f\xac\x01" + \
            bytes([self.gtk_index, 0]) + self.gtk
        if self.net.akm() == 8:
            # WPA3: the IGTK KDE too (key ID, the IPN to start from, the key)
            kde += b"\xdd" + bytes([4 + 2 + 6 + 16]) + b"\x00\x0f\xac\x09" + \
                struct.pack("<H", self.igtk_index) + \
                (self.bip_ipn + 1).to_bytes(6, "little") + self.igtk
        return kde + bytes((-len(kde)) % 8)

    def bip_frame(self, subtype, body):
        # a group management frame with an MMIE (BIP-CMAC-128): the MIC over
        # the masked header and the body with the MIC zeroed
        self.bip_ipn += 1
        header = struct.pack("<HH", subtype << 4, 0) + b"\xff" * 6 + self.net.bssid + \
            self.net.bssid + struct.pack("<H", 0)
        mmie = bytes([76, 16]) + struct.pack("<H", self.igtk_index) + \
            self.bip_ipn.to_bytes(6, "little")
        aad = bytes([header[0], header[1] & ~0x38 & 0xff]) + header[4:22]
        mic = wc.aes_cmac(self.igtk, aad + body + mmie + bytes(8))[:8]
        return header + body + mmie + mic

    def start_group_rekey(self):
        # The group key handshake (IEEE 802.11 12.7.7), message 1: a new
        # key under the other index, wrapped with the KEK, signed with the KCK.
        self.gtk = os.urandom(16)
        self.gtk_index = 2 if self.gtk_index == 1 else 1
        self.group_pn = 0
        self.replay += 1
        ki = (1 << 7) | (1 << 8) | (1 << 9) | (1 << 12) | (2 if self.net.akm() == 2 else 0)
        wrapped = wc.aes_key_wrap(self.ptk[16:32], self.gtk_kde())
        self.rekeying = True
        # (with the keys in place it travels encrypted with the pairwise key,
        # as on a real access point; the station refuses plain unicast now)
        self.ap.send_data(self, b"\x88\x8e", self.eapol(ki, b"", wrapped, self.ptk[:16]))
        self.ap.log("station %s: group key handshake started (index %d)" %
                    (self.mac.hex(), self.gtk_index))

    def start_ptk_rekey(self):
        # The 4-way handshake again while joined (the pairwise key renewed):
        # messages travel under the old key until message 4 has arrived.
        self.anonce = os.urandom(32)
        self.replay += 1
        self.ptk_rekeying = True
        self.new_ptk = None
        ki = (1 << 3) | (1 << 7) | (2 if self.net.akm() == 2 else 0)
        self.ap.send_data(self, b"\x88\x8e", self.eapol(ki, self.anonce))
        self.ap.log("station %s: pairwise key handshake started again" % self.mac.hex())

    def encrypt_qos(self, header, body):
        # CCMP for QoS data: the QoS control's TID in the AAD and the nonce
        self.tx_pn += 1
        pn = self.tx_pn
        c = bytes([pn & 0xff, (pn >> 8) & 0xff, 0, 0x20,
                   (pn >> 16) & 0xff, (pn >> 24) & 0xff, (pn >> 32) & 0xff, (pn >> 40) & 0xff])
        tid = header[24] & 0x0f
        aad = bytes([header[0] & 0x8f, ((header[1] & ~0x38) | 0x40) & 0x7f]) + \
            header[4:22] + bytes([header[22] & 0x0f, 0]) + bytes([tid, 0])
        nonce = self.ccmp_nonce(header[10:16], pn, tid)
        enc = wc.ccm_encrypt(self.ptk[32:48], nonce, aad, body, 8)
        return header[:1] + bytes([header[1] | 0x40]) + header[2:] + c + enc

    def decrypt_data(self, frame):
        header = frame[:24]
        c = frame[24:32]
        pn = (c[0] | c[1] << 8 | c[4] << 16 | c[5] << 24 | c[6] << 32 | c[7] << 40)
        aad = bytes([header[0] & 0x8f, (header[1] & ~0x38) | 0x40]) + header[4:22] + \
            bytes([header[22] & 0x0f, 0])
        nonce = self.ccmp_nonce(header[10:16], pn)
        tk = self.ptk[32:48]
        return wc.ccm_decrypt(tk, nonce, aad, frame[32:], 8)

    # --- the 4-way handshake (authenticator) ---
    def eapol(self, key_info, nonce=b"", key_data=b"", mic_key=None, install=False):
        ki = key_info
        body = struct.pack(">BH", 2, ki) + struct.pack(">H", 16)
        body += struct.pack(">Q", self.replay)
        body += (nonce + bytes(32))[:32]
        body += bytes(16)              # EAPOL IV
        body += bytes(8)               # RSC
        body += bytes(8)               # reserved
        mic = bytes(16)
        body += mic
        body += struct.pack(">H", len(key_data)) + key_data
        frame = struct.pack(">BBH", 2, 3, len(body)) + body
        if mic_key is not None:
            off = 4 + 1 + 2 + 2 + 8 + 32 + 16 + 8 + 8     # to the MIC field
            zeroed = frame[:off] + bytes(16) + frame[off + 16:]
            if self.net.akm() == 8:
                mic = wc.aes_cmac(mic_key, zeroed)[:16]
            else:
                import hashlib
                import hmac
                mic = hmac.new(mic_key, zeroed, hashlib.sha1).digest()[:16]
            frame = frame[:off] + mic + frame[off + 16:]
        return frame

    def start_4way(self):
        self.replay += 1
        ki = (1 << 3) | (1 << 7) | (2 if self.net.akm() == 2 else 0)   # pairwise, ack, ver
        self.ap.send_eapol(self.mac, self.net, self.eapol(ki, self.anonce))

    def handle_eapol(self, body):
        if len(body) < 95:
            return
        ki = struct.unpack(">H", body[5:7])[0]
        snonce = body[17:49]
        key_data_len = struct.unpack(">H", body[99:101])[0] if len(body) >= 101 else 0
        _ = key_data_len
        has_mic = ki & (1 << 8)
        if self.ptk_rekeying and has_mic and self.new_ptk is None:
            # restarted message 2: the new key; message 3 under the old one
            sha256 = self.net.akm() == 8
            self.new_ptk = wc.ptk(self.pmk, self.net.bssid, self.mac, self.anonce, snonce, sha256)
            self.replay += 1
            wrapped = wc.aes_key_wrap(self.new_ptk[16:32], self.gtk_kde())
            ki3 = (1 << 3) | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9) | (1 << 12) | \
                (2 if self.net.akm() == 2 else 0)
            self.ap.send_data(self, b"\x88\x8e",
                              self.eapol(ki3, self.anonce, wrapped, self.new_ptk[:16]))
            return
        if self.ptk_rekeying and has_mic:
            # restarted message 4: the new key is live; ask something under it
            self.ptk, self.new_ptk, self.ptk_rekeying, self.tx_pn = self.new_ptk, None, False, 0
            self.ap.log("station %s: pairwise key handshake done again" % self.mac.hex())
            threading.Timer(0.5, self.ap.ping_station, (self,)).start()
            return
        if has_mic and self.ptk is None:
            sha256 = self.net.akm() == 8
            self.ptk = wc.ptk(self.pmk, self.net.bssid, self.mac, self.anonce, snonce, sha256)
            # msg3: install, GTK wrapped with the KEK
            self.replay += 1
            kek = self.ptk[16:32]
            wrapped = wc.aes_key_wrap(kek, self.gtk_kde())
            ki3 = (1 << 3) | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9) | (1 << 12) | \
                (2 if self.net.akm() == 2 else 0)
            if self.net.bad_m3:
                # an attacker's message 3 first: the right nonce and counter,
                # but key data it cannot make and so no valid MIC
                self.ap.send_eapol(self.mac, self.net,
                                   self.eapol(ki3, self.anonce, os.urandom(len(wrapped))))
                self.ap.log("station %s: forged message 3 sent" % self.mac.hex())
            self.ap.send_eapol(self.mac, self.net,
                               self.eapol(ki3, self.anonce, wrapped, self.ptk[:16]))
        elif has_mic and self.rekeying and not ki & (1 << 3):
            # group message 2: the station has the new key. Ask it something
            # only that key opens: a broadcast ARP question for its address.
            self.rekeying = False
            self.ap.log("station %s: group key handshake done" % self.mac.hex())
            self.ap.broadcast_arp(self)
        elif has_mic and self.ptk is not None:
            # msg4: the handshake is done; keys are live
            self.state = "ready"
            self.ap.log("station %s: 4-way handshake done" % self.mac.hex())


class AP:
    def __init__(self, guest_port, local_port, channel, gateway, nets):
        self.channel = channel
        self.gateway = gateway
        self.prefix = ".".join(gateway.split(".")[:3])
        self.nets = [Net(s, i) for i, s in enumerate(nets)]
        self.by_bssid = {n.bssid: n for n in self.nets}
        self.guest = ("127.0.0.1", guest_port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", local_port))
        self.stations = {}
        self.ap_mac = bssid_for(0)
        self.next_ip = 10
        self.csa = None                         # [new channel, beacons to go]
        self.logf = sys.stdout

    def log(self, msg):
        print("testwifi: " + msg, flush=True)

    def wire(self, frame):
        # each network's frames carry its own signal (roaming tests fade one)
        net = self.by_bssid.get(frame[10:16]) or self.by_bssid.get(frame[16:22])
        signal = net.signal if net else -56
        self.sock.sendto(bytes([self.channel, signal & 0xff, 0, 0]) + frame, self.guest)

    def mgmt(self, subtype, bssid, to, body):
        fc = subtype << 4
        return struct.pack("<HH", fc, 0) + to + bssid + bssid + struct.pack("<H", 0) + body

    def send_eapol(self, sta_mac, net, eapol):
        # 4-way messages are plaintext at the link layer (the "Encrypted Key
        # Data" bit wraps the key data with the KEK, not the frame with CCMP).
        header = struct.pack("<HH", 0x0208, 0) + sta_mac + net.bssid + net.bssid + \
            struct.pack("<H", 0)      # From DS
        self.wire(header + SNAP + b"\x88\x8e" + eapol)

    def send_data(self, sta, dst_ether_type, payload):
        header = struct.pack("<HH", 0x0208, 0) + sta.mac + sta.net.bssid + sta.net.bssid + \
            struct.pack("<H", 0)
        body = SNAP + dst_ether_type + payload
        if sta.ptk:
            self.wire(sta.encrypt_data(header, body))
        else:
            self.wire(header + body)

    # --- IP gateway over the link ---
    def handle_ip(self, sta, ether_type, payload):
        if ether_type == b"\x08\x06":                       # ARP
            if len(payload) >= 28 and payload[6:8] == b"\x00\x02" and sta.ip and \
                    socket.inet_ntoa(payload[14:18]) == sta.ip:            # its reply
                self.log("station %s: answered the broadcast ARP" % sta.mac.hex())
            if len(payload) >= 28 and payload[6:8] == b"\x00\x01":   # request
                sender_ip = socket.inet_ntoa(payload[14:18])
                target_ip = payload[24:28]
                if socket.inet_ntoa(target_ip) == self.gateway:
                    reply = (payload[:6] + b"\x00\x02" + self.ap_mac +
                             socket.inet_aton(self.gateway) + payload[8:14] + payload[14:18])
                    self.send_data(sta, b"\x08\x06", reply)
        elif ether_type == b"\x08\x00":                     # IPv4
            if len(payload) < 20:
                return
            proto = payload[9]
            src = payload[12:16]
            dst = payload[16:20]
            ihl = (payload[0] & 0xf) * 4
            if proto == 17 and payload[ihl + 2:ihl + 4] == struct.pack(">H", 67):  # DHCP
                self.handle_dhcp(sta, payload[ihl:])
            elif proto == 17 and payload[ihl + 2:ihl + 4] == struct.pack(">H", 53):  # DNS
                self.handle_dns(sta, src, dst, payload[ihl:])
            elif proto == 1 and len(payload) > ihl and payload[ihl] == 0:          # echo reply
                ident, seq = struct.unpack(">HH", payload[ihl + 4:ihl + 8])
                if ident == 0x4242:
                    self.log("station %s: answered burst ping %d" % (sta.mac.hex(), seq))
                else:
                    self.log("station %s: answered the ping under the new pairwise key" %
                             sta.mac.hex())
            elif proto == 1 and len(payload) > ihl and payload[ihl] == 8:          # ICMP echo
                icmp = bytearray(payload[ihl:])
                icmp[0] = 0                                 # echo reply
                icmp[2:4] = b"\x00\x00"
                icmp[2:4] = struct.pack(">H", ip_checksum(bytes(icmp)))
                self.send_ip(sta, dst, src, 1, bytes(icmp))

    def send_ip(self, sta, src_ip, dst_ip, proto, payload):
        total = 20 + len(payload)
        hdr = bytearray(struct.pack(">BBHHHBBH4s4s", 0x45, 0, total, 0, 0, 64, proto, 0,
                                    src_ip, dst_ip))
        hdr[10:12] = struct.pack(">H", ip_checksum(bytes(hdr)))
        self.send_data(sta, b"\x08\x00", bytes(hdr) + payload)

    def handle_dns(self, sta, src, dst, udp):
        # The gateway is also the network's DNS server (DHCP option 6): every
        # name is the gateway's own address, so a ping by name gets answers.
        q = udp[8:]
        if len(q) < 17 or q[2] & 0x80 or dst != socket.inet_aton(self.gateway):
            return                                      # (only questions to the gateway)
        end = 12
        while end < len(q) and q[end]:
            end += 1 + q[end]
        question = q[12:end + 5]
        name = []
        at = 12
        while at < end:
            name.append(q[at + 1:at + 1 + q[at]].decode(errors="replace"))
            at += 1 + q[at]
        answer = q[:2] + b"\x81\x80\x00\x01\x00\x01\x00\x00\x00\x00" + question
        answer += b"\xc0\x0c\x00\x01\x00\x01" + struct.pack(">IH", 60, 4)
        answer += socket.inet_aton(self.gateway)
        self.log("station %s: DNS question for %s" % (sta.mac.hex(), ".".join(name)))
        sport = struct.unpack(">H", udp[0:2])[0]
        reply = struct.pack(">HHHH", 53, sport, 8 + len(answer), 0) + answer
        self.send_ip(sta, dst, src, 17, reply)

    def handle_dhcp(self, sta, udp):
        dhcp = udp[8:]
        if len(dhcp) < 240:
            return
        xid = dhcp[4:8]
        chaddr = dhcp[28:34]
        msg_type = 0
        i = 240
        while i + 2 <= len(dhcp):
            opt, length = dhcp[i], dhcp[i + 1]
            if opt == 53:
                msg_type = dhcp[i + 2]
            if opt == 255:
                break
            i += 2 + length
        if msg_type not in (1, 3):                          # discover, request
            return
        if sta.ip is None:
            sta.ip = "%s.%d" % (self.prefix, self.next_ip)
            self.next_ip += 1
        offer = 2 if msg_type == 1 else 5                   # offer / ack
        reply = bytearray(240)
        reply[0] = 2
        reply[1] = 1
        reply[2] = 6
        reply[4:8] = xid
        reply[16:20] = socket.inet_aton(sta.ip)
        reply[20:24] = socket.inet_aton(self.gateway)
        reply[28:34] = chaddr
        reply[236:240] = b"\x63\x82\x53\x63"
        opts = bytes([53, 1, offer])
        opts += bytes([1, 4]) + socket.inet_aton("255.255.255.0")
        opts += bytes([3, 4]) + socket.inet_aton(self.gateway)
        opts += bytes([6, 4]) + socket.inet_aton(self.gateway)
        opts += bytes([51, 4]) + struct.pack(">I", 86400)
        opts += bytes([54, 4]) + socket.inet_aton(self.gateway)
        opts += bytes([255])
        payload = bytes(reply) + opts
        udp_reply = struct.pack(">HHHH", 67, 68, 8 + len(payload), 0) + payload
        self.send_ip(sta, socket.inet_aton(self.gateway), b"\xff\xff\xff\xff", 17, udp_reply)

    def beaconer(self):
        while True:
            csa = self.csa
            for net in self.nets:
                self.wire(net.beacon(self.channel, csa))
            if csa:
                if csa[1] == 0:                     # the switch: everyone moves now
                    self.channel = csa[0]
                    self.csa = None
                    self.log("moved to channel %d" % self.channel)
                else:
                    csa[1] -= 1
            time.sleep(0.1)

    def handle_mgmt(self, subtype, frame):
        source = frame[10:16]
        bssid = frame[16:22]
        net = self.by_bssid.get(bssid)
        if subtype == 4:                                    # probe request
            asked = b""
            if len(frame) >= 26 and frame[24] == 0:
                asked = frame[26:26 + frame[25]]
            for n in self.nets:
                # a hidden network answers only a request for its name
                if n.hidden and asked != n.ssid.encode():
                    continue
                if asked and asked != n.ssid.encode():
                    continue
                self.wire(self.mgmt(5, n.bssid, source, struct.pack("<QHH", 0, 100,
                          0x0001 | (0x0010 if n.privacy() else 0)) + n.ies(self.channel)))
                if n.hidden:
                    self.log("probe response for hidden %s" % n.ssid)
            return
        if net is None:
            return
        body = frame[24:]
        if subtype == 13 and len(body) >= 9 and body[0] == 3 and body[1] == 1:
            sta = self.stations.get(source)                 # ADDBA response
            if sta and body[2] == sta.ba_token and struct.unpack("<H", body[3:5])[0] == 0:
                window = struct.unpack("<H", body[5:7])[0] >> 6
                self.log("station %s: block ack agreed (window %d)" % (source.hex(), window))
                threading.Timer(0.3, self.burst_out_of_order, (sta,)).start()
            return
        if subtype == 11:                                   # authentication
            self.handle_auth(net, source, body)
        elif subtype == 0:                                  # association request
            sta = self.stations.get(source)
            if not sta:
                return
            # association response: status 0, AID 1
            resp = struct.pack("<HHH", 0x0001 | (0x0010 if net.privacy() else 0), 0, 1)
            resp += bytes([1, 4, 0x82, 0x84, 0x8b, 0x96])
            self.wire(self.mgmt(1, net.bssid, source, resp))
            self.log("station %s: associated to %s" % (source.hex(), net.ssid))
            if net.security == "open":
                sta.state = "ready"
            else:
                sta.start_4way()

    def handle_auth(self, net, source, body):
        alg = struct.unpack("<H", body[0:2])[0]
        seq = struct.unpack("<H", body[2:4])[0]
        if alg == 0:                                        # open system
            sta = Station(self, net, source)
            self.stations[source] = sta
            self.wire(self.mgmt(11, net.bssid, source, struct.pack("<HHH", 0, 2, 0)))
        elif alg == 3:                                      # SAE
            sta = self.stations.get(source)
            if seq == 1:                                    # commit: body = group(2) scalar(32) elem(64)
                sta = Station(self, net, source)
                self.stations[source] = sta
                sta.sae = wc.SAE(net.pt, net.bssid, source)
                sta.sae.process(body[6 + 2:6 + 2 + 32], body[6 + 34:6 + 34 + 64])
                out = struct.pack("<HHH", 3, 1, 0) + struct.pack("<H", 19) + sta.sae.commit()
                self.wire(self.mgmt(11, net.bssid, source, out))
            elif seq == 2 and sta and sta.sae:              # confirm: body = send_confirm(2) confirm(32)
                sc = sta.sae
                sta.pmk = sc.pmk
                ap_s = sc.scalar.to_bytes(32, "big")
                ap_e = sc.element[0].to_bytes(32, "big") + sc.element[1].to_bytes(32, "big")
                conf = sc.confirm(0, ap_s, ap_e, sc.peer_scalar, sc.peer_element)
                self.wire(self.mgmt(11, net.bssid, source, struct.pack("<HHH", 3, 2, 0) +
                                    struct.pack("<H", 0) + conf))

    def broadcast_arp(self, sta):
        # who-has the station's address, tell the gateway: to everyone, with
        # the group key (From DS: address 1 broadcast, 2 the BSSID, 3 us)
        if not sta.ip:
            return
        arp = b"\x00\x01\x08\x00\x06\x04\x00\x01" + self.ap_mac + \
            socket.inet_aton(self.gateway) + bytes(6) + socket.inet_aton(sta.ip)
        header = struct.pack("<HH", 0x0208, 0) + b"\xff" * 6 + sta.net.bssid + self.ap_mac + \
            struct.pack("<H", 0)
        self.wire(sta.encrypt_group(header, SNAP + b"\x08\x06" + arp))
        self.log("station %s: broadcast ARP sent with the new group key" % sta.mac.hex())

    def ping_station(self, sta):
        # an echo request from the gateway, under the station's current key
        if not sta.ip:
            return
        icmp = bytearray(b"\x08\x00\x00\x00\x52\x49\x00\x01" + b"ritteros")
        icmp[2:4] = struct.pack(">H", ip_checksum(bytes(icmp)))
        self.send_ip(sta, socket.inet_aton(self.gateway), socket.inet_aton(sta.ip), 1,
                     bytes(icmp))

    def announce_switch(self, *_):
        # SIGURG: move to channel 11 after five beacons, announced in them
        self.csa = [11, 5]
        self.log("announcing a move to channel 11")

    def fade(self, *_):
        # SIGPROF: the first network fades, the second comes up strong
        self.nets[0].signal = -85
        if len(self.nets) > 1:
            self.nets[1].signal = -45
        self.log("network 1 faded, network 2 strong")

    def addba_all(self, *_):
        # SIGWINCH: propose to send to every ready station in bursts (TID 0)
        for sta in list(self.stations.values()):
            if sta.ptk is not None and sta.state == "ready":
                sta.ba_token = (sta.ba_token + 1) & 0xff or 1
                body = bytes([3, 0, sta.ba_token]) + \
                    struct.pack("<HHH", (1 << 1) | (0 << 2) | (64 << 6), 0, sta.qos_seq << 4)
                self.wire(self.mgmt(13, sta.net.bssid, sta.mac, body))
                self.log("station %s: ADDBA request sent" % sta.mac.hex())

    def burst_out_of_order(self, sta):
        # Four pings as QoS data, numbered and encrypted in order, sent 2 1 4 3:
        # only a station that puts them back in order answers all four.
        frames = []
        for i in range(4):
            icmp = bytearray(struct.pack(">BBHHH", 8, 0, 0, 0x4242, i + 1) + b"in bursts")
            icmp[2:4] = struct.pack(">H", ip_checksum(bytes(icmp)))
            total = 20 + len(icmp)
            ip = bytearray(struct.pack(">BBHHHBBH4s4s", 0x45, 0, total, 0, 0, 64, 1, 0,
                                       socket.inet_aton(self.gateway), socket.inet_aton(sta.ip)))
            ip[10:12] = struct.pack(">H", ip_checksum(bytes(ip)))
            header = struct.pack("<HH", 0x0288, 0) + sta.mac + sta.net.bssid + self.ap_mac + \
                struct.pack("<H", sta.qos_seq << 4) + struct.pack("<H", 0)
            sta.qos_seq = (sta.qos_seq + 1) & 0xfff
            frames.append(sta.encrypt_qos(header, SNAP + b"\x08\x00" + bytes(ip) + bytes(icmp)))
        for i in (1, 0, 3, 2):
            self.wire(frames[i])
        self.log("station %s: four pings sent out of order" % sta.mac.hex())

    def forge_all(self, *_):
        # SIGTTIN: what anyone in range could send, unprotected: a broadcast
        # EAPOL message 1 with the highest replay counter (a station that
        # takes it starts a handshake nobody finishes), and a QoS data frame
        # numbered far ahead of the burst window. Then four genuine pings out
        # of order: a station whose window the forgery moved drops them.
        for sta in list(self.stations.values()):
            if sta.ptk is None or sta.state != "ready":
                continue
            ki = (1 << 3) | (1 << 7) | (2 if sta.net.akm() == 2 else 0)
            saved, sta.replay = sta.replay, (1 << 64) - 1
            eapol = sta.eapol(ki, os.urandom(32))
            sta.replay = saved
            header = struct.pack("<HH", 0x0208, 0) + b"\xff" * 6 + sta.net.bssid + \
                sta.net.bssid + struct.pack("<H", 0)
            self.wire(header + SNAP + b"\x88\x8e" + eapol)
            ahead = (sta.qos_seq + 0x7ff) & 0xfff
            header = struct.pack("<HH", 0x0288, 0) + sta.mac + sta.net.bssid + self.ap_mac + \
                struct.pack("<H", ahead << 4) + struct.pack("<H", 0)
            self.wire(header + SNAP + b"\x08\x00" + bytes(28))
            self.log("station %s: forged frames sent" % sta.mac.hex())
            threading.Timer(1.0, self.burst_out_of_order, (sta,)).start()

    def bip_test(self, *_):
        # SIGQUIT: a forged broadcast deauthentication (no MMIE, reason 7),
        # then 1.5 s later one protected with BIP (reason 3)
        for sta in list(self.stations.values()):
            if sta.ptk is None or sta.state != "ready":
                continue
            self.wire(self.mgmt(12, sta.net.bssid, b"\xff" * 6, struct.pack("<H", 7)))
            self.log("station %s: forged broadcast deauthentication sent" % sta.mac.hex())

            def protected(sta=sta):
                self.wire(sta.bip_frame(12, struct.pack("<H", 3)))
                self.log("station %s: BIP-protected broadcast deauthentication sent" %
                         sta.mac.hex())
                self.stations.pop(sta.mac, None)        # (it is gone for us too)
            threading.Timer(1.5, protected).start()

    def ptk_rekey_all(self, *_):
        # SIGHUP: the 4-way handshake again with every station that is ready.
        for sta in list(self.stations.values()):
            if sta.ptk is not None and sta.state == "ready":
                sta.start_ptk_rekey()

    def rekey_all(self, *_):
        # SIGUSR2: renew the group key with every station that is ready.
        for sta in list(self.stations.values()):
            if sta.ptk is not None and sta.state == "ready":
                sta.start_group_rekey()

    def deauth_all(self, *_):
        # SIGUSR1: send every station away (reason 3, "leaving"), as an access
        # point that restarts does; they must notice and join again.
        for mac, sta in list(self.stations.items()):
            self.wire(self.mgmt(12, sta.net.bssid, mac, struct.pack("<H", 3)))
            self.log("station %s: deauthenticated" % mac.hex())
        self.stations.clear()

    def run(self):
        signal.signal(signal.SIGUSR1, self.deauth_all)
        signal.signal(signal.SIGUSR2, self.rekey_all)
        signal.signal(signal.SIGHUP, self.ptk_rekey_all)
        signal.signal(signal.SIGWINCH, self.addba_all)
        signal.signal(signal.SIGQUIT, self.bip_test)
        signal.signal(signal.SIGURG, self.announce_switch)
        signal.signal(signal.SIGPROF, self.fade)
        signal.signal(signal.SIGTTIN, self.forge_all)
        for net in self.nets:
            if net.security == "wpa2":
                net.pmk = wc.pmk_psk(net.passphrase, net.ssid)
        threading.Thread(target=self.beaconer, daemon=True).start()
        self.log("listening")
        while True:
            data, _ = self.sock.recvfrom(4096)
            if len(data) < 4 + 24:
                continue
            channel, frame = data[0], data[4:]
            if channel not in (0, self.channel):
                continue
            fc = struct.unpack("<H", frame[0:2])[0]
            ftype, subtype = (fc >> 2) & 3, (fc >> 4) & 0xf
            try:
                if ftype == 0:
                    self.handle_mgmt(subtype, frame)
                elif ftype == 2:
                    self.handle_data(frame)
            except Exception as e:                       # one bad frame must not stop the AP
                self.log("error: %r" % e)

    def handle_data(self, frame):
        source = frame[10:16]
        sta = self.stations.get(source)
        if not sta:
            return
        protected = frame[1] & 0x40
        if protected:
            if not sta.ptk:
                return
            body = sta.decrypt_data(frame)
            if body is None:
                return
        else:
            body = frame[24:]
        if len(body) < 8 or body[:6] != SNAP:
            return
        ether_type = body[6:8]
        if ether_type == b"\x88\x8e":                       # EAPOL
            sta.handle_eapol(body[8:])
        elif sta.state == "ready":
            self.handle_ip(sta, ether_type, body[8:])


def main():
    if len(sys.argv) < 6:
        sys.exit("usage: testwifi.py GUEST_PORT LOCAL_PORT CHANNEL GATEWAY NET...")
    AP(int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5:]).run()


if __name__ == "__main__":
    main()
