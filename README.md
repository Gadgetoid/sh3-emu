# velo-emu

A minimal emulator for the Philips Velo 1 (1997, Windows CE 1.0 Handheld PC), in a screen-only window with the SHAM-7X0 green backlit LCD simulation.

It boots the stock CE 1.0 ROM through the setup wizard (touch calibration, time zone, owner details) to the desktop.

## ROM

Not included. It expects the 7,799,876-byte `nk.bin` from CERF's `philips_velo_1_ce1` bundle (VA 0x9F400000-0x9FB70444). Link or copy it to `rom/nk.bin`.

## Build

```
brew install sdl3 libslirp
make
make run
make headless
make test    # framebuffer hashes at the wizard, desktop, suspend/resume and a card listing; a saved state resuming identically; PPP coming up
```

## Use

```
./velo rom/nk.bin
./velo --screenshot=out.bmp --seconds=8 rom/nk.bin
./headless rom/nk.bin --seconds=12 --key=8:4B --tap=12:240:120 --pgm=out.pgm
./headless rom/nk.bin --seconds=3 --load=state.bin --save=state.bin
./headless rom/nk.bin --seconds=10 --load=state.bin --power=2 --power=6 --wav=out.wav
```

The mouse is the stylus. Hold it on each calibration target for about half a second. Host keys map to the Velo keyboard. `--verbose` logs unmodelled register accesses and dumps CPU state on exit.

Menus:

| Menu | Item | Shortcut |
|---|---|---|
| Run | Power Button (suspend and resume) | Cmd-Shift-P |
| Run | Pause | Cmd-P |
| Run | Reset (cold boot, clears RAM) | Cmd-R |
| Run | Save State | Cmd-S |
| Run | Load State | Cmd-L |
| Card | Insert Card Image… | Cmd-O |
| Card | Eject Card | Cmd-E |
| Serial | Network (PPP) | Cmd-Shift-N |
| Serial | Pseudo-terminal | |
| Serial | Disconnect | |
| Emulation | Backlight (presses the Velo's backlight key) | Cmd-B |
| Emulation | Sound | |

The backlight is under CE's control: the Backlight key toggles it, and the Backlight control panel's idle timeout turns it off (30 seconds by default, since the Velo reports external power). The checkmark shows its state.

## PC Card storage

The PC Card slot takes a CompactFlash (ATA) card backed by a raw disk image. CE mounts it as `\PC Card`. Make one, optionally copying folders onto it:

```
tools/mkcard.sh card.img 32 ~/Downloads/PYTHON
./velo --card=card.img rom/nk.bin
```

Insert it with Card > Insert Card Image… or `--card=IMAGE`; the image path is kept in the saved state. To change its contents on the Mac, eject it first, then `hdiutil attach -imagekey diskimage-class=CRawDiskImage card.img`.

To install Python CE 1.0b1, copy `Python.exe` and `PYTHON15.DLL` from the card to `\Windows` in Explorer (View > Options > Show all files to see the DLL), then run `python` from Start > Run.

## Serial and networking

Serial > Network (PPP), or `--serial=net`, plugs COM1 into a built-in PPP server on a libslirp user-mode network. Connecting the cable starts CE's own desktop connection: CE sends `CLIENT`, the emulator answers `CLIENTSERVER`, and PPP comes up with the Velo at 10.0.2.15, the Mac at 10.0.2.2 and DNS at 10.0.2.3. The connection icon appears in the taskbar and CE's sockets reach the Mac and the internet (outgoing only). For example, in Python CE:

```
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(('10.0.2.2', 47123))
```

connects to port 47123 on the Mac's loopback.

Serial > Pseudo-terminal, or `--serial=pty`, puts COM1 on a pty and prints its path (for example `/dev/ttys002`) on stderr and in the title bar, for your own terminal or PPP tools.

Without libslirp the build still works, with no Network (PPP) option.

## Saved state

The machine is saved to `$XDG_DATA_HOME/velo-emu/state.bin` (default `~/.local/share/velo-emu`) on quit, every minute, and by Save State. On launch it is restored with the RTC advanced by the time away; `--fresh` ignores it. Load State returns to the last save. A state only loads with the ROM it was made with. States are stored as named records, so ones from older builds load, with any new fields at their power-on defaults. A state that can't be read is moved to `state.bin.old`.

The menus are native on macOS; other platforms build without them.

Headless options:

- `--key=SECONDS:SCANCODE` presses a Velo scancode (hex) for 50 ms. The backlight key is 5E.
- `--tap=SECONDS:X:Y[:HOLD]` holds the pen at a screen position, for 500 ms by default. Use 0.08 for double taps.
- `--power=SECONDS` presses the power button for 200 ms.
- `--wav=FILE` writes the sound output, with the silences between sounds removed.
- `--card=IMAGE` inserts a card image, after `--load`.
- `--net=SECONDS` connects COM1 to the PPP gateway. `--serial=SECONDS` connects a bare cable, and `--serial-send=SECONDS:TEXT` sends bytes. Anything CE transmits is printed.
- `--type=SECONDS:TEXT` types text (US layout, `\n` for Enter).

## What's emulated

- CPU: MIPS-I interpreter with the TX39 CP0, 32-entry TLB and branch-likely instructions (`src/mips.c`).
- PR31500 (`src/machine.c`):
  - interrupt controller, including the high-priority encoder
  - periodic timer, RTC and alarm
  - power, with STOPCPU idle, the stop timer and suspend (clock stop, resume in place, woken by the power button or an enabled interrupt)
  - LCD controller (2bpp and VIDEO_CTL7 shade map)
  - SPI
  - SIB subframe 0 and sound transmit DMA (half and end interrupts, 16-bit high byte first)
  - I/O and MFIO
- Velo 1 board:
  - 4MB DRAM
  - keyboard controller enable packet and scancodes
  - UCB1100 touch and battery ADC
  - debug module probe
  - M-Module (IT8368) ID
  - Miniature Card slots reporting empty, and serial DCD off
  - LCD panel power on MFIO 17 (active low) and the backlight on MFIO 25
  - M-Module IT8368E PC Card socket (card detect, power, reset, interrupt to the IR block's CARDET) and the PR31500 card windows
  - CompactFlash card in ATA mode (CIS, task file, PIO read and write)
  - UART A (COM1) with its circular receive DMA, CTS on MFIO 30 and DCD on IO 4 (active low)

Guest time is the instruction count at 36.864 MHz, so headless runs are deterministic. Registers that aren't modelled read back the last value written.

Not emulated: sound input, UART B, IrDA, other PC Cards, and Miniature Cards.

## Credits

Peripheral behaviour, the memory map and the keyboard table follow [CERF](https://github.com/gweslab/cerf) (MIT, `licences/MIT-CERF.txt`). The LCD simulation is from SHAM-7X0.
