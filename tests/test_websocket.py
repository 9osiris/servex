#!/usr/bin/env python3
# websocket: rfc 6455 handshake, echo, ping/pong, fragments, close, errors.
import sys
import os
import base64
import hashlib
import socket
import struct
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from helpers import *  # noqa

WS_KEY = "dGhlIHNhbXBsZSBub25jZQ=="
WS_ACCEPT = base64.b64encode(
    hashlib.sha1((WS_KEY + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
                 .encode()).digest()).decode()

CONF_WS = """\
port = @PORT@
workers = 4
log = @BASE@/access.log

server {
    host = x
    docroot = @BASE@/www

    location /ws/ {
        websocket = on
    }
}
"""


class WsClient:
    def __init__(self, port, path="/ws/", key=WS_KEY, version="13",
                 extra_headers=()):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        req = ("GET %s HTTP/1.1\r\nHost: x\r\n"
               "Upgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: %s\r\n" % (path, key, version))
        for k, v in extra_headers:
            req += "%s: %s\r\n" % (k, v)
        req += "\r\n"
        self.s.sendall(req.encode())
        self.buf = b""
        self.status_line, self.headers, _ = self.read_http_response()

    def read_http_response(self):
        while b"\r\n\r\n" not in self.buf:
            piece = self.s.recv(4096)
            if not piece:
                break
            self.buf += piece
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        lines = head.decode("latin1").split("\r\n")
        headers = {}
        for l in lines[1:]:
            k, v = l.split(":", 1)
            headers[k.strip().lower()] = v.strip()
        return lines[0], headers, b""

    def send_frame(self, opcode, payload=b"", fin=True, mask=True):
        b0 = (0x80 if fin else 0x00) | opcode
        n = len(payload)
        b1 = 0x80 if mask else 0x00
        hdr = bytes([b0])
        if n < 126:
            hdr += bytes([b1 | n])
        elif n < 65536:
            hdr += bytes([b1 | 126]) + struct.pack(">H", n)
        else:
            hdr += bytes([b1 | 127]) + struct.pack(">Q", n)
        if mask:
            m = b"\x11\x22\x33\x44"
            hdr += m
            payload = bytes(c ^ m[i % 4] for i, c in enumerate(payload))
        self.s.sendall(hdr + payload)

    def recv_frame(self, timeout=10):
        self.s.settimeout(timeout)
        while len(self.buf) < 2:
            piece = self.s.recv(4096)
            if not piece:
                return None
            self.buf += piece
        b0, b1 = self.buf[0], self.buf[1]
        fin = bool(b0 & 0x80)
        opcode = b0 & 0x0f
        masked = bool(b1 & 0x80)
        n = b1 & 0x7f
        off = 2
        if n == 126:
            while len(self.buf) < 4:
                self.buf += self.s.recv(4096)
            n = struct.unpack(">H", self.buf[2:4])[0]
            off = 4
        elif n == 127:
            while len(self.buf) < 10:
                self.buf += self.s.recv(4096)
            n = struct.unpack(">Q", self.buf[2:10])[0]
            off = 10
        if masked:
            while len(self.buf) < off + 4:
                self.buf += self.s.recv(4096)
            m = self.buf[off:off + 4]
            off += 4
        while len(self.buf) < off + n:
            piece = self.s.recv(65536)
            if not piece:
                return None
            self.buf += piece
        payload = self.buf[off:off + n]
        self.buf = self.buf[off + n:]
        if masked:
            payload = bytes(c ^ m[i % 4] for i, c in enumerate(payload))
        return fin, opcode, payload

    def close(self):
        self.s.close()


def main():
    if not build():
        return summary()
    base = tempfile.mkdtemp(prefix="servex-test-")
    proc = None
    try:
        make_docroot(base)
        proc, base, port = start_server(CONF_WS, base=base)

        # handshake
        c = WsClient(port)
        check("websocket handshake is 101",
              c.status_line.startswith("HTTP/1.1 101")
              and c.headers.get("upgrade") == "websocket"
              and c.headers.get("sec-websocket-accept") == WS_ACCEPT,
              c.status_line)
        c.close()

        # text echo
        c = WsClient(port)
        c.send_frame(0x1, b"hello ws")
        f = c.recv_frame()
        check("text frames are echoed",
              f is not None and f[1] == 0x1 and f[2] == b"hello ws"
              and f[0] is True, str(f)[:80])

        # binary echo
        c.send_frame(0x2, bytes(range(256)))
        f = c.recv_frame()
        check("binary frames are echoed",
              f is not None and f[1] == 0x2 and f[2] == bytes(range(256)),
              "opcode=%s len=%s" % (f[1], len(f[2])) if f else None)

        # ping -> pong
        c.send_frame(0x9, b"pingdata")
        f = c.recv_frame()
        check("ping gets a pong",
              f is not None and f[1] == 0xA and f[2] == b"pingdata",
              str(f)[:80])

        # fragmented message is reassembled
        c.send_frame(0x1, b"hel", fin=False)
        c.send_frame(0x0, b"lo", fin=True)
        f = c.recv_frame()
        check("fragmented message is reassembled and echoed",
              f is not None and f[1] == 0x1 and f[2] == b"hello",
              str(f)[:80])

        # close handshake
        c.send_frame(0x8, struct.pack(">H", 1000))
        f = c.recv_frame()
        closed = False
        try:
            closed = c.s.recv(1) == b""
        except socket.timeout:
            pass
        check("close frame is answered and the socket closes",
              f is not None and f[1] == 0x8 and closed, str(f)[:40])
        c.close()

        # unmasked client frame kills the connection
        c = WsClient(port)
        c.send_frame(0x1, b"no mask", mask=False)
        f = c.recv_frame()
        dead = False
        try:
            dead = c.s.recv(1) == b""
        except socket.timeout:
            pass
        check("unmasked frames are rejected",
              f is not None and f[1] == 0x8 and dead, str(f)[:40])
        c.close()

        # bad version
        c = WsClient(port, version="12")
        check("bad ws version is rejected",
              c.status_line.startswith("HTTP/1.1 400"), c.status_line)
        c.close()

        # missing key
        s = socket.create_connection(("127.0.0.1", port), timeout=10)
        s.sendall(b"GET /ws/ HTTP/1.1\r\nHost: x\r\n"
                  b"Upgrade: websocket\r\nConnection: Upgrade\r\n"
                  b"Sec-WebSocket-Version: 13\r\n\r\n")
        line = s.recv(100).decode("latin1").split("\r\n")[0]
        check("missing ws key is rejected",
              line.startswith("HTTP/1.1 400"), line)
        s.close()

        # plain get on a ws location needs an upgrade
        st, h, b = get(port, "/ws/")
        check("plain get on ws location is 426", st == 426,
              "status=%s" % st)
    finally:
        if proc:
            stop_server(proc, base)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
