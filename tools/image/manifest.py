import hashlib
import struct
import sys

TOC_ENTRY_SIZE = 32
FILES_ENTRY_SIZE = 28


def load_records(path):
    with open(path, "rb") as handle:
        data = handle.read()
    if not data.startswith(b"B000FF\n"):
        raise SystemExit(f"{path}: not a B000FF image")
    image_start, image_length = struct.unpack_from("<II", data, 7)
    memory = bytearray(image_length)
    offset = 15
    while offset + 12 <= len(data):
        address, length, _ = struct.unpack_from("<III", data, offset)
        offset += 12
        if address == 0:
            break
        memory[address - image_start:address - image_start + length] = data[offset:offset + length]
        offset += length
    return data, image_start, image_length, bytes(memory)


def main():
    path, title = sys.argv[1], sys.argv[2]
    data, image_start, image_length, memory = load_records(path)

    def read_u32(address):
        return struct.unpack_from("<I", memory, (address & 0x1FFFFFFF) - (image_start & 0x1FFFFFFF))[0]

    def read_string(address):
        start = (address & 0x1FFFFFFF) - (image_start & 0x1FFFFFFF)
        return memory[start:memory.index(b"\0", start)].decode("latin-1")

    if memory[0x40:0x44] == b"CECE":
        header = read_u32(image_start + 0x44)
    else:
        bounds = struct.pack("<II", image_start, image_start + image_length)
        found = memory.find(bounds)
        while found >= 0 and found % 4:
            found = memory.find(bounds, found + 1)
        if found < 8:
            raise SystemExit(f"{path}: cannot find the ROM header")
        header = image_start + found - 8
    module_count = read_u32(header + 16)
    file_count = read_u32(header + 48)
    ram_start, ram_free, ram_end = read_u32(header + 20), read_u32(header + 24), read_u32(header + 28)
    fs_ram_percent = read_u32(header + 56)
    for header_size in (72, 76, 80, 84):
        first_name = read_u32(header + header_size + 16)
        if (first_name & 0x1FFFFFFF) - (image_start & 0x1FFFFFFF) < image_length and read_string(first_name).lower() == "nk.exe":
            break
    else:
        raise SystemExit(f"{path}: cannot find the module table")
    modules_at = header + header_size
    files_at = modules_at + module_count * TOC_ENTRY_SIZE
    modules = [read_string(read_u32(modules_at + index * TOC_ENTRY_SIZE + 16)) for index in range(module_count)]
    files = [read_string(read_u32(files_at + index * FILES_ENTRY_SIZE + 20)) for index in range(file_count)]

    print(f"{path.rsplit('/', 1)[-1]}: {title}")
    print(f"sha256 {hashlib.sha256(data).hexdigest()}")
    print(f"image {image_start:08X}-{image_start + image_length - 1:08X} ({image_length} bytes); RAM {ram_start:08X}-{ram_end - 1:08X}, "
          f"first free {ram_free:08X}; FSRAMPERCENT 0x{fs_ram_percent:08X}")
    print()
    print("MODULES")
    for name in modules:
        print(f"  {name}")
    print()
    print("FILES")
    for name in files:
        print(f"  {name}")


main()
