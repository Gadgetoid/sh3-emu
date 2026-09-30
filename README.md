# velo-emu

A minimal emulator for the Philips Velo 1 (1997, Windows CE 1.0 Handheld PC), in a screen-only window with the SHAM-7X0 green backlit LCD simulation.

It boots the stock CE 1.0 ROM through the setup wizard (touch calibration, time zone, owner details) to the desktop.

## ROM

Not included. It expects the 7,799,876-byte `nk.bin` from CERF's `philips_velo_1_ce1` bundle (VA 0x9F400000-0x9FB70444). Link or copy it to `rom/nk.bin`.

The emulator patches one instruction in the loaded ROM, not the file. In CE 1.0's `fatfs.dll`, the function that sizes a direct multi-sector write computes the bytes left in a contiguous cluster run as `run_end - (pos - run_start)` instead of `run_end - pos` (`subu $2, $25, $3` at 0x9F5B4FCC). A large write into a fragmented card then runs past the end of the run, over other files' clusters. The patch uses `pos` (`subu $2, $25, $5`), and only applies if the original word is there.

### Windows CE 2.0

The Velo 1's CE 2.0 upgrade shipped as a ROM Miniature Card. CERF's `philips_velo_1_ce2` bundle has its `nk.bin` (4,185,248 bytes): pass it in place of the CE 1.0 ROM. An `nk.bin` whose single ROM header spans the whole file is mapped at the header's `physfirst` (0x90001000, the card window at physical 0x10000000) and started there, as the Velo's boot block would hand off to the card; the first 4 KB of the card, missing from the dump, reads as erased flash. A B000FF image (`B000FF\n`, then records of address, length, byte-sum checksum and data, ending with the entry) also loads, with records in the card window and the internal ROM window at 0x1F400000. It reaches the CE 2.0 desktop in about 20 seconds, with the LCD in 16 greys (4 bpp). The upgrade asks for 12 MB, so use `--memory=20` (4 MB and the 16 MB DRAM card).

The ROM's shortcuts and desktop icons point at `\Storage Card`, CE 2.0's name for the PC Card, where the upgrade kept the Microsoft applications. Insert a CompactFlash image holding them with `--card=IMAGE`. Reset (Start > Run, `reset`, or after an install) jumps to the MIPS reset vector at 0xBFC00000, the Velo's boot block, which isn't in either dump. The emulator's boot block does a warm reset: the machine restarts from the ROM's entry with RAM kept, so CE keeps its object store and loads newly installed drivers. The desktop connection doesn't work yet. The stock ROM lacks `rapisrv.exe`, and CE 2.0 reports "Out of Memory" when the cable is connected. With it installed, PPP comes up and CE 2.0 connects to port 5679 with its device information; the emulator answers as SynCE's dccm does (a ping), and RAPI works, but CE 2.0 closes the link about three seconds later.

`make test` runs a few CE 2.0 checks when `rom/ce2/nk.bin` exists, or with `make test CE2_ROM=PATH`.

## Build

```
brew install sdl3 libslirp
make         # velo and velo-rapi
make run
make headless
make test    # framebuffer hashes at the wizard, desktop, suspend/resume, a card listing and 16 and 32 MB System info; a saved state resuming identically; PPP up; the web proxy's rewriting, image conversion and a page through it in Pocket IE; a file round trip, a folder sync, the proxy and 115200 setup and a `.load` script over RAPI; a large write to a fragmented card; backlight key
```

## Use

```
./velo rom/nk.bin
./velo --screenshot=out.bmp --seconds=8 rom/nk.bin
./velo --state=powertoys.bin rom/nk.bin
./headless rom/nk.bin --seconds=12 --key=8:4B --tap=12:240:120 --pgm=out.pgm
./headless rom/nk.bin --seconds=3 --load=state.bin --save=state.bin
./headless rom/nk.bin --seconds=10 --load=state.bin --power=2 --power=6 --wav=out.wav
```

The mouse is the stylus. The touch panel reports what a real Velo does, so CE 2.0's built-in calibration is right before it is recalibrated; states saved by older builds keep the readings they were calibrated with. Hold it on each calibration target for about half a second. Host keys map to the Velo keyboard. `--verbose` logs unmodelled register accesses and dumps CPU state on exit.

Menus:

| Menu | Item | Shortcut |
|---|---|---|
| Run | Power Button (suspend and resume) | Cmd-Shift-P |
| Run | Pause | Cmd-P |
| Run | Reset (cold boot, clears RAM) | Cmd-R |
| Run | Save State | Cmd-S |
| Run | Load State | Cmd-L |
| Run | Show Saved State in Finder | |
| Card | Insert Card Image… | Cmd-O |
| Card | Eject Card | Cmd-E |
| Serial | Network (PPP) | Cmd-Shift-N |
| Serial | Pseudo-terminal | |
| Serial | Disconnect | |
| Desktop | Send Files to Velo… (into \My Documents) | |
| Desktop | Copy My Documents to Mac… | |
| Desktop | Shared Folder…, Sync Shared Folder Now, Stop Sharing Folder | |
| Desktop | Set Up Pocket IE Proxy | |
| Desktop | Desktop Connection Speed: 19200 (original), 38400, 57600, 115200 | |
| Emulation | Backlight (presses the Velo's backlight key) | Cmd-B |
| Emulation | Sound | |
| Emulation | Memory (after Reset): 4 MB (original), 8 MB, 16 MB, 20 MB (4 MB + 16 MB DRAM card), 32 MB (16 MB + 16 MB DRAM card) | |
| Emulation | CPU Speed: 1x (original), 2x, 4x, 8x | |

The backlight is under CE's control: the Backlight key toggles it, and the Backlight control panel's idle timeout turns it off (30 seconds by default, since the Velo reports external power). The checkmark shows its state.

## PC Card storage

The PC Card slot takes a CompactFlash (ATA) card backed by a raw disk image. CE mounts it as `\PC Card`. Make one, optionally copying folders onto it:

```
tools/mkcard.sh card.img 32 ~/Downloads/PYTHON
./velo --card=card.img rom/nk.bin
```

Insert it with Card > Insert Card Image… or `--card=IMAGE`; the image path is kept in the saved state. Inserting over a card ejects the old one and inserts the new one a second later, so CE sees the change. To change its contents on the Mac, eject it first, then `hdiutil attach -imagekey diskimage-class=CRawDiskImage card.img`.

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

### Desktop connection

With PPP up, CE connects to the desktop at 10.0.2.2 port 5679, sends four zero bytes and closes the connection, as CE 1.0 does with Handheld PC Explorer. The emulator answers it (with nothing listening, CE shows "Cannot start communications with the desktop computer", Error 10061, after about five minutes). The desktop then reaches the Velo with RAPI, CE's remote API, on its port 990. The emulator makes that port available on the Mac as a Unix socket, `$XDG_DATA_HOME/velo-emu/rapi.sock` (default `~/.local/share/velo-emu`), with no TCP port. The protocol follows [SynCE](https://sourceforge.net/projects/synce/)'s librapi2.

The Desktop menu uses it:

- Send Files to Velo… copies files into `\My Documents`.
- Copy My Documents to Mac… copies `\My Documents`, with its folders, into a Mac folder.
- Shared Folder… pairs a Mac folder with `\My Documents` and syncs them each time the Velo connects, or with Sync Shared Folder Now. A file changed on one side is copied to the other. A file deleted on one side, and unchanged on the other since the last sync, is deleted there too: on the Mac it goes to the Trash. When both sides changed a file, the Mac keeps its copy and the Velo's comes over as `name (Velo).ext`. Uploads that don't fit in the Velo's free storage are skipped. Empty folders aren't removed. The pairing is kept as `shared_folder=` in `emu.ini`, and the last sync's state in `sync-manifest.txt` next to `rapi.sock`.

`velo-rapi` does the same from the command line, while the emulator is running with Network (PPP) connected:

```
./velo-rapi info
./velo-rapi ls
./velo-rapi put notes.txt
./velo-rapi get Samples/Letter.pwd
./velo-rapi run /Windows/pword.exe
./velo-rapi sync ~/Velo
```

Velo paths are relative to `\My Documents` unless they start with `/` or `\`; both separate folders. It also has `rm`, `mkdir`, `rmdir`, `mv`, `load SCRIPT [DEST]`, `proxy on|off`, `baud RATE`, and `reg ls|dump|get|set` for the registry (keys start with `HKCU`, `HKLM`, `HKCR` or `HKU`, for example `./velo-rapi reg dump HKCU/Software/Apps/PocketIE`). `--socket=PATH` picks another socket, for headless runs with `--rapi=PATH`.

CE's desktop connection runs at 19200 baud, about 1.6 KB/s. Desktop > Desktop Connection Speed, or `velo-rapi baud 115200`, adds a hidden `` `Desktop @ 115200` `` connection to the Velo's registry and makes it the PC Connection; the menu then reconnects the cable, and with `velo-rapi` it applies from the next connection. 19200 goes back to CE's own. At 115200 the emulated CPU sets the pace: about 1.9 KB/s at CPU Speed 1x and 5.8 KB/s at 4x (8x is no faster).

### Installing CE 1.0 software

CE 1.0 programs were installed from Windows by H/PC Explorer, which ran a `.load` script for each package over RAPI. `velo-rapi load SCRIPT [DEST]` does the same: it copies files (taking the `.mips` build where there is one), creates folders and shortcuts, writes registry strings and starts programs. `.` in the script is the script's folder as a source, and DEST (default `\Program Files\Accessories`) as a destination.

For example, Microsoft's Power Toys 1.0 for CE 1.0 (Cascading Menus, Mute, Pocket Paint, sound schemes, wallpapers, control panel annunciators and Remote Control). `powtoy.exe` is in archive.org's [Windows CE 1.0 Programs](https://archive.org/details/windowsce1.0) collection. It is an InstallShield 3 package: extract the two embedded archives with [unshieldv3](https://github.com/wfr/unshieldv3), then run each component's script:

```
python3 -c 'import struct,sys; d=open("powtoy.exe","rb").read(); p=0xcc00
while True:
    n=struct.unpack_from("<I",d,p)[0]
    if not 0<n<260: break
    size=struct.unpack_from("<I",d,p+8+n)[0]; blob=d[p+12+n:p+12+n+size]; p+=12+n+size
    if blob[:4]==b"\x13\x5d\x65\x8c": open("part%d.Z" % p,"wb").write(blob)'
mkdir powertoys
unshieldv3 extract "$(ls -S part*.Z | head -1)" powertoys
for s in annun/Annunciator cascade/Cascade mute/Mute ppaint/Ppaint rcontrol/remotecontrol sound1/Analog sound2/Metallic sound3/Organic wall/Wallpaper; do
    ./velo-rapi load powertoys/$s.load
done
```

Cascading Menus and Mute start straight away in the taskbar, Paint is in Programs > Accessories, the schemes are in Volume & Sounds and the wallpapers in Display. Remote Control needs its Windows desktop half.

### Web proxy

Pocket IE can't talk to modern HTTPS. The network has a web proxy at 10.0.2.4 port 8080 that fetches pages with libcurl on the Mac. Desktop > Set Up Pocket IE Proxy, or `velo-rapi proxy on`, sets it in the Velo's registry for Pocket IE's next start. By hand: in Pocket IE, View > Options > Proxy Server, tick Use Proxy Server, enter `10.0.2.4` and port `8080`, and press Enter. Either way it's kept in the saved state. Only Pocket IE's requests use it; other traffic is unaffected, and it opens no port on the Mac.

Type addresses as `http://`: Pocket IE makes `https://` connections itself, not through the proxy, and they fail. For `http://` addresses without a port the proxy tries HTTPS first, then plain HTTP. Before a response reaches the Velo it:

- rewrites `https://` links and redirects to `http://`, so they come back through the proxy
- removes `<script>`, `<style>`, `<svg>` and comments, which Pocket IE would show as text
- converts UTF-8 text to Windows-1252 and drops the charset
- drops `Secure` from cookies and maps 303, 307 and 308 redirects to 301 and 302
- turns PNG, JPEG, GIF, BMP and SVG images into four-grey dithered GIFs, drawn at the size the page's `<img width height>` gives (it remembers these from the page) and at most 436 pixels wide, the widest Pocket IE shows unscaled. SVG `<use>` references are expanded. Other formats, such as WebP, pass through unchanged.

It sends a Lynx user agent upstream in place of Pocket IE's `Mozilla/1.1 (compatible; MSPIE 1.1; Windows CE)`, which some sites block (Cloudflare error 1010). Sites generally serve text browsers their simplest pages, such as Google's basic HTML results. Set it with `user_agent=` in `emu.ini` or `--user-agent=TEXT` (also in headless); an empty value passes Pocket IE's own through.

Through the proxy, `127.0.0.1` is the Mac's loopback. It needs libcurl (part of macOS).

## Memory and speed

Memory sets the RAM for the next cold boot (Run > Reset, which clears the machine) or `--memory=`. CE sizes the built-in RAM at boot and uses at most 16 MB of it. Beyond that, 20 MB and 32 MB add a 16 MB DRAM Miniature Card in slot 1, the Velo's own memory expansion: CE reads its ID EEPROM and maps it as a second RAM region, reporting 20,348 KB and 32,636 KB, split between storage and programs in Control Panel > System > Memory. A saved machine keeps the memory it was booted with (a 32 MB save is about 33 MB).

CPU Speed runs that many instructions per 36.864 MHz clock tick; `--speed=` does the same. Timers, the RTC, the LCD frame rate, sound and serial stay on the real clock, so only the CPU gets faster: at 4x CE reaches the setup wizard in 2 seconds instead of 4. At 1x one instruction takes one clock.

Both are remembered in `$XDG_CONFIG_HOME/velo-emu/emu.ini` (default `~/.config/velo-emu/emu.ini`), with `user_agent=` and `shared_folder=`.

## Saved state

The machine is saved to `$XDG_DATA_HOME/velo-emu/state.bin` (default `~/.local/share/velo-emu`) on quit, every minute, and by Save State. On launch it is restored with the RTC advanced by the time away; `--fresh` ignores it. Load State returns to the last save. A state only loads with the ROM it was made with. States are stored as named records, so ones from older builds load, with any new fields at their power-on defaults. A state that can't be read is moved to `state.bin.old`. If a serial cable was connected when the state was saved, the restored machine sees it unplugged and, two seconds later, plugged back in (same mode), so CE redials rather than trusting a PPP session the Mac side no longer has. `--state=FILE` uses FILE instead, for loading, saving and autosaving.

The menus are native on macOS; other platforms build without them.

Headless options:

- `--key=SECONDS:SCANCODE` presses a Velo scancode (hex) for 50 ms. The backlight key is 5E.
- `--tap=SECONDS:X:Y[:HOLD]` holds the pen at a screen position, for 500 ms by default. Use 0.08 for double taps.
- `--power=SECONDS` presses the power button for 200 ms.
- `--wav=FILE` writes the sound output, with the silences between sounds removed.
- `--card=IMAGE` inserts a card image, after `--load`.
- `--net=SECONDS` connects COM1 to the PPP gateway. `--serial=SECONDS` connects a bare cable, and `--serial-send=SECONDS:TEXT` sends bytes. Anything CE transmits is printed.
- `--type=SECONDS:TEXT` types text (US layout, `\n` for Enter).
- `--memory=MB`, `--speed=N`, `--backlight=SECONDS` (press the backlight key), `--user-agent=TEXT`.
- `--rapi=SOCKET` makes the Velo's RAPI port available at SOCKET. A loaded state starts with the cable unplugged, so `--net` reconnects it.
- `--realtime[=N]` holds the machine to N times real time (default 1). Unpaced, an idle Velo runs about 1000 times faster than real time, which outpaces anything driving it over RAPI; the tests use `--realtime=10`. SIGTERM or SIGINT ends the run early and still writes `--save`, `--pgm` and `--wav`.

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
  - Miniature Card slot 2 empty; slot 1 empty or a 16 MB DRAM card with its I2C ID EEPROM (MFIO 18/20)
  - LCD panel power on MFIO 17 (active low) and the backlight on MFIO 25
  - M-Module IT8368E PC Card socket (card detect, power, reset, interrupt to the IR block's CARDET) and the PR31500 card windows
  - CompactFlash card in ATA mode (CIS, task file, PIO read and write)
  - UART A (COM1) with its circular receive DMA, CTS on MFIO 30 and DCD on IO 4 (active low)

Guest time is the instruction count at 36.864 MHz, so headless runs are deterministic. Registers that aren't modelled read back the last value written.

Not emulated: sound input, UART B, IrDA, other PC Cards, and Miniature Cards.

## Credits

Peripheral behaviour, the memory map and the keyboard table follow [CERF](https://github.com/gweslab/cerf) (MIT, `licences/MIT-CERF.txt`). The LCD simulation is from SHAM-7X0. The web proxy decodes images with [stb_image](https://github.com/nothings/stb) (public domain) and [nanosvg](https://github.com/memononen/nanosvg) (zlib, `licences/Zlib-nanosvg.txt`).
