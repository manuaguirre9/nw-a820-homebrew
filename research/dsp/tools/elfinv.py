import os, sys, hashlib, io
from elftools.elf.elffile import ELFFile
from elftools.elf.dynamic import DynamicSection
from elftools.elf.gnuversions import GNUVerNeedSection

root = sys.argv[1]
seen = {}
for dp, dn, fn in os.walk(root):
    for f in sorted(fn):
        p = os.path.join(dp, f)
        rel = p[len(root):].replace(os.sep, '/')
        with open(p, 'rb') as fh:
            data = fh.read()
        if data[:4] != b'\x7fELF':
            continue
        h = hashlib.md5(data).hexdigest()
        if h in seen:
            print(f"{rel}\tDUP of {seen[h]}")
            continue
        seen[h] = rel
        try:
            e = ELFFile(io.BytesIO(data))
            needed = []; soname = ''; interp = ''; vers = set()
            for s in e.iter_sections():
                if isinstance(s, DynamicSection):
                    for t in s.iter_tags():
                        if t.entry.d_tag == 'DT_NEEDED': needed.append(t.needed)
                        if t.entry.d_tag == 'DT_SONAME': soname = t.soname
                if isinstance(s, GNUVerNeedSection):
                    for vn, auxs in s.iter_versions():
                        for a in auxs:
                            vers.add(a.name)
                if s.name == '.interp':
                    interp = s.data().rstrip(b'\0').decode()
            typ = e['e_type']
            stripped = e.get_section_by_name('.symtab') is None
            kind = 'static' if (not needed and typ == 'ET_EXEC') else 'dyn'
            print(f"{rel}\t{len(data)}\t{typ}\t{kind}\tsoname={soname}\tneeded={','.join(needed)}\t{'stripped' if stripped else 'SYMS'}\tflags={hex(e['e_flags'])}\tvers={','.join(sorted(vers))}")
        except Exception as ex:
            print(rel, 'ERR', ex)
