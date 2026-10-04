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
expect_fail headless_not_image "not a B000FF" ./headless Makefile --seconds=1
expect_fail headless_bad_folder "cannot open folder" ./headless Makefile --seconds=1 --folder=/nonexistent/folder
expect_fail headless_bad_agent "cannot listen on agent socket" ./headless Makefile --seconds=1 --agent=/nonexistent/folder/agent.sock
if [ -x ./sh3emu ]; then
    expect_fail sh3emu_unknown "unknown option --frob" ./sh3emu --frob
    expect_fail sh3emu_bad_memory "--memory" ./sh3emu --memory=4
fi
expect_fail sh3_run_no_program "usage: sh3-run" ./sh3-run
