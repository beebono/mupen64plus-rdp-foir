#!/bin/sh
# on device. prsp_e2e.sh ROMFILE MODE RSPSO TAG TMO [FRAMES]: deterministic run for RSP bit-exactness
#   MODE hle (rsp-hle + RspFallback=RSPSO) | parallel (RSPSO as the RSP plugin)
# per-DoRspCycles FNV hash (RSPT_HASH, see rspt.c), screenshots at FRAMES (FOIR_SHOT_FRAMES), AI DMA stream with AUDIO=1; results in $D/e2e/TAG/{shots.md5,rsp.hash.md5,rsp.hash.lines,audio.raw}
D=/storage/prsp-dev; . $D/prsp_common.sh
ROM="/storage/roms/n64/$1"; rsp_args $2 $3; TAG=$4; TMO=${5:-70}; FR=${6:-300,900,1500,2100,2700}
LAST=$(echo $FR | tr ',' '\n' | tail -1)
OUT=$D/e2e/$TAG; rm -rf $OUT; mkdir -p $OUT
export FOIR_THREADS=3 FOIR_SHOT_DIR=$OUT FOIR_SHOT_FRAMES=$FR RSPT_HASH=$OUT/rsp.hash RSPT_LOG=$OUT/rspt
AUD="--audio dummy"
if [ "$AUDIO" = "1" ]; then export ADUMP_FILE=$OUT/audio.raw; AUD="--audio $D/plugins/mupen64plus-audio-adump.so"; fi
timeout -s KILL $TMO /usr/local/bin/mupen64plus --configdir $D/cfg --datadir /usr/local/share/mupen64plus \
  --plugindir $D/plugins --gfx mupen64plus-rdp-foir.so $RSPARGS \
  $AUD --input mupen64plus-input-sdl.so --noosd --set Core[RandomizeInterrupt]=False --emumode 2 "$ROM" > $OUT/log 2>&1 &
MP=$!
i=0
if [ "$AUDIO" = "1" ]; then LIMIT=$((TMO*2)); else LIMIT=$((TMO*2)); fi
while [ $i -lt $LIMIT ]; do
  sleep 0.5; i=$((i+1))
  [ -f $OUT/frame_$(printf %05d $LAST).ppm ] && { sleep 0.5; break; }
  kill -0 $MP 2>/dev/null || break
done
pkill -KILL -x mupen64plus; sleep 0.3
( cd $OUT && md5sum frame_*.ppm > shots.md5 2>/dev/null; wc -l < rsp.hash > rsp.hash.lines; md5sum rsp.hash > rsp.hash.md5; rm -f frame_*.ppm )
true
