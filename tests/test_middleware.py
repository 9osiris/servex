#!/usr/bin/env python3
# middleware: basic auth, rewrites, redirects, custom response headers.
import sys
import os
import base64
import hashlib
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from helpers import *  # noqa

CONF_MW = """\
port = @PORT@
workers = 4
log = @BASE@/access.log

server {
    host = x
    docroot = @BASE@/www

    location /private/ {
        auth_realm = "test realm"
        auth_file = @BASE@/htpasswd
    }

    location /legacy/ {
        rewrite = ^/legacy/(.*)$ /new/$1
    }

    location /old/ {
        redirect = /new/
        redirect_code = 301
    }

    location /moved/ {
        redirect = /new/
        redirect_code = 302
    }

    location /hdr/ {
        add_header = X-Custom-Thing: hello
        add_header = X-Another: 42
    }
}
"""


def basic(user, pw):
    tok = base64.b64encode(("%s:%s" % (user, pw)).encode()).decode()
    return {"Authorization": "Basic " + tok}


def main():
    if not build():
        return summary()
    base = tempfile.mkdtemp(prefix="servex-test-")
    proc = None
    try:
        make_docroot(base, extra={
            os.path.join("new", "page.txt"): b"new page here",
            os.path.join("private", "secret.txt"): b"top secret",
            os.path.join("hdr", "f.txt"): b"hdr file",
        })
        sha = base64.b64encode(hashlib.sha1(b"hunter2").digest()).decode()
        with open(os.path.join(base, "htpasswd"), "w") as f:
            f.write("alice:secret123\n")
            f.write("bob:{SHA}%s\n" % sha)
        proc, base, port = start_server(CONF_MW, base=base)

        # auth: no credentials -> 401 with www-authenticate
        st, h, b = get(port, "/private/secret.txt")
        check("no credentials is 401",
              st == 401 and h.get("www-authenticate") == 'Basic realm="test realm"',
              "status=%s hdr=%s" % (st, h.get("www-authenticate")))
        # wrong password -> 401
        st, h, b = get(port, "/private/secret.txt",
                       headers=basic("alice", "wrong"))
        check("wrong password is 401", st == 401, "status=%s" % st)
        # unknown user -> 401
        st, h, b = get(port, "/private/secret.txt",
                       headers=basic("mallory", "x"))
        check("unknown user is 401", st == 401)
        # plain password ok
        st, h, b = get(port, "/private/secret.txt",
                       headers=basic("alice", "secret123"))
        check("correct plain password serves the file",
              st == 200 and b == b"top secret", "status=%s" % st)
        # {SHA} password ok
        st, h, b = get(port, "/private/secret.txt",
                       headers=basic("bob", "hunter2"))
        check("{SHA} password verifies", st == 200, "status=%s" % st)
        # unprotected paths still open
        st, h, b = get(port, "/note.txt")
        check("paths outside auth location stay open", st == 200)

        # rewrite
        st, h, b = get(port, "/legacy/page.txt")
        check("rewrite maps legacy path to new file",
              st == 200 and b == b"new page here", "status=%s" % st)
        st, h, b = get(port, "/new/page.txt")
        check("rewritten target still served directly", st == 200)

        # redirects
        st, h, b = get(port, "/old/page.txt")
        check("redirect location gives 301",
              st == 301 and h.get("location") == "/new/",
              "status=%s loc=%s" % (st, h.get("location")))
        st, h, b = get(port, "/moved/page.txt")
        check("redirect_code 302 honored",
              st == 302 and h.get("location") == "/new/",
              "status=%s" % st)
        # query string preserved through redirect
        c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        c.request("GET", "/old/x?a=b", headers={})
        r = c.getresponse()
        loc = r.getheader("Location")
        r.read()
        c.close()
        check("redirect keeps the query string",
              loc == "/new/?a=b", "location=%s" % loc)

        # custom response headers
        st, h, b = get(port, "/hdr/f.txt")
        check("add_header values land on the response",
              st == 200 and h.get("x-custom-thing") == "hello"
              and h.get("x-another") == "42",
              str({k: v for k, v in h.items() if k.startswith("x-")}))
    finally:
        if proc:
            stop_server(proc, base)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
