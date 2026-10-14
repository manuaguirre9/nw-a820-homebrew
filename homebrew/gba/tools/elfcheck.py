# elfcheck.py: sanity checks for NW-A829 binaries (run on the PC after build.sh).
#  - DT_NEEDED libraries and the GLIBC symbol versions required (device glibc 2.3.3 exports up to GLIBC_2.4 here)
#  - program headers (the .jit translation cache must be in a writable + executable segment)
#  - scans all executable code for EABI syscalls (svc #0), which the Sony kernel does not understand (OABI only),
#    and lists the OABI swi numbers used directly by our code.
import sys
from elftools.elf.elffile import ELFFile
from elftools.elf.dynamic import DynamicSection
from elftools.elf.gnuversions import GNUVerNeedSection
import capstone

path = sys.argv[1] if len(sys.argv) > 1 else 'gba'
f = ELFFile(open(path, 'rb'))
print('e_flags', hex(f.header.e_flags), 'entry', hex(f.header.e_entry))
for s in f.iter_sections():
    if isinstance(s, DynamicSection):
        print('NEEDED', [t.needed for t in s.iter_tags() if t.entry.d_tag == 'DT_NEEDED'])
    if isinstance(s, GNUVerNeedSection):
        for verneed, auxs in s.iter_versions():
            print('verneed', verneed.name, sorted(a.name for a in auxs))
for p in f.iter_segments():
    if p['p_type'] in ('PT_LOAD', 'PT_GNU_STACK'):
        fl = p['p_flags']
        print(p['p_type'], hex(p['p_vaddr']), 'filesz', hex(p['p_filesz']), 'memsz', hex(p['p_memsz']),
              ('R' if fl & 4 else '-') + ('W' if fl & 2 else '-') + ('X' if fl & 1 else '-'))
for s in f.iter_sections():
    if s.name in ('.jit', '.text', '.data', '.bss'):
        print('section', s.name, hex(s['sh_addr']), hex(s['sh_size']), 'flags', hex(s['sh_flags']))

md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
bad, swis = 0, {}
for s in f.iter_sections():
    if s['sh_flags'] & 4 and s['sh_type'] == 'SHT_PROGBITS':
        data = s.data()
        for off in range(0, len(data) - 3, 4):
            w = int.from_bytes(data[off:off + 4], 'little')
            if (w & 0x0F000000) == 0x0F000000 and (w >> 28) != 0xF:	# SWI/SVC, any condition
                imm = w & 0xFFFFFF
                if imm == 0:
                    bad += 1
                    print('EABI svc #0 at', hex(s['sh_addr'] + off))
                else:
                    swis[imm] = swis.get(imm, 0) + 1
print('svc #0 count', bad, '; swi immediates', {hex(k): v for k, v in swis.items()})
