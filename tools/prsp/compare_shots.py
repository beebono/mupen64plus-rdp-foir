import os,sys
import numpy as np
from PIL import Image
R=os.path.dirname(os.path.abspath(__file__))+"/"+(sys.argv[1] if len(sys.argv)>1 else "det")
modes=sys.argv[2].split(",") if len(sys.argv)>2 else ["parallel","hle"]
ref="cxd4"
names=sorted(os.listdir(R+"/"+ref))
frames=(300,900,1500,2100,2700)
print("fraction of pixels differing from cxd4 (any channel >0), per frame", frames)
tot={m:[0,0,0] for m in modes}  # identical, differing, missing
for n in names:
    line=f"{n[:38]:38s}"
    for m in modes:
        cells=[]
        for f in frames:
            a=f"{R}/{ref}/{n}/frame_{f:05d}.ppm"; b=f"{R}/{m}/{n}/frame_{f:05d}.ppm"
            if not (os.path.exists(a) and os.path.exists(b)): cells.append("  -  "); tot[m][2]+=1; continue
            x=np.asarray(Image.open(a).convert("RGB")).astype(int); y=np.asarray(Image.open(b).convert("RGB")).astype(int)
            if x.shape!=y.shape: cells.append("shape"); tot[m][1]+=1; continue
            d=(np.abs(x-y).max(axis=2)>0).mean()
            if d==0: tot[m][0]+=1
            else: tot[m][1]+=1
            cells.append(f"{d:5.3f}" if d else "  =  ")
        line+=f" | {m[:3]}: "+" ".join(cells)
    print(line)
print({m:{"identical":v[0],"differing":v[1],"missing":v[2]} for m,v in tot.items()})
