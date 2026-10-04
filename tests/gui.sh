#!/bin/sh
set -e
ROM=${1:-rom/odo-sh3-ce212.bin}
PROGRAM=${PPFS_PROGRAM:-rom/mbtest.exe}
OUT=${TMPDIR:-/tmp}/sh3-gui
DISPLAY_NUMBER=${GUI_DISPLAY:-:9}
rm -rf "$OUT/data" "$OUT/config"
mkdir -p "$OUT/data" "$OUT/config/sh3-emu" "$OUT/folder"
NAME=
if [ -f "$PROGRAM" ]; then
    cp "$PROGRAM" "$OUT/folder/"
    NAME=$(basename "$PROGRAM" .exe)
    echo "host_folder=$OUT/folder" > "$OUT/config/sh3-emu/sh3emu.ini"
fi
/usr/lib/xorg/Xorg "$DISPLAY_NUMBER" -config "$(pwd)/tests/gui/dummy.conf" -configdir /nonexistent -noreset -nolisten tcp -logfile "$OUT/xorg.log" > /dev/null 2>&1 &
SERVER=$!
sleep 3
DISPLAY=$DISPLAY_NUMBER XDG_DATA_HOME="$OUT/data" XDG_CONFIG_HOME="$OUT/config" ./sh3emu --fresh "$ROM" > "$OUT/app.log" 2>&1 &
APP=$!
trap 'kill $APP $SERVER 2>/dev/null || true' EXIT
DISPLAY=$DISPLAY_NUMBER python3 tests/gui/drive.py "$OUT/console.png" $NAME
kill $APP
wait $APP 2>/dev/null || true
if [ -d "$OUT/data/sh3-emu" ] && [ -z "$(find "$OUT/data" "$OUT/config" -name "velo-emu*")" ]; then echo "ok   gui_folders"; else echo "FAIL gui_folders"; exit 1; fi
if [ -z "$NAME" ]; then echo "skip gui_ppfs: no $PROGRAM"
elif grep -q "$NAME: version" "$OUT/data/sh3-emu/debug.log"; then echo "ok   gui_ppfs"
else echo "FAIL gui_ppfs"; exit 1; fi
echo "screenshot: $OUT/console.png"
