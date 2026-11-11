#!/usr/bin/env python3
"""A small web server for the tests of fetch (the guest reaches it as
10.0.2.2:PORT).

Usage: testhttp.py PORT

  /hello.txt      "Hello from the Mac!\\n" (Content-Length)
  /big.bin        1,000,000 bytes, byte i = i % 251 (Content-Length)
  /chunked.txt    lines sent in chunks (Transfer-Encoding: chunked)
  /old            301 to /hello.txt; /away: 302 to http://mac.test:PORT/hello.txt
                  (a host name, for the tests' DNS server: this one again);
                  /loop: 302 to itself
  /short          says 100 bytes, sends 10 and closes
  anything else   404 Not Found

Every request is printed (method, path, Host header). Runs until killed.
"""
import http.server
import socketserver
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else sys.exit(__doc__)
CHUNKS = [b"first chunk\n", b"second, a bit longer chunk\n", b"x" * 3000 + b"\n", b"last\n"]


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def send(self, status, body, headers=()):
        self.send_response(status)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        print(f"{self.command} {self.path} Host: {self.headers.get('Host')}", flush=True)
        if self.path == "/hello.txt":
            self.send(200, b"Hello from the Mac!\n", [("Content-Type", "text/plain")])
        elif self.path == "/big.bin":
            self.send(200, bytes(i % 251 for i in range(1000000)))
        elif self.path == "/chunked.txt":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for c in CHUNKS:
                self.wfile.write(b"%x\r\n" % len(c) + c + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        elif self.path == "/old":
            self.send(301, b"moved\n", [("Location", "/hello.txt")])
        elif self.path == "/away":
            self.send(302, b"", [("Location", f"http://mac.test:{PORT}/hello.txt")])
        elif self.path == "/loop":
            self.send(302, b"", [("Location", "/loop")])
        elif self.path == "/short":
            self.send_response(200)
            self.send_header("Content-Length", "100")
            self.end_headers()
            self.wfile.write(b"0123456789")
            self.close_connection = True
        else:
            self.send(404, b"not here\n")


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


server = Server(("127.0.0.1", PORT), Handler)
print("listening", flush=True)
server.serve_forever()
