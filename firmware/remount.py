# Force Windows to drop its cached view of H: (FAT written by the device) so new files become visible.
import ctypes, ctypes.wintypes as W, sys
k = ctypes.windll.kernel32
h = k.CreateFileW(r'\\.\H:', 0xC0000000, 3, None, 3, 0, None)
if h in (-1, ctypes.c_void_p(-1).value): sys.exit('open failed %d' % k.GetLastError())
r = W.DWORD()
for name, code in (('LOCK', 0x90018), ('DISMOUNT', 0x90020)):
    ok = k.DeviceIoControl(h, code, None, 0, None, 0, ctypes.byref(r), None)
    print(name, 'ok' if ok else 'failed err %d' % k.GetLastError())
k.CloseHandle(h)
