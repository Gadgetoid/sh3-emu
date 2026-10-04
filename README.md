# sh3-emu

An emulator for Windows CE on the Hitachi SH-3, for testing an SH3 port of a Windows CE toolchain. It's a hard fork of velo-emu, the Philips Velo 1 (MIPS) emulator, with the MIPS core replaced by an SH-3 core and the Velo board by Microsoft's Odo reference board with the SH3 CPU module.

It boots a Windows CE 2.11 image built from Platform Builder 2.11 for Odo SH3 to its shell, with the 480x240 2 bpp display, PS/2 keyboard and touch panel working. It runs without a window only, as `headless`.

## ROMs

No ROMs are included. The target image is a Platform Builder 2.11 `nk.bin` for the Odo platform, `_TGTCPU=SH3`, project MAXALL, built as a RAM image (loaded at 8C600000). Platform Builder 2.11 ships the Odo board support package with its source, and the SH3 kernel and OAK libraries, but no prebuilt SH3 image. Its build tools run under Wine. Two changes to the stock tree are needed for a usable image:

- `PUBLIC/COMMON/OAK/MISC/SRCGEN1.BAT`: under Wine, `nmake` hands the batch file an argument with a trailing line break, which breaks `sources.gen`. Use `%_PROJECTROOT%\cesysgen` in place of `%3`.
- `PLATFORM/ODO/KERNEL/HAL/MDPPFS.C`: set `NoPPFS = TRUE` at the top of `OEMParallelPortGetByte` and `OEMParallelPortSendByte`. Without a parallel port cable to a host, each PPFS access otherwise spins for seconds before timing out.

Put the image at `rom/odo-sh3.bin` for the tests. For `rom/odo-sh3-debug.bin`, also build debugmgr as an Odo platform component (a `PLATFORM/ODO/TEST/DEBUGMGR` directory with a `SOURCES` file, `debugmgr.c` with its `host_call` replaced by the SH helper below, and the helper as `SHX/HOSTCALL.SRC`), and list it in `PLATFORM/ODO/FILES/PLATFORM.BIB` under MODULES as `debugmgr.exe $(_FLATRELEASEDIR)\debugmgr.exe NK S`.

## Running

```
make
./headless rom/odo-sh3.bin --seconds=3 --debug-output --png=calibration.png
./headless rom/odo-sh3.bin --seconds=22 --tap=4:240:120 --tap=6:48:24 --tap=8:48:216 --tap=10:432:216 --tap=12:432:24 --key=15:5A --save=desktop.state
./headless rom/odo-sh3.bin --load=desktop.state --seconds=12 --key=1:11+0D --tap=3:71:198 "--type=5:cmd\n" --png=cmd.png
./headless --help
```

The first boot shows touch calibration. The Odo driver's five targets are the centre and points 1/10 of the screen in from each corner, so the taps above calibrate it, and Enter (5A) accepts. MAXALL's shell is a wallpaper with Task Manager on Alt-Tab (`11+0D`), whose Run button starts programs such as `cmd`.

`--key` takes PS/2 set 2 scancodes in hex, joined by `+` for a chord; codes from 80 up are sent with the E0 prefix. `--type` types text with `\n` for Enter. `--debug-output` prints the kernel's debug serial port. `--trace-exceptions` logs CPU exceptions other than TLB misses and CE's system call traps. `--pgm` and `--png` save the screen, and `--save` and `--load` keep the machine's state. Runs are deterministic.

## Debugging and file transfer

The host mailbox and GDB stub are velo-emu's, ported to the SH-3.

- Host mailbox: a user-mode `trapa #0xCE` with r4 = operation (0 probe, 1 receive, 2 send), r5 = buffer and r6 = length returns its result in r0, and the probe's maximum message size in r1. `--agent=SOCKET` passes the messages to one client on a Unix socket, framed as a little-endian u32 length and the message. A guest helper:

  ```
  _host_call:            ; int host_call(int operation, void *buffer, int length, int *extra)
  	mov.l	r7, @-r15
  	trapa	#h'CE
  	mov.l	@r15+, r7
  	tst	r7, r7
  	bt	done
  	mov.l	r1, @r7
  done:
  	rts
  	nop
  ```

- velo-toolchain's guest agent, debugmgr, builds for SH3 with Platform Builder's compiler using that helper. With it running on the device, file transfer, program start and kill work over `--agent`, and through the GDB stub.
- `--gdb=PORT` serves GDB's remote protocol. The target description names the `sh3` architecture, and the `g` packet follows GDB's SH-3 register layout (r0-r15, pc, pr, gbr, vbr, mach, macl, sr, unused FPU slots, ssr, spc and both banks of r0-r7). Breakpoints, watchpoints and single steps are the emulator's own, not code patches. `monitor processes` and `monitor modules` read CE 2.11's process and module lists. In extended mode (`target extended-remote`), `remote put` and `remote get`, `set remote exec-file` with `starti` or `run`, and `kill` go through debugmgr.

```
./headless rom/odo-sh3-debug.bin --load=debugmgr.state --gdb=1234
gdb -ex "set architecture sh3" -ex "target extended-remote :1234" -ex 'set remote exec-file \Windows\cmd.exe' -ex starti
```

## What's emulated

- CPU (`src/core/sh3.c`): the SH-3 instruction set, little-endian, with delay slots; banked registers, SR.MD/RB/BL; exceptions, TRAPA and interrupts through VBR+0x100/0x400/0x600 with EXPEVT, INTEVT, TRA, SPC and SSR; the MMU with the 128-entry 4-way UTLB, 1 KB and 4 KB pages, ASIDs, shared pages, MMUCR (AT, IX, TF, RC, SV), LDTLB, TLB miss, invalid, protection and initial page write exceptions, and the memory-mapped TLB arrays; SLEEP. No FPU or DSP.
- On-chip peripherals (`src/core/sh7709.c`), SH7708 and SH7709: the INTC (IRL levels, IPRA to IPRE, IRQ0-5 on the SH7709), TMU channels 0-2 with underflow interrupts, the RTC (BCD counters, 64 Hz counter, alarm, periodic and carry interrupts), SCI, the two SH7709 SCIFs, and register storage for the BSC, CPG, WDT, CCR and the SH7709 ports. The cache isn't modelled.
- Odo board (`src/core/machine.c`): 16 MB DRAM at 0x0C000000 (32 or 64 with `--memory`), the system ASIC at 0x10000000 (interrupt status and mask on IRL level 4, debug serial port output, the 480x240 2 bpp display DMA, the PS/2 keyboard, the touch and sound block with the UCB register interface and pen timer, empty PC Card sockets), and the housekeeping FPGA LEDs.

Guest time is the instruction count at 58.98 MHz, with the peripheral clock at 14.75 MHz.

Not yet: sound output, serial ports to the host, PC Cards, suspend and the parallel port (PPFS). The GUI app (`make velo`) builds against the SH3 machine but is untested, and its menus still offer Velo features.

## Testing

- `make check` needs no ROMs: the command lines and the web proxy.
- `make test` runs the CPU tests, boots `rom/odo-sh3.bin` (or `make test ROM=PATH`) through calibration to the desktop and the console comparing framebuffer hashes, and checks the GDB stub. With `rom/odo-sh3-debug.bin` (`DEBUG_ROM=PATH`), an image that also has `debugmgr.exe`, it starts debugmgr from Task Manager and checks GDB's file transfer, run, step and kill through it.
- `tests/sh3/run.sh` assembles `tests/sh3/*.s` with an `sh-elf` binutils (`SH_PREFIX`) and runs them on `sh3-run`, a bare harness for the core: exceptions, banks, user mode and the MMU.
- `make sh3-fuzz` compares random user-mode instruction streams between `sh3-run` and a reference, `qemu-sh4` by default; `SH_REFERENCE=HOST:qemu-sh4` runs it on another machine over ssh. qemu 10.2 gets T wrong after ROTL and ROTR and DIV1 by zero, so the fuzzer avoids those.

## Building

macOS: `brew install sdl3 libslirp`, then `make`. Debian or Ubuntu: `sudo apt install build-essential pkg-config libsdl3-dev libslirp-dev libcurl4-openssl-dev zlib1g-dev`, then `make`.

## Licence

MIT, see `LICENSE`. ROMs and Windows CE software are not included.

## Credits

The SH-3 core and on-chip peripherals are written from Hitachi's SH-3 and SH7708/SH7709 hardware manuals and Microsoft's SH-3 reference; no emulator code was copied. The Odo system ASIC's register behaviour follows the Odo board support package in Platform Builder 2.11 and CERF's Odo ARM720 board (MIT, `licences/MIT-CERF.txt`), which shares the ASIC. velo-emu's tools and the rest of this tree also draw on [CERF](https://github.com/gweslab/cerf), [stb_image and stb_truetype](https://github.com/nothings/stb) (public domain) and [nanosvg](https://github.com/memononen/nanosvg) (zlib, `licences/Zlib-nanosvg.txt`).
