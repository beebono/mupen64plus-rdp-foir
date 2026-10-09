import re,os,glob,sys,numpy as np
D=os.path.dirname(os.path.abspath(__file__))+"/timing"
games={"Super_Mario_64":600,"Star_Fox_64":1080,"Donkey_Kong_64":1080,"Banjo-Kazooie":600,"Pokemon_Snap":600}
def load(tag):
    f=f"{D}/T_{tag}.foirt"; r=f"{D}/T_{tag}.rspt"
    if not (os.path.exists(f) and os.path.exists(r)): return None
    fr={}
    for l in open(f):
        m=re.match(r"([\d.]+) frame (\d+): ([\d.]+) ms/frame wall, ([\d.]+) ms/frame RDP",l)
        if m: fr[int(m.group(2))]=(float(m.group(1)),float(m.group(3)),float(m.group(4)))
    rs=np.array([[float(x) for x in l.split()] for l in open(r) if len(l.split())==3])
    return fr,rs
def res(tag,n):
    x=load(tag)
    if not x: return None
    fr,rs=x
    if n not in fr or (n-120) not in fr: return None
    # exact frame durations from the plugin's wall ms/frame; anchor to absolute time with the least-lagged log line
    ns=sorted(k for k in fr if k>=240)
    cum=0.0; cums={}
    for k in sorted(fr):
        if k<240: continue
        cum+=120*fr[k][1]/1000.0; cums[k]=cum
    a=min(fr[k][0]-cums[k] for k in ns)
    t1=a+cums[n]; t0=t1-120*fr[n][1]/1000.0
    c=lambda t: np.interp(t,rs[:,0],rs[:,2])
    return fr[n][1],fr[n][2],(c(t1)-c(t0))/120.0
def main():
    out=[]
    for g,n in games.items():
        row=[]
        for m in ("cxd4","parallel","hle"):
            vals=[res(os.path.basename(p)[2:-6],n) for p in sorted(glob.glob(f"{D}/T_{g}_{m}_*.foirt"))]
            vals=[v for v in vals if v]
            if not vals: row.append("n/a"); continue
            a=np.mean(vals,axis=0)
            row.append(f"{a[0]:.1f} ms / {min(60,1000/a[0]):.0f} fps ; RSP {a[2]:.1f} (net of RDP {a[2]-a[1]:.1f}) ms  [n={len(vals)}]")
        print(f"| {g} ({n}) | "+" | ".join(row)+" |")
if __name__=='__main__': main()
