# NW-A829 audio DSP — findings (2026-09-27)

## What it is
- Separate programmable DSP inside the NEC MP201 SoC: NEC **SPX family ("K602" in NEC's MP201 header)**, 16-bit fixed-point, mixed 16/32-bit instructions. No public ISA docs / toolchain. Sony's firmware image calls itself `OmfSpxCore`.
- All of Sony's audio (and video) decoding runs on it via NEC's **OpenMAX IL 1.0** stack in `/usr/lib/omf`: MP3, AAC/HE-AAC, WMA, LPCM dec/enc, ATRAC3/3plus/Lossless, ATRAC encode, H.264, MPEG-4. Only a video-mode AAC fallback runs on the ARM.
- gemini uses the **"FTB"** decoder variants: the DSP hands decoded PCM back to the ARM, which writes /dev/sound/dsp or /dev/sound/adsp.
- Sony's sound effects (EQ, VPT, DSEE, channel separation, normalizer, AVLS) run inside the DSP as component configs.

## Firmware
- `/usr/lib/omf/{audio,audio_ftb,video}/omf_dsp_manager.mp201` (1.8 MB / 1.5 MB / 200 KB). The `omf_me_*.mp201` files are segment tables selecting which parts of the manager to load per codec.
- Format decoded by `scripts/spximg.py`: code at DSP address 0x200/0x400, ~28 codec overlays at DSP 0x4000xxxx.

## Memory map (device kernel; differs from NEC's GPL header)
- DSP window phys 0x33A00000–0x34000000; code at 0x33B00400 (DSP 0x400, bank remapper 0x33b0); audio data 0x33C0xxxx; mailbox 0x33FE0000 (128 KB). Framebuffer 0x33700000. Likely 32 MB RAM mirrored.

## Host interface
- Built-in kernel driver NEC **InterDSP v0.13**, char major 101, `/dev/InterDSP/control`; 3 channels each way, 8-byte message headers. ARM->DSP interrupt via the interrupt controller; **IRQ 1 "interdsp"** = DSP->ARM (~183k while gemini runs). Reset/clock via SMU 0xC8010000; bank remapper 0x40060000.
- ioctls ('D'): READ 0x80184401, WRITE 0x40184402, SET_CHINFO 0x40184411, SET_TASKCH 0xC0084413, DOWNLOAD 0xC0244430, SET_DCV_REGS 0x40084450.
- `libomf_core_hw_dep.so` (disassembly in notes/hw_dep_spx.dis): opens control, six 16 KB channel buffers, task->channel map, receiver thread; loads firmware = reset, copy segments, start clock, release reset.
- From our code: `libomx_core.so` `OMX_Init(cfg_path)` (non-standard arg), standard OpenMAX 1.0 component struct. Only one process can own the DSP (gemini must not run — our boot hook runs before it).

## Plan
1. Experiment 1 (test/): `dspprobe` (read-only /dev/mem: DSP clocks, fault status, remapper, mailbox, code window) + `omxprobe` (OMX_Init with Sony's audio_ftb cfg, list components, open/free OMX.NEC.EL.MP3D.FTB). Success: OMX_Init = 0, IRQ 1 count rises, mailbox 0x33FE0300 = b5cc7a92, words at 0x33B00200 = 00000000 1eb41e00 cb281e04 ...
2. MP3 decoded on the DSP with PCM returned to us (port/param layouts from gemini's MP3 component usage); compare ARM load and battery vs libmad.
3. Sony's DSP effects (EQ/VPT/DSEE) on our own PCM via the LPCM decoder component.
4. hbplayer backend: DSP as optional decoder (also gives ATRAC/OMA playback).
5. Moonshot: own DSP code (reverse the ISA from the 1.8 MB image; load with the DOWNLOAD ioctl, only inside the DSP window; a stuck DSP only takes out audio).
