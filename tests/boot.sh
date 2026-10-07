#!/bin/sh
set -e
OUT=${TMPDIR:-/tmp}/sh3-boot
mkdir -p "$OUT"
gdb_check() {
    name=$1; rom=$2
    ./headless "$rom" --seconds=20 --save="$OUT/$name.state" > /dev/null 2>&1
    PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
    ./headless "$rom" --load="$OUT/$name.state" --gdb="$PORT" > "$OUT/$name.log" 2>&1 &
    STUB=$!
    if python3 tests/gdb_client.py "$PORT"; then echo "ok   $name"; else echo "FAIL $name"; kill $STUB; cat "$OUT/$name.log"; exit 1; fi
    wait $STUB
}
rapi_check() {
    name=$1; rom=$2; net_at=$3
    socket=/tmp/sh3emu-rapi-$$.sock
    rm -f "$socket"
    echo "sent by sh3emu-rapi" > "$OUT/$name.txt"
    ./headless "$rom" --seconds=300 --net="$net_at" --rapi="$socket" --realtime=4 > "$OUT/$name.log" 2>&1 &
    EMULATOR=$!
    for attempt in $(seq 1 120); do grep -q "^desktop: connection" "$OUT/$name.log" && break; sleep 0.5; done
    sleep 2
    ./sh3emu-rapi --socket="$socket" put "$OUT/$name.txt" > /dev/null 2>&1 || true
    ./sh3emu-rapi --socket="$socket" get "$name.txt" "$OUT/$name.back" > /dev/null 2>&1 || true
    kill $EMULATOR 2>/dev/null || true
    wait $EMULATOR 2>/dev/null || true
    rm -f "$socket"
    if cmp -s "$OUT/$name.txt" "$OUT/$name.back"; then echo "ok   $name"; else echo "FAIL $name"; tail -5 "$OUT/$name.log"; exit 1; fi
}
reconnect_check() {
    name=$1; rom=$2; shift 2
    ./headless "$rom" "$@" > "$OUT/$name.log" 2>&1
    if [ "$(grep -c "^desktop: connection from the device" "$OUT/$name.log")" = 2 ]; then echo "ok   $name"; else echo "FAIL $name"; exit 1; fi
}
dictionary_check() {
    name=$1; rom=$2; dictionary=$3
    socket=/tmp/sh3emu-dic-$$.sock
    rm -f "$socket"
    ./headless "$rom" --dictionary="$dictionary" --seconds=60 --key=20:5A --key=23:5A --tap=25:240:120:1.5 --tap=28:48:48:1.5 \
        --tap=31:48:192:1.5 --tap=34:432:192:1.5 --tap=37:432:48:1.5 --key=40:5A --tap=44:447:227:0.2 --tap=46:447:227:0.2 \
        --tap=48:447:227:0.2 --tap=50:447:227:0.2 --tap=52:447:227:0.2 --tap=54:447:227:0.2 --tap=56:447:227:0.2 --tap=58:447:227:0.2 \
        --save="$OUT/$name.state" > /dev/null 2>&1
    ./headless "$rom" --dictionary="$dictionary" --load="$OUT/$name.state" --seconds=40 --net=2 --rapi="$socket" --realtime=4 \
        --save="$OUT/$name.running.state" > "$OUT/$name.log" 2>&1 &
    EMULATOR=$!
    for attempt in $(seq 1 120); do grep -q "^desktop: connection" "$OUT/$name.log" && break; sleep 0.5; done
    ./sh3emu-rapi --socket="$socket" run '\Windows\dic.exe' > /dev/null 2>&1 || true
    wait $EMULATOR 2>/dev/null || true
    rm -f "$socket"
    ./headless "$rom" --dictionary="$dictionary" --load="$OUT/$name.running.state" --seconds=3 --pgm="$OUT/$name.pgm" > /dev/null 2>&1
    actual=$(shasum -a 256 "$OUT/$name.pgm" | cut -d' ' -f1)
    if [ "$actual" = "$CASIO_DICTIONARY_HASH" ]; then echo "ok   $name"; else echo "FAIL $name $actual"; exit 1; fi
}
CASIO_ROM=${CASIO_ROM:-rom/nk-a51-ce1.01.bin}
CASIO_DICTIONARY=${CASIO_DICTIONARY:-rom/a51-dictionary.bin}
CASIO_DICTIONARY_HASH=9ad67910268497c4982d6fcc8cffc85e91142a2f6cae14228ac49adf0b5e71e0
CASIO_HASH=6122ca760147dfd2806e4b32bb8df2c7b0d973b99a4d724c5e50a4fad94851b2
CASIO_ENTER_HASH=cb4d8287eeb33e024c267b33a87ccf1acc1d2263f76d10996fc178ec45d60781
CASIO_TOUCH_HASH=29217d26a613e7975b69d46a1290d374c8c1981f63df873c1bb5a502277413d8
CASIO_CARD_HASH=ac572c440c42daa5c55bc679024626b640cd6fc7eeecfb165a38bd9e81931544
if [ -f "$CASIO_ROM" ]; then
    ./headless "$CASIO_ROM" --debug-output --seconds=20 --pgm="$OUT/casio.pgm" > "$OUT/casio.log" 2>&1
    actual=$(shasum -a 256 "$OUT/casio.pgm" | cut -d' ' -f1)
    if grep -q "^debug: Windows CE Kernel for Hitachi SH" "$OUT/casio.log" && [ "$actual" = "$CASIO_HASH" ]; then echo "ok   casio"; else echo "FAIL casio $actual"; exit 1; fi
    ./headless "$CASIO_ROM" --seconds=24 --key=20:5A --pgm="$OUT/casio_enter.pgm" > /dev/null 2>&1
    actual=$(shasum -a 256 "$OUT/casio_enter.pgm" | cut -d' ' -f1)
    if [ "$actual" = "$CASIO_ENTER_HASH" ]; then echo "ok   casio_enter"; else echo "FAIL casio_enter $actual"; exit 1; fi
    ./headless "$CASIO_ROM" --seconds=47 --key=20:5A --key=23:5A --tap=25:240:120:1.5 --tap=28:48:48:1.5 --tap=31:48:192:1.5 \
        --tap=34:432:192:1.5 --tap=37:432:48:1.5 --key=40:5A --tap=44:447:227:0.2 --pgm="$OUT/casio_touch.pgm" > /dev/null 2>&1
    actual=$(shasum -a 256 "$OUT/casio_touch.pgm" | cut -d' ' -f1)
    if [ "$actual" = "$CASIO_TOUCH_HASH" ]; then echo "ok   casio_touch"; else echo "FAIL casio_touch $actual"; exit 1; fi
    rm -rf "$OUT/casiocard" && mkdir -p "$OUT/casiocard/cardfiles" && echo "hello from the host" > "$OUT/casiocard/cardfiles/HELLO.TXT"
    sh tools/mkcard.sh "$OUT/casio.img" 16 "$OUT/casiocard/cardfiles" > /dev/null
    ./headless "$CASIO_ROM" --card="$OUT/casio.img" --seconds=68 --key=20:5A --key=23:5A --tap=25:240:120:1.5 --tap=28:48:48:1.5 \
        --tap=31:48:192:1.5 --tap=34:432:192:1.5 --tap=37:432:48:1.5 --key=40:5A --tap=44:447:227:0.2 --tap=46:447:227:0.2 \
        --tap=48:447:227:0.2 --tap=50:447:227:0.2 --tap=52:447:227:0.2 --tap=54:447:227:0.2 --tap=56:447:227:0.2 --tap=58:447:227:0.2 \
        --tap=61:40:30:0.08 --tap=61.2:40:30:0.08 --tap=64:334:68:0.08 --tap=64.2:334:68:0.08 --pgm="$OUT/casio_card.pgm" > /dev/null 2>&1
    actual=$(shasum -a 256 "$OUT/casio_card.pgm" | cut -d' ' -f1)
    if [ "$actual" = "$CASIO_CARD_HASH" ]; then echo "ok   casio_card"; else echo "FAIL casio_card $actual"; exit 1; fi
    if ./headless "$CASIO_ROM" --seconds=22 --backlight=20 --trace-pc 2>&1 | grep "^t=" | tail -1 | grep -q "backlight=1"; then echo "ok   casio_backlight"; else echo "FAIL casio_backlight"; exit 1; fi
    gdb_check casio_gdb "$CASIO_ROM"
    if ./headless "$CASIO_ROM" --net --seconds=0.01 2>&1 | grep -q libslirp; then
        echo "skip casio_net: headless built without libslirp"
    else
        ./headless "$CASIO_ROM" --seconds=50 --net=22 > "$OUT/casio_net.log" 2>&1
        if grep -q "^ppp: IPCP up" "$OUT/casio_net.log" && grep -q "^desktop: connection from the device" "$OUT/casio_net.log"; then echo "ok   casio_net"; else echo "FAIL casio_net"; exit 1; fi
        reconnect_check casio_replug "$CASIO_ROM" --seconds=60 --net=22 --replug=37
        reconnect_check casio_soft_reset "$CASIO_ROM" --seconds=110 --net=22 --soft-reset=60 --replug=88
        rapi_check casio_rapi "$CASIO_ROM" 22
        if [ -f "$CASIO_DICTIONARY" ]; then dictionary_check casio_dictionary "$CASIO_ROM" "$CASIO_DICTIONARY"; else echo "skip casio_dictionary: no $CASIO_DICTIONARY"; fi
    fi
else
    echo "skip casio: no $CASIO_ROM (Casio Cassiopeia A-51 ROM image)"
fi

HP_ROM=${HP_ROM:-rom/nk-hp320lx.bin}
HP_HASH=ae7adf8be6004cf273fee8626b4d64730a3eb18e6fd36ffb44410a87d77edc45
HP_TYPE_HASH=c8fc4b822f02af409978961afb093fc7ec416da04a730dd3ebe40341057f9dc5
HP_WAKE_HASH=375abeea2ef582a1b1409d78d3d2df58c76b18c143001e610227150d74fc304b
HP_TOUCH_HASH=c958c0911596fcc2ddf9feaae152a7d2c12a11bd6ebf03b17db551a3b3b40531
if [ -f "$HP_ROM" ]; then
    ./headless "$HP_ROM" --debug-output --seconds=20 --pgm="$OUT/hp.pgm" > "$OUT/hp.log" 2>&1
    actual=$(shasum -a 256 "$OUT/hp.pgm" | cut -d' ' -f1)
    if grep -q "^debug: Pegasus Luke OEMInit() completed" "$OUT/hp.log" && [ "$actual" = "$HP_HASH" ]; then echo "ok   hp"; else echo "FAIL hp $actual"; exit 1; fi
    ./headless "$HP_ROM" --seconds=28 --key=20:9F --key=21:2D --type=23:"Hello, World!" --pgm="$OUT/hp_type.pgm" > /dev/null 2>&1
    actual=$(shasum -a 256 "$OUT/hp_type.pgm" | cut -d' ' -f1)
    if [ "$actual" = "$HP_TYPE_HASH" ]; then echo "ok   hp_type"; else echo "FAIL hp_type $actual"; exit 1; fi
    ./headless "$HP_ROM" --seconds=45 --key=20.5:5A --key=22:5A --tap=24:320:120:1.5 --tap=27:128:48:1.5 --tap=30:128:192:1.5 \
        --tap=33:512:192:1.5 --tap=36:512:48:1.5 --key=38:5A --tap=42:216:47:0.2 --pgm="$OUT/hp_touch.pgm" > /dev/null 2>&1
    actual=$(shasum -a 256 "$OUT/hp_touch.pgm" | cut -d' ' -f1)
    if [ "$actual" = "$HP_TOUCH_HASH" ]; then echo "ok   hp_touch"; else echo "FAIL hp_touch $actual"; exit 1; fi
    ./headless "$HP_ROM" --seconds=32 --key=20:9F --key=21:3C --key=25:5A --key=28:9F --trace-pc --pgm="$OUT/hp_wake.pgm" > "$OUT/hp_wake.log" 2>&1
    actual=$(shasum -a 256 "$OUT/hp_wake.pgm" | cut -d' ' -f1)
    if [ "$actual" = "$HP_WAKE_HASH" ] && grep -q "^t=2[2-4].* lcd=0" "$OUT/hp_wake.log"; then echo "ok   hp_wake"; else echo "FAIL hp_wake $actual"; exit 1; fi
    ./headless "$HP_ROM" --seconds=24 --wav="$OUT/hp_sound.wav" > /dev/null 2>&1
    SOUND='import array, sys, wave; w = wave.open(sys.argv[1]); a = array.array("h", w.readframes(w.getnframes())); sys.exit(0 if w.getframerate() == 22050 and 2.4 < len(a) / 22050 < 2.6 and max(map(abs, a)) > 2000 else 1)'
    if python3 -c "$SOUND" "$OUT/hp_sound.wav"; then echo "ok   hp_sound"; else echo "FAIL hp_sound"; exit 1; fi
    if ./headless "$HP_ROM" --seconds=22 --backlight=20 --trace-pc 2>&1 | grep "^t=" | tail -1 | grep -q "backlight=1"; then echo "ok   hp_backlight"; else echo "FAIL hp_backlight"; exit 1; fi
    gdb_check hp_gdb "$HP_ROM"
    if ./headless "$HP_ROM" --net --seconds=0.01 2>&1 | grep -q libslirp; then
        echo "skip hp_net: headless built without libslirp"
    else
        ./headless "$HP_ROM" --seconds=50 --net=20 > "$OUT/hp_net.log" 2>&1
        if grep -q "^ppp: IPCP up" "$OUT/hp_net.log" && grep -q "^desktop: Handheld_PC, Windows CE 2" "$OUT/hp_net.log"; then echo "ok   hp_net"; else echo "FAIL hp_net"; exit 1; fi
        reconnect_check hp_replug "$HP_ROM" --seconds=60 --net=20 --replug=35
        reconnect_check hp_soft_reset "$HP_ROM" --seconds=110 --net=20 --soft-reset=60 --replug=88
        rapi_check hp_rapi "$HP_ROM" 20
    fi
else
    echo "skip hp: no $HP_ROM (HP 320LX ROM image)"
fi
