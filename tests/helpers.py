#!/usr/bin/env python3
# shared helpers for the servex integration test files.
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
SRC = os.path.join(ROOT, "src")

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
    for _ in range(200):
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", 0))
            p = s.getsockname()[1]
            s.close()
            return p
        except OSError:
            continue
    raise RuntimeError("no free test port")


def build():
    files = [os.path.join(SRC, f) for f in os.listdir(SRC)
             if f.endswith(".cpp")]
    r = subprocess.run(["g++", "-std=c++17", "-O2", "-o", BIN] + files,
                       capture_output=True, text=True)
    check("build binary", r.returncode == 0, r.stderr[:300])
    return r.returncode == 0


def make_docroot(base, extra=None):
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
    if extra:
        files.update(extra)
    for name, data in files.items():
        p = os.path.join(doc, name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
    return doc


def start_server(conf_text, port=None, base=None):
    port = port or free_port()
    if base is None:
        base = tempfile.mkdtemp(prefix="servex-test-")
    conf_path = os.path.join(base, "test.conf")
    with open(conf_path, "w") as f:
        f.write(conf_text.replace("@PORT@", str(port))
                           .replace("@BASE@", base))
    proc = subprocess.Popen([BIN, conf_path],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    ok = wait_for_server(port)
    if not ok:
        proc.terminate()
        raise RuntimeError("server never opened port %d" % port)
    return proc, base, port


def wait_for_server(port):
    for _ in range(100):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


def stop_server(proc, base):
    try:
        proc.terminate()
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
    shutil.rmtree(base, ignore_errors=True)


def get(port, path, method="GET", body=None, headers=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    c.request(method, path, body=body, headers=headers or {})
    r = c.getresponse()
    data = r.read()
    hdrs = dict((k.lower(), v) for k, v in r.getheaders())
    status = r.status
    c.close()
    return status, hdrs, data


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
    if headers.get("transfer-encoding") == "chunked":
        while True:
            size_line = f.readline().decode("latin1")
            n = int(size_line.split(";")[0].strip(), 16)
            if n == 0:
                f.readline()
                break
            chunk = b""
            while len(chunk) < n:
                piece = f.read(n - len(chunk))
                if not piece:
                    break
                chunk += piece
            body += chunk
            f.readline()
    else:
        n = int(headers.get("content-length", "0"))
        while len(body) < n:
            chunk = f.read(n - len(body))
            if not chunk:
                break
            body += chunk
    return line.strip(), headers, body


def raw(port, payload, responses=1, timeout=10):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.sendall(payload)
    f = s.makefile("rb")
    out = [read_response(f) for _ in range(responses)]
    s.close()
    return out


def summary():
    print("\n%d passed, %d failed" % (PASS, FAIL))
    return 1 if FAIL else 0


CONF_BASIC = """\
port = @PORT@
docroot = @BASE@/www
dir_listing = on
log = @BASE@/access.log
"""
