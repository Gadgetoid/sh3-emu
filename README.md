# velo-emu

A minimal emulator for the Philips Velo 1 (1997, Windows CE 1.0 Handheld PC), in a screen-only window with the SHAM-7X0 green backlit LCD simulation.

It boots the stock CE 1.0 ROM through the setup wizard (touch calibration, time zone, owner details) to the desktop.

## ROM

Not included. It expects the 7,799,876-byte `nk.bin` from CERF's `philips_velo_1_ce1` bundle (VA 0x9F400000-0x9FB70444). Link or copy it to `rom/nk.bin`.

## Build

```
brew install sdl3
make
make run
make headless
make test    # framebuffer hashes at the wizard and the desktop, and a saved state resuming identically
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
| Emulation | Backlight (presses the Velo's backlight key) | Cmd-B |
| Emulation | Sound | |

The backlight is under CE's control: the Backlight key toggles it, and the Backlight control panel's idle timeout turns it off (30 seconds by default, since the Velo reports external power). The checkmark shows its state.

The machine is saved to `$XDG_DATA_HOME/velo-emu/state.bin` (default `~/.local/share/velo-emu`) on quit, every minute, and by Save State. On launch it is restored with the RTC advanced by the time away; `--fresh` ignores it. Load State returns to the last save. A state only loads with the ROM it was made with.

The menus are native on macOS; other platforms build without them.

Headless options:

- `--key=SECONDS:SCANCODE` presses a Velo scancode (hex) for 50 ms. The backlight key is 5E.
- `--tap=SECONDS:X:Y[:HOLD]` holds the pen at a screen position, for 500 ms by default. Use 0.08 for double taps.
- `--power=SECONDS` presses the power button for 200 ms.
- `--wav=FILE` writes the sound output, with the silences between sounds removed.

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

Guest time is the instruction count at 36.864 MHz, so headless runs are deterministic. Registers that aren't modelled read back the last value written.

Not emulated: sound input, serial, IrDA, and PC Card and Miniature Cards.

## Credits

Peripheral behaviour, the memory map and the keyboard table follow [CERF](https://github.com/gweslab/cerf) (MIT, `licences/MIT-CERF.txt`). The LCD simulation is from SHAM-7X0.
