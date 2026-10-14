"""The installer window (Walkman-Homebrew-Installer.exe): for people who are not developers. It watches for the
Walkman, says plainly what it found, allows only the step that makes sense (Sony's official 1.02 first, then the
homebrew), and runs the same checked steps as install.py. Spanish or English after Windows' language.
International NWZ-A82x units get a read-only diagnostic report instead."""
import ctypes, datetime, os, platform, queue, subprocess, sys, threading, time, traceback
import tkinter as tk
from tkinter import ttk, messagebox

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import install as I
import fwtrig

VERSION = '0.2.0'
JP_PIDS = ('035D', '0324')	# NW-A82x mass storage / rescue mode
INTL_PIDS = ('035B', '035C')	# NWZ-A82x (MTP)

ES = (ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff) == 0x0a
T = {
    'title': ('Instalador de Walkman Homebrew', 'Walkman Homebrew Installer'),
    'sub': ('Reproductor de música, audiolibros y podcasts para el Walkman NW-A820',
            'Music, audiobook and podcast player for the Walkman NW-A820'),
    'step1': ('Walkman conectado', 'Walkman connected'),
    'step2': ('Firmware oficial de Sony 1.02', "Sony's official firmware 1.02"),
    'step3': ('Homebrew instalado', 'Homebrew installed'),
    'battery': ('La batería está cargada y no voy a desconectar el cable durante la instalación.',
                "The battery is charged and I won't unplug the cable during the installation."),
    'details': ('Detalles', 'Details'),
    'none_t': ('Conectá tu Walkman', 'Connect your Walkman'),
    'none_d': ('Usá el cable USB, con el Walkman en su modo normal de Sony (pantalla "Conectando"). Esta ventana lo detecta sola.',
               'Use the USB cable, with the Walkman in its normal Sony mode ("Connecting" screen). This window finds it by itself.'),
    'hbmode_t': ('El Walkman está en modo homebrew', 'The Walkman is in homebrew mode'),
    'hbmode_d': ('Desconectalo, mantené HOME 3 segundos para volver al Walkman de Sony y conectalo de nuevo.',
                 "Unplug it, hold HOME for 3 seconds to go back to Sony's Walkman, and connect it again."),
    'nodrive_t': ('Walkman detectado', 'Walkman found'),
    'nodrive_d': ('Windows todavía no muestra su memoria. Esperá unos segundos; si no aparece, desconectalo y volvé a conectarlo.',
                  "Windows doesn't show its storage yet. Wait a few seconds; if it doesn't appear, unplug it and connect it again."),
    'intl_t': ('Modelo internacional (NWZ-A82x)', 'International model (NWZ-A82x)'),
    'intl_d': ('Este modelo todavía no está soportado. Podés ayudar a que lo esté: tocá "Guardar informe" y compartilo en GitHub o Reddit. El informe solo lee datos; no cambia nada del Walkman.',
               "This model isn't supported yet. You can help: press \"Save report\" and share it on GitHub or Reddit. The report only reads information; it changes nothing on the Walkman."),
    'other_t': ('Este Walkman no es compatible', "This Walkman isn't supported"),
    'other_d': ('Detecté un %s. El homebrew funciona en el NW-A826, NW-A828 y NW-A829.',
                'Found a %s. The homebrew works on the NW-A826, NW-A828 and NW-A829.'),
    'old_t': ('%s · firmware %s', '%s · firmware %s'),
    'old_d': ('Primero hay que actualizarlo al firmware oficial 1.02 de Sony. El instalador lo hace por vos: lo descarga de Sony y lo instala (unos 2 minutos).',
              "First it needs Sony's official firmware 1.02. The installer does it for you: it downloads it from Sony and installs it (about 2 minutes)."),
    'ready_t': ('%s · firmware 1.02', '%s · firmware 1.02'),
    'ready_d': ('Todo listo para instalar. Tarda unos 2 minutos.', 'Ready to install. It takes about 2 minutes.'),
    'hook_t': ('%s · homebrew instalado', '%s · homebrew installed'),
    'hook_d': ('Podés actualizar el homebrew a esta versión (%s) o desinstalarlo.', 'You can update the homebrew to this version (%s) or uninstall it.'),
    'b_firmware': ('Paso 1: actualizar a 1.02', 'Step 1: update to 1.02'),
    'b_install': ('Instalar', 'Install'),
    'b_update': ('Actualizar homebrew', 'Update homebrew'),
    'b_uninstall': ('Desinstalar…', 'Uninstall…'),
    'b_report': ('Guardar informe', 'Save report'),
    'b_close': ('Cerrar', 'Close'),
    'need_battery': ('Marcá la casilla de la batería para continuar.', 'Tick the battery box to continue.'),
    'p_download': ('Descargando el firmware oficial de Sony…', "Downloading Sony's official firmware…"),
    'p_check': ('Verificando el firmware…', 'Checking the firmware…'),
    'p_copy': ('Copiando el homebrew al Walkman…', 'Copying the homebrew to the Walkman…'),
    'p_copyfw': ('Copiando el firmware al Walkman…', 'Copying the firmware to the Walkman…'),
    'p_flash': ('Actualizando el Walkman. No lo desconectes. (%d s)', "Updating the Walkman. Don't unplug it. (%d s)"),
    'p_reboot': ('El Walkman se está actualizando y reiniciando. No lo desconectes. (%d s)', "The Walkman is updating and restarting. Don't unplug it. (%d s)"),
    'working_t': ('Trabajando…', 'Working…'),
    'done_install': ('¡Listo! Ya podés desconectar el Walkman.\n\nPara abrir el reproductor: en el Walkman de Sony, mantené apretado HOME 3 segundos. La primera vez te pregunta el idioma. Para volver al Walkman de Sony, otra vez HOME 3 segundos.',
                     "Done! You can unplug the Walkman now.\n\nTo open the player: in Sony's Walkman, hold HOME for 3 seconds. The first time it asks for the language. To go back to Sony's Walkman, HOME for 3 seconds again."),
    'done_update': ('Homebrew actualizado. Ya podés desconectar el Walkman.', 'Homebrew updated. You can unplug the Walkman now.'),
    'done_firmware': ('Firmware 1.02 instalado. Ahora tocá "Instalar".', 'Firmware 1.02 installed. Now press "Install".'),
    'done_uninstall': ('Listo: el Walkman volvió al firmware oficial de Sony. La carpeta HB quedó en su memoria; podés borrarla cuando quieras.',
                       "Done: the Walkman is back on Sony's official firmware. The HB folder is still on it; delete it whenever you like."),
    'done_t': ('Listo', 'Done'),
    'err_t': ('No se pudo completar', "Couldn't finish"),
    'err_more': ('Si vuelve a pasar, tocá "Guardar informe" y compartilo.', 'If it happens again, press "Save report" and share it.'),
    'confirm_uninstall': ('Esto vuelve a instalar el firmware oficial de Sony (unos 2 minutos) y desactiva el homebrew. ¿Seguir?',
                          "This puts Sony's official firmware back (about 2 minutes) and turns the homebrew off. Continue?"),
    'confirm_close': ('El Walkman se está actualizando. Esperá a que termine antes de cerrar.', 'The Walkman is updating. Wait until it finishes before closing.'),
    'report_saved': ('Informe guardado en el Escritorio:\n%s', 'Report saved on the Desktop:\n%s'),
}


def tr(k, *a):
    s = T[k][0 if ES else 1]
    return s % a if a else s


def desktop():
    buf = ctypes.create_unicode_buffer(260)
    if ctypes.windll.shell32.SHGetFolderPathW(None, 0x10, None, 0, buf) == 0:	# CSIDL_DESKTOPDIRECTORY
        return buf.value
    return os.path.expanduser('~')


def sony_usb():
    """the Sony USB devices Windows sees now: [(pid, name, class, id)]"""
    ps = ("Get-CimInstance Win32_PnPEntity | Where-Object { $_.DeviceID -like '*VID_054C*' -or $_.Name -like '*WALKMAN*' } | "
          "ForEach-Object { $_.DeviceID + '|' + $_.Name + '|' + $_.PNPClass }")
    try:
        r = subprocess.run(['powershell', '-NoProfile', '-Command', ps], capture_output=True, text=True, timeout=20,
                           creationflags=I.NO_WINDOW)
    except (OSError, subprocess.SubprocessError):
        return []
    out = []
    for line in r.stdout.splitlines():
        p = line.strip().split('|')
        if len(p) < 3:
            continue
        pid = p[0].upper().split('PID_')[1][:4] if 'PID_' in p[0].upper() else ''
        out.append((pid, p[1], p[2], p[0]))
    return out


class App:
    def __init__(self, root):
        self.root = root
        self.q = queue.Queue()
        self.busy = False
        self.flashing = False
        self.state = None	# what the last scan found
        self.logf = None
        os.makedirs(I.DEFAULT_CACHE, exist_ok=True)
        try:
            self.logf = open(os.path.join(I.DEFAULT_CACHE, 'installer.log'), 'a', encoding='utf-8')
        except OSError:
            pass
        I.LOG = fwtrig.LOG = lambda s: self.q.put(('log', s))
        self.build()
        self.log('installer %s on %s' % (VERSION, platform.platform()))
        threading.Thread(target=self.watch, daemon=True).start()
        self.root.after(100, self.pump)
        self.root.protocol('WM_DELETE_WINDOW', self.close)

    # ---------- window ----------
    def build(self):
        r = self.root
        r.title(tr('title'))
        k = max(1.0, r.winfo_fpixels('1i') / 96)	# Windows display scaling: the window grows with the text
        self.k = k
        r.geometry('%dx%d' % (640 * k, 500 * k))
        r.minsize(int(600 * k), int(470 * k))
        st = ttk.Style()
        try:
            st.theme_use('vista')
        except tk.TclError:
            pass
        base = ('Segoe UI', 10)
        st.configure('.', font=base)
        st.configure('H.TLabel', font=('Segoe UI Semibold', 16))
        st.configure('S.TLabel', foreground='#555')
        st.configure('B.TLabel', font=('Segoe UI Semibold', 13))
        st.configure('Big.TButton', font=('Segoe UI Semibold', 11), padding=(18, 8))
        f = ttk.Frame(r, padding=20)
        f.pack(fill='both', expand=True)
        ttk.Label(f, text=tr('title'), style='H.TLabel').pack(anchor='w')
        ttk.Label(f, text=tr('sub'), style='S.TLabel').pack(anchor='w', pady=(0, 14))

        card = tk.Frame(f, bg='#f4f6f8', highlightbackground='#d8dde3', highlightthickness=1)
        card.pack(fill='x')
        self.dot = tk.Label(card, text='●', font=('Segoe UI', 28), bg='#f4f6f8', fg='#9aa3ad')
        self.dot.pack(side='left', padx=(14, 6), pady=10)
        tx = tk.Frame(card, bg='#f4f6f8')
        tx.pack(side='left', fill='x', expand=True, pady=10, padx=(0, 14))
        self.st_title = tk.Label(tx, text='', font=('Segoe UI Semibold', 13), bg='#f4f6f8', anchor='w', justify='left')
        self.st_title.pack(fill='x')
        self.st_detail = tk.Label(tx, text='', font=base, bg='#f4f6f8', fg='#333', anchor='w', justify='left', wraplength=int(500 * k))
        self.st_detail.pack(fill='x')

        steps = ttk.Frame(f)
        steps.pack(fill='x', pady=(14, 8))
        self.steps = []
        for k in ('step1', 'step2', 'step3'):
            row = ttk.Frame(steps)
            row.pack(fill='x', pady=1)
            mark = tk.Label(row, text='○', font=('Segoe UI', 12), fg='#9aa3ad')
            mark.pack(side='left')
            ttk.Label(row, text=tr(k)).pack(side='left', padx=6)
            self.steps.append(mark)

        self.battery = tk.BooleanVar(value=False)
        ttk.Checkbutton(f, text=tr('battery'), variable=self.battery).pack(anchor='w', pady=(6, 10))

        self.progbox = ttk.Frame(f)	# shown only while working
        self.progbox.pack(fill='x')
        self.prog_label = ttk.Label(self.progbox, text='')
        self.prog = ttk.Progressbar(self.progbox, mode='determinate', maximum=100)

        bar = ttk.Frame(f)
        bar.pack(fill='x', pady=(4, 0))
        self.b_main = ttk.Button(bar, text=tr('b_install'), style='Big.TButton', command=self.on_main)
        self.b_main.pack(side='right')
        self.b_uninstall = ttk.Button(bar, text=tr('b_uninstall'), command=self.on_uninstall)
        self.b_report = ttk.Button(bar, text=tr('b_report'), command=self.on_report)
        self.b_report.pack(side='left')
        self.b_details = ttk.Button(bar, text=tr('details'), command=self.toggle_details)
        self.b_details.pack(side='left', padx=6)

        self.details = tk.Text(f, height=8, font=('Consolas', 9), bg='#fbfbfb', relief='solid', borderwidth=1)
        self.details_on = False
        self.show_state(None)

    def toggle_details(self):
        self.details_on = not self.details_on
        if self.details_on:
            self.details.pack(fill='both', expand=True, pady=(10, 0))
        else:
            self.details.pack_forget()

    def log(self, s):
        line = '%s %s' % (datetime.datetime.now().strftime('%H:%M:%S'), s)
        self.details.insert('end', line + '\n')
        self.details.see('end')
        if self.logf:
            self.logf.write(line + '\n'); self.logf.flush()

    def status(self, color, title, detail):
        self.dot.config(fg=color)
        self.st_title.config(text=title)
        self.st_detail.config(text=detail)

    def set_steps(self, a, b, c):
        for mark, ok in zip(self.steps, (a, b, c)):
            mark.config(text='✔' if ok else '○', fg='#1e8e3e' if ok else '#9aa3ad')

    def main_button(self, key=None):
        self.action = key
        if key:
            self.b_main.config(text=tr(key), state='normal')
        else:
            self.b_main.config(text=tr('b_install'), state='disabled')

    # ---------- what is connected ----------
    def watch(self):
        n = 0
        while True:
            if not self.busy:
                try:
                    st = self.detect(n)
                    self.q.put(('state', st))
                except Exception as e:	# never let the watcher die
                    self.q.put(('log', 'detect: %r' % e))
                n += 1
            time.sleep(2)

    def detect(self, n):
        d, info = I.scan()
        if info:
            return {'kind': 'walkman', 'drive': d, 'info': info}
        if d:
            return {'kind': 'hbmode', 'drive': d}
        if n % 2 == 0:	# the device list is slower to ask: every other round
            usb = sony_usb()
            self.last_usb = usb
            pids = {p[0] for p in usb}
            if pids & set(INTL_PIDS):
                return {'kind': 'intl'}
            if pids & set(JP_PIDS):
                return {'kind': 'nodrive'}
        elif getattr(self, 'last_usb', None):
            pids = {p[0] for p in self.last_usb}
            if pids & set(INTL_PIDS):
                return {'kind': 'intl'}
        return {'kind': 'none'}

    def show_state(self, s):
        if self.busy:
            return
        changed = s != self.state
        self.state = s
        self.b_uninstall.pack_forget()
        k = s['kind'] if s else 'none'
        if k == 'none':
            self.status('#9aa3ad', tr('none_t'), tr('none_d')); self.set_steps(0, 0, 0); self.main_button(None)
        elif k == 'hbmode':
            self.status('#e8a33d', tr('hbmode_t'), tr('hbmode_d')); self.set_steps(1, 1, 1); self.main_button(None)
        elif k == 'nodrive':
            self.status('#e8a33d', tr('nodrive_t'), tr('nodrive_d')); self.set_steps(1, 0, 0); self.main_button(None)
        elif k == 'intl':
            self.status('#e8a33d', tr('intl_t'), tr('intl_d')); self.set_steps(1, 0, 0); self.main_button(None)
        else:
            info = s['info']
            name = info['product']
            if name.startswith('NWZ'):
                self.status('#e8a33d', tr('intl_t'), tr('intl_d')); self.set_steps(1, 0, 0); self.main_button(None)
            elif name not in I.MODELS:
                self.status('#d93025', tr('other_t'), tr('other_d', name)); self.set_steps(1, 0, 0); self.main_button(None)
            elif info['rev'] != '1.02':
                self.status('#e8a33d', tr('old_t', name, info['rev']), tr('old_d')); self.set_steps(1, 0, 0); self.main_button('b_firmware')
            elif info['subrev'] == I.HOOK_SUBREV:
                self.status('#1e8e3e', tr('hook_t', name), tr('hook_d', VERSION)); self.set_steps(1, 1, 1); self.main_button('b_update')
                self.b_uninstall.pack(side='right', padx=8)
            else:
                self.status('#1e8e3e', tr('ready_t', name), tr('ready_d')); self.set_steps(1, 1, 0); self.main_button('b_install')
        if changed and s:
            self.log('found: %s' % ({k2: v for k2, v in s.items()}))

    # ---------- actions ----------
    def on_main(self):
        if self.busy or not self.state or not self.action:
            return
        if not self.battery.get():
            messagebox.showinfo(tr('title'), tr('need_battery')); return
        self.run(self.action, self.state['drive'], self.state['info'])

    def on_uninstall(self):
        if self.busy or not self.state or self.state.get('kind') != 'walkman':
            return
        if not self.battery.get():
            messagebox.showinfo(tr('title'), tr('need_battery')); return
        if messagebox.askyesno(tr('title'), tr('confirm_uninstall')):
            self.run('uninstall', self.state['drive'], self.state['info'])

    def run(self, action, drive, info):
        self.busy = True
        self.b_main.config(state='disabled')
        self.b_uninstall.pack_forget()
        self.status('#1a73e8', tr('working_t'), '')
        threading.Thread(target=self.work, args=(action, drive, info), daemon=True).start()

    def work(self, action, drive, info):
        q = self.q
        prog = lambda text, pct=None: q.put(('prog', text, pct))
        def tick(secs, gone):
            prog(tr('p_reboot' if gone else 'p_flash', int(secs) + 15), None)
        try:
            I.check_model(info)
            prog(tr('p_download'), 0)
            official = I.official_firmware(I.DEFAULT_CACHE, lambda done, total: prog(tr('p_download'), 100 * done / max(total, 1)))
            prog(tr('p_check'), None)
            if action == 'b_firmware':
                prog(tr('p_copyfw'), None)
                self.flashing = True
                I.flash(drive, official, I.OFFICIAL_MD5, '1.02', '00', tick)
                q.put(('done', tr('done_firmware'), False)); return
            hook = I.hook_firmware(official)
            a2d = I.a2d_hb(official)
            if action == 'uninstall':
                I.disable_homebrew(drive)
                if info['subrev'] != '00':
                    prog(tr('p_copyfw'), None)
                    self.flashing = True
                    I.flash(drive, official, I.OFFICIAL_MD5, '1.02', '00', tick)
                q.put(('done', tr('done_uninstall'), True)); return
            prog(tr('p_copy'), None)
            I.copy_homebrew(drive, I.DEFAULT_HB, a2d)
            if action == 'b_install' or info['subrev'] != I.HOOK_SUBREV:
                prog(tr('p_copyfw'), None)
                self.flashing = True
                I.flash(drive, hook, I.HOOK_MD5, '1.02', I.HOOK_SUBREV, tick)
                q.put(('done', tr('done_install'), True)); return
            q.put(('done', tr('done_update'), True))
        except I.InstallError as e:
            q.put(('log', str(e)))
            q.put(('error', e.es if ES else str(e)))
        except Exception as e:
            q.put(('log', traceback.format_exc()))
            q.put(('error', repr(e)))
        finally:
            self.flashing = False

    def on_report(self):
        threading.Thread(target=self.report, daemon=True).start()

    def report(self):
        """read-only: what Windows sees of the Walkman, for an issue on GitHub / a Reddit post"""
        lines = ['Walkman Homebrew installer %s report, %s' % (VERSION, datetime.datetime.now().isoformat(timespec='seconds')),
                 'Windows: %s' % platform.platform(), '', 'Sony USB devices (Windows device list):']
        for pid, name, cls, did in sony_usb():
            lines.append('  PID %s  %s  [%s]  %s' % (pid or '-', name, cls, did))
        lines += ['', 'Drives:']
        for d in I.drives():
            info = fwtrig.devinfo(d)
            lines.append('  %s  walkman=%s  devinfo=%s' % (d, fwtrig.is_walkman(d), info))
        lines += ['', 'Installer log (last lines):']
        try:
            lines += open(os.path.join(I.DEFAULT_CACHE, 'installer.log'), encoding='utf-8').read().splitlines()[-60:]
        except OSError:
            pass
        p = os.path.join(desktop(), 'Walkman-report.txt')
        open(p, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
        self.q.put(('report', p))

    # ---------- events from the threads ----------
    def pump(self):
        try:
            while True:
                ev = self.q.get_nowait()
                kind = ev[0]
                if kind == 'log':
                    self.log(ev[1])
                elif kind == 'state':
                    self.show_state(ev[1])
                elif kind == 'prog':
                    if not self.prog.winfo_ismapped():
                        self.prog_label.pack(anchor='w'); self.prog.pack(fill='x', pady=(2, 10))
                    self.prog_label.config(text=ev[1])
                    if ev[2] is None:
                        if str(self.prog['mode']) != 'indeterminate':
                            self.prog.config(mode='indeterminate'); self.prog.start(15)
                    else:
                        if str(self.prog['mode']) != 'determinate':
                            self.prog.stop(); self.prog.config(mode='determinate')
                        self.prog['value'] = ev[2]
                elif kind == 'done':
                    self.finish()
                    self.log('done: %s' % ev[1])
                    self.status('#1e8e3e', tr('done_t'), ev[1])
                    self.set_steps(1, 1, ev[2])
                    messagebox.showinfo(tr('done_t'), ev[1])
                elif kind == 'error':
                    self.finish()
                    self.log('ERROR: %s' % ev[1])
                    self.status('#d93025', tr('err_t'), ev[1] + '\n' + tr('err_more'))
                elif kind == 'report':
                    self.log('report: %s' % ev[1])
                    messagebox.showinfo(tr('title'), tr('report_saved', ev[1]))
                    try:
                        os.startfile(ev[1])
                    except OSError:
                        pass
        except queue.Empty:
            pass
        self.root.after(100, self.pump)

    def finish(self):
        self.prog.stop(); self.prog.config(mode='determinate'); self.prog['value'] = 0
        self.prog_label.pack_forget(); self.prog.pack_forget()
        self.state = None
        self.root.after(4000, self.resume_watch)	# the result stays on screen a moment before the watcher takes over

    def resume_watch(self):
        self.busy = False

    def close(self):
        if self.flashing:
            messagebox.showwarning(tr('title'), tr('confirm_close')); return
        self.root.destroy()


def selftest(path):
    """--selftest FILE: the whole checked chain without a Walkman (Sony's updater, hook, Bluetooth helper, bundled
    homebrew); the result goes to FILE, since the windowed program has no console"""
    lines = []
    I.LOG = fwtrig.LOG = lines.append
    ok = False
    try:
        official = I.official_firmware(I.DEFAULT_CACHE)
        I.hook_firmware(official)
        I.a2d_hb(official)
        files = sorted(os.path.relpath(os.path.join(r, f), I.DEFAULT_HB) for r, _, fs in os.walk(I.DEFAULT_HB) for f in fs)
        lines.append('bundled HB: %s' % ', '.join(files))
        ok = 'bin' + os.sep + 'hbplayer' in files
    except Exception:
        lines.append(traceback.format_exc())
    lines.append('SELFTEST %s' % ('OK' if ok else 'FAILED'))
    open(path, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    return 0 if ok else 1


def main():
    if len(sys.argv) > 2 and sys.argv[1] == '--selftest':
        sys.exit(selftest(sys.argv[2]))
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)	# sharp text on scaled screens
    except (AttributeError, OSError):
        pass
    root = tk.Tk()
    try:
        root.iconbitmap(os.path.join(I.BUNDLE, 'walkman.ico'))
    except tk.TclError:
        pass
    App(root)
    root.mainloop()


if __name__ == '__main__':
    main()
