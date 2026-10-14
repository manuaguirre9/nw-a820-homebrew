"""Extract primitive type numbers from the add_serialize_f registrations in all.dis (tiny register tracker)."""
import re, sys
regs = {}; func = None; out = []
for line in open('all.dis'):
    if line.startswith('===='):
        func = line.split()[1]; regs = {}; continue
    m = re.match(r'\s+([0-9a-f]+):\s+(\S+)\s+(.*)', line)
    if not m: continue
    mn, ops = m.group(2), m.group(3)
    ops_ = ops.split(' ;')[0]
    a = [x.strip() for x in ops_.split(',')]
    def val(x):
        if x.startswith('#'): return int(x[1:], 0)
        return regs.get(x)
    try:
        if mn == 'mov' and len(a) == 2: regs[a[0]] = val(a[1])
        elif mn == 'mvn' and len(a) == 2 and val(a[1]) is not None: regs[a[0]] = ~val(a[1]) & 0xffffffff
        elif mn in ('add', 'orr', 'sub') and len(a) == 3:
            x, y = val(a[1]), val(a[2])
            regs[a[0]] = None if x is None or y is None else (x + y if mn == 'add' else x | y if mn == 'orr' else x - y) & 0xffffffff
        elif mn == 'ldr' and '=' in ops:
            nm = ops.split('=')[-1].strip()
            regs[a[0]] = nm if not nm.startswith('0x') else int(nm, 16)
        elif mn == 'bl':
            if 'add_serialize_f' in ops or 'add_deserialize_f' in ops:
                out.append((func, 'S' if 'add_serialize' in ops else 'D', regs.get('r1'), regs.get('r2')))
            for r in ('r0', 'r1', 'r2', 'r3', 'r12', 'lr'): regs.pop(r, None)
        elif a and re.match(r'r\d+$', a[0]) and mn not in ('str', 'strb', 'strh', 'cmp', 'tst', 'push', 'stm', 'b', 'bx'):
            regs[a[0]] = None
    except Exception as ex:
        pass
seen = set()
for f, k, t, n in out:
    if k != 'S': continue
    key = (f, t, n)
    if key in seen: continue
    seen.add(key)
    print(f'{f:30s} {t if t is None else hex(t):>8s}  {n}')
