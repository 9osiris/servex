#!/usr/bin/env python3
# gzip responses: off by default, opt-in per server block,
# only for compressible types above the size threshold.
import gzip
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from helpers import (CONF_BASIC, build, check, free_port, get, make_docroot,
                     start_server, stop_server, summary)

BIG_TEXT = b"lorem ipsum dolor sit amet, consectetur adipiscing elit. " * 400
SMALL_TEXT = b"tiny file"
BIG_PNG = b"\x89PNG\r\n\x1a\n" + os.urandom(5000)

CONF_GZIP = """\
port = @PORT@
docroot = @BASE@/www
dir_listing = on
log = @BASE@/access.log
gzip = on
gzip_min_size = 1024
"""

CONF_GZIP_KB = """\
port = @PORT@
docroot = @BASE@/www
gzip = on
gzip_min_size = 2kb
"""


def main():
    if not build():
        return summary()

    extra = {
        "big.txt": BIG_TEXT,
        "small.txt": SMALL_TEXT,
        "mid.txt": b"mid-size line of repetitive text. " * 45,
        "big.png": BIG_PNG,
        "big.json": b'{"key": "value", "n": 123}, ' * 300,
    }

    import tempfile
    base = tempfile.mkdtemp(prefix="servex-gzip-")
    make_docroot(base, extra)
    proc, base, port = start_server(CONF_GZIP, base=base)

    s, h, b = get(port, "/big.txt", headers={"Accept-Encoding": "gzip"})
    check("gzip 200", s == 200, "got %d" % s)
    check("gzip content-encoding", h.get("content-encoding") == "gzip",
          str(h.get("content-encoding")))
    try:
        dec = gzip.decompress(b)
        check("gzip roundtrip", dec == BIG_TEXT)
    except Exception as e:
        check("gzip roundtrip", False, str(e))
    check("gzip actually smaller", len(b) < len(BIG_TEXT),
          "%d vs %d" % (len(b), len(BIG_TEXT)))
    check("gzip keeps content-type", h.get("content-type") == "text/plain",
          str(h.get("content-type")))
    check("gzip vary header", "accept-encoding" in h.get("vary", "").lower(),
          str(h.get("vary")))
    check("gzip etag present", "etag" in h, "no etag")

    s, h, b = get(port, "/big.txt")
    check("no-ae 200", s == 200)
    check("no-ae not gzipped", "content-encoding" not in h, str(h))
    check("no-ae body intact", b == BIG_TEXT)
    check("no-ae vary present", "accept-encoding" in h.get("vary", "").lower())

    s, h, b = get(port, "/small.txt", headers={"Accept-Encoding": "gzip"})
    check("small not gzipped", "content-encoding" not in h, str(h))
    check("small body intact", b == SMALL_TEXT)

    s, h, b = get(port, "/big.png", headers={"Accept-Encoding": "gzip"})
    check("png not gzipped", "content-encoding" not in h, str(h))
    check("png body intact", b == BIG_PNG, "len %d" % len(b))
    check("png no vary", "vary" not in h, str(h.get("vary")))

    s, h, b = get(port, "/big.json", headers={"Accept-Encoding": "gzip"})
    check("json gzipped", h.get("content-encoding") == "gzip")
    check("json roundtrip", gzip.decompress(b) == extra["big.json"])

    s, h, b = get(port, "/big.txt", method="HEAD",
                  headers={"Accept-Encoding": "gzip"})
    check("head 200", s == 200)
    check("head content-encoding", h.get("content-encoding") == "gzip")
    check("head empty body", b == b"", "len %d" % len(b))
    _, h2, b2 = get(port, "/big.txt", headers={"Accept-Encoding": "gzip"})
    check("head length matches get",
          h.get("content-length") == h2.get("content-length"),
          "%s vs %s" % (h.get("content-length"), h2.get("content-length")))

    _, h, _ = get(port, "/big.txt", headers={"Accept-Encoding": "gzip"})
    etag = h.get("etag")
    s, h, b = get(port, "/big.txt",
                  headers={"Accept-Encoding": "gzip",
                           "If-None-Match": etag})
    check("gzip 304", s == 304, "got %d" % s)
    check("304 empty body", b == b"")

    s, h, b = get(port, "/big.txt",
                  headers={"Accept-Encoding": "gzip",
                           "Range": "bytes=0-99"})
    check("range 206", s == 206, "got %d" % s)
    check("range not gzipped", "content-encoding" not in h, str(h))
    check("range body intact", b == BIG_TEXT[0:100])

    s, h, b = get(port, "/big.txt", headers={"Accept-Encoding": "x-gzip"})
    check("x-gzip accepted", h.get("content-encoding") == "gzip")

    s, h, b = get(port, "/big.txt",
                  headers={"Accept-Encoding": "deflate, gzip;q=0.8"})
    check("gzip qvalue accepted", h.get("content-encoding") == "gzip")

    stop_server(proc, base)

    base = tempfile.mkdtemp(prefix="servex-nogzip-")
    make_docroot(base, extra)
    proc, base, port = start_server(CONF_BASIC, base=base)
    s, h, b = get(port, "/big.txt", headers={"Accept-Encoding": "gzip"})
    check("default off: not gzipped", "content-encoding" not in h, str(h))
    check("default off: body intact", b == BIG_TEXT)
    check("default off: no vary", "vary" not in h, str(h.get("vary")))
    stop_server(proc, base)

    base = tempfile.mkdtemp(prefix="servex-gzipkb-")
    make_docroot(base, extra)
    proc, base, port = start_server(CONF_GZIP_KB, base=base)
    s, h, b = get(port, "/mid.txt", headers={"Accept-Encoding": "gzip"})
    check("2kb threshold skips 1.5k txt", "content-encoding" not in h,
          str(h.get("content-encoding")))
    check("2kb mid.txt body intact", b == extra["mid.txt"])
    s, h, b = get(port, "/big.json", headers={"Accept-Encoding": "gzip"})
    check("2kb threshold keeps 8k json", h.get("content-encoding") == "gzip")
    s, h, b = get(port, "/big.txt", headers={"Accept-Encoding": "gzip"})
    check("2kb threshold keeps big txt", h.get("content-encoding") == "gzip")
    stop_server(proc, base)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
