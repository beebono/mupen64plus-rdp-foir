import sys,collections
VU="VMULF VMULU VRNDP VMULQ VMUDL VMUDM VMUDN VMUDH VMACF VMACU VRNDN VMACQ VMADL VMADM VMADN VMADH VADD VSUB v18 VABS VADDC VSUBC v22 v23 v24 v25 v26 v27 v28 VSAR v30 v31 VLT VEQ VNE VGE VCL VCH VCR VMRG VAND VNAND VOR VNOR VXOR VNXOR v46 v47 VRCP VRCPL VRCPH VMOV VRSQ VRSQL VRSQH VNOP v56 v57 v58 v59 v60 v61 v62 VNOP2".split()
LW="LBV LSV LLV LDV LQV LRV LPV LUV LHV LFV LWV LTV".split()+["l%d"%i for i in range(12,32)]
SW="SBV SSV SLV SDV SQV SRV SPV SUV SHV SFV SWV STV".split()+["s%d"%i for i in range(12,32)]
def name(i):
    if i<1024: return VU[i//16]
    if i<1056: return LW[i-1024]
    if i<1088: return SW[i-1056]
    if i<1120: return "COP2.rs%d"%(i-1088)
    if i<1152: return "COP0.rs%d"%(i-1120)
    if i<1216: return "T%02o"%(i-1152)
    if i<1280: return "SPECIAL%02o"%(i-1216)
    if 1400<=i<1432: return "slowL.%s"%LW[i-1400]
    if 1432<=i<1464: return "slowS.%s"%SW[i-1432]
    if 1500<=i<1532: return "MFC0.r%d"%(i-1500)
    if 1532<=i<1564: return "MTC0.r%d"%(i-1532)
    return "REGIMM%d"%(i-1280)
for f in sys.argv[1:]:
    c=collections.Counter(); ops={}; e=collections.Counter(); rvu={};rvl={}; tot=0; ent=0
    for l in open(f):
        w=l.split()
        if w[0]=="total": tot=int(w[1])
        elif w[0]=="enter": ent=int(w[1])
        elif w[0]=="op":
            i=int(w[1]); n=int(w[2]); c[name(i)]+=n
            if i<1024: e[(VU[i//16], (i%16))]+=n
        elif w[0]=="runvu": rvu[int(w[1])]=int(w[2])
        elif w[0]=="runvl": rvl[int(w[1])]=int(w[2])
    vu=sum(n for k,n in c.items() if k in VU); ls=sum(n for k,n in c.items() if k in LW or k in SW)
    print("=====",f,"total instrs",tot,"VU %.1f%% LS %.1f%% blocks-enter %.2f%%"%(100*vu/tot,100*ls/tot,100*ent/tot))
    print(" top:", ", ".join("%s %.1f%%"%(k,100*n/tot) for k,n in c.most_common(32)))
    ez=sum(n for (k,x),n in e.items() if x==0); print(" VU with e==0: %.1f%% ; e 2..3(q): %.1f%% ; e 4-7(h) %.1f%% ; e 8-15 (w) %.1f%%"%tuple(100*sum(n for (k,x),n in e.items() if a<=x<=b)/max(vu,1) for a,b in ((0,1),(2,3),(4,7),(8,15))))
    def runstats(r):
        tot_instr=sum(k*n for k,n in r.items()); runs=sum(r.values())
        return "runs %d avg len %.2f ; instr in runs>=2: %.1f%% ; >=4: %.1f%% ; >=8 %.1f%%"%(runs,tot_instr/max(runs,1),*[100*sum(k*n for k,n in r.items() if k>=m)/max(tot_instr,1) for m in (2,4,8)])
    print(" VU-run:",runstats(rvu)); print(" VU/LS-run:",runstats(rvl))
