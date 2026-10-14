"""podsync: download the newest podcast episodes and copy them to the Walkman (H:\\Podcasts).

usage:  python podsync.py            download the newest N episodes of every feed, then sync to H: if present
        python podsync.py --no-copy  only download

Feeds and N live in feeds.txt next to this script: one line per feed, "Name | RSS URL | N".
Episodes are stored in ..\\media\\Podcasts\\<Name>\\YYYY-MM-DD - Title.mp3 so they sort by date.
Only files missing on the Walkman are copied; nothing is ever deleted there.
"""
import os, re, sys, shutil, urllib.request, xml.etree.ElementTree as ET
from email.utils import parsedate_to_datetime

HERE = os.path.dirname(os.path.abspath(__file__))
STORE = os.path.join(HERE, '..', 'media', 'Podcasts')
WALKMAN = 'H:\\Podcasts'
UA = {'User-Agent': 'Mozilla/5.0 podsync'}


FALLBACK = {0x2018: "'", 0x2019: "'", 0x201A: "'", 0x201B: "'", 0x2032: "'", 0x201C: "'", 0x201D: "'", 0x201E: "'",
            0x201F: "'", 0x2033: "'", 0x2010: '-', 0x2011: '-', 0x2012: '-', 0x2013: '-', 0x2014: '-', 0x2015: '-',
            0x2212: '-', 0x2026: '...', 0x2022: '-', 0x20AC: 'EUR', 0x2122: 'TM'}


def clean(s):
    s = re.sub(r'[\\/:*?"<>|]', '', s).strip()
    return re.sub(r'\s+', ' ', s)[:90]


def fat_name(s):
    """The Walkman mounts its FAT partition in ISO-8859-1: keep Latin-1, map the rest like the device does
    (homebrew/hbplayer/fsname.c utf8_to_fs), so both sides produce the same file names."""
    return ''.join(c if ord(c) < 0x100 else FALLBACK.get(ord(c), '' if ord(c) >= 0x1F000 else '_') for c in s)


def feeds():
    for line in open(os.path.join(HERE, 'feeds.txt'), encoding='utf-8'):
        line = line.strip()
        if line and not line.startswith('#'):
            name, url, n = [p.strip() for p in line.split('|')]
            yield name, url, int(n)


def download(name, url, n):
    req = urllib.request.Request(url, headers=UA)
    root = ET.fromstring(urllib.request.urlopen(req, timeout=60).read())
    items = root.findall('./channel/item')[:n]
    folder = os.path.join(STORE, clean(name))
    os.makedirs(folder, exist_ok=True)
    for it in items:
        enc = it.find('enclosure')
        if enc is None or 'audio' not in (enc.get('type') or 'audio'):
            continue
        try:
            date = parsedate_to_datetime(it.findtext('pubDate')).strftime('%Y-%m-%d')
        except Exception:
            date = '0000-00-00'
        fn = os.path.join(folder, fat_name('%s - %s.mp3' % (date, clean(it.findtext('title') or 'episodio'))))
        if os.path.exists(fn):
            print('  ya está:', os.path.basename(fn))
            continue
        print('  bajando:', os.path.basename(fn))
        tmp = fn + '.part'
        with urllib.request.urlopen(urllib.request.Request(enc.get('url'), headers=UA), timeout=120) as r, open(tmp, 'wb') as f:
            shutil.copyfileobj(r, f, 1 << 20)
        os.replace(tmp, fn)


def sync():
    if not os.path.isdir('H:\\'):
        print('Walkman no conectado: solo descargado.')
        return
    for name in os.listdir(STORE):
        src = os.path.join(STORE, name)
        dst = os.path.join(WALKMAN, name)
        os.makedirs(dst, exist_ok=True)
        # episodes deleted on the Walkman are listed there in borrados.txt: never copy them back
        deleted = set()
        try:
            deleted = {l.strip() for l in open(os.path.join(dst, 'borrados.txt'), encoding='latin-1') if l.strip()}
        except OSError:
            pass
        for f in sorted(os.listdir(src)):
            if f.endswith('.mp3') and f not in deleted and not os.path.exists(os.path.join(dst, f)):
                print('  copiando al Walkman:', name, '/', f)
                shutil.copyfile(os.path.join(src, f), os.path.join(dst, f))


if __name__ == '__main__':
    for name, url, n in feeds():
        print(name)
        download(name, url, n)
    if '--no-copy' not in sys.argv:
        sync()
    print('listo')
