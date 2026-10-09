#!/bin/sh
# on device. LLE audio dump for a fixed number of AI buffers' worth of time: prsp_e2e_audio.sh ROMFILE RSPSO TAG SECONDS
# (parallel-rsp as the RSP plugin runs the audio ucode in LLE; the stream is dumped by mupen64plus-audio-adump.so).
D=/storage/prsp-dev; . $D/prsp_common.sh
ROM="/storage/roms/n64/$1"; rsp_args parallel $2; TAG=$3; SEC=${4:-40}
OUT=$D/e2e/$TAG; rm -rf $OUT; mkdir -p $OUT
export FOIR_THREADS=3 ADUMP_FILE=$OUT/audio.raw RSPT_HASH=$OUT/rsp.hash RSPT_LOG=$OUT/rspt
timeout -s KILL $((SEC+20)) /usr/local/bin/mupen64plus --configdir $D/cfg --datadir /usr/local/share/mupen64plus \
  --plugindir $D/plugins --gfx mupen64plus-rdp-foir.so $RSPARGS \
  --audio $D/plugins/mupen64plus-audio-adump.so --input mupen64plus-input-sdl.so --noosd --set Core[RandomizeInterrupt]=False --emumode 2 "$ROM" > $OUT/log 2>&1 &
sleep $SEC
pkill -KILL -x mupen64plus; sleep 0.3
( cd $OUT && wc -c < audio.raw > audio.bytes; md5sum audio.raw > audio.md5; wc -l < rsp.hash > rsp.hash.lines; md5sum rsp.hash > rsp.hash.md5 )
true
