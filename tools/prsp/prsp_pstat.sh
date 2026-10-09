#!/bin/sh
# on device. prsp_pstat.sh ROMFILE MODE RSPSO TAG -> logs/pstat_<TAG>.txt : perf stat of the emulation (main) thread, 10 s window
D=/storage/prsp-dev; . $D/prsp_common.sh
ROM="/storage/roms/n64/$1"; rsp_args $2 $3; TAG=$4
export FOIR_THREADS=3 RSPT_LOG=$D/logs/PS_$TAG.rspt
timeout -s KILL 40 /usr/local/bin/mupen64plus --configdir $D/cfg --datadir /usr/local/share/mupen64plus --plugindir $D/plugins --gfx mupen64plus-rdp-foir.so $RSPARGS --audio dummy --input mupen64plus-input-sdl.so --noosd --emumode 2 "$ROM" > $D/logs/pstat_run.log 2>&1 &
sleep 18
PID=$(pgrep -x mupen64plus | head -1)
perf stat -t $PID -e cycles,instructions,l1i_cache_refill,l1i_cache,l1d_cache_refill,l2d_cache_refill,l1i_tlb_refill,br_mis_pred,br_pred,stall_frontend,stall_backend -- sleep 10 > $D/logs/pstat_$TAG.txt 2>&1
pkill -KILL -x mupen64plus
true
