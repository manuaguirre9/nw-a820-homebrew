import sys; sys.argv=['probe.py',r'\\.\H:']
exec(open('probe.py').read().split("ok,st,sense,data=scsi([0x12")[0])
ok,st,sense,data=dnk(0x12,0,64); print('devinfo:',repr(data.rstrip(b'\0')))
ok,st,sense,data=dnk(0x23,9,4); print('model_id:',data.hex())
ok,st,sense,data=dnk(0x23,8,4); print('destination:',data.hex())
k.CloseHandle(h)
