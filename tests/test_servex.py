#!/usr/bin/env python3
# integration tests for servex. builds the binary, starts it on a test
# port with a fixture docroot, and asserts real http behavior.
# stdlib only: socket, http.client, subprocess.
import http.client
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "servex")
PORT = None
PASS = 0
FAIL = 0


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("ok   " + name)
    else:
        FAIL += 1
        print("FAIL " + name + (" -- " + detail if detail else ""))


def free_port():
    for p in range(18080, 18100):
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", p))
            s.close()
            return p
        except OSError:
            continue
    raise RuntimeError("no free test port")


def build():
    r = subprocess.run(
        ["g++", "-std=c++17", "-O2", "-o", BIN] +
        [os.path.join(ROOT, "src", f) for f in os.listdir(os.path.join(ROOT, "src"))
         if f.endswith(".cpp")],
        capture_output=True, text=True)
    check("build binary", r.returncode == 0, r.stderr[:300])


def make_docroot(base):
    doc = os.path.join(base, "www")
    os.makedirs(os.path.join(doc, "sub"))
    files = {
        "index.html": b"<html><body><h1>fixture home</h1></body></html>",
        "style.css": b"body { color: red; }",
        "app.js": b"console.log(1);",
        "data.json": b'{"a": 1}',
        "note.txt": b"plain text here",
        "pic.png": b"\x89PNG\r\n\x1a\n" + b"0" * 100,
        os.path.join("sub", "inner.txt"): b"inside subdir",
    }
    for name, data in files.items():
        with open(os.path.join(doc, name), "wb") as f:
            f.write(data)
    return doc


def write_config(base, doc):
    path = os.path.join(base, "test.conf")
    with open(path, "w") as f:
        f.write("port = %d\n" % PORT)
        f.write("docroot = %s\n" % doc)
        f.write("dir_listing = on\n")
        f.write("log = %s\n" % os.path.join(base, "access.log"))
    return path


def wait_for_server():
    for _ in range(100):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=1)
            s.close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


def get(path, method="GET", body=None, headers=None):
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=5)
    c.request(method, path, body=body, headers=headers or {})
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, dict(r.getheaders()), data


def read_response(f):
    line = f.readline().decode("latin1")
    if not line:
        return None
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
        chunk = f.read(n - len(body))
        if not chunk:
            break
        body += chunk
    return line.strip(), headers, body


def raw(payload, responses=1):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    s.sendall(payload)
    f = s.makefile("rb")
    out = [read_response(f) for _ in range(responses)]
    s.close()
    return out


def run_tests(base):
    # static files and mime types
    st, h, b = get("/")
    check("GET / serves index.html",
          st == 200 and h.get("Content-Type") == "text/html"
          and b"fixture home" in b, "status=%s" % st)
    st, h, b = get("/style.css")
    check("css mime type", st == 200 and h.get("Content-Type") == "text/css")
    st, h, b = get("/app.js")
    check("js mime type", st == 200 and h.get("Content-Type") == "text/javascript")
    st, h, b = get("/data.json")
    check("json mime type",
          st == 200 and h.get("Content-Type") == "application/json"
          and json.loads(b) == {"a": 1})
    st, h, b = get("/note.txt")
    check("txt mime type", st == 200 and h.get("Content-Type") == "text/plain"
          and b == b"plain text here")
    st, h, b = get("/pic.png")
    check("png served as binary", st == 200
          and h.get("Content-Type") == "image/png"
          and b == b"\x89PNG\r\n\x1a\n" + b"0" * 100)

    # errors
    st, h, b = get("/nope.txt")
    check("missing file is 404", st == 404, "status=%s" % st)
    (line, _, _), = raw(b"GET /../secret.txt HTTP/1.1\r\n"
                        b"Host: x\r\nConnection: close\r\n\r\n")
    check("path traversal blocked", line.startswith("HTTP/1.1 403"), line)
    (line, _, _), = raw(b"GARBAGE\r\n\r\n")
    check("malformed request is 400", line.startswith("HTTP/1.1 400"), line)

    # keep-alive: two requests on one raw socket
    req = (b"GET /hello HTTP/1.1\r\nHost: x\r\n"
           b"Connection: keep-alive\r\n\r\n")
    r1, r2 = raw(req + req, responses=2)
    check("keep-alive serves two requests on one socket",
          r1[0].startswith("HTTP/1.1 200") and r2[0].startswith("HTTP/1.1 200")
          and r1[2] == b"Hello from servex\n" and r2[2] == b"Hello from servex\n",
          "%s / %s" % (r1[0], r2[0]))
    check("keep-alive response advertises reuse",
          r1[1].get("connection") == "keep-alive", str(r1[1].get("connection")))

    # directory listing
    st, h, b = get("/sub/")
    check("directory listing shows files",
          st == 200 and b"inner.txt" in b, "status=%s" % st)
    st, h, b = get("/sub")
    check("bare directory redirects with trailing slash",
          st == 301 and h.get("Location") == "/sub/", "status=%s" % st)

    # demo routes
    st, h, b = get("/hello")
    check("/hello returns plain text",
          st == 200 and h.get("Content-Type") == "text/plain"
          and b == b"Hello from servex\n")
    st, h, b = get("/echo?foo=bar", method="POST", body=b"hello-body",
                   headers={"Content-Type": "text/plain"})
    check("/echo reflects the request",
          st == 200 and b"method: POST" in b and b"query: foo=bar" in b
          and b"hello-body" in b)
    st, h, b = get("/status")
    try:
        info = json.loads(b)
        ok = (st == 200 and h.get("Content-Type") == "application/json"
              and info["server"] == "servex"
              and isinstance(info["uptime_seconds"], int)
              and info["requests"] >= 1)
    except (ValueError, KeyError):
        ok = False
    check("/status returns json with uptime and request count", ok, b[:120])

    # access log
    time.sleep(0.2)
    log_path = os.path.join(base, "access.log")
    logged = open(log_path).read() if os.path.exists(log_path) else ""
    check("access log written",
          'GET /status' in logged and ' 200 ' in logged,
          "log tail: %s" % logged.strip().split("\n")[-1:])


def main():
    global PORT
    PORT = free_port()
    build()
    if FAIL:
        sys.exit(1)
    base = tempfile.mkdtemp(prefix="servex-test-")
    proc = None
    try:
        doc = make_docroot(base)
        conf = write_config(base, doc)
        proc = subprocess.Popen([BIN, conf], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        if not wait_for_server():
            check("server starts", False, "port %d never opened" % PORT)
            sys.exit(1)
        run_tests(base)
    finally:
        if proc:
            proc.terminate()
            proc.wait(timeout=5)
        shutil.rmtree(base, ignore_errors=True)
    print("\n%d passed, %d failed" % (PASS, FAIL))
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()
