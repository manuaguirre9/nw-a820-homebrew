# Research

Reverse-engineering notes and the throwaway tools that produced them. Start with
[`dsp/FINDINGS.md`](dsp/FINDINGS.md) (the NEC audio DSP and Sony's OpenMAX layer).

- `dsp/notes/`, `dsp/effects/*_notes.txt`: per-codec and per-feature notes.
- `dsp/scripts/`, `dsp/tools/`: Python helpers (ARM disassembly, ELF/kernel-module inspection, Sony `.dpk`
  textures, ext2 listing, ...). They work on files you extract from the official firmware yourself.
- `dsp/test/`, `dsp/effects/`: probe programs run on the Walkman. Their build scripts expect copies of the device's
  `/usr/lib/omf` libraries in `dsp/rootfs_omf/omf`, which are not distributed here (or use the stub approach of
  `homebrew/dspdec/build.sh`).
- `ui/dpk.py`: unpacker for the Sony UI texture packages.

The notes quote addresses and short disassembly excerpts of Sony binaries; full disassemblies and the binaries
themselves are not included.
