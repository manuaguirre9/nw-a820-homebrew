# Sony Walkman firmware-update trigger over SCSI pass-through (Rockbox scsitool protocol). Windows only.
#   python fwtrig.py info [DRIVE]           -> read-only: 0xFC 'dbmn' DEVINFO + DNK devinfo
#   python fwtrig.py go <upg-md5> [DRIVE]   -> checks DRIVE\NW_WM_FW.UPG has that md5, then sends the
#                                              0xFC 0x04 'dbmn' upgrade command (flags 0x80, then 0x00)
# DRIVE defaults to H:. install.py uses devinfo() and trigger() from here.
import ctypes, ctypes.wintypes as W, hashlib, os, sys

GENERIC_READ=0x80000000; GENERIC_WRITE=0x40000000; FILE_SHARE=3; OPEN_EXISTING=3
IOCTL_SCSI_PASS_THROUGH_DIRECT=0x4D014; SCSI_IOCTL_DATA_IN=1
k=ctypes.windll.kernel32
class SPTD(ctypes.Structure):
    _fields_=[('Length',W.USHORT),('ScsiStatus',ctypes.c_ubyte),('PathId',ctypes.c_ubyte),('TargetId',ctypes.c_ubyte),('Lun',ctypes.c_ubyte),
              ('CdbLength',ctypes.c_ubyte),('SenseInfoLength',ctypes.c_ubyte),('DataIn',ctypes.c_ubyte),('DataTransferLength',W.ULONG),
              ('TimeOutValue',W.ULONG),('DataBuffer',ctypes.c_void_p),('SenseInfoOffset',W.ULONG),('Cdb',ctypes.c_ubyte*16)]
class SPTDB(ctypes.Structure):
    _fields_=[('sptd',SPTD),('sense',ctypes.c_ubyte*32)]
def scsi(h,cdb,n):
    buf=ctypes.create_string_buffer(n); s=SPTDB(); s.sptd.Length=ctypes.sizeof(SPTD); s.sptd.CdbLength=len(cdb)
    s.sptd.SenseInfoLength=32; s.sptd.DataIn=SCSI_IOCTL_DATA_IN; s.sptd.DataTransferLength=n; s.sptd.TimeOutValue=30
    s.sptd.DataBuffer=ctypes.cast(buf,ctypes.c_void_p); s.sptd.SenseInfoOffset=ctypes.sizeof(SPTD)
    for i,b in enumerate(cdb): s.sptd.Cdb[i]=b
    ret=W.DWORD(0)
    ok=k.DeviceIoControl(h,IOCTL_SCSI_PASS_THROUGH_DIRECT,ctypes.byref(s),ctypes.sizeof(s),ctypes.byref(s),ctypes.sizeof(s),ctypes.byref(ret),None)
    return ok, s.sptd.ScsiStatus, bytes(s.sense[:18]), buf.raw, k.GetLastError()
def openh(drive):
    h=k.CreateFileW('\\\\.\\' + drive.rstrip('\\/'),GENERIC_READ|GENERIC_WRITE,FILE_SHARE,None,OPEN_EXISTING,0,None)
    if h in (ctypes.c_void_p(-1).value,-1): return None
    return h

def devinfo(drive):
    """{'vendor', 'product', 'rev', 'subrev'} of a Sony Walkman on drive, or None (not one, or no access)"""
    h=openh(drive)
    if not h: return None
    try:
        ok,st,sense,data,err=scsi(h,[0x12,0,0,0,36,0],36)
        if not ok or st or not data[8:36].startswith(b'SONY    WALKMAN'): return None
        ok,st,sense,data,err=scsi(h,[0xFC,0,0x20,ord('d'),ord('b'),ord('m'),ord('n'),0,0x80,0,0,0],0x80)
        if not ok or st or not data.startswith(b'DEVINFO'): return None
        t=lambda b: b.decode('latin1').strip('\0 ')
        return {'vendor':t(data[16:24]),'product':t(data[24:40]),'rev':t(data[40:44]),'subrev':t(data[44:48])}
    finally:
        k.CloseHandle(h)

def trigger(drive, want_md5):
    """asks the Walkman on drive to flash DRIVE\\NW_WM_FW.UPG (must have want_md5); True if it accepted"""
    p=os.path.join(drive.rstrip('\\/')+'\\','NW_WM_FW.UPG')
    md5=hashlib.md5(open(p,'rb').read()).hexdigest(); print('%s md5 %s size %d'%(p,md5,os.path.getsize(p)))
    if md5!=want_md5: print('md5 mismatch, aborting'); return False
    os.system('powershell -NoProfile -Command "Write-VolumeCache -DriveLetter %s" >nul 2>&1'%drive[0])
    h=openh(drive)
    if not h: print('cannot open %s'%drive); return False
    try:
        for flags in (0x80,0x00):
            ok,st,sense,data,err=scsi(h,[0xFC,0,0x04,ord('d'),ord('b'),ord('m'),ord('n'),0,flags,0,0,0],0x80)
            print('FC04 upgrade flags=0x%02x ok=%s st=%d sense=%s err=%d data=%s'%(flags,ok,st,sense[:3].hex(),err,data[:16].hex()))
            if ok and st==0: print('accepted'); return True
        return False
    finally:
        k.CloseHandle(h)

if __name__ == '__main__':
    mode=sys.argv[1] if len(sys.argv)>1 else 'info'
    if mode=='info':
        drive=sys.argv[2] if len(sys.argv)>2 else 'H:'
        h=openh(drive)
        if not h: sys.exit('open failed %s: err %d'%(drive,k.GetLastError()))
        ok,st,sense,data,err=scsi(h,[0x12,0,0,0,36,0],36); print('INQUIRY ok=%s st=%d'%(ok,st),data[8:36])
        ok,st,sense,data,err=scsi(h,[0xFC,0,0x20,ord('d'),ord('b'),ord('m'),ord('n'),0,0x80,0,0,0],0x80)
        print('FC20 DEVINFO ok=%s st=%d sense=%s err=%d'%(ok,st,sense[:3].hex(),err)); print('  raw:',data[:16]); print('  vendor:',data[16:24],'product:',data[24:40],'rev:',data[40:44],'subrev:',data[44:48],'size:',data[48:52],'serial:',data[52:68])
        ok,st,sense,data,err=scsi(h,[0xDD,0,0,0,0,0,0,0xBC,0,64,0x12,0],64); print('DNK devinfo ok=%s st=%d'%(ok,st),data.rstrip(b'\0'))
        k.CloseHandle(h)
    elif mode=='go':
        sys.exit(0 if trigger(sys.argv[3] if len(sys.argv)>3 else 'H:', sys.argv[2]) else 1)
