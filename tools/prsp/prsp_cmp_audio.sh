#!/bin/sh
# on device. prsp_cmp_audio.sh ROMFILE REFSO NEWSO TAG SECONDS [reuse]  -> compares the LLE audio stream (common prefix) and the RSP hash stream
D=/storage/prsp-dev
ROM="$1"; REF=$2; NEW=$3; TAG=$4; SEC=$5; REUSE=$6
[ "$REUSE" = "reuse" ] && [ -f $D/e2e/aref_$TAG/audio.bytes ] || $D/prsp_e2e_audio.sh "$ROM" $REF aref_$TAG $SEC
$D/prsp_e2e_audio.sh "$ROM" $NEW anew_$TAG $SEC
R=$D/e2e/aref_$TAG; N=$D/e2e/anew_$TAG
BR=$(cat $R/audio.bytes); BN=$(cat $N/audio.bytes); M=$BR; [ $BN -lt $M ] && M=$BN
if cmp -s -n $M $R/audio.raw $N/audio.raw; then AS=IDENTICAL; else AS=DIFFERENT; fi
LR=$(cat $R/rsp.hash.lines); LN=$(cat $N/rsp.hash.lines); L=$LR; [ $LN -lt $L ] && L=$LN
HR=$(head -n $L $R/rsp.hash | md5sum | cut -d' ' -f1); HN=$(head -n $L $N/rsp.hash | md5sum | cut -d' ' -f1)
[ "$HR" = "$HN" ] && HS=IDENTICAL || HS=DIFFERENT
echo "AUDIO $TAG audio(ref=$BR new=$BN common=$M $AS) rsp-hash(ref=$LR new=$LN common=$L $HS)"
rm -f $R/audio.raw $N/audio.raw
true
