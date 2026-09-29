# servex

a http/1.1 server written from scratch in c++. no frameworks, no dependencies, just posix sockets and about 3500 lines of code. linux only (it uses `MSG_NOSIGNAL` and a few other linux-isms).

it parses requests, routes them, serves static files, proxies to upstreams, runs cgi scripts, speaks websocket, and logs everything. good enough to poke at with curl, not something you would put on the real internet.

## build

```
make
```

which is just:

```
g++ -std=c++17 -O2 -Wall -o servex src/*.cpp
```

## run

```
./servex                 # uses servex.conf in the current directory
./servex my.conf         # use a different config file
./servex --port 9000     # override the port from the config
```

## config

`servex.conf` is `key = value` lines, `#` starts a comment. there are two styles. the flat style is just global keys:

```
port = 8080
workers = 32
docroot = ./www
log = ./access.log
```

the block style adds `server {}` blocks with virtual hosts and `location` prefixes:

```
log_max_bytes = 10485760
log_archives = 5

server {
    host = example.com
    docroot = ./www
    dir_listing = on
    rate = 100
    burst = 200

    location /api/ {
        proxy_pass = 127.0.0.1:3000
    }
    location /cgi-bin/ {
        cgi = on
    }
    location /ws/ {
        websocket = on
    }
    location /private/ {
        auth_realm = private area
        auth_file = ./private.htpasswd
    }
}
```

global keys: `port`, `workers`, `log`, `log_max_bytes` (rotate the access log past this size, 0 = off), `log_archives` (numbered archives kept), `include` (glob of extra config files), `set` (variables expanded as `${name}`).

per server: `host` (exact or `*.example.com` wildcard, first match wins), `docroot`, `dir_listing`, `log` (per-host access log), `rate`/`burst` (token bucket per client ip).

per location: `proxy_pass` (reverse proxy to `host:port` with pooled keep-alive upstream connections), `cgi` (run executable files as cgi/1.1 scripts), `websocket` (allow rfc 6455 upgrades), `auth_realm`/`auth_file` (basic auth, plain or `{SHA}` htpasswd entries), `rewrite_pattern`/`rewrite_replacement` (regex internal rewrite), `redirect_to`/`redirect_code` (301/302 keeping the query string), `add_headers` (custom response headers), `rate`/`burst` (per-location token bucket).

## what it does

http core: keep-alive, pipelining, chunked request bodies (with extensions and trailers), chunked responses, `Expect: 100-continue`, HEAD, OPTIONS (including `OPTIONS *`), absolute-form targets. conflicting or duplicate content-lengths are rejected, and content-length plus transfer-encoding together is rejected.

static files: mime types by extension, etags, `If-Modified-Since` / `If-None-Match` / `If-Match` / `If-Unmodified-Since`, single and multipart byte ranges with `If-Range`, directory index files and listings, `..` escapes blocked.

middleware: basic auth, regex rewrites, redirects, custom headers per location.

rate limiting: token buckets per client ip, server-wide and per location, 429 with `Retry-After`.

reverse proxy: pooled persistent upstream connections, `X-Forwarded-For` / `X-Real-IP`, hop-by-hop headers stripped, chunked upstream responses re-streamed as chunked, 502 when the upstream is down.

cgi: fork/exec with the full cgi/1.1 environment, `PATH_INFO` splitting, stdin from the request body, response headers parsed (`Status`, `Location`, `Content-Type`), output streamed back chunked, scripts killed with 504 after 5 seconds.

websocket: rfc 6455 handshake (`Sec-WebSocket-Accept` verified in tests), text/binary echo, ping/pong, fragmented message reassembly, close handshake, unmasked client frames rejected.

logging: apache combined-ish access logs to stdout or a file, per-host logs, size-based rotation keeping N numbered archives.

`/status` returns json with uptime, request/byte counters, active connections, queue depth, worker count, per-route hits, and counters for 429s, upstream errors, cgi runs, and websocket connections/messages.

## routes

exact and prefix routes registered in code (`src/demo.cpp`):

- `/hello` - plain text greeting
- `/echo` - reflects the method, path, query, headers, and body back at you
- `/status` - the json described above

anything else falls through to the static file server.

## tests

```
make test
```

every suite builds the binary itself, starts it on a throwaway port with a fixture docroot, and talks to it over real localhost sockets. stdlib only, no test frameworks. currently 11 suites covering the original 18 checks plus http core, conditional/range, admin/stats, virtual hosts, middleware, rate limiting, proxy, cgi, websocket, and log rotation.

## what is NOT implemented

- no tls. plain http only, use a reverse proxy if you need https.
- no gzip.
- no request timeouts beyond the cgi kill timer and websocket idle timeout.
- the config file has no validation beyond "bad value, keep the default".
