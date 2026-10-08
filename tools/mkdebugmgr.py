import sys

BUILDS = (("DEBUGMGR_CE1", "ce1-sh3"), ("DEBUGMGR_CE2", "ce2-sh3"))


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: mkdebugmgr.py VELO_TOOLCHAIN_DEBUGMGR_BUILD OUTPUT.c")
    build, output = sys.argv[1], sys.argv[2]
    lines = ['#include "rapi/debugmgr_images.h"', ""]
    for name, folder in BUILDS:
        with open(f"{build}/{folder}/velo-debugmgr.exe", "rb") as file:
            image = file.read()
        lines.append(f"const uint8_t {name}[] = {{")
        for start in range(0, len(image), 16):
            lines.append("    " + " ".join(f"0x{byte:02X}," for byte in image[start:start + 16]))
        lines.append("};")
        lines.append(f"const size_t {name}_SIZE = sizeof {name};")
        lines.append("")
    with open(output, "w") as file:
        file.write("\n".join(lines))


main()
