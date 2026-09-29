#!/usr/bin/env python3
# cgi: env vars, post bodies, status/location headers, timeout, exec bit.
import sys
import os
import stat
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from helpers import *  # noqa

ENV_SCRIPT = """#!/bin/sh
echo "Content-Type: text/plain"
echo "X-CGI-Seen: yes"
echo ""
echo "method=$REQUEST_METHOD"
echo "query=$QUERY_STRING"
echo "script=$SCRIPT_NAME"
echo "pathinfo=$PATH_INFO"
echo "remote=$REMOTE_ADDR"
echo "ctype=$CONTENT_TYPE"
echo "clen=$CONTENT_LENGTH"
echo "ua=$HTTP_USER_AGENT"
echo "gw=$GATEWAY_INTERFACE"
echo "---body---"
cat
"""

STATUS_SCRIPT = """#!/bin/sh
echo "Status: 201 Created"
echo "Content-Type: text/plain"
echo ""
echo "made it"
"""

REDIR_SCRIPT = """#!/bin/sh
echo "Location: /somewhere-else"
echo ""
echo "redirecting"
"""

SLOW_SCRIPT = """#!/bin/sh
sleep 8
echo "Content-Type: text/plain"
echo ""
echo "too late"
"""

NOEXEC_SCRIPT = """#!/bin/sh
echo "Content-Type: text/plain"
echo ""
echo "should not run"
"""

CONF_CGI = """\
port = @PORT@
workers = 4
log = @BASE@/access.log

server {
    host = x
    docroot = @BASE@/www

    location /cgi-bin/ {
        cgi = on
    }
}
"""


def write_script(base, name, body, executable=True):
    d = os.path.join(base, "www", "cgi-bin")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, name)
    with open(p, "w") as f:
        f.write(body)
    mode = 0o755 if executable else 0o644
    os.chmod(p, mode)


def main():
    if not build():
        return summary()
    base = tempfile.mkdtemp(prefix="servex-test-")
    proc = None
    try:
        write_script(base, "env.sh", ENV_SCRIPT)
        write_script(base, "status.sh", STATUS_SCRIPT)
        write_script(base, "redir.sh", REDIR_SCRIPT)
        write_script(base, "slow.sh", SLOW_SCRIPT)
        write_script(base, "noexec.sh", NOEXEC_SCRIPT, executable=False)
        proc, base, port = start_server(CONF_CGI, base=base)

        # get with query string: env vars arrive
        st, h, b = get(port, "/cgi-bin/env.sh?name=x&n=2",
                       headers={"User-Agent": "cgi-test/1.0"})
        lines = dict(l.split("=", 1) for l in b.decode().splitlines()
                     if "=" in l and not l.startswith("---"))
        check("cgi runs the script", st == 200 and h.get("x-cgi-seen") == "yes",
              "status=%s" % st)
        check("cgi output is streamed with chunked encoding",
              h.get("transfer-encoding") == "chunked",
              h.get("transfer-encoding"))
        check("cgi sets request env vars",
              lines.get("method") == "GET"
              and lines.get("query") == "name=x&n=2"
              and lines.get("script") == "/cgi-bin/env.sh"
              and lines.get("gw") == "CGI/1.1"
              and lines.get("ua") == "cgi-test/1.0",
              str(lines))
        check("cgi sets remote addr", lines.get("remote") == "127.0.0.1",
              lines.get("remote"))

        # post body reaches the script on stdin
        st, h, b = get(port, "/cgi-bin/env.sh", method="POST",
                       body=b"hello-cgi",
                       headers={"Content-Type": "text/plain"})
        text = b.decode()
        check("cgi passes the body on stdin",
              st == 200 and "clen=9" in text
              and text.split("---body---")[1].strip() == "hello-cgi"
              and "ctype=text/plain" in text,
              text[:160])

        # path info: extra segments after the script name
        st, h, b = get(port, "/cgi-bin/env.sh/extra/path?q=1")
        text = b.decode()
        check("cgi splits path_info",
              "pathinfo=/extra/path" in text
              and "script=/cgi-bin/env.sh" in text, text[:160])

        # status header controls the response code
        st, h, b = get(port, "/cgi-bin/status.sh")
        check("cgi status header sets the code",
              st == 201 and b == b"made it\n", "status=%s body=%r" % (st, b))

        # location header becomes a redirect
        st, h, b = get(port, "/cgi-bin/redir.sh")
        check("cgi location header redirects",
              st == 302 and h.get("location") == "/somewhere-else",
              "status=%s loc=%s" % (st, h.get("location")))

        # missing script is a 404
        st, h, b = get(port, "/cgi-bin/nope.sh")
        check("missing cgi script is 404", st == 404, "status=%s" % st)

        # non-executable file is a 403
        st, h, b = get(port, "/cgi-bin/noexec.sh")
        check("non-executable cgi is 403", st == 403, "status=%s" % st)

        # hanging script is killed with a 504
        st, h, b = get(port, "/cgi-bin/slow.sh")
        check("timed out cgi is 504", st == 504, "status=%s" % st)
    finally:
        if proc:
            stop_server(proc, base)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
