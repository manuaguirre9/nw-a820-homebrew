"""Translation coverage: every string literal of the player that looks like Spanish interface text, with its printf
formats filled with sample values, must come out different from trcheck (= ui_tr found an entry). Technical strings
(log lines, file names, tags) are skipped by the same rules as the extractor; IGNORE lists what is deliberately not
translated (names of things, sample data, text never shown)."""
import glob, os, re, subprocess

IGNORE = {
    'Walkman NW-A829 homebrew', 'Game Boy', 'Game Boy Advance', 'Bluetooth', 'Podcast', 'Podcasts', 'DSP', 'NES', 'GBA', 'Doom',
    'Info', 'Hold', 'Preset', 'Crossfeed', 'OK', 'Heavy', 'Pop', 'Jazz', 'Español', 'English', 'Idioma / Language',
    'Argentina', 'Chile', 'Colombia', 'Uruguay', 'México', 'España', 'Brasil', 'Estados Unidos', 'Reino Unido',
    'canciones', 'canción', 'episodio', 'episodios', 'una', 'todo', 'Máxima batería off',	# pieces of composed texts
    '%s%d episodio%s%s%s', '%s%s%d episodio%s', '%s%sen el Walkman', 'Lo seguís · ', 'Repetir: %s',	# composed at run time (unit.c checks the results)
    'Mbed TLS · Sonic · TJpgDec', 'libmad · FAAD2 · gnuboy · lwIP', 'MP3 · FLAC · AAC · EQ · Bluetooth', 'Game Boy · NES · Doom',
}
SPANISH = re.compile(r'[áéíóúñÁÉÍÓÚÑ¿¡]|\b(de|del|con|para|los|las|el|la|y|no|sí|en|un|una|que|tu|tus|hay|ya|más|sin|por)\b|'
                     r'\b(Buscar|Volver|Bajar|Borrar|Cancelar|Seguir|Ver|Leyendo|Abriendo|Ajustes|Apagar|Sonido|Pantalla|Despertador|'
                     r'Velocidad|Temporizador|Repetir|Aleatorio|Conectar|Conectando|Emparejando|Encendiendo|Esperando|Probá|Revisá|'
                     r'Copiá|Elegí|Activá|Agregá|Esperá|Usá|Restablecer|Suspender|Juegos|Canciones|Artistas|Música|Episodio|episodios?|'
                     r'canciones?|Capítulos?|Auriculares|Voz|Bajos|Agudos|Fondo|Duración|Hora|Pitido|Brillo|Descargas|Teléfono|'
                     r'Modo|Mando|Receptor|Desconectar|Cargando|Buscando|Dejar|Lo seguís|Ya lo|Ahora|Listo|Hubo|Cancelado|pausa|salir|terminar|faltan?|desconectado|conectado|sonando|guardar|cambiar|apagado|encendido|elegir|seguir|volver|borrar)\b')
FMT = re.compile(r'%[-+ 0#]*(\d+|\*)?(\.\d+)?(l|ll|h)?([dsufxXc%])')


def literals(hb):
    files = [f for f in glob.glob(os.path.join(hb, 'hbplayer', '*.c')) if not re.search(r'(mock|shots|podsearch|i18n)\.c$', f)]
    files.append(os.path.join(hb, 'net', 'pan.c'))
    lit = re.compile(r'"((?:[^"\\]|\\.)*)"')
    skip = ('lg(', 'fopen(', 'access(', 'unlink(', 'getenv(', 'strcmp(', 'strncmp(', 'system(', 'execl', 'mkdir(', 'sscanf',
            'strstr(', 'endswith', 'opendir(', 'rename(', 'printf(stderr', 'fprintf(', 'setenv(', 'fail(', 'kb_chars')
    out = {}
    for f in files:
        for n, line in enumerate(open(f, encoding='utf-8', errors='replace'), 1):
            st = line.strip()
            if st.startswith(('#', '/*', '*', '//')) or any(c in line for c in skip):
                continue
            # a line can hold several literals that are pieces of one text ("a" "b"): join adjacent ones
            line = re.sub(r'"\s*"', '', line)
            for m in lit.finditer(line):
                s = bytes(m.group(1), 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8', 'replace')
                if s.startswith('/') or len(s.encode('utf-8')) <= 2:	# paths; one letter (a key of the keyboard)
                    continue
                s = re.sub(r'^\d/4 ', '', s)	# pan.c's "n/4 text": the number is cut before it is shown
                if SPANISH.search(s) and s not in IGNORE:
                    out.setdefault(s, '%s:%d' % (os.path.relpath(f, hb), n))
    return out


def sample(s):
    """printf format -> a text as the screen would show it ("%d episodio%s" -> "7 episodios")"""
    def sub(m):
        c = m.group(4)
        if c == 's':	# right after a word: a plural ending ("episodio%s"); else a name
            return 's' if m.start() > 0 and s[m.start() - 1].isalpha() else 'Pixel'
        return {'d': '7', 'u': '7', 'f': '7', 'x': '7', 'X': '7', 'c': 'x', '%': '%'}[c]
    return FMT.sub(sub, s)


def check(hb, zig, out, cc):
    exe = os.path.join(out, 'trcheck.exe')
    ok, _ = cc(exe, [os.path.join(hb, 'tests', 'trcheck.c'), os.path.join(hb, 'hbplayer', 'i18n.c'), os.path.join(hb, 'common', 'gfx.c')])
    if not ok:
        return False, 'build failed'
    lits = literals(hb)
    texts = []
    for s, where in sorted(lits.items()):
        texts.append((s, sample(s).replace(chr(10), ' ').replace(chr(13), ' '), where))	# one text per line for trcheck
    r = subprocess.run([exe], input='\n'.join(t for _, t, _ in texts) + '\n', capture_output=True, text=True, encoding='utf-8')
    res = r.stdout.split('\n')
    left = [(s, w) for (s, t, w), e in zip(texts, res) if e == t]
    with open(os.path.join(out, 'untranslated_code.txt'), 'w', encoding='utf-8') as f:
        for s, w in left:
            f.write('%s\t%s\n' % (w, s))
    for s, w in left:
        print('   no English:', w, repr(s))
    return not left, '%d texts checked, %d without English' % (len(texts), len(left))
