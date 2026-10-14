import sys,re
n=int(sys.argv[2]) if len(sys.argv)>2 else 4
d=open(sys.argv[1],'rb').read()
for m in re.finditer(rb'[\x20-\x7e\t]{%d,}'%n,d): print(m.group().decode())
