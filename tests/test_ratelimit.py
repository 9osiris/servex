#!/usr/bin/env python3
# rate limiting: token bucket per client ip, 429 with retry-after.
import sys
import os
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from helpers import *  # noqa

CONF_RL = """\
port = @PORT@
workers = 4
log = @BASE@/access.log

server {
    host = x
    docroot = @BASE@/www
    rate = 1
    burst = 2

    location /api/ {
        rate = 100
        burst = 100
    }

    location /tight/ {
        rate = 1
        burst = 1
    }
}
"""


def main():
    if not build():
        return summary()
    base = tempfile.mkdtemp(prefix="servex-test-")
    proc = None
    try:
        make_docroot(base, extra={
            os.path.join("tight", "x"): b"tight file",
        })
        proc, base, port = start_server(CONF_RL, base=base)

        # burst of 2 at the server level: first two pass
        st1, _, _ = get(port, "/hello")
        st2, _, _ = get(port, "/hello")
        check("requests inside the burst succeed",
              st1 == 200 and st2 == 200, "%s %s" % (st1, st2))

        # hammer: with rate=1/s the rest must mostly 429
        codes = []
        retry_afters = []
        for _ in range(6):
            st, h, _ = get(port, "/hello")
            codes.append(st)
            if st == 429:
                retry_afters.append(h.get("retry-after"))
        n429 = codes.count(429)
        check("burst exhausted gives 429s", n429 >= 2, str(codes))
        check("429 carries retry-after",
              retry_afters and all(r and int(r) >= 1 for r in retry_afters),
              str(retry_afters))

        # bucket refills over time
        time.sleep(2.5)
        st, _, _ = get(port, "/hello")
        check("tokens refill after waiting", st == 200, "status=%s" % st)

        # location-level bucket is separate and tighter
        st1, _, _ = get(port, "/tight/x")
        st2, h2, _ = get(port, "/tight/x")
        check("location burst of 1 allows one then 429s",
              st1 == 200 and st2 == 429, "%s %s" % (st1, st2))

        # the 429s were counted in stats (wait for a token first)
        time.sleep(1.2)
        st, h, b = get(port, "/status")
        if st == 429:
            time.sleep(1.2)
            st, h, b = get(port, "/status")
        info = json.loads(b)
        check("rejected_429 counter tracks denials",
              st == 200 and info.get("rejected_429", 0) >= 2,
              "status=%s rejected=%s" % (st, info.get("rejected_429")))
    finally:
        if proc:
            stop_server(proc, base)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
