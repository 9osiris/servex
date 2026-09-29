#!/usr/bin/env python3
# conditional requests (etag, if-modified-since, if-match, if-none-match)
# and byte ranges (single, multipart, suffix, 416, if-range).
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
        doc = make_docroot(base, extra={
            "big.bin": bytes(range(256)) * 64,  # 16384 bytes
        })
        proc, base, port = start_server(CONF_BASIC, base=base)

        st, h, b = get(port, "/note.txt")
        etag = h.get("etag", "")
        lastmod = h.get("last-modified", "")
        check("static responses carry etag and last-modified",
              st == 200 and etag.startswith('"') and lastmod != "",
              "etag=%s lastmod=%s" % (etag, lastmod))
        check("accept-ranges advertised",
              h.get("accept-ranges") == "bytes", h.get("accept-ranges"))

        # if-none-match -> 304
        st, h, b = get(port, "/note.txt",
                       headers={"If-None-Match": etag})
        check("if-none-match with current etag is 304",
              st == 304 and b == b"", "status=%s" % st)
        st, h, b = get(port, "/note.txt",
                       headers={"If-None-Match": '"deadbeef"'})
        check("if-none-match with stale etag is 200", st == 200)
        st, h, b = get(port, "/note.txt",
                       headers={"If-None-Match": "*"})
        check("if-none-match * is 304", st == 304, "status=%s" % st)

        # if-modified-since -> 304
        st, h, b = get(port, "/note.txt",
                       headers={"If-Modified-Since": lastmod})
        check("if-modified-since with current date is 304",
              st == 304, "status=%s" % st)
        st, h, b = get(port, "/note.txt",
                       headers={"If-Modified-Since":
                                "Sun, 06 Nov 1994 08:49:37 GMT"})
        check("if-modified-since with old date is 200", st == 200)

        # if-match / if-unmodified-since
        st, h, b = get(port, "/note.txt",
                       headers={"If-Match": etag})
        check("if-match with current etag is 200", st == 200)
        st, h, b = get(port, "/note.txt",
                       headers={"If-Match": '"nope"'})
        check("if-match with wrong etag is 412",
              st == 412, "status=%s" % st)
        st, h, b = get(port, "/note.txt",
                       headers={"If-Unmodified-Since":
                                "Sun, 06 Nov 1994 08:49:37 GMT"})
        check("if-unmodified-since with old date is 412",
              st == 412, "status=%s" % st)

        # 304 keeps validators
        st, h, b = get(port, "/note.txt",
                       headers={"If-None-Match": etag})
        check("304 response keeps etag",
              st == 304 and h.get("etag") == etag)

        # single range
        st, h, b = get(port, "/big.bin",
                       headers={"Range": "bytes=0-99"})
        check("single range is 206 with content-range",
              st == 206 and h.get("content-range") == "bytes 0-99/16384"
              and len(b) == 100 and b == bytes(range(100)),
              "status=%s cr=%s len=%d" % (st, h.get("content-range"), len(b)))

        # open-ended range
        st, h, b = get(port, "/big.bin",
                       headers={"Range": "bytes=16300-"})
        check("open-ended range clamped to end",
              st == 206 and h.get("content-range") == "bytes 16300-16383/16384"
              and len(b) == 84, h.get("content-range"))

        # suffix range
        st, h, b = get(port, "/big.bin",
                       headers={"Range": "bytes=-10"})
        check("suffix range returns last bytes",
              st == 206 and len(b) == 10
              and b == (bytes(range(256)) * 64)[-10:],
              "len=%d" % len(b))

        # range past the end -> 416
        st, h, b = get(port, "/big.bin",
                       headers={"Range": "bytes=99999-100000"})
        check("unsatisfiable range is 416",
              st == 416 and h.get("content-range") == "bytes */16384",
              "status=%s" % st)

        # multipart ranges
        st, h, b = get(port, "/big.bin",
                       headers={"Range": "bytes=0-9, 20-29"})
        ctype = h.get("content-type", "")
        check("multipart range response",
              st == 206 and ctype.startswith("multipart/byteranges")
              and b"bytes 0-9/16384" in b and b"bytes 20-29/16384" in b
              and bytes(range(10)) in b and bytes(range(20, 30)) in b,
              "status=%s ctype=%s" % (st, ctype))

        # if-range with matching etag -> 206, stale etag -> 200
        _, bh, _ = get(port, "/big.bin")
        big_etag = bh.get("etag", "")
        st, h, b = get(port, "/big.bin",
                       headers={"Range": "bytes=0-9", "If-Range": big_etag})
        check("if-range with fresh etag gives 206", st == 206,
              "status=%s" % st)
        st, h, b = get(port, "/big.bin",
                       headers={"Range": "bytes=0-9",
                                "If-Range": '"stale"'})
        check("if-range with stale etag gives full 200",
              st == 200 and len(b) == 16384, "status=%s" % st)

        # HEAD with if-none-match still 304s
        st, h, b = get(port, "/note.txt", method="HEAD",
                       headers={"If-None-Match": etag})
        check("HEAD honors if-none-match", st == 304 and b == b"")

        # POST to a static file is 405
        st, h, b = get(port, "/note.txt", method="POST", body=b"x")
        check("POST to static file is 405",
              st == 405 and "GET" in h.get("allow", ""), "status=%s" % st)
    finally:
        if proc:
            stop_server(proc, base)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
