#!/usr/bin/env python3
# http core tests: chunked request bodies, expect 100-continue,
# absolute uris, HEAD, OPTIONS, header edge cases.
import sys
import os
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from helpers import *  # noqa


def main():
    if not build():
        return summary()
    base = tempfile.mkdtemp(prefix="servex-test-")
    proc = None
    try:
        make_docroot(base)
        proc, base, port = start_server(CONF_BASIC, base=base)

        # chunked request body is reassembled for /echo
        payload = (b"POST /echo HTTP/1.1\r\nHost: x\r\n"
                   b"Transfer-Encoding: chunked\r\n"
                   b"Connection: close\r\n\r\n"
                   b"5\r\nhello\r\n"
                   b"6;ext=1\r\n world\r\n"
                   b"0\r\nX-Trailer: yes\r\n\r\n")
        (line, h, body), = raw(port, payload)
        check("chunked request body reassembled",
              line.startswith("HTTP/1.1 200") and b"hello world" in body,
              line)

        # chunked body sent in two tcp segments
        s = socket.create_connection(("127.0.0.1", port), timeout=10)
        s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\n"
                  b"Transfer-Encoding: chunked\r\n"
                  b"Connection: close\r\n\r\n"
                  b"4\r\nWiki\r\n")
        time.sleep(0.3)
        s.sendall(b"5\r\npedia\r\n0\r\n\r\n")
        f = s.makefile("rb")
        line, h, body = read_response(f)
        s.close()
        check("chunked body across tcp segments",
              line.startswith("HTTP/1.1 200") and b"Wikipedia" in body, line)

        # expect: 100-continue
        s = socket.create_connection(("127.0.0.1", port), timeout=10)
        s.sendall(b"POST /echo HTTP/1.1\r\nHost: x\r\n"
                  b"Content-Length: 11\r\nExpect: 100-continue\r\n"
                  b"Connection: close\r\n\r\n")
        f = s.makefile("rb")
        interim = f.readline().decode("latin1")
        check("server sends 100 continue",
              interim.strip() == "HTTP/1.1 100 Continue", interim.strip())
        f.readline()  # blank line ending the interim response
        s.sendall(b"hello world")
        line, h, body = read_response(f)
        s.close()
        check("body after 100-continue is read",
              line.startswith("HTTP/1.1 200") and b"hello world" in body,
              line)

        # absolute uri in request line
        (line, h, body), = raw(port,
                               b"GET http://example.com/hello HTTP/1.1\r\n"
                               b"Host: example.com\r\n"
                               b"Connection: close\r\n\r\n")
        check("absolute uri request works",
              line.startswith("HTTP/1.1 200") and b"Hello from servex" in body,
              line)

        # HEAD returns headers, no body, correct content-length
        st, h, b = get(port, "/note.txt", method="HEAD")
        check("HEAD has no body but keeps content-length",
              st == 200 and b == b"" and h.get("content-length") == "15",
              "status=%s len=%s" % (st, h.get("content-length")))
        st2, h2, b2 = get(port, "/note.txt", method="GET")
        check("HEAD content-length matches GET body",
              h.get("content-length") == h2.get("content-length")
              and len(b2) == 15)

        # OPTIONS
        st, h, b = get(port, "/hello", method="OPTIONS")
        check("OPTIONS returns allow header",
              st == 200 and "GET" in h.get("allow", "")
              and "HEAD" in h.get("allow", ""), h.get("allow"))
        (line, h, body), = raw(port, b"OPTIONS * HTTP/1.1\r\n"
                                     b"Host: x\r\nConnection: close\r\n\r\n")
        check("OPTIONS * works", line.startswith("HTTP/1.1 200"), line)

        # conflicting duplicate content-length values are rejected
        (line, h, body), = raw(port,
                               b"POST /echo HTTP/1.1\r\nHost: x\r\n"
                               b"Content-Length: 4\r\nContent-Length: 5\r\n"
                               b"Connection: close\r\n\r\nping")
        check("conflicting content-lengths are 400",
              line.startswith("HTTP/1.1 400"), line)

        # content-length plus transfer-encoding is rejected
        (line, h, body), = raw(port,
                               b"POST /echo HTTP/1.1\r\nHost: x\r\n"
                               b"Content-Length: 4\r\n"
                               b"Transfer-Encoding: chunked\r\n"
                               b"Connection: close\r\n\r\n"
                               b"4\r\nping\r\n0\r\n\r\n")
        check("content-length with chunked is 400",
              line.startswith("HTTP/1.1 400"), line)

        # garbage chunk size is rejected
        (line, h, body), = raw(port,
                               b"POST /echo HTTP/1.1\r\nHost: x\r\n"
                               b"Transfer-Encoding: chunked\r\n"
                               b"Connection: close\r\n\r\n"
                               b"zz\r\nping\r\n0\r\n\r\n")
        check("bad chunk size is 400", line.startswith("HTTP/1.1 400"), line)

        # chunked keep-alive: two chunked requests on one socket
        chunked_req = (b"POST /echo HTTP/1.1\r\nHost: x\r\n"
                       b"Transfer-Encoding: chunked\r\n\r\n"
                       b"3\r\nabc\r\n0\r\n\r\n")
        r1, r2 = raw(port, chunked_req + chunked_req, responses=2)
        check("two chunked requests on one keep-alive socket",
              r1[0].startswith("HTTP/1.1 200")
              and r2[0].startswith("HTTP/1.1 200")
              and b"\nabc\n" in r1[2] and b"\nabc\n" in r2[2],
              "%s / %s" % (r1[0], r2[0]))
    finally:
        if proc:
            stop_server(proc, base)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
