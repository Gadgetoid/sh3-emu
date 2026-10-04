#!/bin/sh
set -e
ROM=${1:-rom/odo-sh3.bin}
OUT=${TMPDIR:-/tmp}/sh3-gui
DISPLAY_NUMBER=${GUI_DISPLAY:-:9}
mkdir -p "$OUT/data" "$OUT/config"
/usr/lib/xorg/Xorg "$DISPLAY_NUMBER" -config "$(pwd)/tests/gui/dummy.conf" -configdir /nonexistent -noreset -nolisten tcp -logfile "$OUT/xorg.log" > /dev/null 2>&1 &
SERVER=$!
sleep 3
DISPLAY=$DISPLAY_NUMBER XDG_DATA_HOME="$OUT/data" XDG_CONFIG_HOME="$OUT/config" ./velo --fresh "$ROM" > "$OUT/app.log" 2>&1 &
APP=$!
trap 'kill $APP $SERVER 2>/dev/null || true' EXIT
DISPLAY=$DISPLAY_NUMBER python3 tests/gui/drive.py "$OUT/console.png"
echo "screenshot: $OUT/console.png"
