#!/usr/bin/env python3
# thread pool, live stats, and the admin json api.
import sys
import os
import tempfile
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from helpers import *  # noqa


def main():
    if not build():
        return summary()
    base = tempfile.mkdtemp(prefix="servex-test-")
    proc = None
    try:
        make_docroot(base)
        conf = CONF_BASIC + "workers = 4\n"
        proc, base, port = start_server(conf, base=base)

        st, h, b = get(port, "/status")
        try:
            info = json.loads(b)
            ok = (st == 200 and info["server"] == "servex"
                  and isinstance(info["uptime_seconds"], int)
                  and info["requests"] >= 1
                  and info["requests_total"] == info["requests"]
                  and info["workers"] == 4
                  and isinstance(info["queue_depth"], int)
                  and isinstance(info["active_connections"], int)
                  and isinstance(info["routes"], dict))
        except (ValueError, KeyError):
            ok = False
        check("/status has the full admin json shape", ok, b[:200])

        before = info["requests_total"]
        get(port, "/hello")
        get(port, "/hello")
        get(port, "/note.txt")
        st, h, b = get(port, "/status")
        info = json.loads(b)
        check("request counter increments",
              info["requests_total"] == before + 4,  # 3 + this /status
              "before=%d after=%d" % (before, info["requests_total"]))
        check("per-route hits tracked",
              info["routes"].get("/hello", 0) >= 2
              and info["routes"].get("static", 0) >= 1,
              str(info["routes"]))
        check("byte counters move",
              info["bytes_in"] > 0 and info["bytes_out"] > 0,
              "in=%s out=%s" % (info["bytes_in"], info["bytes_out"]))

        # hold three keep-alive connections open, then check the gauge
        held = []
        for _ in range(3):
            s = socket.create_connection(("127.0.0.1", port), timeout=10)
            s.sendall(b"GET /hello HTTP/1.1\r\nHost: x\r\n"
                      b"Connection: keep-alive\r\n\r\n")
            f = s.makefile("rb")
            line, _, _ = read_response(f)
            assert line.startswith("HTTP/1.1 200"), line
            held.append((s, f))
        st, h, b = get(port, "/status")
        info = json.loads(b)
        check("active_connections counts open sockets",
              info["active_connections"] >= 4,  # 3 held + this one
              "active=%s" % info["active_connections"])
        for s, f in held:
            f.close()
            s.close()
        time.sleep(0.5)
        st, h, b = get(port, "/status")
        info = json.loads(b)
        check("active_connections drops after close",
              info["active_connections"] <= 2,
              "active=%s" % info["active_connections"])

        # many parallel clients all get served
        errors = []
        results = []

        def hammer():
            try:
                st, _, b = get(port, "/hello")
                results.append(st == 200 and b == b"Hello from servex\n")
            except Exception as e:  # noqa
                errors.append(str(e))

        threads = [threading.Thread(target=hammer) for _ in range(24)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=20)
        check("24 parallel requests all succeed",
              len(results) == 24 and all(results) and not errors,
              "ok=%d errors=%s" % (len(results), errors[:2]))

        # sigterm stops the server promptly (graceful shutdown)
        import signal
        t0 = time.time()
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
            stopped = time.time() - t0 < 5
        except Exception:
            stopped = False
        check("sigterm shuts the server down cleanly", stopped)
        proc = None
    finally:
        if proc:
            stop_server(proc, base)
        else:
            shutil.rmtree(base, ignore_errors=True)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
