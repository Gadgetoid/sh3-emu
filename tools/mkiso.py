#!/usr/bin/env python3
import os
import struct
import sys

BLOCK = 2048
PRIMARY_BLOCK = 16
TERMINATOR_BLOCK = 17
PATH_TABLE_L_BLOCK = 18
PATH_TABLE_M_BLOCK = 19
ROOT_BLOCK = 20
FLAG_DIRECTORY = 0x02
RECORDING_DATE = bytes([126, 10, 16, 0, 0, 0, 0])
UNSET_DATE = b"0" * 16 + b"\0"


def both16(value):
    return struct.pack("<H", value) + struct.pack(">H", value)


def both32(value):
    return struct.pack("<I", value) + struct.pack(">I", value)


def padded(text, length):
    return text.encode("ascii").ljust(length, b" ")


def record(name, block, size, flags):
    length = 33 + len(name) + (1 - len(name) % 2)
    entry = struct.pack("<BB", length, 0) + both32(block) + both32(size) + RECORDING_DATE
    entry += struct.pack("<BBB", flags, 0, 0) + both16(1) + struct.pack("<B", len(name)) + name
    return entry.ljust(length, b"\0")


def iso_name(path):
    stem, extension = os.path.splitext(os.path.basename(path))
    return (stem[:8] + extension[:4]).upper().encode("ascii") + b";1"


def blocks_for(size):
    return max(1, (size + BLOCK - 1) // BLOCK)


def build(volume, paths):
    files = [(iso_name(path), open(path, "rb").read()) for path in paths]
    files.sort(key=lambda item: item[0])
    next_block = ROOT_BLOCK + 1
    placed = []
    for name, data in files:
        placed.append((name, next_block, data))
        next_block += blocks_for(len(data))
    root = record(b"\0", ROOT_BLOCK, BLOCK, FLAG_DIRECTORY) + record(b"\1", ROOT_BLOCK, BLOCK, FLAG_DIRECTORY)
    for name, block, data in placed:
        root += record(name, block, len(data), 0)
    if len(root) > BLOCK:
        sys.exit("mkiso.py: too many files for one directory block")
    path_table_l = struct.pack("<BBIH", 1, 0, ROOT_BLOCK, 1) + b"\0\0"
    path_table_m = struct.pack(">BBIH", 1, 0, ROOT_BLOCK, 1) + b"\0\0"
    primary = bytearray(BLOCK)
    primary[0:8] = b"\x01CD001\x01\0"
    primary[8:40] = padded("", 32)
    primary[40:72] = padded(volume, 32)
    primary[80:88] = both32(next_block)
    primary[120:124] = both16(1)
    primary[124:128] = both16(1)
    primary[128:132] = both16(BLOCK)
    primary[132:140] = both32(len(path_table_l))
    primary[140:144] = struct.pack("<I", PATH_TABLE_L_BLOCK)
    primary[148:152] = struct.pack(">I", PATH_TABLE_M_BLOCK)
    primary[156:190] = record(b"\0", ROOT_BLOCK, BLOCK, FLAG_DIRECTORY)
    for offset, length in ((190, 128), (318, 128), (446, 128), (574, 128), (702, 37), (739, 37), (776, 37)):
        primary[offset:offset + length] = padded("", length)
    for offset in (813, 830, 847, 864):
        primary[offset:offset + 17] = UNSET_DATE
    primary[881] = 1
    terminator = bytearray(BLOCK)
    terminator[0:7] = b"\xffCD001\x01"
    image = bytearray(BLOCK * next_block)
    image[PRIMARY_BLOCK * BLOCK:(PRIMARY_BLOCK + 1) * BLOCK] = primary
    image[TERMINATOR_BLOCK * BLOCK:(TERMINATOR_BLOCK + 1) * BLOCK] = terminator
    image[PATH_TABLE_L_BLOCK * BLOCK:PATH_TABLE_L_BLOCK * BLOCK + len(path_table_l)] = path_table_l
    image[PATH_TABLE_M_BLOCK * BLOCK:PATH_TABLE_M_BLOCK * BLOCK + len(path_table_m)] = path_table_m
    image[ROOT_BLOCK * BLOCK:ROOT_BLOCK * BLOCK + len(root)] = root
    for name, block, data in placed:
        image[block * BLOCK:block * BLOCK + len(data)] = data
    return bytes(image)


def main():
    if len(sys.argv) < 4:
        sys.exit("usage: mkiso.py IMAGE VOLUME FILE...")
    with open(sys.argv[1], "wb") as file:
        file.write(build(sys.argv[2], sys.argv[3:]))


if __name__ == "__main__":
    main()
