# UPG v1 pack/unpack (Rockbox format): md5(16) | DES-ECB[hdr: sig8 + nr_files u32 + pad u32 ; entries: (offset,size) u32 pairs] | files zero-padded to 8, DES-ECB
import struct, hashlib
from cryptography.hazmat.primitives.ciphers import Cipher, modes
from cryptography.hazmat.decrepit.ciphers.algorithms import TripleDES
def _des(key, data, enc):
    c = Cipher(TripleDES(key*3), modes.ECB()); x = c.encryptor() if enc else c.decryptor(); return x.update(data)+x.finalize()
def unpack(blob, key):
    assert hashlib.md5(blob[16:]).digest() == blob[:16], 'md5 mismatch'
    hdr = _des(key, blob[16:32], False); sig = hdr[:8]; nf = struct.unpack('<I', hdr[8:12])[0]
    tbl = _des(key, blob[32:32+8*nf], False)
    files = []
    for i in range(nf):
        off, sz = struct.unpack_from('<II', tbl, 8*i); r = (sz+7)//8*8
        files.append(_des(key, blob[off:off+r], False)[:sz])
    return sig, files
def pack(files, key, sig):
    nf = len(files); hdr_sz = 16 + 8*nf; off = 16 + hdr_sz
    entries = b''; body = b''
    for f in files:
        r = (len(f)+7)//8*8
        entries += struct.pack('<II', off, len(f)); body += _des(key, f + b'\0'*(r-len(f)), True); off += r
    hdr = _des(key, sig + struct.pack('<II', nf, 0) + entries, True)
    rest = hdr + body
    return hashlib.md5(rest).digest() + rest
