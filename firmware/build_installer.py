"""Builds dist/Walkman-Homebrew-Installer.exe: the installer window with the homebrew (build/HB) inside, one file,
no Python needed on the user's computer. Run from anywhere after build_all.sh:
    pip install pyinstaller cryptography
    python firmware/build_installer.py [OUTDIR]      (default: dist/)"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HB = os.path.join(ROOT, 'build', 'HB')
if not os.path.exists(os.path.join(HB, 'bin', 'hbplayer')):
    sys.exit('build/HB is missing: run "sh build_all.sh" first')
work = tempfile.mkdtemp(prefix='hbinst-')
out = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(ROOT, 'dist')
cmd = [sys.executable, '-m', 'PyInstaller', '--noconfirm', '--onefile', '--windowed',
       '--name', 'Walkman-Homebrew-Installer',
       '--icon', os.path.join(HERE, 'walkman.ico'),
       '--add-data', '%s%sHB' % (HB, os.pathsep),
       '--add-data', '%s%s.' % (os.path.join(HERE, 'walkman.ico'), os.pathsep),
       '--paths', HERE, '--paths', os.path.join(ROOT, 'homebrew', 'bt'),
       '--hidden-import', 'patch_a2d',
       '--distpath', out, '--workpath', work, '--specpath', work,
       os.path.join(HERE, 'installer_gui.py')]
subprocess.check_call(cmd)
print('->', os.path.join(out, 'Walkman-Homebrew-Installer.exe'))
