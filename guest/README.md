# Velo Guest Additions

Programs and drivers that run inside CE and only make sense under velo-emu, because they talk to its emulated hardware. velo-emu owns their source and build. velo-emu-ce-2.0 packages them as its `velo-emu-guest` patch set, for a card or the merged ROM.

`make guest` builds every component for CE 1.0 and 2.0 with velo-toolchain (`../velo-toolchain`, or `VELO_TOOLCHAIN=PATH`). Each component is a folder here with a `CMakeLists.txt` and a `<component>.reg`, and its output lands in:

```
build/guest/<component>/ce1/<files>
build/guest/<component>/ce2/<files>
```

next to a copy of `<component>.reg`. The `.reg` file lists the registry entries the component needs, in REGEDIT4 format, and its files go in `\Windows`.

| Component | Files | Registry | What it does |
| --- | --- | --- | --- |
| `vdisk` | `vdisk.dll` | `HKLM\Drivers\BuiltIn\VDisk` | Mounts the paravirtual disk's image as a FAT volume, following inserts and ejects |
