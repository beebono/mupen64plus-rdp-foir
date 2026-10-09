import numpy as np,sys
d,name=sys.argv[1:3]
b={m:np.fromfile(f"{d}/{m}.raw",dtype='>i2') for m in ("cxd4","parallel","hle")}
def fd(x,y):
    n=min(len(x),len(y)); i=np.nonzero(x[:n]!=y[:n])[0]; return (int(i[0]) if len(i) else n), n
print(name,"samples:",{m:len(v) for m,v in b.items()},"nonzero:",{m:int((v!=0).sum()) for m,v in b.items()})
for a,c in (("parallel","cxd4"),("hle","cxd4")):
    i,n=fd(b[a],b[c]); print(f"  {a} vs {c}: identical over the common {n} samples" if i==n else f"  {a} vs {c}: first difference at sample {i} of {n}; max |diff| in the next 4000 = {np.abs(b[a][i:i+4000].astype(int)-b[c][i:i+4000]).max()}")
