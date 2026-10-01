#!/bin/sh
set -e
app=${1:-Velo.app}
version=$(git describe --always --dirty 2>/dev/null || echo unknown)
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" "$app/Contents/Resources"
cp velo velo-rapi "$app/Contents/MacOS/"

cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Velo</string>
    <key>CFBundleDisplayName</key><string>Velo</string>
    <key>CFBundleIdentifier</key><string>org.velo-emu.Velo</string>
    <key>CFBundleExecutable</key><string>velo</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$version</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSHumanReadableCopyright</key><string>Emulates a Philips Velo 1. ROMs not included.</string>
</dict>
</plist>
EOF

external_libraries() {
    otool -L "$1" | awk 'NR > 1 { print $1 }' | grep -E '^(/opt/homebrew|/usr/local)/' || true
}

bundle() {
    for library in $(external_libraries "$1"); do
        name=$(basename "$library")
        install_name_tool -change "$library" "@rpath/$name" "$1" 2>/dev/null
        if [ ! -f "$app/Contents/Frameworks/$name" ]; then
            cp "$library" "$app/Contents/Frameworks/$name"
            chmod u+w "$app/Contents/Frameworks/$name"
            install_name_tool -id "@rpath/$name" "$app/Contents/Frameworks/$name" 2>/dev/null
            bundle "$app/Contents/Frameworks/$name"
        fi
    done
}

bundle "$app/Contents/MacOS/velo"
bundle "$app/Contents/MacOS/velo-rapi"
for binary in "$app/Contents/MacOS/velo" "$app/Contents/MacOS/velo-rapi" "$app/Contents/Frameworks/"*.dylib; do
    [ -f "$binary" ] || continue
    for path in $(otool -l "$binary" | awk '/LC_RPATH/ { found = 1 } found && $1 == "path" { print $2; found = 0 }'); do
        install_name_tool -delete_rpath "$path" "$binary" 2>/dev/null || true
    done
done
for binary in "$app/Contents/MacOS/velo" "$app/Contents/MacOS/velo-rapi"; do
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$binary" 2>/dev/null || true
done

for library in "$app/Contents/Frameworks/"*.dylib; do
    [ -f "$library" ] && codesign --force --sign - "$library"
done
codesign --force --sign - "$app/Contents/MacOS/velo-rapi"
codesign --force --sign - "$app"
echo "built $app ($version)"
