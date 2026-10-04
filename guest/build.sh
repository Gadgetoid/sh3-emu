#!/bin/sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
if [ -z "$VELO_TOOLCHAIN" ] || [ -z "$VELO_SH3_LLVM" ]; then
    echo "usage: VELO_TOOLCHAIN=DIR VELO_SH3_LLVM=DIR [OUTPUT=DIR] $0" >&2
    exit 2
fi
OUTPUT=${OUTPUT:-$HERE/../build/guest}
cmake -S "$HERE" -B "$OUTPUT" -DCMAKE_TOOLCHAIN_FILE="$VELO_TOOLCHAIN/cmake/velo-ce.cmake" -DVELO_CE_VERSION=2 -DVELO_ARCH=sh3 -DVELO_LLVM_ROOT="$VELO_SH3_LLVM" > "$OUTPUT.log" 2>&1 || { cat "$OUTPUT.log"; exit 1; }
cmake --build "$OUTPUT"
