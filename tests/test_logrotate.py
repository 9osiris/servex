#!/usr/bin/env python3
# log rotation: size-triggered rotation keeps N archives, loses no lines.
import sys
import os
import glob
import re
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from helpers import *  # noqa

CONF_ROTATE = """\
port = @PORT@
workers = 4
log = @BASE@/access.log
log_max_bytes = 2000
log_archives = 3

server {
    host = x
    docroot = @BASE@/www
}
"""

CONF_NOROTATE = """\
port = @PORT@
workers = 4
log = @BASE@/access.log

server {
    host = x
    docroot = @BASE@/www
}
"""


def count_lines(path):
    with open(path, "rb") as f:
        return sum(1 for _ in f)


def main():
    if not build():
        return summary()
    proc = None
    try:
        # rotation on
        base = tempfile.mkdtemp(prefix="servex-test-")
        make_docroot(base)
        proc, base, port = start_server(CONF_ROTATE, base=base)
        n = 120
        for i in range(n):
            st, _, _ = get(port, "/index.html?n=%d" % i)
            assert st == 200, st
        files = sorted(glob.glob(os.path.join(base, "access.log*")))
        names = [os.path.basename(f) for f in files]
        check("rotation creates numbered archives",
              "access.log.1" in names and "access.log.2" in names
              and "access.log.3" in names, str(names))
        check("archives are capped at log_archives",
              "access.log.4" not in names, str(names))
        # every kept line belongs to one contiguous tail of the requests:
        # nothing lost except the oldest rotations past the archive cap
        seen = []
        for f in files:
            with open(f, "rb") as fh:
                for line in fh.read().decode("latin1").splitlines():
                    m = re.search(r"n=(\d+)", line)
                    if m:
                        seen.append(int(m.group(1)))
        seen.sort()
        check("kept lines are a gapless tail of the requests",
              seen == list(range(n - len(seen), n)),
              "kept=%d first=%s last=%s" % (
                  len(seen), seen[0] if seen else None,
                  seen[-1] if seen else None))
        # the log keeps working after rotations
        st, _, _ = get(port, "/index.html?n=999")
        assert st == 200
        with open(os.path.join(base, "access.log"), "rb") as f:
            current = f.read().decode("latin1")
        check("current log keeps receiving lines", "n=999" in current,
              current[-80:])
    finally:
        if proc:
            stop_server(proc, base)

    # rotation off by default
    base = tempfile.mkdtemp(prefix="servex-test-")
    proc = None
    try:
        make_docroot(base)
        proc, base, port = start_server(CONF_NOROTATE, base=base)
        for i in range(30):
            st, _, _ = get(port, ["/index.html", "/style.css", "/app.js", "/data.json", "/note.txt"][i % 5])
            assert st == 200, st
        names = [os.path.basename(f)
                 for f in glob.glob(os.path.join(base, "access.log*"))]
        check("no rotation without log_max_bytes",
              names == ["access.log"], str(names))
        check("all lines land in the one log",
              count_lines(os.path.join(base, "access.log")) == 30,
              str(names))
    finally:
        if proc:
            stop_server(proc, base)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
