#!/bin/sh
set -e
PREFIX=${SH_PREFIX:-sh-elf-}
RUNNER=${RUNNER:-./sh3-run}
OUT=${TMPDIR:-/tmp}/sh3-tests
if ! command -v "${PREFIX}as" > /dev/null; then
    echo "skip sh3 tests: no ${PREFIX}as (set SH_PREFIX)"
    exit 0
fi
mkdir -p "$OUT"
cat > "$OUT/link.ld" <<'LINK'
ENTRY(_start)
SECTIONS
{
    . = 0x80400000;
    .text : { *(.text) }
    . = 0x80480000;
    .data : { *(.data) }
}
LINK
status=0
for source in tests/sh3/*.s; do
    name=$(basename "$source" .s)
    ${PREFIX}as --little --isa=sh3 -I tests/sh3 -o "$OUT/$name.o" "$source"
    ${PREFIX}ld -EL -T "$OUT/link.ld" -o "$OUT/$name.elf" "$OUT/$name.o"
    if "$RUNNER" --privileged "$OUT/$name.elf"; then
        echo "ok   $name"
    else
        echo "FAIL $name (check $?)"
        status=1
    fi
done
exit $status
