# gba: Game Boy Advance emulator for the NW-A829

This is the libretro **gpSP** core (ARM dynarec, open-source built-in BIOS) with a small Walkman frontend (`walkman_gba.c`). The frontend follows the conventions of our gnuboy port.

- Core: `homebrew/third_party/gpsp/`, libretro/gpsp at commit `5819380c` (2026-09-19), GPL v2.
- The ARM dynarec supports ARMv5 (`__ARM_ARCH < 6` code paths). The build uses gpSP's own `miyoo` settings (ARM926EJ-S, `SMALL_TRANSLATION_CACHE`).
- BIOS: `bios/open_gba_bios.bin` (Normmatt / VBA-M open-source replacement, GPL v2) is compiled into the binary. The `gpsp_bios=builtin` option is forced, so Nintendo's BIOS is never used or needed.

## Build

```
sh build.sh          # Git Bash; produces ./gba (ARM EABI, dynamic against device glibc 2.3.3, fixpthread applied)
python tools/elfcheck.py gba   # optional: needed libs / versions / segments / no EABI "svc #0"
```

The binary is about 630 KB. The build compiles with no errors, `elfcheck` reports no `svc #0`, and all 72 imported symbols exist in the device's `libc-2.3.3.so` (checked against the rootfs copy).

## Run

```
echo M248 > /proc/pm        # the player normally does this; at 124 MHz games will be much too slow
gba /contents/Juegos/GBA/game.gba 2>/var/hb/gba.log
```

- Exit code 0 means the user quit (or SIGTERM/SIGINT/SIGHUP). Exit code 1 means the game could not start: there is no ROM, it is bigger than 32 MB, or memory ran out. In that case a message stays on screen for 3 s.
- Environment variables (same names as gnuboy):
  - `HB_SAVEDIR`: folder for `.sav`, `.ss0` and `walkman_gba.cfg`. The default is the ROM's folder.
  - `HB_SOUND=0`: muted. No `/dev/sound/dsp` is used, and timing comes from the clock. Use this when the player keeps music playing.
  - `HB_DSP`: output device, e.g. `/dev/sound/adsp` for Bluetooth (44100 Hz, non-blocking).
  - `HB_VOL`: start volume, 0..20.
  - `HB_RATE`: output rate. The default is 22050 wired and 44100 on BT. The core mixes at 32768 Hz; the frontend resamples linearly to the rate OSS grants.
  - `HB_GBA_ROMMB`: largest ROM kept fully in RAM, in MB (default 8, see Memory).
  - `HB_GBA_DRC=0`: interpreter only. Very slow; for debugging dynarec problems.
- stderr: startup info, then `fps X drawn N/M mode sound turbo` every 10 s (the same idea as the gnuboy log).

## Controls (gnuboy's mapping, plus L/R)

| Walkman | GBA |
|---|---|
| d-pad | d-pad (remapped when the screen is rotated, exactly like gnuboy) |
| PLAY | A |
| OPTION | B |
| VOL- / VOL+ | L / R (one rocker, so L+R together is impossible) |
| BACK tap | START (4-frame synthetic press) |
| BACK held 2 s | quit (the save RAM is written) |
| BT tap | SELECT |
| BT held 2 s | in-game menu |
| HOLD switch on | fast forward: muted, 1 frame in 6 drawn. Keys keep working. |

**In-game menu** (Sony font and orange bar, like gnuboy): Continuar · Guardar estado · Cargar estado (slot 0, `<rom>.ss0`, ~416 KB) · Pantalla (LEFT/RIGHT/PLAY cycles) · Volumen (LEFT/RIGHT; the VOL keys are L/R in game) · Salir. BACK closes the menu. The bottom line shows the last measured fps.

**Screen modes** (saved in `walkman_gba.cfg` as `mode N vol N`):
0. Landscape 1.33x (320x213, nearest). The image is rotated 90° clockwise, the same orientation as gnuboy's "rotada". This is the default.
1. Landscape 1:1 (240x160, rotated and centred).
2. Portrait 1:1 (240x160 in the middle of the upright screen; the d-pad is not remapped).

**Frameskip** is automatic. Before each frame the frontend tells the core "underrun likely" when the OSS buffer holds less than 2 frames of sound, or, when muted, when the clock is more than half a frame behind. Skipped frames are not rendered by the core at all. At most 4 frames in a row are skipped.

## Saves

- `<savedir>/<rom>.sav` is the raw 128 KB backup, the same as RetroArch gpSP. It is loaded at start. It is written when it changes (checked every 5 s), when the menu opens, and on quit. The file is written as a temporary file, then fsync and rename. Games that never touch their backup never get a `.sav`.
- `<savedir>/<rom>.ss0`: save state.

## Memory

The target has about 13 MB free.

- ROM: fully loaded when it is no larger than `min(HB_GBA_ROMMB (8), MemFree+Cached − 4 MB)`. Bigger ROMs use gpSP's own pager: 32 KB pages loaded from the file into the 1 MB buffers, least recently used page evicted. That path is upstream code used on PSP; it is untested here.
- Emulator: the JIT caches (2 MB ROM + 384 KB RAM) are in `.bss` and are only backed by RAM as code gets generated. GBA RAM, VRAM, tables, the save-RAM shadow and the Sony UI back buffers take about 1.6 MB. Expect roughly 3–4.5 MB plus the ROM.
- 1 MB "Classic NES" ROMs are expanded to a 4 MB mirror, and gpSP turns the dynarec off for them.

## Patches to the gpSP core (grep `WALKMAN`)

1. `cpu_threaded.c` `platform_cache_sync()`: the icache/dcache flush after emitting code is the OABI private syscall `swi 0x9f0002` (r0 start, r1 end, r2 0). It replaces libgcc `__clear_cache`, which would issue an EABI `svc 0` that this kernel rejects.
2. `gba_memory.c` `init_gamepak_buffer()`: allocates `gpsp_rom_buffer_mb` 1 MB blocks (set by the frontend before `retro_init`) instead of always 32.
3. `arm/arm_stub.S`: the translation caches go in plain `.bss` instead of a separate `.jit` "awx" nobits section. That section produced a zero-fill PT_LOAD in the middle of the image, which old `binfmt_elf` may not zero-fill. With the change, the segment layout is the same as gnuboy's (checked with `elfcheck.py`). ARMv5 has no execute-never bit, so `.bss` is executable, and it is well within branch range of the stubs.

## Test ROMs (`roms/`, all free)

- `ucity-advance.gba`: µCity Advance v1.0.3 by AntonioND, GPL v3. A real game with a battery save, good for testing `.sav`.
- `tonc/*.gba`: Tonc demos by J. Vijn. `m7_ex` (Mode 7), `sbb_aff`, `obj_aff`, `bld_demo`, `key_demo` (shows pressed keys, useful to check the mapping and rotation), `snd1_demo` (sound).
- `test_*.gba`: jsmolka gba-tests (MIT): arm, thumb, memory, bios, sram, and ppu hello/shades/stripes.

## Verified on the PC (`tools/build_hosttest.sh`, `tools/hosttest.exe`)

This is the same core and options with gpSP's interpreter (x86-64 Windows), not the ARM dynarec.

- uCity Advance reaches its title menu, and tonc `m7_ex` renders Mode 7 correctly using the built-in BIOS.
- Sound comes out at about 32.7 kHz.
- Save states serialize and unserialize.
- The mode-0 rotation mapping gives the expected picture (`tools/out/grid.png`, `tools/out/rot.png`).
- jsmolka baseline with the interpreter: memory "All tests passed", arm "Failed test 225", thumb "Failed test 211", bios "Failed test 001". These are gpSP inaccuracies, not port bugs. On the device, the dynarec should give the same results; different numbers would point to a dynarec or cache-flush problem.

## Not verified / open issues

- **Nothing has run on the Walkman yet.** No qemu or WSL was available. First test: `test_arm.gba` / `test_thumb.gba` should show the same result as above, then `tonc/key_demo.gba`, then uCity.
- **Speed at 248 MHz is unknown.** gpSP needed about 200 MHz on the GP2X for many games, so expect full speed only in lighter games; auto frameskip will be busy. If it is too slow, try `HB_SOUND=0` first. Mixing at 32768 Hz already halves the audio cost.
- The fb writes (rotated, 2 pixels per store) go straight to the mmapped `/dev/fb/0`, as in gnuboy; the cost of that is unmeasured.
- The wired DSP probably refuses 32768 Hz. We request 22050 and resample, and whatever rate `SNDCTL_DSP_SPEED` returns is used. The `SETFRAGMENT` request (8 x 2 KB) may be ignored; latency then follows the driver default.
- The ROM paging path (ROMs over 8 MB or low memory) and 1 MB mini ROMs are untested.
- Single-threaded; no threads needed. RTC games use the system clock (gpSP default).
- The player (hbplayer) does not list or launch GBA games yet. It needs a `.gba` kind in the games list that launches this binary the way it launches gnuboy (M248 before, M124 after, same env vars, stderr to `/var/hb/gba.log`).
