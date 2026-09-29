#!/usr/bin/env python3
# virtual hosts, block config, includes, and ${variables}.
import sys
import os
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from helpers import *  # noqa

CONF_VHOSTS = """\
port = @PORT@
workers = 4
log = @BASE@/global.log

set $doc_a @BASE@/site_a
set $doc_b @BASE@/site_b

server {
    host = a.example.com
    docroot = ${doc_a}
    log = @BASE@/a.log
}

server {
    host = *.example.com
    docroot = ${doc_b}
    log = @BASE@/b.log
}
"""


def main():
    if not build():
        return summary()
    base = tempfile.mkdtemp(prefix="servex-test-")
    proc = None
    try:
        for site, text in (("site_a", b"site A home"),
                           ("site_b", b"site B home")):
            d = os.path.join(base, site)
            os.makedirs(d)
            with open(os.path.join(d, "index.html"), "wb") as f:
                f.write(b"<html>" + text + b"</html>")
        proc, base, port = start_server(CONF_VHOSTS, base=base)

        st, h, b = get(port, "/", headers={"Host": "a.example.com"})
        check("exact host name selects its docroot",
              st == 200 and b"site A home" in b, "status=%s" % st)

        st, h, b = get(port, "/", headers={"Host": "deep.example.com"})
        check("wildcard host selects its docroot",
              st == 200 and b"site B home" in b, "status=%s" % st)

        st, h, b = get(port, "/", headers={"Host": "other.org"})
        check("unknown host falls back to the default server",
              st == 200 and b"site A home" in b, "status=%s" % st)

        # demo routes exist on every vhost
        st, h, b = get(port, "/hello", headers={"Host": "deep.example.com"})
        check("demo routes work on all vhosts",
              st == 200 and b == b"Hello from servex\n")

        # per-host access logs: distinct paths per host, then check split
        get(port, "/from-a", headers={"Host": "a.example.com"})
        get(port, "/from-b", headers={"Host": "deep.example.com"})
        time.sleep(0.3)
        a_log = open(os.path.join(base, "a.log")).read() \
            if os.path.exists(os.path.join(base, "a.log")) else ""
        b_log = open(os.path.join(base, "b.log")).read() \
            if os.path.exists(os.path.join(base, "b.log")) else ""
        check("per-host access logs split by host",
              "GET /from-a " in a_log and "GET /from-b " not in a_log
              and "GET /from-b " in b_log and "GET /from-a " not in b_log,
              "a.log=%r b.log=%r" % (a_log[-80:], b_log[-80:]))

        # include directive pulls in another server block
        inc_dir = os.path.join(base, "conf.d")
        os.makedirs(inc_dir)
        site_c = os.path.join(base, "site_c")
        os.makedirs(site_c)
        with open(os.path.join(site_c, "index.html"), "wb") as f:
            f.write(b"site C via include")
        with open(os.path.join(inc_dir, "c.conf"), "w") as f:
            f.write("server {\n    host = c.example.com\n"
                    "    docroot = %s\n}\n" % site_c)
        conf2 = ("port = @PORT@\nworkers = 4\nlog = @BASE@/g2.log\n"
                 "include %s\n"
                 "server {\n    host = main.example.com\n"
                 "    docroot = %s\n}\n" % (os.path.join(inc_dir, "*.conf"),
                                            os.path.join(base, "site_a")))
        proc2, base2, port2 = start_server(conf2)
        try:
            st, h, b = get(port2, "/", headers={"Host": "c.example.com"})
            check("include directive loads the extra server block",
                  st == 200 and b"site C via include" in b, "status=%s" % st)
            st, h, b = get(port2, "/", headers={"Host": "main.example.com"})
            check("main config server still works with includes",
                  st == 200 and b"site A home" in b)
        finally:
            stop_server(proc2, base2)
    finally:
        if proc:
            stop_server(proc, base)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
