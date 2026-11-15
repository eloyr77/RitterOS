#!/usr/bin/env python3
"""The web servers of curl's test (46d-2), on the Mac, written for it from
the RFCs (HTTP/2: RFC 9113, HPACK: RFC 7541, WebSockets: RFC 6455). The
guest reaches 127.0.0.1's ports as 10.0.2.2:PORT, ::1's through any fec0::
address.

Usage: testcurl.py HTTP_PORT TLS_PORT PROXY_PORT CHAIN KEY BROTLI NGHTTP2_SRC

  HTTP_PORT   HTTP/1.1 on 127.0.0.1 and ::1:
    /hello.txt     "Hello from testcurl!\\n"
    /big           200,000 bytes, byte i = (i * 7 + 3) % 256
    /gzip          TEXT gzip-coded (Content-Encoding: gzip)
    /br            the file BROTLI (a brotli stream: brotli's own test data)
                   as it is, Content-Encoding: br
    /setcookie     two cookies: a=1 for Domain=co.uk (a public suffix: a
                   client must refuse it), b=2 for Domain=example.co.uk
    /cookies       the request's Cookie header as the body
    /ws            a WebSocket: every text or binary message sent back
  TLS_PORT    the same over TLS (CHAIN, KEY), ALPN h2 and http/1.1: HTTP/2
              when the client chose h2 (/hello.txt, /big, /gzip; the
              answer says which: "x-protocol: h2"), else HTTP/1.1
  PROXY_PORT  an HTTP proxy: a request for an absolute URL is answered
              here ("proxied: GET URL"), CONNECT HOST:PORT is a tunnel to
              127.0.0.1:PORT
NGHTTP2_SRC is nghttp2's source folder: its HPACK Huffman code table
(lib/nghttp2_hd_huffman_data.c, RFC 7541's Appendix B as C) is read from
there. Each request is printed ("http GET /path", "h2 GET /path", "proxy
...", "ws message N bytes"); "listening" once all are up. Runs until killed.
"""
import base64
import gzip
import hashlib
import http.server
import re
import socket
import socketserver
import ssl
import struct
import sys
import threading

if len(sys.argv) != 8:
    sys.exit(__doc__)
HTTP_PORT, TLS_PORT, PROXY_PORT = (int(a) for a in sys.argv[1:4])
CHAIN, KEY, BROTLI, NGHTTP2_SRC = sys.argv[4:8]
TEXT = b"".join(b"line %d: curl decodes gzip and brotli\n" % i for i in range(400))
BIG = bytes((i * 7 + 3) % 256 for i in range(200000))
BR = open(BROTLI, "rb").read()


def say(*words):
    print(*words, flush=True)


def page(path):
    """(status, headers, body) for a path both servers know."""
    if path == "/hello.txt":
        return 200, [("Content-Type", "text/plain")], b"Hello from testcurl!\n"
    if path == "/big":
        return 200, [("Content-Type", "application/octet-stream")], BIG
    if path == "/gzip":
        return 200, [("Content-Encoding", "gzip")], gzip.compress(TEXT, mtime=0)
    if path == "/br":
        return 200, [("Content-Encoding", "br")], BR
    return 404, [], b"not found\n"


# ---- HTTP/1.1, WebSockets ----

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def send(self, status, headers, body):
        self.send_response(status)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        say("http GET", self.path)
        if self.path == "/setcookie":
            self.send_response(200)
            self.send_header("Set-Cookie", "a=1; Domain=co.uk; Path=/")
            self.send_header("Set-Cookie", "b=2; Domain=example.co.uk; Path=/")
            self.send_header("Content-Length", "3")
            self.end_headers()
            self.wfile.write(b"ok\n")
        elif self.path == "/cookies":
            self.send(200, [], ("cookie: %s\n" % self.headers.get("Cookie", "")).encode())
        elif self.path == "/ws" and self.headers.get("Upgrade", "").lower() == "websocket":
            self.websocket()
        else:
            self.send(*page(self.path))

    def websocket(self):
        key = self.headers["Sec-WebSocket-Key"]
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept.decode())
        self.end_headers()
        self.wfile.flush()
        r, w = self.rfile, self.wfile
        while True:
            head = r.read(2)
            if len(head) < 2:
                return
            op, n = head[0] & 15, head[1] & 127
            if n == 126:
                n = struct.unpack(">H", r.read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", r.read(8))[0]
            mask = r.read(4) if head[1] & 128 else b"\0\0\0\0"
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(r.read(n)))
            if op == 8:                             # close: answered, done
                w.write(bytes([0x88, len(data[:2])]) + data[:2])
                w.flush()
                return
            if op == 9:                             # ping: pong
                op = 10
            elif op in (1, 2):
                say("ws message", n, "bytes")
            else:
                continue
            if n < 126:
                w.write(bytes([0x80 | op, n]) + data)
            else:
                w.write(bytes([0x80 | op, 126]) + struct.pack(">H", n) + data)
            w.flush()


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class Server6(Server):
    address_family = socket.AF_INET6


# ---- HPACK (RFC 7541) and HTTP/2 (RFC 9113) ----

STATIC = [None, (":authority", ""), (":method", "GET"), (":method", "POST"), (":path", "/"),
          (":path", "/index.html"), (":scheme", "http"), (":scheme", "https"), (":status", "200"),
          (":status", "204"), (":status", "206"), (":status", "304"), (":status", "400"), (":status", "404"),
          (":status", "500"), ("accept-charset", ""), ("accept-encoding", "gzip, deflate"),
          ("accept-language", ""), ("accept-ranges", ""), ("accept", ""), ("access-control-allow-origin", ""),
          ("age", ""), ("allow", ""), ("authorization", ""), ("cache-control", ""), ("content-disposition", ""),
          ("content-encoding", ""), ("content-language", ""), ("content-length", ""), ("content-location", ""),
          ("content-range", ""), ("content-type", ""), ("cookie", ""), ("date", ""), ("etag", ""), ("expect", ""),
          ("expires", ""), ("from", ""), ("host", ""), ("if-match", ""), ("if-modified-since", ""),
          ("if-none-match", ""), ("if-range", ""), ("if-unmodified-since", ""), ("last-modified", ""),
          ("link", ""), ("location", ""), ("max-forwards", ""), ("proxy-authenticate", ""),
          ("proxy-authorization", ""), ("range", ""), ("referer", ""), ("refresh", ""), ("retry-after", ""),
          ("server", ""), ("set-cookie", ""), ("strict-transport-security", ""), ("transfer-encoding", ""),
          ("user-agent", ""), ("vary", ""), ("via", ""), ("www-authenticate", "")]


def huffman_codes():
    text = open(NGHTTP2_SRC + "/lib/nghttp2_hd_huffman_data.c").read()
    block = text[text.index("huff_sym_table[] = {"):]
    block = block[:block.index("};")]
    pairs = re.findall(r"\{(\d+), 0x([0-9A-Fa-f]+)U\}", block)
    if len(pairs) != 257:
        sys.exit("testcurl: %d Huffman codes, not 257" % len(pairs))
    return {(int(n), int(c, 16) >> (32 - int(n))): sym for sym, (n, c) in enumerate(pairs)}


CODES = huffman_codes()


def huffman_decode(data):
    out, code, bits = bytearray(), 0, 0
    for byte in data:
        for k in range(7, -1, -1):
            code, bits = code << 1 | (byte >> k & 1), bits + 1
            sym = CODES.get((bits, code))
            if sym is not None:
                if sym == 256:
                    raise ValueError("EOS in a string")
                out.append(sym)
                code = bits = 0
    if bits > 7 or code != (1 << bits) - 1:
        raise ValueError("bad Huffman padding")
    return bytes(out)


class Decoder:
    def __init__(self):
        self.dynamic, self.size, self.limit = [], 0, 4096

    def integer(self, data, at, prefix):
        v = data[at] & ((1 << prefix) - 1)
        at += 1
        if v < (1 << prefix) - 1:
            return v, at
        shift = 0
        while True:
            b = data[at]
            at += 1
            v += (b & 127) << shift
            shift += 7
            if not b & 128:
                return v, at

    def string(self, data, at):
        huff = data[at] & 128
        n, at = self.integer(data, at, 7)
        raw = bytes(data[at:at + n])
        return (huffman_decode(raw) if huff else raw).decode("latin-1"), at + n

    def entry(self, i):
        if i < len(STATIC):
            return STATIC[i]
        return self.dynamic[i - len(STATIC)]

    def add(self, name, value):
        self.dynamic.insert(0, (name, value))
        self.size += len(name) + len(value) + 32
        while self.size > self.limit:
            n, v = self.dynamic.pop()
            self.size -= len(n) + len(v) + 32

    def decode(self, data):
        fields, at = [], 0
        while at < len(data):
            b = data[at]
            if b & 128:                             # indexed
                i, at = self.integer(data, at, 7)
                fields.append(self.entry(i))
            elif b & 64 or not b & 32:              # a literal (incremental 6, else 4)
                index = bool(b & 64)
                i, at = self.integer(data, at, 6 if index else 4)
                if i:
                    name = self.entry(i)[0]
                else:
                    name, at = self.string(data, at)
                value, at = self.string(data, at)
                fields.append((name, value))
                if index:
                    self.add(name, value)
            else:                                   # a table size update
                self.limit, at = self.integer(data, at, 5)
                while self.size > self.limit:
                    n, v = self.dynamic.pop()
                    self.size -= len(n) + len(v) + 32
        return fields


def selftest():
    """RFC 7541's C.4: three requests through one decoder."""
    d = Decoder()
    want = [[(":method", "GET"), (":scheme", "http"), (":path", "/"), (":authority", "www.example.com")],
            [(":method", "GET"), (":scheme", "http"), (":path", "/"), (":authority", "www.example.com"),
             ("cache-control", "no-cache")],
            [(":method", "GET"), (":scheme", "https"), (":path", "/index.html"), (":authority", "www.example.com"),
             ("custom-key", "custom-value")]]
    for block, fields in zip(["828684418cf1e3c2e5f23a6ba0ab90f4ff", "828684be5886a8eb10649cbf",
                              "828785bf408825a849e95ba97d7f8925a849e95bb8e8b4bf"], want):
        if d.decode(bytes.fromhex(block)) != fields:
            sys.exit("testcurl: the HPACK decoder fails RFC 7541's C.4")


def literal(name_index, value):
    """A field without indexing, its name from the static table, raw value."""
    v = value.encode()
    return bytes([0x0F, name_index - 15]) + bytes([len(v)]) + v if name_index >= 15 \
        else bytes([name_index]) + bytes([len(v)]) + v


class H2:
    def __init__(self, conn):
        self.c, self.decoder, self.window = conn, Decoder(), 65535
        self.stream_windows, self.initial = {}, 65535

    def read(self, n):
        data = b""
        while len(data) < n:
            more = self.c.recv(n - len(data))
            if not more:
                raise EOFError
            data += more
        return data

    def frame(self):
        head = self.read(9)
        n = int.from_bytes(head[:3], "big")
        return head[3], head[4], int.from_bytes(head[5:9], "big") & 0x7FFFFFFF, self.read(n)

    def send(self, kind, flags, stream, payload=b""):
        self.c.sendall(len(payload).to_bytes(3, "big") + bytes([kind, flags]) + stream.to_bytes(4, "big") + payload)

    def control(self, kind, flags, stream, payload):
        """SETTINGS, PING, WINDOW_UPDATE; true for GOAWAY."""
        if kind == 4 and not flags & 1:             # SETTINGS: kept, acknowledged
            for i in range(0, len(payload), 6):
                key, value = struct.unpack(">HI", payload[i:i + 6])
                if key == 4:
                    for s in self.stream_windows:
                        self.stream_windows[s] += value - self.initial
                    self.initial = value
            self.send(4, 1, 0)
        elif kind == 6 and not flags & 1:
            self.send(6, 1, 0, payload)
        elif kind == 8:
            add = int.from_bytes(payload, "big") & 0x7FFFFFFF
            if stream:
                self.stream_windows[stream] = self.stream_windows.get(stream, self.initial) + add
            else:
                self.window += add
        return kind == 7

    def answer(self, stream, path):
        status, headers, body = page(path)
        say("h2 GET", path)
        block = literal(8, str(status)) + literal(28, str(len(body)))
        for k, v in headers:
            block += literal({"Content-Type": 31, "Content-Encoding": 26}[k], v)
        block += b"\x00" + bytes([10]) + b"x-protocol" + bytes([2]) + b"h2"
        self.send(1, 4 | (0 if body else 1), stream, block)
        self.stream_windows.setdefault(stream, self.initial)
        at = 0
        while at < len(body):
            room = min(16384, self.window, self.stream_windows[stream], len(body) - at)
            if room <= 0:                           # wait for the client's window
                kind, flags, s, payload = self.frame()
                if self.control(kind, flags, s, payload):
                    return
                continue
            self.send(0, 1 if at + room == len(body) else 0, stream, body[at:at + room])
            self.window -= room
            self.stream_windows[stream] -= room
            at += room

    def serve(self):
        if self.read(24) != b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n":
            return
        self.send(4, 0, 0)                          # our SETTINGS: the defaults
        block, stream = b"", 0
        while True:
            kind, flags, s, payload = self.frame()
            if kind in (1, 9):                      # HEADERS, CONTINUATION
                if kind == 1:
                    if flags & 8:                   # PADDED
                        payload = payload[1:len(payload) - payload[0]]
                    if flags & 0x20:                # PRIORITY
                        payload = payload[5:]
                    block, stream = payload, s
                else:
                    block += payload
                if flags & 4:                       # END_HEADERS
                    fields = dict(self.decoder.decode(block))
                    self.answer(stream, fields.get(":path", "/"))
            elif self.control(kind, flags, s, payload):
                return


def tls_server():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(CHAIN, KEY)
    ctx.set_alpn_protocols(["h2", "http/1.1"])
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", TLS_PORT))
    listener.listen(16)

    def one(conn):
        try:
            tls = ctx.wrap_socket(conn, server_side=True)
            if tls.selected_alpn_protocol() == "h2":
                H2(tls).serve()
            else:
                Handler(tls, ("guest", 0), None)
        except (OSError, EOFError, ssl.SSLError, ValueError):
            pass
        finally:
            conn.close()

    while True:
        conn, _ = listener.accept()
        threading.Thread(target=one, args=(conn,), daemon=True).start()


# ---- the proxy ----

def pump(a, b):
    try:
        while True:
            data = a.recv(65536)
            if not data:
                break
            b.sendall(data)
    except OSError:
        pass
    finally:
        try:
            b.shutdown(socket.SHUT_WR)
        except OSError:
            pass


class Proxy(socketserver.StreamRequestHandler):
    def handle(self):
        line = self.rfile.readline().decode("latin-1").strip()
        while self.rfile.readline() not in (b"\r\n", b"\n", b""):
            pass
        method, target = line.split()[:2]
        say("proxy", method, target)
        if method == "CONNECT":
            port = int(target.rsplit(":", 1)[1])
            up = socket.create_connection(("127.0.0.1", port))
            self.wfile.write(b"HTTP/1.1 200 Connection established\r\n\r\n")
            self.wfile.flush()
            t = threading.Thread(target=pump, args=(up, self.connection), daemon=True)
            t.start()
            pump(self.connection, up)
            t.join()
            up.close()
        else:
            body = ("proxied: %s %s\n" % (method, target)).encode()
            self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % len(body) + body)


class ProxyServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


selftest()
servers = [Server(("127.0.0.1", HTTP_PORT), Handler), Server6(("::1", HTTP_PORT), Handler),
           ProxyServer(("127.0.0.1", PROXY_PORT), Proxy)]
for s in servers:
    threading.Thread(target=s.serve_forever, daemon=True).start()
threading.Thread(target=tls_server, daemon=True).start()
say("listening")
threading.Event().wait()
