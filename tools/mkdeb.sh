#!/bin/sh
set -e
out=${1:-dist}
version=$(printf "%s" "${VERSION:-0.1+git$(date +%Y%m%d)}" | tr - "~")
arch=$(dpkg --print-architecture)
package=sh3-emu
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT

make -s all sh3emu icons
icons=${BUILD:-build}/icons

install -D -m 755 sh3emu "$root/usr/bin/sh3emu"
install -D -m 755 headless "$root/usr/bin/sh3emu-headless"
install -D -m 644 README.md "$root/usr/share/doc/$package/README.md"
install -D -m 644 LICENSE "$root/usr/share/doc/$package/LICENSE"
install -D -m 644 assets/icon.svg "$root/usr/share/icons/hicolor/scalable/apps/$package.svg"
for size in 16 32 64 128 256 512; do install -D -m 644 "$icons/icon-$size.png" "$root/usr/share/icons/hicolor/${size}x${size}/apps/$package.png"; done
for licence in licences/*; do install -D -m 644 "$licence" "$root/usr/share/doc/$package/licences/$(basename "$licence")"; done
install -D -m 755 tools/mkcard.sh "$root/usr/share/$package/mkcard.sh"

cat > "$root/usr/share/doc/$package/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: sh3-emu
Comment: No ROMs are included.

Files: *
Copyright: 2026 Phil Howard
License: MIT
 See /usr/share/doc/$package/LICENSE
Comment: The Odo system ASIC's behaviour follows CERF (MIT), see /usr/share/doc/$package/licences/MIT-CERF.txt

Files: src/vendor/stb_truetype.h
License: public-domain
 stb_truetype by Sean Barrett, public domain.

Files: src/vendor/nanosvg.h src/vendor/nanosvgrast.h
License: Zlib
 See /usr/share/doc/$package/licences/Zlib-nanosvg.txt
EOF

mkdir -p "$root/usr/share/applications"
cat > "$root/usr/share/applications/$package.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=SH3Emu
Comment=Hitachi SH-3 Windows CE emulator
Exec=sh3emu
Icon=$package
StartupWMClass=sh3-emu
Terminal=false
Categories=Emulator;Game;
EOF

work=$(mktemp -d)
mkdir -p "$work/debian"
printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' "$package" "$package" > "$work/debian/control"
depends=$(cd "$work" && dpkg-shlibdeps -O "$root/usr/bin/sh3emu" "$root/usr/bin/sh3emu-headless" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')
rm -rf "$work"

size=$(du -sk "$root/usr" | cut -f1)
mkdir -p "$root/DEBIAN"
cat > "$root/DEBIAN/control" <<EOF
Package: $package
Version: $version
Architecture: $arch
Maintainer: Phil Howard <phil@pimoroni.com>
Installed-Size: $size
Depends: $depends
Recommends: mtools, dosfstools, fdisk, fonts-dejavu-core | fonts-noto-core
Section: otherosfs
Priority: optional
Description: Hitachi SH-3 Windows CE emulator
 Emulates Windows CE machines built on the Hitachi SH-3: Microsoft's Odo
 reference board, the Clarion AutoPC, the Casio Cassiopeia A-51 and the
 HP 320LX, with a simulated LCD, PC Card images and a PPP network.
 .
 sh3emu is the emulator and sh3emu-headless runs it without a window for
 tests and scripts. ROMs are not included.
EOF

mkdir -p "$out"
dpkg-deb --root-owner-group --build "$root" "$out/${package}_${version}_${arch}.deb" >/dev/null
echo "$out/${package}_${version}_${arch}.deb"
