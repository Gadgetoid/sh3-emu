import os
import struct
import sys

HEADER_SIZE = 0x54
HEADER_FIELDS = ["dllfirst", "dlllast", "physfirst", "physlast", "nummods", "ramstart", "ramfree", "ramend",
                 "copyentries", "copyoffset", "profilelen", "profileoffset", "numfiles"]
MODULE_ENTRY = 0x130
FILE_ENTRY = 0x12C
NAME_SIZE = 260
SECTION_ENTRY = 40
IMAGE_SCN_COMPRESSED = 0x2000
FILE_ATTRIBUTE_COMPRESSED = 0x800
LZW_CODES = 4096
LZW_CLEAR = 256
LZW_FIRST = 257
FILE_BLOCK = 4096
RAW_BLOCK = 0x80000000
DOS_HEADER_SIZE = 0x80
FILE_ALIGNMENT = 0x200


def lzw_decode(data, size):
    entries = [bytes((index,)) for index in range(256)] + [b""] + [None] * (LZW_CODES - LZW_FIRST)
    output = bytearray()
    value = int.from_bytes(data, "little")
    bit, total_bits = 0, len(data) * 8
    next_code, width, previous = LZW_FIRST, 9, None
    while bit + width <= total_bits and len(output) < size:
        code = (value >> bit) & ((1 << width) - 1)
        bit += width
        if code == LZW_CLEAR:
            next_code, width, previous = LZW_FIRST, 9, None
            continue
        if code < next_code and entries[code] is not None:
            entry = entries[code]
        elif code == next_code and previous is not None:
            entry = previous + previous[:1]
        else:
            raise ValueError(f"bad LZW code {code}")
        output += entry
        if previous is not None and next_code < LZW_CODES:
            entries[next_code] = previous + entry[:1]
            next_code += 1
        previous = entry
        if next_code >= (1 << width) and width < 12:
            width += 1
    return bytes(output[:size])


def file_decode(data, size):
    count = (size + FILE_BLOCK - 1) // FILE_BLOCK
    starts = struct.unpack_from(f"<{count}I", data, 0) if count else ()
    output = bytearray()
    for index, start in enumerate(starts):
        end = starts[index + 1] & ~RAW_BLOCK if index + 1 < count else len(data)
        chunk = data[start & ~RAW_BLOCK:end]
        length = min(FILE_BLOCK, size - FILE_BLOCK * index)
        output += chunk[:length] if start & RAW_BLOCK else lzw_decode(chunk, length)
    return bytes(output)


class Image:
    def __init__(self, data):
        self.data = data
        self.base = 0
        self.header_address = self.find_header()
        values = struct.unpack_from(f"<{len(HEADER_FIELDS)}I", data, self.offset(self.header_address))
        self.header = dict(zip(HEADER_FIELDS, values))

    def find_header(self):
        for offset in range(0, len(self.data) - 32, 4):
            physfirst, physlast = struct.unpack_from("<II", self.data, offset + 8)
            if physlast - physfirst == len(self.data) and physfirst & 0xFFF == 0:
                self.base = physfirst
                return physfirst + offset
        raise SystemExit("no ROM header whose physfirst-physlast matches the image size")

    def offset(self, address):
        offset = (address & 0x1FFFFFFF) - (self.base & 0x1FFFFFFF)
        if not 0 <= offset < len(self.data):
            raise ValueError(f"{address:08X} is outside the image")
        return offset

    def word(self, address):
        return struct.unpack_from("<I", self.data, self.offset(address))[0]

    def read(self, address, length):
        start = self.offset(address)
        return self.data[start:start + length]

    def modules(self):
        table = self.header_address + HEADER_SIZE
        for index in range(self.header["nummods"]):
            raw = self.read(table + MODULE_ENTRY * index, MODULE_ENTRY)
            e32, o32, load = struct.unpack_from("<3I", raw, MODULE_ENTRY - 12)
            name = raw[0x10:0x10 + NAME_SIZE].split(b"\0")[0].decode("latin-1")
            attributes, time_low, time_high, size = struct.unpack_from("<4I", raw, 0)
            yield {"name": name, "attributes": attributes, "time": time_low | time_high << 32, "size": size, "e32": e32, "o32": o32, "load": load}

    def files(self):
        table = self.header_address + HEADER_SIZE + MODULE_ENTRY * self.header["nummods"]
        for index in range(self.header["numfiles"]):
            raw = self.read(table + FILE_ENTRY * index, FILE_ENTRY)
            attributes, time_low, time_high, real_size, stored_size = struct.unpack_from("<5I", raw, 0)
            name = raw[0x14:0x14 + NAME_SIZE].split(b"\0")[0].decode("latin-1")
            load = struct.unpack_from("<I", raw, FILE_ENTRY - 4)[0]
            yield {"name": name, "attributes": attributes, "time": time_low | time_high << 32, "real_size": real_size, "stored_size": stored_size, "load": load}

    def copy_entries(self):
        for index in range(self.header["copyentries"]):
            yield struct.unpack_from("<4I", self.data, self.offset(self.header["copyoffset"] + 16 * index))

    def pe_header(self, module):
        e32 = module["e32"]
        if self.read(e32, 4) != b"PE\0\0":
            raise ValueError(f"{module['name']}: no PE header at {e32:08X}")
        machine, section_count = struct.unpack_from("<HH", self.data, self.offset(e32 + 4))
        optional_size = struct.unpack_from("<H", self.data, self.offset(e32 + 20))[0]
        entry, image_base = struct.unpack_from("<I", self.data, self.offset(e32 + 40))[0], self.word(e32 + 52)
        return {"machine": machine, "sections": section_count, "optional_size": optional_size, "entry": entry, "image_base": image_base}

    def sections(self, module, header):
        for index in range(header["sections"]):
            raw = self.read(module["o32"] + SECTION_ENTRY * index, SECTION_ENTRY)
            name = raw[:8].split(b"\0")[0].decode("latin-1")
            virtual_size, rva, stored_size, pointer = struct.unpack_from("<4I", raw, 8)
            flags = struct.unpack_from("<I", raw, 36)[0]
            yield {"name": name, "raw": raw, "virtual_size": virtual_size, "rva": rva, "stored_size": stored_size, "pointer": pointer, "flags": flags}

    def section_data(self, section):
        if not section["stored_size"]:
            return b""
        stored = self.read(section["pointer"], section["stored_size"])
        if section["flags"] & IMAGE_SCN_COMPRESSED:
            return lzw_decode(stored, section["virtual_size"])
        return stored

    def rebuild_pe(self, module):
        header = self.pe_header(module)
        sections = list(self.sections(module, header))
        pe_size = 24 + header["optional_size"]
        headers_size = DOS_HEADER_SIZE + pe_size + SECTION_ENTRY * len(sections)
        position = align(headers_size, FILE_ALIGNMENT)
        dos = bytearray(DOS_HEADER_SIZE)
        dos[0:2] = b"MZ"
        struct.pack_into("<I", dos, 0x3C, DOS_HEADER_SIZE)
        pe = bytearray(self.read(module["e32"], pe_size))
        struct.pack_into("<I", pe, 24 + 36, FILE_ALIGNMENT)
        struct.pack_into("<I", pe, 24 + 60, position)
        table, bodies = bytearray(), bytearray()
        for section in sections:
            body = self.section_data(section)
            raw = bytearray(section["raw"])
            struct.pack_into("<II", raw, 16, align(len(body), FILE_ALIGNMENT) if body else 0, position + len(bodies) if body else 0)
            struct.pack_into("<I", raw, 36, section["flags"] & ~IMAGE_SCN_COMPRESSED)
            table += raw
            if body:
                bodies += body + bytes(align(len(body), FILE_ALIGNMENT) - len(body))
        headers = dos + pe + table
        return bytes(headers + bytes(position - len(headers)) + bodies)

    def file_data(self, entry):
        stored = self.read(entry["load"], entry["stored_size"])
        if entry["attributes"] & FILE_ATTRIBUTE_COMPRESSED:
            return file_decode(stored, entry["real_size"])
        return stored[:entry["real_size"]]


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


MACHINES = {0x1A2: "SH3", 0x1A3: "SH3DSP", 0x166: "R4000", 0x1C0: "ARM"}


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: ce1rom.py IMAGE OUTPUT_FOLDER")
    path, output = sys.argv[1], sys.argv[2]
    image = Image(open(path, "rb").read())
    os.makedirs(os.path.join(output, "modules"), exist_ok=True)
    os.makedirs(os.path.join(output, "files"), exist_ok=True)
    lines = [f"{os.path.basename(path)}: {len(image.data)} bytes", f"ROM header at {image.header_address:08X}"]
    lines += [f"  {name:14} {value:08X}" for name, value in image.header.items()]
    lines.append("copy entries (source, destination, copy length, destination length)")
    lines += [f"  {source:08X} {destination:08X} {copy:08X} {length:08X}" for source, destination, copy, length in image.copy_entries()]
    lines.append("")
    lines.append("MODULES (name, image base, entry, machine, sections: name rva virtual stored flags)")
    for module in image.modules():
        header = image.pe_header(module)
        lines.append(f"  {module['name']:16} base {header['image_base']:08X} entry {header['image_base'] + header['entry']:08X} {MACHINES.get(header['machine'], hex(header['machine']))}")
        for section in image.sections(module, header):
            lines.append(f"    {section['name']:8} {section['rva']:08X} {section['virtual_size']:8X} {section['stored_size']:8X} at {section['pointer']:08X} {section['flags']:08X}")
        with open(os.path.join(output, "modules", module["name"]), "wb") as handle:
            handle.write(image.rebuild_pe(module))
    lines.append("")
    lines.append("FILES (name, size, stored, attributes, address)")
    for entry in image.files():
        lines.append(f"  {entry['name']:24} {entry['real_size']:8} {entry['stored_size']:8} {entry['attributes']:04X} {entry['load']:08X}")
        with open(os.path.join(output, "files", entry["name"]), "wb") as handle:
            handle.write(image.file_data(entry))
    with open(os.path.join(output, "manifest.txt"), "w") as handle:
        handle.write("\n".join(lines) + "\n")


main()
