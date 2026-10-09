# Summarises logs/T_<game>_<mode>_<build>_<run>.{foirt,rspt} written by prsp_time.sh (copy them to a "timing" dir next to this script).
import re,os,glob,sys,numpy as np,collections
D=os.path.dirname(os.path.abspath(__file__))+"/timing"
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
    ns=sorted(k for k in fr if k>=240)
    cum=0.0; cums={}
    for k in sorted(fr):
        if k<240: continue
        cum+=120*fr[k][1]/1000.0; cums[k]=cum
    a=min(fr[k][0]-cums[k] for k in ns)
    t1=a+cums[n]; t0=t1-120*fr[n][1]/1000.0
    c=lambda t: np.interp(t,rs[:,0],rs[:,2])
    return fr[n][1],fr[n][2],(c(t1)-c(t0))/120.0
games={"Super_Mario_64":600,"Star_Fox_64":1080,"Donkey_Kong_64":1080,"Banjo-Kazooie":600,"Pokemon_Snap":600}
if __name__=="__main__":
    # tags: <game>_<mode>_<build>_<run>
    tags=sorted(os.path.basename(p)[2:-6] for p in glob.glob(D+"/T_*.foirt"))
    groups=collections.defaultdict(list)
    for t in tags:
        m=re.match(r"(.+)_(hle|parallel)_([A-Za-z0-9]+?)_F?(\d+)$",t)
        if not m: continue
        g,mode,b,run=m.groups()
        if g not in games: continue
        v=res(t,games[g])
        if v: groups[(g,mode,b)].append(v)
    for (g,mode,b),vs in sorted(groups.items()):
        a=np.mean(vs,axis=0)
        print(f"{g:16s} {mode:8s} {b:10s} n={len(vs)}  wall {a[0]:5.1f} ms ({min(60,1000/a[0]):4.1f} fps)  RSP {a[2]:5.1f} ms (net {a[2]-a[1]:5.1f})   runs: "+" ".join(f"{v[0]:.1f}/{v[2]:.1f}" for v in vs))
