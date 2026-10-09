#!/bin/sh
# on device. usage: prsp_perf.sh ROMFILE MODE RSPSO TAG  -> logs/perf_<TAG>.txt (perf report by dso,sym) + .data
D=/storage/prsp-dev; . $D/prsp_common.sh
ROM="/storage/roms/n64/$1"; rsp_args $2 $3; TAG=$4
export FOIR_THREADS=3 RSPT_LOG=$D/logs/P_$TAG.rspt
timeout -s KILL 45 /usr/local/bin/mupen64plus --configdir $D/cfg --datadir /usr/local/share/mupen64plus --plugindir $D/plugins --gfx mupen64plus-rdp-foir.so $RSPARGS --audio dummy --input mupen64plus-input-sdl.so --noosd --emumode 2 "$ROM" > $D/logs/perf_run.log 2>&1 &
sleep 20
PID=$(pgrep -x mupen64plus | head -1)
cp /proc/$PID/maps $D/logs/perf_$TAG.maps
# emulation thread == main thread (tid == pid)
perf record -F 1999 -t $PID -o $D/logs/perf_$TAG.data -- sleep 12 >/dev/null 2>&1
perf report -i $D/logs/perf_$TAG.data --sort dso,sym --stdio 2>/dev/null | head -80 > $D/logs/perf_$TAG.txt
pkill -KILL -x mupen64plus
true
