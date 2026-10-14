# Third-party code

Everything under `homebrew/third_party/` is upstream code, trimmed to the parts the builds use. Each folder keeps
its upstream licence file. Changes made for the Walkman are listed here and, inside the gpSP sources, marked `WALKMAN`.

| Component | Version | Licence | Used by | Walkman changes |
|---|---|---|---|---|
| [libmad](https://www.underbit.com/products/mad/) | 0.15.1b | GPL-2.0-or-later | hbplayer (MP3) | added `config.h` |
| [FAAD2](https://github.com/knik0/faad2) | 2.11.1 | GPL-2.0-or-later | hbplayer (AAC) | added `config.h` |
| [dr_flac](https://github.com/mackron/dr_libs) | 0.13.4 | Public domain / MIT-0 | hbplayer (FLAC) | none |
| [minimp4](https://github.com/lieff/minimp4) | — | CC0 | hbplayer, dspdec (.m4a) | none |
| [sonic](https://github.com/waywardgeek/sonic) | 2010 | Apache-2.0 | hbplayer (speed) | fixed-point reciprocal (`WALKMAN`) |
| [TJpgDec](http://elm-chan.org/fsw/tjpgd/) | R0.03 | ChaN (BSD-style) | hbplayer (cover art) | `tjpgd.h`, `tjpgdcnf.h` configuration |
| [stb_image](https://github.com/nothings/stb) | — | MIT / Public domain | hbplayer (cover art) | none |
| [zlib](https://zlib.net) | 1.3.1 | zlib | hbplayer (inflate) | none |
| [lwIP](https://savannah.nongnu.org/projects/lwip/) | 2.1.3 | BSD-3-Clause | hbplayer, pantest (TCP/IP over Bluetooth PAN) | `tcp_priv.h`: `TCP_ACK_EVERY_SEGMENT` |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | 2.28.8 | Apache-2.0 OR GPL-2.0-or-later | hbplayer (HTTPS) | none |
| gnuboy | master snapshot (2023) | GPL-2.0 | Game Boy / Color emulator | our frontend: `sys/walkman/walkman.c`, `build_walkman.sh` |
| [gpSP](https://github.com/libretro/gpsp) | `5819380c` | GPL-2.0 | GBA emulator | `cpu_threaded.c`, `gba_memory.c`, `arm/arm_stub.S` (OABI cache flush, ROM buffer, code cache in .bss) |
| [agnes](https://github.com/kgabis/agnes) | master | MIT | NES emulator | none |
| [doomgeneric](https://github.com/ozkl/doomgeneric) | master (2026) | GPL-2.0 | Doom | none (our platform layer is `homebrew/doom/`) |

gpSP ships the open-source GBA BIOS by Normmatt (`third_party/gpsp/bios/`), so no Nintendo BIOS is needed.

## Not included

- **Anything from Sony's firmware.** The firmware tools start from the official updater, downloaded from Sony, and
  patch it on your computer; `dspdec` links against stand-ins (`homebrew/dspdec/omx_stub.c`) and uses the real NEC OpenMAX
  libraries that are already on the Walkman.
- **Games and media.** Free test content: [Freedoom](https://freedoom.github.io/), [µCity Advance](https://github.com/AntonioND/ucity-advance),
  [jsmolka gba-tests](https://github.com/jsmolka/gba-tests), [Tonc demos](https://www.coranac.com/tonc/text/).
- **CA certificates** for HTTPS: download [cacert.pem](https://curl.se/docs/caextract.html) to `H:\HB\cacert.pem`.

## Prior work

- [Rockbox](https://www.rockbox.org/) — the Sony NWZ/NW firmware format (`upg.c`), the DES pass key and the SCSI
  update/DNK protocol (`scsitool.c`) that `firmware/upgpack.py` and `firmware/fwtrig.py` reimplement.
