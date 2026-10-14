# Read-only DNK probe for the Sony Walkman (Rockbox scsitool protocol). No writes.
import ctypes, ctypes.wintypes as W, struct, sys
dev = sys.argv[1] if len(sys.argv)>1 else r'\\.\H:'
GENERIC_READ=0x80000000; GENERIC_WRITE=0x40000000; FILE_SHARE=3; OPEN_EXISTING=3
IOCTL_SCSI_PASS_THROUGH_DIRECT=0x4D014; SCSI_IOCTL_DATA_IN=1
k=ctypes.windll.kernel32
class SPTD(ctypes.Structure):
    _fields_=[('Length',W.USHORT),('ScsiStatus',ctypes.c_ubyte),('PathId',ctypes.c_ubyte),('TargetId',ctypes.c_ubyte),('Lun',ctypes.c_ubyte),
              ('CdbLength',ctypes.c_ubyte),('SenseInfoLength',ctypes.c_ubyte),('DataIn',ctypes.c_ubyte),('DataTransferLength',W.ULONG),
              ('TimeOutValue',W.ULONG),('DataBuffer',ctypes.c_void_p),('SenseInfoOffset',W.ULONG),('Cdb',ctypes.c_ubyte*16)]
class SPTDB(ctypes.Structure):
    _fields_=[('sptd',SPTD),('sense',ctypes.c_ubyte*32)]
h=k.CreateFileW(dev,GENERIC_READ|GENERIC_WRITE,FILE_SHARE,None,OPEN_EXISTING,0,None)
if h==ctypes.c_void_p(-1).value or h==-1: sys.exit(f'open failed {dev}: err {k.GetLastError()} (try as administrator)')
def scsi(cdb,n):
    buf=ctypes.create_string_buffer(n); s=SPTDB(); s.sptd.Length=ctypes.sizeof(SPTD); s.sptd.CdbLength=len(cdb)
    s.sptd.SenseInfoLength=32; s.sptd.DataIn=SCSI_IOCTL_DATA_IN; s.sptd.DataTransferLength=n; s.sptd.TimeOutValue=10
    s.sptd.DataBuffer=ctypes.cast(buf,ctypes.c_void_p); s.sptd.SenseInfoOffset=SPTD.sense.offset if hasattr(SPTD,'sense') else ctypes.sizeof(SPTD)
    for i,b in enumerate(cdb): s.sptd.Cdb[i]=b
    ret=W.DWORD(0)
    ok=k.DeviceIoControl(h,IOCTL_SCSI_PASS_THROUGH_DIRECT,ctypes.byref(s),ctypes.sizeof(s),ctypes.byref(s),ctypes.sizeof(s),ctypes.byref(ret),None)
    return ok, s.sptd.ScsiStatus, bytes(s.sense[:18]), buf.raw
def dnk(cmd,sub,n,arg=0):
    cdb=[0xDD,0,0,0,(arg>>8)&0xff,arg&0xff,0,0xBC,(n>>8)&0xff,n&0xff,cmd,sub]; return scsi(cdb,n)
ok,st,sense,data=scsi([0x12,0,0,0,36,0],36); print('INQUIRY ok=%s status=%d ->'%(ok,st),data[8:36])
for name,sub,n in (('product_id',6,12),('destination',8,4),('model_id',9,4),('storage_gb',4,4),('serial',1,16),('ufn',21,8),('kas',22,64)):
    ok,st,sense,data=dnk(0x23,sub,n)
    print(f'{name:12} ok={ok} status={st} sense={sense[:3].hex()} data={data.hex()}  ascii={data.decode("latin1")!r}')
k.CloseHandle(h)
