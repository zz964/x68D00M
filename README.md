# x68D00M - Optimized Doom port for accelerated Sharp x68000 computers.

My intent in creating this port was to create something nice that could take advantage of
Sharp x68000 CPU accelerators, because as of this posting there are very few pieces of 
software designed for those accelerators.

I consider the game playable on an 060turbo, though not exactly great.  It runs about like 
a low end 486.  The CPU is plenty fast enough for this game, but the GVRAM is most certainly not.  
On an x68030, the GVRAM can only be written at 4.7MB/s, and it takes almost 27ms to  
send the 320x168 viewport pixels of Doom to the GVRAM since each pixel in GVRAM is 16-bit.  
That's almost all of the 33ms budget for 30fps.  The 68060 is a competant CPU for a game  
like Doom, but it's not THAT competant.

PhantomX, on the other hand, when emulating a 400mhz 68030, can run the game actually  
pretty well.  Thanks to the GVRAM bottleneck, which is actually even higher with PhantomX,  
I still only get 18-22fps at high detail.  Fortunately it doesn't drop much in intense scenes  
and low detail brings it up to a locked 35fps.  Unfortunately, the game randomly crashes for me 
on PhantomX in an odd manner.  The music code, which runs from a regular interrupt, keeps going,  
but the main game loop just stops.  I've only ever seen the crash happen on PhantomX.  I  
haven't been able to solve this one yet and it may or may not even be my code's fault.  I doubt  
many programs push the PhantomX this hard.

NOTE: This port requires a 68030 or better CPU. The stock 68000 in most X68000 models is 
NOT fast enough to run Doom and I didn't compile for it and I didn't create compatible assembly. 
You need one of:

- An X68030 (Kindof playable-ish at min screen size + low detail)
- A very fast CPU accelerator card (060turbo, PhantomX, etc.)

Two binaries are provided:  
  doom.x      68030 build (X68030, Xellent30, PhantomX)  
  doom060.x   68060 build (060turbo, PhantomX 060 mode, xm6g)

This port is based on the X68000 Doom port by neozeed and neko68k, which was based  
on the original Doom source code released under the GNU GPL v2.

## Quick Start

1. IF you are using an 060turbo or xm6g in 060 mode, you need to set up the 060turbo.sys driver correctly. See below.
  Note that if you're using a PhantomX, you're better off in 030 mode.  Also see below.
2. Make sure hiocs.x is running, as well as cache.x ON.  I run these from autoexec.bat
3. Copy doom.x (or doom060.x) and your WAD file to the same directory on your Sharp x68000.
4. (Optional) If you want more than one sound effect to be playable at a time, run pcm8a, or if you have a Mercury Unit, run pcm8pp.
5. From the Human68k command line:
  doom.x  
   Or with a specific IWAD:  
     doom.x -iwad tnt.wad

## Features

Improvements over the base neozeed/neko68k X68000 Doom port:

### Performance

In XM6g 68060 50mhz mode, I was able to get the 11fps from the original port up to 25fps purely through optimizations.

- 68060 FPU-accelerated FixedMul/FixedDiv (inline, ~6 cycles each)
- 68030 native MULS.L 32x32->64 FixedMul (~28 cycles, replaces slow library call)
- Hand-optimized 68060 assembly for column and span rendering
- Inline FPU in plane renderer and wall texture loop (68060)
- 16-bit pre-remapped framebuffer (screens16) eliminates per-pixel palette lookup during blit
- Minimal blit: smaller viewports blit fewer rows, low detail skips columns, status bar only reblits on change
- 060turbo HIMEM support for zone heap, framebuffers, and lookup tables
- A few other things I forgot.

### Video

- Three selectable video modes: 15kHz, 25kHz, 31kHz
- Double-buffered vsync with GVRAM scroll register flipping. (Costs FPS.  Don't use this unless you have a very powerful setup and can't stand even a little bit of tearing.)
- FPS counter with vsync indicator (V suffix)
- Working Low detail rendering mode

### Sound Effects

- Multi-channel sound via PCM8A (software mixer, up to 8 simultaneous sounds)
- Mercury Unit V4 support via PCM8PP (hardware volume, 8 channels, best quality)
- SFX volume slider with perceptual distance falloff curve when using Mercury Unit
- Auto-detection of PCM driver at startup

### Music

- FM OPL2 emulation via YM2151 with GENMIDI patch conversion (poor quality, sorry)
- FM General MIDI emulation via YM2151 (poor quality, sorry)
- External MIDI output via YM3802 UART (auto-detected) <- Use this!
- Music mode selector in sound menu

### Input

- Full mouse support (fire, strafe, sensitivity slider)
- Mouse walk on/off toggle
- WASD keys for movement/strafing
- Keyboard remapping via config file

## Video Modes

Three video modes are available from the Options menu:


| Mode         | Description                                                                                                                                                                       |
| ------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 15 KHZ 61 HZ | x68k Standard 15kHz progressive. Best for real x68k monitors.                                                                                                                     |
| 25 KHZ 55 HZ | Non-standard ~25kHz line-doubled mode. Fills more of the screen but not all monitors support it. I borrowed the idea for this from BCC's recent Doom8088st_x68k port. Thanks BCC. |
| 31 KHZ 55 HZ | Standard 31kHz (VGA-compatible). **Default.** Works with VGA monitors and most scalers/capture devices.                                                                           |


The game renders at 320x200 in all modes. In 15kHz and 31kHz modes,
the image is centered within a standard 512-pixel-wide display using
GVRAM hardware scroll registers.

## Sound Effects

Three sound configurations are detected automatically at startup:


| Driver                  | Description                                                                                                                                                                                                                                                                 |
| ----------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| No PCM driver           | IOCS ADPCMOUT plays one sound at a time (no mixing). Functional but only one sound effect can play at once.                                                                                                                                                                 |
| PCM8A.X                 | Multi-channel ADPCM mixing via software. Multiple simultaneous sound effects. Uses some CPU. **Limitation:** PCM8A cannot stop individual sounds mid-playback, so rapid-fire sounds (e.g. chainsaw) may echo/overlap.                                                       |
| PCM8PP.X (Mercury Unit) | Mercury Unit V4 PCM playback with hardware volume. **Best sound quality.** Per-channel volume control with proper sound stopping. 16 kHz sample rate. Mercury Unit V4: run REQEN.X before loading PCM8PP.X. Recommended to use -S3 option with PCM8PP for best performance. |


Sound effects are converted from the WAD file on first launch and cached
to disk (doom_adp.cache or doom_pcm.cache). Subsequent launches load
from cache for faster startup. The cache auto-regenerates if the WAD
changes or sound settings are modified.

## Music

Three music modes are available from the Sound menu.  The FM modes aren't very good but they work with the internal YM2151 music chip.  Sorry, I don't really understand how to do FM music properly.


| Mode              | Description                                                                                                                                                            |
| ----------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| FM OPL2 Emulation | OPL2 instrument patches converted to YM2151. Poorly approximates the PC Sound Blaster sound. Includes pitch bend support for guitar slides.                            |
| FM MIDI Emulation | General MIDI patches mapped to YM2151. Different tonal character from OPL2 mode.                                                                                       |
| General MIDI      | External MIDI output via YM3802 UART. Requires MIDI interface and sound module. Best music quality with appropriate hardware. Only shown if MIDI hardware is detected. |


## Performance

Expected frame rates at E1M1 start position:


| Hardware                                                   | Detail | Size | FPS       |
| ---------------------------------------------------------- | ------ | ---- | --------- |
| 060turbo (50 MHz)                                          | High   | Full | 15        |
| PhantomX on XVI 16mhz, 060 mode (RPi 4B). Unstable for me. | High   | Full | 16        |
| PhantomX on XVI 16mhz, 030 mode (RPi 4B). Unstable for me. | High   | Full | 20-21     |
| X68030 (25 MHz)                                            | Low    | Min  | ~18       |
| XM6g emulator (68060 50 MHz, 2x mult)                      | High   | Full | 25        |
| XM6g emulator (68060 200 MHz, 4x mult)                     | High   | Full | 35 locked |


Note: XM6g uses inauthentically high RAM speeds.  Especially GVRAM.  
This causes it to run this game considerably faster than real hardware, 
even with the CPU running at the same mhz.

The 68060 build uses:

- FPU-accelerated FixedMul/FixedDiv (~6 cycles vs ~100 for C library)
- Hand-optimized 68060 assembly for column/span rendering
- Inline FPU in plane renderer and wall texture loop
- 060turbo local RAM (HIMEM) for all hot data structures
- 16-bit pre-remapped framebuffer (screens16) for fast GVRAM blit

The 68030 build uses:

- Native MULS.L 32x32->64 FixedMul (~28 cycles)
- Same optimized column/span assembly (68030-compatible subset)
- C plane renderer with function-call FixedMul
- 16-bit pre-remapped framebuffer (screens16) in system RAM
- System RAM (no HIMEM available)

Performance is primarily limited by GVRAM write bandwidth (~3.5-5.6 MB/s
depending on system bus speed), not CPU rendering speed on 68060.
The blit is minimized in several ways: smaller viewport sizes blit fewer
rows, low detail mode skips every other column (halving blit bandwidth),
and the status bar is only reblitted when it changes (not every frame).

## Controls


| Key             | Action                                |
| --------------- | ------------------------------------- |
| Arrow keys      | Move forward/back, turn left/right    |
| W/A/S/D         | Forward/strafe left/back/strafe right |
| XF1             | Fire                                  |
| Space           | Use (open doors, activate switches)   |
| Shift           | Run                                   |
| Alt + direction | Strafe                                |
| Tab             | Automap                               |
| Escape          | Menu                                  |
| F1-F12          | Standard Doom function keys           |
| 1-7             | Weapon select                         |
| Mouse           | Supported (left=fire, right=strafe)   |


## Key Remapping

Keys can be remapped by editing `doomrc` (created after the first
run). Each key setting takes a Doom key code as an integer value.

Remappable keys and their defaults:


| Setting         | Default | Key         |
| --------------- | ------- | ----------- |
| key_right       | 174     | Right arrow |
| key_left        | 172     | Left arrow  |
| key_up          | 173     | Up arrow    |
| key_down        | 175     | Down arrow  |
| key_strafeleft  | 44      | , (comma)   |
| key_straferight | 46      | . (period)  |
| key_fire        | 157     | XF1         |
| key_use         | 32      | Space       |
| key_strafe      | 184     | OPT1        |
| key_speed       | 182     | Shift       |


Common key codes for remapping:


| Code | Key        | Code | Key         |
| ---- | ---------- | ---- | ----------- |
| 8    | Backspace  | 13   | Return      |
| 27   | Escape     | 32   | Space       |
| 9    | Tab        | 127  | Delete      |
| 44   | ,          | 46   | .           |
| 47   | /          | 92   | \           |
| 157  | XF1        | 182  | Shift       |
| 184  | OPT1       | 172  | Left arrow  |
| 173  | Up arrow   | 174  | Right arrow |
| 175  | Down arrow |      |             |


Letter and number keys use their ASCII values (e.g. 97 = 'a', 49 = '1').

There is no in-game key remapping UI. Edit the file "doomrc" with a text  
editor, save, and restart the game.

## Menu Options


| Option            | Description                                      |
| ----------------- | ------------------------------------------------ |
| End Game          | Return to title screen                           |
| Messages          | Toggle on-screen messages                        |
| Graphic Detail    | High (320px) or Low (160px doubled)              |
| Screen Size       | Viewport size (smaller = faster)                 |
| Mouse Sensitivity | Mouse turn speed                                 |
| Sound             | Music mode, FM volume, SFX volume (Mercury only) |
| Mouse Walk        | Enable/disable mouse forward/back movement       |
| VSync             | Vertical sync on/off                             |
| FPS Counter       | Show frame rate counter                          |
| Video Mode        | 15kHz / 25kHz / 31kHz display mode               |


## Building from Source

Requires the xdev68k cross-compilation toolchain and WSL (Windows
Subsystem for Linux) or a native Linux environment. WSL is needed
because `run68` (an X68000 CPU emulator that hosts the HAS060
assembler and HLK linker) is a Linux ELF binary. A Windows-native
build would be possible with a Windows port of `run68`.

Tip: Claude Code (claude.ai/code) can handle the entire build and
deploy process. Point it at this repo, give it access to WSL, and
it will build both targets for you.

### Setup

1. Install WSL with a Linux distribution (e.g. Ubuntu)
2. Clone or download the xdev68k toolchain:
  [https://github.com/yosio68k/xdev68k](https://github.com/yosio68k/xdev68k)
3. Build xdev68k following its README instructions (builds m68k-elf-gcc,
  HAS060.X, HLK, and run68 inside WSL)
4. Note the path to the xdev68k-main directory

### Building

**Note:** Replace the paths below with your actual paths. WSL maps
Windows drives as `/mnt/c/`, `/mnt/d/`, etc. For example,
`C:\Doom\x68D00M` becomes `/mnt/c/Doom/x68D00M`.

From a WSL bash shell:

```bash
# cd to the x68D00M source directory
cd /mnt/c/Doom/x68D00M

# Set toolchain path
export XDEV68K_DIR=/mnt/c/Doom/xdev68k-main

# Build 68060 version -> doom060.x
make -f makefile.x68k

# Build 68030 version -> doom.x
make -f makefile.x68k TARGET=030

# Clean build artifacts
make -f makefile.x68k clean
```

Or as a one-liner from Windows cmd/PowerShell:

```cmd
wsl -e bash -c "cd /mnt/c/Doom/x68D00M && export XDEV68K_DIR=/mnt/c/Doom/xdev68k-main && make -f makefile.x68k -j4"
```

### Output

Executables are created in the source directory (same directory as makefile.x68k):

- `doom.x` -- 68030 build (default for most users)
- `doom060.x` -- 68060 build (060turbo, real 68060 hardware)

## 060turbo Configuration

Recommended 060turbo.sys settings in CONFIG.SYS.  
I use these for both xm6g and a real 060turbo:

```
DEVICE = \060SYS\060turbo.sys -cm1 -lt -dv -ss -xm
```

Key flags:


| Flag | Description                                                                                                                                       |
| ---- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| -cm1 | Copy-back cache mode. Fastest cache mode for rendering.                                                                                           |
| -lt  | Place MMU address translation table in local RAM. Speeds up every TLB miss by avoiding the slow system bus. Significant FPS improvement for Doom. |
| -dv  | Device driver buffer for local memory access. Required by -xm.                                                                                    |
| -ss  | Software SCSI transfers for local memory. Required by -xm.                                                                                        |
| -xm  | Enable extended memory (HIMEM). Doom allocates its zone, framebuffers, and lookup tables in local RAM for fast access. Requires -dv and -ss.      |


## PhantomX Configuration

For PhantomX users, recommended phantomx.ini settings:


| Setting  | Value     |
| -------- | --------- |
| EMUMODEL | 030       |
| HIMEM    | 64 or 128 |


Use doom.x (the 68030 build). PhantomX runs significantly faster
in 030 mode than 060 mode:


| Mode                    | Executable | FPS |
| ----------------------- | ---------- | --- |
| 030 mode (400MHz equiv) | doom.x     | ~20 |
| 060 mode (120MHz equiv) | doom060.x  | ~16 |


The 68030 emulation runs at a much higher equivalent clock.  
The 3x clock advantage outweighs the lack of FPU.

For fastest load times, copy doom.x and your WAD file to the
PhantomX SD card's WindrvXM shared folder and run from there.
Loading from SCSI/CF storage goes through the X68000 system bus
and is significantly slower.

NOTE: The game randomly crashes on PhantomX for me.  Sometimes it will
run for many levels without issue, then sometimes it will crash within
the first level several times in a row.  I have been unable to
replicate the crash on either 060turbo or xm6g, but I've had it crash
many times on PhantomX in both 060 and 030 mode.  I don't think it's a 
bug in my code since it only happens on PhantomX, but I can't rule it 
out, either.  I spent a long time looking but I can't find anything. 

## License

This port is based on the Doom source code released by id Software  
under the GNU General Public License v2. See LICENSE file for details.

The port-specific code (X68000 hardware interface, optimized renderers,
sound system, video modes) is also released under GPL v2.

Doom is a registered trademark of id Software / ZeniMax Media.
You must own a licensed copy of Doom to use its WAD data files.

## Credits

Original game:        id Software (John Carmack, John Romero, et al.)  
Original X68000 port: neozeed and neko68k  
x68D00M port:         zz964

## Acknowledgements

neozeed and neko68k - The original x68000 Doom port upon which I built this.

BCC - doom8088_x68k, from which the 24kHz video mode idea originated.

The Amiga Doom ports (ADoom by Peter McGavin, DoomAttack by Cosmos)
provided valuable reference for optimizing Doom on 68k hardware.
