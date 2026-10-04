#!/bin/sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
EMU=$(cd "$HERE/../.." && pwd)
usage() {
    echo "usage: PB212_TREE=DIR DEBUGMGR=EXE [SH3_APPS=DIR] [WINE=wine] [OUTPUT=FILE] [FULL=1] $0" >&2
    exit 2
}
[ -n "$PB212_TREE" ] && [ -d "$PB212_TREE/PLATFORM/ODO" ] || usage
[ -n "$DEBUGMGR" ] && [ -f "$DEBUGMGR" ] || usage
TREE=$(cd "$PB212_TREE" && pwd)
WINE=${WINE:-wine}
WINESERVER=${WINESERVER:-$(dirname "$(command -v "$WINE")")/wineserver}
WORK=${WORK:-$EMU/build/image}
OUTPUT=${OUTPUT:-$WORK/odo-sh3-ce212.bin}
LOG_LIMIT=${LOG_LIMIT:-200000000}
export WINEPREFIX=${WINEPREFIX:-$WORK/wineprefix} WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="

mkdir -p "$WORK/drive" "$(dirname "$OUTPUT")"
ln -sfn "$TREE" "$WORK/drive/TREE"
if [ ! -d "$WINEPREFIX/dosdevices" ]; then
    "$WINE" wineboot -i > "$WORK/wineboot.log" 2>&1
    "$WINESERVER" -w
fi
ln -sfn "$WORK/drive" "$WINEPREFIX/dosdevices/w:"

crlf() {
    sed 's/$/\r/' > "$1"
}
crlf "$WORK/drive/env.bat" <<'EOF'
@echo off
set PROCESSOR_ARCHITECTURE=x86
set USERNAME=sh3
set COPYCMD=/Y
set _WINCEROOT=W:\TREE
call W:\TREE\PUBLIC\COMMON\OAK\MISC\WINCE.BAT SHx SH3 CE MAXALL ODO
EOF
crlf "$WORK/drive/sysgen.bat" <<'EOF'
@echo off
call W:\env.bat
call blddemo
EOF
crlf "$WORK/drive/image.bat" <<'EOF'
@echo off
call W:\env.bat
cd /d W:\TREE\PLATFORM\ODO
build
if exist build.err type build.err
call buildrel.bat
makeimg
EOF

run() {
    log=$1
    "$WINE" cmd /c "$2" > "$log" 2>&1 < /dev/null &
    pid=$!
    while kill -0 $pid 2>/dev/null; do
        size=$(wc -c < "$log")
        if [ "$size" -gt "$LOG_LIMIT" ] || tail -c 20000 "$log" | grep -q 'Overwrite'; then
            echo "build: runaway log $log ($size bytes), stopping Wine" >&2
            "$WINESERVER" -k
            wait $pid 2>/dev/null || true
            exit 1
        fi
        sleep 5
    done
    wait $pid || true
    "$WINESERVER" -w
}

python3 "$HERE/prepare.py" "$TREE" "$DEBUGMGR" "$SH3_APPS"
if [ -n "$FULL" ] || [ ! -f "$TREE/PUBLIC/MAXALL/cesysgen/oak/target/SHx/SH3/CE/retail/coredll.dll" ]; then
    echo "build: blddemo (log $WORK/sysgen.log)"
    run "$WORK/sysgen.log" 'W:\sysgen.bat'
fi
rm -f "$TREE/release/NK.BIN"
echo "build: platform and makeimg (log $WORK/image.log)"
run "$WORK/image.log" 'W:\image.bat'
if [ ! -f "$TREE/release/NK.BIN" ] || grep -q -i 'error' "$TREE/PLATFORM/ODO/build.err" 2>/dev/null; then
    echo "build: no image; see $WORK/image.log" >&2
    exit 1
fi
cp "$TREE/release/NK.BIN" "$OUTPUT"
[ -f "$TREE/release/nknodbg.map" ] && cp "$TREE/release/nknodbg.map" "${OUTPUT%.bin}.nk.map"
python3 "$HERE/manifest.py" "$OUTPUT" "Windows CE 2.12 beta (Platform Builder 2.12 beta), MAXALL with the Explorer shell, Odo SH3, retail; built by tools/image/build.sh" > "${OUTPUT%.bin}.manifest.txt"
shasum -a 256 "$OUTPUT" | sed 's|  .*/|  |' > "$OUTPUT.sha256"
echo "build: $OUTPUT"
cat "$OUTPUT.sha256"
