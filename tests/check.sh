#!/bin/sh
set -e
OUT=${TMPDIR:-/tmp}/sh3-check
mkdir -p "$OUT"
expect_fail() {
    name=$1; pattern=$2; shift 2
    if "$@" > "$OUT/out.txt" 2>&1; then echo "FAIL $name (succeeded)"; exit 1; fi
    if grep -q -- "$pattern" "$OUT/out.txt"; then echo "ok   $name"; else echo "FAIL $name"; cat "$OUT/out.txt"; exit 1; fi
}
TOOLS=headless
[ -x ./sh3emu ] && TOOLS="$TOOLS sh3emu"
for tool in $TOOLS; do
    if ./$tool --help | grep -q "^usage: $tool" && ./$tool --version | grep -q "^$tool "; then echo "ok   ${tool}_help"; else echo "FAIL ${tool}_help"; exit 1; fi
done
expect_fail headless_no_rom "no ROM given" ./headless
expect_fail headless_unknown "unknown option --tpa" ./headless rom.bin --tpa=1:2:3
expect_fail headless_bad_tap "wants SECONDS:X:Y" ./headless rom.bin --tap=21:108
expect_fail headless_joined_taps "wants SECONDS:X:Y" ./headless rom.bin "--tap=4:240:120 --tap=6:48:24"
expect_fail headless_bad_key "wants SECONDS:SCANCODE" ./headless rom.bin --key=1:11+0D+12+14+59
expect_fail headless_not_image "not a Windows CE ROM image" ./headless Makefile --seconds=1
expect_fail headless_bad_agent "cannot listen on agent socket" ./headless Makefile --seconds=1 --agent=/nonexistent/folder/agent.sock
if [ -x ./sh3emu ]; then
    expect_fail sh3emu_unknown "unknown option --frob" ./sh3emu --frob
    expect_fail sh3emu_bad_memory "--memory" ./sh3emu --memory=4
fi
expect_fail sh3_run_no_program "usage: sh3-run" ./sh3-run
if pkg-config --exists slirp libcurl; then
    PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
    python3 -m http.server "$PORT" --bind 127.0.0.1 --directory tests/web >/dev/null 2>&1 &
    SERVER=$!
    trap 'kill $SERVER 2>/dev/null || true' EXIT
    for attempt in 1 2 3 4 5 6 7 8 9 10; do python3 -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:$PORT/page.html', timeout=1)" 2>/dev/null && break; sleep 1; done
    REWRITE='import re, sys; page = sys.stdin.buffer.read().decode("cp1252"); sys.exit(0 if "Caf\u00e9 \u201cquoted\u201d \u2014 dash</p><a href=\"http://example.com/next\">next</a></body>" in page and not re.search("script|style|svg|hidden", page, re.I) else 1)'
    for attempt in 1 2; do
        ./proxycheck "http://127.0.0.1:$PORT/page.html" > "$OUT/page.txt" 2> "$OUT/page.log" || true
        grep -q "^HTTP/1.0 502" "$OUT/page.txt" || break
    done
    if python3 -c "$REWRITE" < "$OUT/page.txt"; then echo "ok   proxy_rewrite"; else echo "FAIL proxy_rewrite"; cat "$OUT/page.log" "$OUT/page.txt"; exit 1; fi
    MOVED=$(./proxycheck "http://127.0.0.1:$PORT/folder" 2>/dev/null)
    if echo "$MOVED" | grep -q "^HTTP/1.0 301" && echo "$MOVED" | grep -q "^Content-Type: text/html" && echo "$MOVED" | grep -q "folder/\">here</A>"; then echo "ok   proxy_redirect"; else echo "FAIL proxy_redirect"; echo "$MOVED"; exit 1; fi
    XHTML=$(./proxycheck "http://127.0.0.1:$PORT/page.xhtml" 2>/dev/null)
    if echo "$XHTML" | grep -q "^Content-Type: text/html" && echo "$XHTML" | grep -q "xhtml page" && ! echo "$XHTML" | grep -qi "<meta"; then echo "ok   proxy_xhtml"; else echo "FAIL proxy_xhtml"; echo "$XHTML"; exit 1; fi
    BODY='import sys, struct, hashlib; head, body = sys.stdin.buffer.read().split(b"\r\n\r\n", 1); print(body[:6].decode("latin-1"), *struct.unpack("<HH", body[6:10]), hashlib.sha256(body).hexdigest())'
    SVG=$(./proxycheck "http://127.0.0.1:$PORT/images.html" "http://127.0.0.1:$PORT/shapes.svg" 2>/dev/null | python3 -c "$BODY")
    PNG=$(./proxycheck "http://127.0.0.1:$PORT/gradient.png" 2>/dev/null | python3 -c "$BODY")
    GZIP='import gzip, sys; head, body = sys.stdin.buffer.read().split(b"\r\n\r\n", 1); sys.exit(0 if b"Content-Encoding: gzip" in head and gzip.decompress(body) == open("tests/web/data.json", "rb").read() else 1)'
    PLAIN='import sys; head, body = sys.stdin.buffer.read().split(b"\r\n\r\n", 1); sys.exit(0 if b"Content-Encoding" not in head and body == open("tests/web/data.json", "rb").read() else 1)'
    if ./proxycheck --header="Accept-Encoding: gzip, deflate" "http://127.0.0.1:$PORT/data.json" 2>/dev/null | python3 -c "$GZIP" &&
       ./proxycheck "http://127.0.0.1:$PORT/data.json" 2>/dev/null | python3 -c "$PLAIN"; then echo "ok   proxy_gzip"; else echo "FAIL proxy_gzip"; exit 1; fi
    if [ "$SVG" = "GIF89a 100 50 4952502eef863d6cd28acd031a890c4d93b43269bebd712c05318955547ac371" ] && [ "${PNG% *}" = "GIF89a 436 218" ]; then echo "ok   proxy_images"; else echo "FAIL proxy_images $SVG / $PNG"; exit 1; fi
fi
