#!/bin/sh
set -e
if [ $# -lt 2 ]; then
    echo "usage: mkcard.sh IMAGE SIZE_MB [DIR...]" >&2
    exit 2
fi
image=$1
size=$2
shift 2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
hdiutil create -quiet -size "${size}m" -layout MBRSPUD -fs "MS-DOS FAT16" -volname VELOCARD -type UDIF "$work/card.dmg"
hdiutil convert -quiet "$work/card.dmg" -format UDTO -o "$work/card"
mv "$work/card.cdr" "$image"
if [ $# -gt 0 ]; then
    mount=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage "$image" | awk '/VELOCARD/ {sub(/^.*\t/, ""); print}')
    touch "$mount/.metadata_never_index"
    for dir in "$@"; do cp -RX "$dir" "$mount/"; done
    rm -f "$mount/.metadata_never_index"
    find "$mount" -name '._*' -delete
    rm -rf "$mount/.fseventsd" "$mount/.Spotlight-V100" "$mount/.Trashes"
    hdiutil detach -quiet "$mount"
fi
