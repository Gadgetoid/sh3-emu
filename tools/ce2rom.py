import argparse
import os
import struct

HEADER_FIELDS = ["dllfirst", "dlllast", "physfirst", "physlast", "nummods", "ramstart", "ramfree", "ramend",
                 "copyentries", "copyoffset", "profilelen", "profileoffset", "numfiles"]
HEADER_SIZE = 0x54
MODULE_ENTRY = 32
FILE_ENTRY = 28
SECTION_ENTRY = 24
PE_SECTION_ENTRY = 40
UNIT_NAMES = ["export", "import", "resource", "exception", "security", "fixup", "debug", "description", "machine"]
UNITS_OFFSET = 24
IMAGE_SCN_CNT_CODE = 0x20
IMAGE_SCN_CNT_INITIALIZED_DATA = 0x40
IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x80
IMAGE_SCN_COMPRESSED = 0x2000
IMAGE_SCN_MEM_WRITE = 0x80000000
IMAGE_FILE_MACHINE_SH3 = 0x1A2
PE32_MAGIC = 0x10B
OPTIONAL_HEADER_SIZE = 0xE0
DIRECTORY_COUNT = 16
LZ_BLOCK = 1024
DOS_HEADER_SIZE = 0x80
SECTION_ALIGNMENT = 0x400
FILE_ALIGNMENT = 0x200
FILETIME_UNIX_EPOCH = 116444736000000000
ADDRESS_MASK = 0x1FFFFFFF


def read24(data, offset):
    return data[offset] | data[offset + 1] << 8 | data[offset + 2] << 16


def lz_decode_block(data):
    output = bytearray()
    position = 0
    while position < len(data):
        flags = data[position]
        position += 1
        for bit in range(8):
            if position >= len(data):
                break
            if not flags & (1 << bit):
                output.append(data[position])
                position += 1
                continue
            token = data[position]
            position += 1
            low, high = token & 15, token >> 4
            if low == 1:
                length = 2
                source = len(output) - high - 2
            else:
                source = high | data[position] << 4
                position += 1
                if low == 0:
                    length = data[position] + 17
                    position += 1
                else:
                    length = low + 1
            for index in range(length):
                output.append(output[source + index])
    return bytes(output)


def lz_decode(data, size):
    if len(data) < 3:
        return bytes(size)
    block_count = read24(data, 0) // LZ_BLOCK + 1
    output = bytearray()
    start = 3 * (block_count + 1)
    for index in range(1, block_count + 1):
        end = read24(data, 3 * index)
        output += lz_decode_block(data[start:end])
        start = end
    return bytes(output[:size])


class Image:
    def __init__(self, data, base=None):
        self.data = data
        self.base, self.header_address = self.find_header(base)
        values = struct.unpack_from(f"<{len(HEADER_FIELDS)}I", data, self.offset(self.header_address))
        self.header = dict(zip(HEADER_FIELDS, values))

    def find_header(self, base):
        found = []
        for offset in range(0, len(self.data) - HEADER_SIZE, 4):
            fields = dict(zip(HEADER_FIELDS, struct.unpack_from(f"<{len(HEADER_FIELDS)}I", self.data, offset)))
            physfirst, physlast = fields["physfirst"] & ADDRESS_MASK, fields["physlast"] & ADDRESS_MASK
            if physfirst & 0xFFF or not physfirst < physlast or not 1 <= fields["nummods"] <= 1000:
                continue
            if not fields["ramstart"] <= fields["ramfree"] <= fields["ramend"]:
                continue
            start = physfirst if base is None else base & ADDRESS_MASK
            if not start <= physfirst or physlast - start > len(self.data) or not physfirst <= start + offset < physlast:
                continue
            self.base = start
            if self.valid_table(start + offset, fields["nummods"]):
                found.append((physlast - physfirst, start, fields["physfirst"] - physfirst + start + offset))
        if not found:
            raise SystemExit("no ROM header found; try giving the image's base address")
        _, start, header_address = max(found)
        return start, header_address

    def valid_table(self, header_address, count):
        for index in range(count):
            position = self.offset(header_address + HEADER_SIZE + MODULE_ENTRY * index, check=False)
            if not 0 <= position <= len(self.data) - MODULE_ENTRY:
                return False
            name = struct.unpack_from("<I", self.data, position + 16)[0]
            if not self.is_name(name):
                return False
        return True

    def is_name(self, address):
        position = self.offset(address, check=False)
        if not 0 <= position < len(self.data):
            return False
        end = self.data.find(b"\0", position, position + 64)
        return end > position and all(32 <= byte < 127 for byte in self.data[position:end])

    def offset(self, address, check=True):
        offset = (address & ADDRESS_MASK) - self.base
        if check and not 0 <= offset < len(self.data):
            raise ValueError(f"{address:08X} is outside the image")
        return offset

    def read(self, address, length):
        start = self.offset(address)
        return self.data[start:start + length]

    def string(self, address):
        start = self.offset(address)
        return self.data[start:self.data.index(b"\0", start)].decode("latin-1")

    def modules(self):
        table = self.header_address + HEADER_SIZE
        for index in range(self.header["nummods"]):
            raw = self.read(table + MODULE_ENTRY * index, MODULE_ENTRY)
            attributes, time_low, time_high, size, name, e32, o32, load = struct.unpack("<8I", raw)
            yield {"name": self.string(name), "attributes": attributes, "time": time_low | time_high << 32, "size": size, "e32": e32, "o32": o32, "load": load}

    def files(self):
        table = self.header_address + HEADER_SIZE + MODULE_ENTRY * self.header["nummods"]
        for index in range(self.header["numfiles"]):
            raw = self.read(table + FILE_ENTRY * index, FILE_ENTRY)
            attributes, time_low, time_high, real_size, stored_size, name, load = struct.unpack("<7I", raw)
            yield {"name": self.string(name), "attributes": attributes, "time": time_low | time_high << 32, "real_size": real_size, "stored_size": stored_size, "load": load}

    def copy_entries(self):
        for index in range(self.header["copyentries"]):
            yield struct.unpack("<4I", self.read(self.header["copyoffset"] + 16 * index, 16))

    def module_header(self, module):
        raw = self.read(module["e32"], UNITS_OFFSET + 8 * len(UNIT_NAMES))
        sections, flags, entry, image_base, subsystem_major, subsystem_minor, image_size, subsystem = struct.unpack_from("<HHIIHHII", raw, 0)
        units = {name: struct.unpack_from("<II", raw, UNITS_OFFSET + 8 * index) for index, name in enumerate(UNIT_NAMES)}
        return {"sections": sections, "flags": flags, "entry": entry, "image_base": image_base, "subsystem_version": (subsystem_major, subsystem_minor),
                "image_size": image_size, "subsystem": subsystem, "units": units}

    def sections(self, module, header):
        for index in range(header["sections"]):
            virtual_size, rva, stored_size, pointer, real_address, flags = struct.unpack("<6I", self.read(module["o32"] + SECTION_ENTRY * index, SECTION_ENTRY))
            yield {"name": section_name(rva, flags, header["units"]), "virtual_size": virtual_size, "rva": rva, "stored_size": stored_size, "pointer": pointer,
                   "real_address": real_address, "flags": flags}

    def section_data(self, section):
        if not section["stored_size"]:
            return b""
        stored = self.read(section["pointer"], section["stored_size"])
        if section["flags"] & IMAGE_SCN_COMPRESSED:
            return lz_decode(stored, section["virtual_size"])
        return stored

    def rebuild_pe(self, module):
        header = self.module_header(module)
        sections = list(self.sections(module, header))
        headers_size = DOS_HEADER_SIZE + 24 + OPTIONAL_HEADER_SIZE + PE_SECTION_ENTRY * len(sections)
        position = align(headers_size, FILE_ALIGNMENT)
        table, bodies = bytearray(), bytearray()
        for section in sections:
            body = self.section_data(section)
            stored = align(len(body), FILE_ALIGNMENT) if body else 0
            table += struct.pack("<8sIIIIIIHHI", section["name"].encode(), section["virtual_size"], section["rva"], stored, position + len(bodies) if body else 0,
                                 0, 0, 0, 0, section["flags"] & ~IMAGE_SCN_COMPRESSED)
            bodies += body + bytes(stored - len(body))
        dos = bytearray(DOS_HEADER_SIZE)
        dos[0:2] = b"MZ"
        struct.pack_into("<I", dos, 0x3C, DOS_HEADER_SIZE)
        file_header = b"PE\0\0" + struct.pack("<HHIIIHH", IMAGE_FILE_MACHINE_SH3, len(sections), unix_time(module["time"]), 0, 0, OPTIONAL_HEADER_SIZE, header["flags"])
        code = [section for section in sections if section["flags"] & IMAGE_SCN_CNT_CODE]
        data = [section for section in sections if not section["flags"] & IMAGE_SCN_CNT_CODE]
        major, minor = header["subsystem_version"]
        optional = struct.pack("<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII", PE32_MAGIC, 0, 0,
                               sum(section["virtual_size"] for section in code),
                               sum(section["virtual_size"] for section in sections if section["flags"] & IMAGE_SCN_CNT_INITIALIZED_DATA),
                               sum(section["virtual_size"] for section in sections if section["flags"] & IMAGE_SCN_CNT_UNINITIALIZED_DATA),
                               header["entry"], code[0]["rva"] if code else 0, data[0]["rva"] if data else 0, header["image_base"],
                               SECTION_ALIGNMENT, FILE_ALIGNMENT, major, minor, 0, 0, major, minor, 0, header["image_size"], position, 0,
                               header["subsystem"], 0, 0, 0, 0, 0, 0, DIRECTORY_COUNT)
        for name in UNIT_NAMES:
            rva, size = header["units"][name]
            inside = any(section["rva"] <= rva < section["rva"] + section["virtual_size"] for section in sections)
            optional += struct.pack("<II", rva, size) if inside else bytes(8)
        optional += bytes(8 * (DIRECTORY_COUNT - len(UNIT_NAMES)))
        headers = bytes(dos) + file_header + optional + bytes(table)
        return headers + bytes(position - len(headers)) + bytes(bodies)

    def file_data(self, entry):
        stored = self.read(entry["load"], entry["stored_size"])
        if entry["stored_size"] != entry["real_size"]:
            return lz_decode(stored, entry["real_size"])
        return stored


def section_name(rva, flags, units):
    for unit, name in (("resource", ".rsrc"), ("exception", ".pdata"), ("fixup", ".reloc")):
        if units[unit][0] == rva and units[unit][1]:
            return name
    if flags & IMAGE_SCN_CNT_CODE:
        return ".text"
    if flags & IMAGE_SCN_MEM_WRITE:
        return ".data"
    return ".rdata"


def unix_time(filetime):
    return max(0, (filetime - FILETIME_UNIX_EPOCH) // 10000000) & 0xFFFFFFFF


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def main():
    parser = argparse.ArgumentParser(description="List a CE 2.0 ROM image and extract its modules as PE files and its files, decompressed")
    parser.add_argument("image")
    parser.add_argument("output")
    parser.add_argument("--base", type=lambda text: int(text, 0), help="address of the image's first byte, when it isn't the ROM's physfirst")
    arguments = parser.parse_args()
    image = Image(open(arguments.image, "rb").read(), arguments.base)
    os.makedirs(os.path.join(arguments.output, "modules"), exist_ok=True)
    os.makedirs(os.path.join(arguments.output, "files"), exist_ok=True)
    lines = [f"{os.path.basename(arguments.image)}: {len(image.data)} bytes, base {image.base | 0x80000000:08X}", f"ROM header at {image.header_address:08X}"]
    lines += [f"  {name:14} {value:08X}" for name, value in image.header.items()]
    lines.append("copy entries (source, destination, copy length, destination length)")
    lines += [f"  {source:08X} {destination:08X} {copy:08X} {length:08X}" for source, destination, copy, length in image.copy_entries()]
    lines.append("")
    lines.append("MODULES (name, image base, entry, sections: name rva virtual stored at flags)")
    for module in image.modules():
        header = image.module_header(module)
        lines.append(f"  {module['name']:16} base {header['image_base']:08X} entry {header['image_base'] + header['entry']:08X}")
        for section in image.sections(module, header):
            lines.append(f"    {section['name']:8} {section['rva']:08X} {section['virtual_size']:8X} {section['stored_size']:8X} at {section['pointer']:08X} {section['flags']:08X}")
        with open(os.path.join(arguments.output, "modules", module["name"]), "wb") as handle:
            handle.write(image.rebuild_pe(module))
    lines.append("")
    lines.append("FILES (name, size, stored, attributes, address)")
    for entry in image.files():
        lines.append(f"  {entry['name']:24} {entry['real_size']:8} {entry['stored_size']:8} {entry['attributes']:04X} {entry['load']:08X}")
        with open(os.path.join(arguments.output, "files", entry["name"]), "wb") as handle:
            handle.write(image.file_data(entry))
    with open(os.path.join(arguments.output, "manifest.txt"), "w") as handle:
        handle.write("\n".join(lines) + "\n")


main()
