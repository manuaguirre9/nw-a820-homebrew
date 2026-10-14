"""Test files for the DSP's AAC and WMA decoders (research/dsp/notes/aac_wma_notes.txt), made with PyAV (pip install av).
Each is 12 s: 0-4 s a 440 Hz tone on the LEFT only, 4-8 s a 660 Hz tone on the RIGHT only, 8-12 s both, -12 dBFS
(the ATRAC test file uses the same idea). Mono files: 440 / 660 / 880 Hz steps.
    python mk_test_aacwma.py [outdir]      -> test_aac.m4a  test_aac.aac  test_aac22m.m4a  test_wma.wma  test_wma22m.wma
    python mk_test_aacwma.py --check FILE  -> decode with ffmpeg (PyAV) and print level / frequency per 4 s segment
    python mk_test_aacwma.py --long [OUT]  -> test_aac_long.m4a: 75 s chords + noise (CPU comparison)
The .aac is the same AAC-LC stream as test_aac.m4a in ADTS framing (remuxed, not re-encoded).
No HE-AAC: PyAV's native encoder only does AAC-LC (an HE-AAC file from elsewhere can be tested as test_he.m4a)."""
import sys, os
import numpy as np
import av

SEG = 4.0


def signal(rate, ch):
    t = np.arange(int(rate * SEG * 3)) / rate
    a = 10 ** (-12 / 20) * 32767
    seg = (t // SEG).astype(int)
    if ch == 2:
        l = np.where(seg != 1, np.sin(2 * np.pi * 440 * t), 0) * a
        r = np.where(seg != 0, np.sin(2 * np.pi * 660 * t), 0) * a
        return np.vstack([l, r]).astype(np.int16)
    f = np.choose(seg, [440, 660, 880])
    return (np.sin(2 * np.pi * f * t) * a).astype(np.int16)[None, :]


def encode(path, fmt, codec, rate, ch, br):
    out = av.open(path, 'w', format=fmt)
    s = out.add_stream(codec, rate=rate)
    s.layout = 'stereo' if ch == 2 else 'mono'
    s.bit_rate = br
    x = signal(rate, ch)
    n = 1024
    for i in range(0, x.shape[1], n):
        blk = np.ascontiguousarray(x[:, i:i + n])
        f = av.AudioFrame.from_ndarray(blk.T.reshape(1, -1) if ch == 2 else blk, format='s16', layout=s.layout.name)
        f.rate = rate
        f.pts = i
        for p in s.encode(f):
            out.mux(p)
    for p in s.encode():
        out.mux(p)
    out.close()


def remux(src, dst, fmt):
    i = av.open(src)
    o = av.open(dst, 'w', format=fmt)
    ins = i.streams.audio[0]
    os_ = o.add_stream_from_template(ins)
    for p in i.demux(ins):
        if p.dts is None:
            continue
        p.stream = os_
        o.mux(p)
    o.close()
    i.close()


def check(path):
    c = av.open(path)
    s = c.streams.audio[0]
    cc = s.codec_context
    print(path, cc.name, cc.profile, s.rate, cc.channels, 'extradata', cc.extradata.hex() if cc.extradata else None)
    pcm = []
    for fr in c.decode(s):
        a = fr.to_ndarray()
        if a.ndim == 2 and a.shape[0] == 1 and cc.channels == 2 and fr.format.is_packed:
            a = a.reshape(-1, 2).T
        if a.dtype != np.float32 and a.dtype != np.float64:
            a = a.astype(np.float64) / 32768
        pcm.append(a)
    x = np.concatenate(pcm, axis=1)
    rate = s.rate
    for k in range(3):
        seg = x[:, int(k * SEG * rate) + rate // 4:int((k + 1) * SEG * rate) - rate // 4]
        out = []
        for ch in range(seg.shape[0]):
            v = seg[ch]
            lev = 10 * np.log10(np.mean(v * v) + 1e-12)
            f = np.fft.rfftfreq(len(v), 1 / rate)[np.argmax(np.abs(np.fft.rfft(v)))]
            out.append('%s %.1f dBFS %.0f Hz' % ('LR'[ch] if seg.shape[0] == 2 else 'M', lev, f))
        print('  seg %d: %s' % (k, ' | '.join(out)))


def long_music(path, secs=75):
    """test_aac_long.m4a: 75 s of chords + noise (music-like spectrum) for the CPU comparison (hbplayer selftest)"""
    rate = 44100
    out = av.open(path, 'w', format='ipod')
    s = out.add_stream('aac', rate=rate)
    s.layout = 'stereo'
    s.bit_rate = 128000
    rng = np.random.default_rng(1)
    roots = [220, 247, 262, 294, 330, 349, 392]
    n = 1024
    for i in range(0, rate * secs, n):
        t = (np.arange(n) + i) / rate
        root = roots[(i // rate) % len(roots)]
        x = sum(np.sin(2 * np.pi * root * m * t + m) / (m * m) for m in (1, 1.25, 1.5, 2, 3, 4))
        l = 0.25 * x + 0.03 * rng.standard_normal(n)
        r = 0.25 * np.roll(x, 7) + 0.03 * rng.standard_normal(n)
        blk = (np.vstack([l, r]).clip(-1, 1) * 32767).astype(np.int16)
        f = av.AudioFrame.from_ndarray(np.ascontiguousarray(blk.T).reshape(1, -1), format='s16', layout='stereo')
        f.rate = rate
        f.pts = i
        for p in s.encode(f):
            out.mux(p)
    for p in s.encode():
        out.mux(p)
    out.close()


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--long':
        long_music(sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'test_aac_long.m4a'))
        sys.exit(0)
    if len(sys.argv) > 2 and sys.argv[1] == '--check':
        for p in sys.argv[2:]:
            check(p)
        sys.exit(0)
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    encode(os.path.join(d, 'test_aac.m4a'), 'ipod', 'aac', 44100, 2, 128000)
    remux(os.path.join(d, 'test_aac.m4a'), os.path.join(d, 'test_aac.aac'), 'adts')
    encode(os.path.join(d, 'test_aac22m.m4a'), 'ipod', 'aac', 22050, 1, 48000)
    encode(os.path.join(d, 'test_wma.wma'), 'asf', 'wmav2', 44100, 2, 128000)
    encode(os.path.join(d, 'test_wma22m.wma'), 'asf', 'wmav2', 22050, 1, 32000)
    for f in ('test_aac.m4a', 'test_aac.aac', 'test_aac22m.m4a', 'test_wma.wma', 'test_wma22m.wma'):
        check(os.path.join(d, f))
