#!/usr/bin/env python3
# reverse proxy: forwarding, headers, chunked, pooling, 502.
import sys
import os
import json
import socket
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from helpers import *  # noqa


class Upstream:
    # tiny keep-alive http server that echoes what it received
    def __init__(self, mode="echo"):
        self.mode = mode
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.sock.listen(16)
        self.sock.settimeout(0.5)
        self.connections = 0
        self.requests = []
        self.lock = threading.Lock()
        self.running = True
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while self.running:
            try:
                c, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with self.lock:
                self.connections += 1
            t = threading.Thread(target=self.handle, args=(c,), daemon=True)
            t.start()

    def handle(self, c):
        f = c.makefile("rb")
        try:
            while True:
                line = f.readline()
                if not line:
                    break
                method, path, _ = line.decode("latin1").split()
                headers = {}
                while True:
                    h = f.readline().decode("latin1")
                    if h in ("\r\n", "\n", ""):
                        break
                    k, v = h.split(":", 1)
                    headers[k.strip().lower()] = v.strip()
                body = b""
                n = int(headers.get("content-length", "0"))
                while len(body) < n:
                    piece = f.read(n - len(body))
                    if not piece:
                        break
                    body += piece
                with self.lock:
                    self.requests.append({"method": method, "path": path,
                                          "headers": headers, "body": body})
                if self.mode == "chunked":
                    payload = (b"HTTP/1.1 200 OK\r\n"
                               b"Transfer-Encoding: chunked\r\n"
                               b"Content-Type: text/plain\r\n"
                               b"Connection: keep-alive\r\n\r\n"
                               b"5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n")
                elif self.mode == "close":
                    info = b"closing"
                    payload = (b"HTTP/1.1 200 OK\r\nContent-Length: 7\r\n"
                               b"Connection: close\r\n\r\n" + info)
                else:
                    info = json.dumps({
                        "method": method, "path": path,
                        "xff": headers.get("x-forwarded-for"),
                        "host": headers.get("host"),
                        "body": body.decode("latin1"),
                    }).encode()
                    payload = (b"HTTP/1.1 200 OK\r\n"
                               b"Content-Type: application/json\r\n"
                               b"Content-Length: %d\r\n"
                               b"Connection: keep-alive\r\n\r\n" % len(info)
                               + info)
                c.sendall(payload)
                if headers.get("connection") == "close":
                    break
        except Exception:
            pass
        finally:
            c.close()

    def stop(self):
        self.running = False
        self.sock.close()

    def last(self):
        with self.lock:
            return self.requests[-1] if self.requests else None


CONF_PROXY = """\
port = @PORT@
workers = 4
log = @BASE@/access.log

server {
    host = x
    docroot = @BASE@/www

    location /api/ {
        proxy_pass = 127.0.0.1:@UP@
    }

    location /dead/ {
        proxy_pass = 127.0.0.1:@DEAD@
    }
}
"""


def main():
    if not build():
        return summary()
    base = tempfile.mkdtemp(prefix="servex-test-")
    proc = None
    up = Upstream()
    try:
        dead = free_port()
        make_docroot(base)
        conf = CONF_PROXY.replace("@UP@", str(up.port)) \
                         .replace("@DEAD@", str(dead))
        proc, base, port = start_server(conf, base=base)

        # basic forwarding
        st, h, b = get(port, "/api/hello")
        info = json.loads(b)
        check("proxy forwards method and path",
              st == 200 and info["method"] == "GET"
              and info["path"] == "/api/hello",
              "status=%s info=%s" % (st, info))
        check("proxy adds x-forwarded-for",
              info["xff"] == "127.0.0.1", info["xff"])
        check("proxy preserves the client host header",
              info["host"] == "127.0.0.1:%d" % port, info["host"])

        # post body forwarded
        st, h, b = get(port, "/api/echo", method="POST",
                       body=b"proxy-body-123",
                       headers={"Content-Type": "text/plain"})
        info = json.loads(b)
        check("proxy forwards the request body",
              st == 200 and info["body"] == "proxy-body-123"
              and info["method"] == "POST", str(info)[:120])

        # chunked client body is de-chunked before forwarding
        payload = (b"POST /api/echo HTTP/1.1\r\nHost: x\r\n"
                   b"Transfer-Encoding: chunked\r\n"
                   b"Connection: close\r\n\r\n"
                   b"4\r\nWiki\r\n3\r\nped\r\n"
                   b"2\r\nia\r\n0\r\n\r\n")
        (line, h2, b2), = raw(port, payload)
        info = json.loads(b2)
        last = up.last()
        check("chunked body arrives whole upstream",
              line.startswith("HTTP/1.1 200")
              and last["headers"].get("transfer-encoding") is None
              and last["body"] == b"Wikipedia",
              str(last["headers"].get("transfer-encoding")))

        # query string forwarded
        st, h, b = get(port, "/api/search?q=test&n=2")
        info = json.loads(b)
        check("proxy forwards the query string",
              info["path"] == "/api/search?q=test&n=2", info["path"])

        # pooling: sequential requests reuse one upstream connection
        before = up.connections
        for _ in range(3):
            st, _, _ = get(port, "/api/pooled")
            assert st == 200
        check("upstream connections are pooled",
              up.connections == before,
              "before=%d after=%d" % (before, up.connections))

        # chunked upstream response is re-streamed as chunked
        up.mode = "chunked"
        # drop pooled plain connections by hitting a fresh path is not
        # needed: the pool reuses the same socket fine
        st, h, b = get(port, "/api/stream")
        check("chunked upstream response stays chunked",
              st == 200 and h.get("transfer-encoding") == "chunked"
              and b == b"hello world",
              "status=%s te=%s body=%r"
              % (st, h.get("transfer-encoding"), b))
        up.mode = "echo"

        # dead upstream is a 502
        st, h, b = get(port, "/dead/nowhere")
        check("dead upstream gives 502", st == 502, "status=%s" % st)

        # upstream errors counted
        time.sleep(0.2)
        st, h, b = get(port, "/status")
        info = json.loads(b)
        check("upstream_errors counter increments",
              info.get("upstream_errors", 0) >= 1,
              str(info.get("upstream_errors")))
    finally:
        up.stop()
        if proc:
            stop_server(proc, base)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
