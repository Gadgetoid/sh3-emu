#!/bin/sh
set -e
ROM=${1:-rom/nk.bin}
OUT=${TMPDIR:-/tmp}/velo-test
mkdir -p "$OUT"
check() {
    name=$1; expected=$2; shift 2
    ./headless "$ROM" --pgm="$OUT/$name.pgm" "$@" 2>/dev/null
    actual=$(shasum -a 256 "$OUT/$name.pgm" | cut -d' ' -f1)
    if [ -z "$expected" ]; then echo "$name $actual"; return; fi
    if [ "$actual" = "$expected" ]; then echo "ok   $name"; else echo "FAIL $name $actual"; exit 1; fi
}
CALIBRATE="--key=8:4B --key=10:4B --tap=12:240:120 --tap=14:96:48 --tap=16:96:192 --tap=18:384:192 --tap=20:384:48 --key=23:4B"
WIZARD="--tap=26:452:227 --tap=29:452:227 --tap=32:452:227 --tap=35:452:227 --tap=38:452:227 --tap=41:452:227"
check wizard   3a4cc79486ecf4d3329ab251349f2cfecaca43246e2f5c197eb7fc99e60d5d28   --seconds=8
check desktop  2eb469a6f1f8a86900314a036e30b0e725df989e4adf0962a797091ebff39033  --seconds=44 $CALIBRATE $WIZARD
./headless "$ROM" --seconds=44 --save="$OUT/desktop.state" $CALIBRATE $WIZARD 2>/dev/null
check resumed 2eb469a6f1f8a86900314a036e30b0e725df989e4adf0962a797091ebff39033 --seconds=1 --load="$OUT/desktop.state"
./headless "$ROM" --pgm="$OUT/start_continuous.pgm" --seconds=47 $CALIBRATE $WIZARD --tap=45:15:227 2>/dev/null
./headless "$ROM" --pgm="$OUT/start_resumed.pgm" --seconds=3 --load="$OUT/desktop.state" --tap=1:15:227 2>/dev/null
if cmp -s "$OUT/start_continuous.pgm" "$OUT/start_resumed.pgm"; then echo "ok   resume matches continuous run"; else echo "FAIL resume differs from continuous run"; exit 1; fi
check suspend_resume 2eb469a6f1f8a86900314a036e30b0e725df989e4adf0962a797091ebff39033 --seconds=10 --load="$OUT/desktop.state" --power=2 --power=6
sh tools/mkcard.sh "$OUT/card.img" 8 tests/card/HELLO
CARD="--card=$OUT/card.img --tap=4:30:25:0.08 --tap=4.12:30:25:0.08 --tap=7:262:65:0.08 --tap=7.12:262:65:0.08"
check card_listing b92dd3e0c4dbf3472d5ebc40937767f061a626fca3209eed6682393c77530786 --seconds=10 --load="$OUT/desktop.state" $CARD
if pkg-config --exists slirp; then
    if ./headless "$ROM" --seconds=6 --load="$OUT/desktop.state" --net=1 2>&1 | grep -q "IPCP up"; then echo "ok   ppp_online"; else echo "FAIL ppp_online"; exit 1; fi
fi
if ./headless "$ROM" --seconds=4 --load="$OUT/desktop.state" --backlight=2 --trace-pc 2>&1 | grep "^t=" | tail -1 | grep -q "backlight=1"; then echo "ok   backlight_button"; else echo "FAIL backlight_button"; exit 1; fi
