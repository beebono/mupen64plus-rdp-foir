#!/bin/sh
# on device. prsp_cmp.sh ROMFILE MODE REFSO NEWSO TAG TMO [reuse]   (ROM file name has spaces replaced by _ in TAG by the caller)
D=/storage/prsp-dev
ROM="$1"; MODE=$2; REF=$3; NEW=$4; TAG=$5; TMO=$6; REUSE=$7
[ "$REUSE" = "reuse" ] && [ -f $D/e2e/ref_$TAG/rsp.hash.lines ] || $D/prsp_e2e.sh "$ROM" $MODE $REF ref_$TAG $TMO
$D/prsp_e2e.sh "$ROM" $MODE $NEW new_$TAG $TMO
R=$D/e2e/ref_$TAG; N=$D/e2e/new_$TAG
SH=$(awk 'NR==FNR{a[$2]=$1;next} ($2 in a){n++; if(a[$2]!=$1) bad++} END{printf "%d/%d", n+0, bad+0}' $R/shots.md5 $N/shots.md5 2>/dev/null)
NSH_R=$(wc -l < $R/shots.md5); NSH_N=$(wc -l < $N/shots.md5)
LR=$(cat $R/rsp.hash.lines); LN=$(cat $N/rsp.hash.lines)
M=$LR; [ $LN -lt $M ] && M=$LN
HR=$(head -n $M $R/rsp.hash | md5sum | cut -d' ' -f1); HN=$(head -n $M $N/rsp.hash | md5sum | cut -d' ' -f1)
[ "$HR" = "$HN" ] && HS=IDENTICAL || HS=DIFFERENT
echo "RESULT $TAG $MODE shots(ref=$NSH_R new=$NSH_N common/bad=$SH) rsp-hash(ref=$LR new=$LN common=$M $HS)"
true
