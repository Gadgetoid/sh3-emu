#!/bin/sh
set -e
ROM=${1:-rom/odo-sh3-ce212.bin}
OUT=${TMPDIR:-/tmp}/sh3-boot
DESKTOP_HASH=f6b35a6e2df61df5b996cd2e8b250bd05f8cd3e968e5ac7b3f0b3712934d8831
START_HASH=d4a4209f465f1cd22a0fded3cd266a3da23df0b171e0515e28325012d74c884f
CONSOLE_HASH=586d21601c84321cb4fbb33a9c8898b49bcece36f1d6fb756e8f99a8d66e5b7d
mkdir -p "$OUT"
check() {
    name=$1; expected=$2; shift 2
    ./headless "$ROM" --pgm="$OUT/$name.pgm" "$@" 2>/dev/null
    actual=$(shasum -a 256 "$OUT/$name.pgm" | cut -d' ' -f1)
    if [ -z "$expected" ]; then echo "$name $actual"; return; fi
    if [ "$actual" = "$expected" ]; then echo "ok   $name"; else echo "FAIL $name $actual"; exit 1; fi
}
run_at() {
    printf -- '--key=%s:14+76 --type=%s:r --type=%s:%s\\n' "$1" $(($1 + 1)) $(($1 + 2)) "$2"
}
if ./headless "$ROM" --debug-output --seconds=1 2>&1 | grep -q "^debug: Windows CE Kernel for Hitachi SH"; then echo "ok   debug_output"; else echo "FAIL debug_output"; exit 1; fi
check desktop "$DESKTOP_HASH" --seconds=25
./headless "$ROM" --seconds=25 --save="$OUT/desktop.state" 2>/dev/null
check resumed "$DESKTOP_HASH" --seconds=1 --load="$OUT/desktop.state"
check start_menu "$START_HASH" --seconds=3 --load="$OUT/desktop.state" --tap=1:30:226:0.2
check console "$CONSOLE_HASH" --seconds=10 --load="$OUT/desktop.state" $(run_at 1 cmd) "--type=7:dir\n"
PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
./headless "$ROM" --load="$OUT/desktop.state" --gdb="$PORT" > "$OUT/gdb.log" 2>&1 &
STUB=$!
if python3 tests/gdb_client.py "$PORT"; then echo "ok   gdb"; else echo "FAIL gdb"; kill $STUB; cat "$OUT/gdb.log"; exit 1; fi
wait $STUB
card_read() {
    if [ "$(uname -s)" = Darwin ]; then
        mount=$(hdiutil attach -readonly -imagekey diskimage-class=CRawDiskImage "$1" | awk '/SH3CARD/ {sub(/^.*\t/, ""); print}')
        cat "$mount/$2"
        hdiutil detach -quiet "$mount"
    else
        mtype -i "$1@@512" "::$2"
    fi
}
rm -rf "$OUT/cardsrc" && mkdir -p "$OUT/cardsrc/data" && echo "written on the host, copied by CE" > "$OUT/cardsrc/data/hello.txt"
sh tools/mkcard.sh "$OUT/card.img" 16 "$OUT/cardsrc/data"
./headless "$ROM" --load="$OUT/desktop.state" --card="$OUT/card.img" --seconds=16 $(run_at 2 cmd) \
    '--type=7:md "\Storage Card\fromce"\n' '--type=10:copy "\Storage Card\data\hello.txt" "\Storage Card\fromce\copy.txt"\n' > /dev/null 2>&1
if [ "$(card_read "$OUT/card.img" fromce/copy.txt)" = "written on the host, copied by CE" ]; then echo "ok   card"; else echo "FAIL card"; exit 1; fi
./headless "$ROM" --load="$OUT/desktop.state" --seconds=8 $(run_at 1 velo-debugmgr) --save="$OUT/debugmgr.state" > /dev/null 2>&1
PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
./headless "$ROM" --load="$OUT/debugmgr.state" --gdb="$PORT" > "$OUT/gdb_run.log" 2>&1 &
STUB=$!
if python3 tests/gdb_client.py "$PORT" run; then echo "ok   gdb_run"; else echo "FAIL gdb_run"; kill $STUB; cat "$OUT/gdb_run.log"; exit 1; fi
kill $STUB 2>/dev/null || true
PROGRAM=${PPFS_PROGRAM:-rom/mbtest.exe}
if [ -f "$PROGRAM" ]; then
    rm -rf "$OUT/folder" && mkdir -p "$OUT/folder" && cp "$PROGRAM" "$OUT/folder/"
    name=$(basename "$PROGRAM" .exe)
    if ./headless "$ROM" --load="$OUT/desktop.state" --folder="$OUT/folder" --debug-output --seconds=8 $(run_at 1 "$name") > "$OUT/ppfs.log" 2>&1 &&
        grep -q "^ppfs: opened .*$name.exe" "$OUT/ppfs.log" && grep -q "^debug: $name: " "$OUT/ppfs.log"; then echo "ok   ppfs"; else echo "FAIL ppfs"; cat "$OUT/ppfs.log"; exit 1; fi
else
    echo "skip ppfs: no $PROGRAM (an SH3 program that isn't in ROM)"
fi
NET_PROGRAM=${NET_PROGRAM:-build/guest/nettest.exe}
if ./headless "$ROM" --net --seconds=0.01 2>&1 | grep -q libslirp; then
    echo "skip net: headless built without libslirp"
elif [ ! -f "$NET_PROGRAM" ]; then
    echo "skip net: no $NET_PROGRAM (guest/build.sh)"
else
    rm -rf "$OUT/netfolder" "$OUT/net.log" "$OUT/net.port" && mkdir -p "$OUT/netfolder" && cp "$NET_PROGRAM" "$OUT/netfolder/"
    TOKEN=token-$$
    python3 tests/net_server.py "$OUT/net.log" "$TOKEN" > "$OUT/net.port" &
    SERVER=$!
    for attempt in 1 2 3 4 5 6 7 8 9 10; do [ -s "$OUT/net.port" ] && break; sleep 0.5; done
    trap 'kill $SERVER 2>/dev/null || true' EXIT
    ./headless "$ROM" --load="$OUT/desktop.state" --folder="$OUT/netfolder" --net --seconds=40 --key=5:14+76 --type=6:r "--type=7:nettest $(cat "$OUT/net.port")\n" > "$OUT/net.txt" 2>&1 || true
    kill $SERVER
    wait $SERVER 2>/dev/null || true
    trap - EXIT
    if [ "$(cat "$OUT/net.log" 2>/dev/null)" = "$(printf '/resolved/10.0.2.2\n/received/%s' "$TOKEN")" ]; then echo "ok   net"; else echo "FAIL net"; cat "$OUT/net.txt" "$OUT/net.log"; exit 1; fi
fi
rm -f "$OUT/agent.sock"
./headless "$ROM" --load="$OUT/debugmgr.state" --agent="$OUT/agent.sock" --seconds=120 --realtime=4 > "$OUT/agent.log" 2>&1 &
STUB=$!
if python3 tests/agent_reconnect.py "$OUT/agent.sock" 20 > "$OUT/reconnect.txt"; then echo "ok   agent_reconnect"; else echo "FAIL agent_reconnect"; kill $STUB; cat "$OUT/reconnect.txt"; exit 1; fi
kill $STUB 2>/dev/null || true
