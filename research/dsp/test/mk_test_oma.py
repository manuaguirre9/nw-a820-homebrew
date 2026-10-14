"""Make a tiny unencrypted ATRAC3 (LP2, 132 kbps, 384-byte frames, 44.1 kHz stereo) OpenMG file for testing the
DSP ATRAC path (homebrew/dspdec "at3", OMX.SONY.AT3D.FTB) without SonicStage / an encoder.

No ATRAC encoder exists on this PC, so the frames are written by hand: every channel sound unit is
  0x28 (6 bits) | coded QMF bands 0 (2) | gain points 0 (3) | tonal components 0 (5) |
  coded subbands K (5) | CLC coding (1) | K+1 x 3-bit table selectors | 6-bit scale factor per coded subband |
  constant-length mantissas
(ATRAC3 bitstream as in ffmpeg's atrac3.c: non joint-stereo frames hold the two channels in the two halves).
A "tone" unit codes one spectral line (bin 35 of the lowest QMF band, ~760 Hz) with the same value in every
frame, which decodes to a buzzy ~760 Hz tone (MDCT phase is not continued, so it is not a clean sine).

Layout: 3 s tone LEFT only, 3 s tone RIGHT only, 3 s both, 1 s silence  -> checks decode + channel order.
usage: python mk_test_oma.py [out.oma]      (default: test_at3.oma next to this script)
The file verifies with ffmpeg (PyAV): python mk_test_oma.py --check out.oma
"""
import os, struct, sys

FRAME = 384            # LP2: 132 kbps, 1024 samples per frame
HALF = FRAME // 2
SPF = 1024
RATE = 44100


class Bits:
    def __init__(self):
        self.v = 0
        self.n = 0

    def put(self, val, bits):
        self.v = (self.v << bits) | (val & ((1 << bits) - 1))
        self.n += bits

    def bytes(self, size):
        pad = size * 8 - self.n
        assert pad >= 0
        return (self.v << pad).to_bytes(size, 'big')


def unit(tone, sf=51):
    b = Bits()
    b.put(0x28, 6)         # sound unit id
    b.put(0, 2)            # coded QMF bands - 1 = 0 (only the lowest band)
    b.put(0, 3)            # gain control points for band 0
    b.put(0, 5)            # tonal components
    if not tone:
        b.put(0, 5)        # one subband (index 0) ...
        b.put(1, 1)        # CLC
        b.put(0, 3)        # ... not coded
    else:
        k = 4              # subbands 0..4 (subband 4 = spectral lines 32..39)
        b.put(k, 5)
        b.put(1, 1)        # CLC
        for i in range(k + 1):
            b.put(3 if i == k else 0, 3)   # selector 3: 3-bit signed mantissas
        b.put(sf, 6)       # scale factor index: 2^((sf-15)/3)
        for j in range(8):
            b.put(1 if j == 3 else 0, 3)   # line 35 = +1 (x sf / 3.5)
    return b.bytes(HALF)


def frame(left, right):
    return unit(left) + unit(right)


def ea3_tag(size=1014):
    # ID3v2.3-style tag with the "ea3" magic, frames: none (padding only)
    ss = bytes([(size >> 21) & 127, (size >> 14) & 127, (size >> 7) & 127, size & 127])
    return b'ea3' + bytes([3, 0, 0]) + ss + bytes(size)


def ea3_header(frame_bytes, js=0):
    h = bytearray(96)
    h[0:3] = b'EA3'
    h[3] = 1                      # version (SonicStage writes 1..3)
    h[4], h[5] = 0, 96            # header size
    h[6], h[7] = 0xff, 0xff       # encryption id: none
    params = (0 << 24) | (js << 17) | (1 << 13) | (frame_bytes // 8)   # codec 0 = ATRAC3, rate index 1 = 44.1k
    h[0x20:0x24] = struct.pack('>I', params)
    return bytes(h)


def make(path):
    fps = RATE / SPF
    segs = [(True, False, 3), (False, True, 3), (True, True, 3), (False, False, 1)]
    data = bytearray()
    for l, r, secs in segs:
        for _ in range(int(round(secs * fps))):
            data += frame(l, r)
    with open(path, 'wb') as f:
        f.write(ea3_tag())
        f.write(ea3_header(FRAME))
        f.write(data)
    print('%s: %d frames, %.1f s, %d bytes' % (path, len(data) // FRAME, len(data) // FRAME * SPF / RATE, os.path.getsize(path)))


def check(path):
    import av, numpy as np
    c = av.open(path)
    s = c.streams.audio[0]
    print('ffmpeg:', s.codec_context.name, s.codec_context.sample_rate, 'Hz', s.codec_context.channels if hasattr(s.codec_context, 'channels') else s.codec_context.layout.nb_channels, 'ch')
    pcm = []
    for fr in c.decode(s):
        a = fr.to_ndarray()
        if a.ndim == 1:
            a = a.reshape(-1, 2).T
        pcm.append(a.astype(np.float64))
    x = np.concatenate(pcm, axis=1)
    if np.abs(x).max() <= 2:
        x = x * 32768
    n = x.shape[1]
    print('decoded %.2f s' % (n / RATE))
    for i, name in enumerate(['0-3 s (L only)', '3-6 s (R only)', '6-9 s (both)', '9-10 s (silence)']):
        seg = x[:, int(i * 3 * RATE + 0.5 * RATE): int(min((i * 3 + 2.5) * RATE, n))]
        if seg.size == 0:
            continue
        lv = [10 * np.log10(np.mean(seg[ch] ** 2) / 32768 ** 2 + 1e-12) for ch in (0, 1)]
        spec = np.abs(np.fft.rfft(seg[0] + seg[1]))
        f = np.argmax(spec) * RATE / len(seg[0])
        print('  %-18s L %6.1f dBFS  R %6.1f dBFS  peak %5.0f  main freq %.0f Hz' % (name, lv[0], lv[1], np.abs(seg).max(), f))


if __name__ == '__main__':
    args = sys.argv[1:]
    if args and args[0] == '--check':
        check(args[1])
    else:
        make(args[0] if args else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'test_at3.oma'))
