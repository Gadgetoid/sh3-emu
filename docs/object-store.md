# Windows CE 1.0 and 2.0 object store

Layout of the RAM object store as found in Velo 1 memory, as read by `tools/velo_state.c`. Derived from saved states, ROM images and RAPI dumps; not from Microsoft documentation. All values little-endian. "u16 units" are UTF-16 code units.

## Locating the store

The kernel's `fslog_t` sits on a page boundary at ROMHDR `ulRAMFree`: PA 0x37000 on the CE 2.0 ROM, 0x20000 on CE 1.0. Find it by scanning DRAM pages for:

| Offset | Size | Field |
|---|---|---|
| 0x00 | u32 | version, 1 |
| 0x04 | 8 | `EKIMEKIM` ("MIKEMIKE" as two u32) |
| 0x10 | u32, u32 | store section 0: VA, length |
| 0x20 | u32, u32 | store section 1: VA, length |
| 0x30, 0x40 | | program memory sections, same form |
| 0x98 | u16 | database list head (CE 2.0: id of a type 0xa object; CE 1.0: first database) |
| 0x9a | u16 | CE 2.0: id of the registry roots object; CE 1.0: 0 |

Section VAs are kseg0. PA below 0x02000000 is main DRAM; from 0x02000000 it is DRAM card memory. Store offsets run through section 0, then section 1.

## Handles

| Store offset | Content |
|---|---|
| 0x000 | u32[]: heap offset of each handle block; entry 0 is 0, list ends at the next 0 |
| 0x100 | heap start |

A handle block is a heap record: +4 u32 block index, +8 u32 entry[1024]. Object id = block * 1024 + slot. Entry bit 0 set = live; `entry & 0xfffffc` = heap offset of the record. Bit 24 is a flag.

## Heap records

| Offset | Size | Field |
|---|---|---|
| 0 | u16 | body size, excluding this header |
| 2 | u8 | 0 |
| 3 | u8 | type in bits 0-3; bit 4 is a flag |
| 4 | | body |

Bodies are padded to 4 bytes. Types:

| Type | Object |
|---|---|
| 2 | handle block |
| 3 | file info |
| 4 | folder |
| 5 | file |
| 6 | file data chunk |
| 7 | database |
| 8 | database record |
| 0xa | database list (CE 2.0) |
| 0xb | registry roots (CE 2.0) |
| 0xc | registry key (CE 2.0) |
| 0xd | registry value (CE 2.0) |

All ids below are u16.

### Folder (4) and file (5)

| Offset | Size | Field |
|---|---|---|
| 0 | u16 | id |
| 2 | u16 | parent folder |
| 4 | u16 | next sibling |
| 6 | u16 | folder: first child; file: file info id, 0 when empty |
| 8 | u64 | FILETIME |
| 16 | u16 | attributes |
| 18 | u16 | name length, u16 units |
| 20 | | name, no terminator |

The root folder is id 0 with an empty name.

### File info (3)

| Offset | Size | Field |
|---|---|---|
| 0 | u16 | id |
| 2 | u16 | (size - 1) >> 12 |
| 4 | u16 | (size - 1) & 0xfff |
| 6 | u16 | owning file |
| 8 | u16[] | chunk ids, ((size - 1) >> 12) + 1 of them |

### Data chunk (6)

| Offset | Size | Field |
|---|---|---|
| 0 | u16 | id |
| 2 | u16 | flags: bits 0-7 = 1 compressed, 2 stored; bits 12-15 = padding bytes at the end of the record |
| 4 | | payload |

Each chunk holds 4096 bytes of the file, the last one the remainder.

### Registry roots (0xb, CE 2.0)

| Offset | Size | Field |
|---|---|---|
| 0 | u16 | id |
| 2 | u16 | first key of HKCR |
| 4 | u16 | first key of HKCU |
| 6 | u16 | first key of HKLM |
| 8 | u16 | first key of HKU |
| 10 | u16 | first value of HKCR |
| 12 | u16 | first value of HKCU |
| 14 | u16 | first value of HKLM |
| 16 | u16 | first value of HKU |

### Registry key (0xc, CE 2.0)

| Offset | Size | Field |
|---|---|---|
| 0 | u16 | id |
| 2 | u16 | next sibling |
| 4 | u16 | first subkey |
| 6 | u16 | first value |
| 8 | u8 | name length, u16 units |
| 9 | u8 | class length, u16 units |
| 10 | | name, then class |

### Registry value (0xd, CE 2.0)

| Offset | Size | Field |
|---|---|---|
| 0 | u16 | id |
| 2 | u16 | next value |
| 4 | u16 | REG_ type |
| 6 | u16 | name length, u16 units |
| 8 | u16 | data length, bytes |
| 10 | | name, no terminator, then data |

The default value is stored with the name `Default`.

## CE 2.0 compression

Used for compressed file chunks and for ROM module sections with o32 flag 0x2000.

| Offset | Size | Field |
|---|---|---|
| 0 | u24 | output size |
| 3 | u24[n] | end offset of each sub-block, from the start of the stream; n = size / 1024 + 1 |
| 3 + 3n | | sub-blocks |

Each sub-block expands to 1024 bytes (the last to the remainder) and stands alone. Stream: a flag byte, then 8 items, flag bits taken from bit 0 up. Clear bit: one literal byte. Set bit: a token, first byte `b`:

- `b & 0xf == 1`: 1 byte. Length 2, source = output position - ((b >> 4) + 2).
- `b & 0xf == 0`: 3 bytes. Source = u16 >> 4, length = third byte + 17.
- otherwise: 2 bytes. Source = u16 >> 4, length = (b & 0xf) + 1.

Source in the 2- and 3-byte forms is an absolute position in the sub-block's output. Copies may overlap their destination. Output the stream does not reach is zero.

## CE 1.0 compression

Used for compressed file chunks and ROM files. LZW: codes packed from bit 0 up, 9 bits wide to start, widening when the next free code reaches 1 << width, up to 12 bits. Code 0x100 resets the table and width; the first free code is 0x101. At 4096 entries the table stops growing.

A ROM file is prefixed with u32 block offsets, one per 4096 output bytes; bit 31 set means the block is stored. File chunks in RAM carry a bare LZW stream.

## CE 1.0 registry

CE 1.0 has no registry objects. The registry is the hidden system file `\Windows\Pegreg.reg`, made of 4096-byte blocks. filesys also caches blocks in its own heap.

Block header, 0x18 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `RGDB` |
| 4 | u32 | block size |

Records follow, each u16 size (including the 4-byte header) and u16 type. A record address is block << 12 | offset in block, which is its offset in the file.

| Type | Record |
|---|---|
| 2 | `CREG` file header, block 0 |
| 4 | key |
| 5 | value |

Key (4):

| Offset | Size | Field |
|---|---|---|
| 4 | u32 | first subkey |
| 8 | u32 | next sibling |
| 12 | u32 | first value |
| 16 | u32 | previous sibling, or parent for a first child; root keys: 0x08000000 + root |
| 20 | u8 | name length, bytes |
| 21 | u8 | class length, bytes |
| 24 | | name, then class |

Root keys are in block 0; root 0 to 3 are HKCR, HKCU, HKLM and HKU.

Value (5):

| Offset | Size | Field |
|---|---|---|
| 4 | u32 | next value |
| 8 | u8 | name length, bytes |
| 9 | u8 | REG_ type |
| 10 | u16 | data length, bytes |
| 12 | | name, then data |
