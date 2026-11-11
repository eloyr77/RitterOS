#!/usr/bin/env python3
"""Network services on the Mac for the tests (QEMU's user network takes the
guest's packets for 10.0.2.2:PORT to 127.0.0.1:PORT here).

Usage:
  testnet.py SERVICE...        run the services until killed:
      udp-echo:PORT            answer each datagram with "echo: " + it
      tcp-echo:PORT            per connection: read once, answer "echo: " +
                               what came, close
      tcp-send:PORT:BYTES      per connection: send BYTES of the pattern
                               (byte i = i % 251), close
      tcp-recv:PORT            per connection: read to the end, report
                               whether it was the pattern
      tcp-slowrecv:PORT        the same, slowly (2 KiB every 5 ms, small
                               buffers: the guest sees the window close)
  testnet.py client PORT TEXT  connect to 127.0.0.1:PORT (a hostfwd into the
                               guest), send TEXT, end sending, print the answer

Everything that happens is printed to stdout, one line each.
"""
import socket
import sys
import threading
import time


def say(text):
    print(text, flush=True)


# (65e) Cut from a precomputed run of the period: built byte by byte, a
# 64 KiB chunk took milliseconds and the Mac was the slower end of netrate.
_PERIOD = bytes(i % 251 for i in range(251)) * 300


def pattern(n, start=0):
    if n <= len(_PERIOD) - 251:
        o = start % 251
        return _PERIOD[o:o + n]
    return bytes((start + i) % 251 for i in range(n))


def udp_echo(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    while True:
        data, peer = s.recvfrom(2048)
        say(f"udp-echo: {len(data)} bytes: {data.decode(errors='replace')}")
        s.sendto(b"echo: " + data, peer)


def tcp_server(port, handle, buffer=0):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if buffer:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, buffer)
    s.bind(("127.0.0.1", port))
    s.listen(4)
    while True:
        c, _ = s.accept()
        threading.Thread(target=handle, args=(c,), daemon=True).start()


def tcp_echo(c):
    data = c.recv(4096)
    say(f"tcp-echo: {len(data)} bytes: {data.decode(errors='replace')}")
    c.sendall(b"echo: " + data)
    c.close()


def tcp_send(n):
    def handle(c):
        sent = 0
        while sent < n:
            chunk = min(65536, n - sent)
            c.sendall(pattern(chunk, sent))
            sent += chunk
        c.close()
        say(f"tcp-send: sent {sent} bytes")
    return handle


def tcp_recv(c, slowly=False, name="tcp-recv"):
    total, wrong = 0, None
    while True:
        data = c.recv(2048 if slowly else 65536)
        if not data:
            break
        if slowly:
            time.sleep(0.005)
        if wrong is None and data != pattern(len(data), total):
            wrong = total + next(i for i in range(len(data)) if data[i] != (total + i) % 251)
        total += len(data)
    c.close()
    say(f"{name}: received {total} bytes, " +
        ("pattern ok" if wrong is None else f"wrong at byte {wrong}"))


def client(port, text):
    c = socket.create_connection(("127.0.0.1", port), timeout=20)
    c.sendall(text.encode())
    c.shutdown(socket.SHUT_WR)
    answer = b""
    while True:
        data = c.recv(4096)
        if not data:
            break
        answer += data
    say(f"client: answer: {answer.decode(errors='replace')}")


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "client":
        client(int(sys.argv[2]), sys.argv[3])
        return
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    threads = []
    for spec in sys.argv[1:]:
        kind, port, *rest = spec.split(":")
        port = int(port)
        if kind == "udp-echo":
            target, args = udp_echo, (port,)
        elif kind == "tcp-echo":
            target, args = tcp_server, (port, tcp_echo)
        elif kind == "tcp-send":
            target, args = tcp_server, (port, tcp_send(int(rest[0])))
        elif kind == "tcp-recv":
            target, args = tcp_server, (port, tcp_recv)
        elif kind == "tcp-slowrecv":
            target, args = tcp_server, (port, lambda c: tcp_recv(c, True, "tcp-slowrecv"), 8192)
        else:
            sys.exit(f"unknown service {spec}")
        t = threading.Thread(target=target, args=args, daemon=True)
        t.start()
        threads.append(t)
    say("listening")
    for t in threads:
        t.join()


if __name__ == "__main__":
    main()
