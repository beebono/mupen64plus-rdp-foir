#!/bin/sh
# on device. usage: run_t.sh ROMFILE MODE TMO TAG   -> logs/T_<TAG>.{foirt,rspt}
. /etc/profile >/dev/null 2>&1
export XDG_RUNTIME_DIR=/var/run/0-runtime-dir WAYLAND_DISPLAY=wayland-1 SDL_VIDEODRIVER=wayland SDL_AUDIODRIVER=dummy HOME=/storage
D=/storage/foir-dev
ROM="/storage/roms/n64/$1"; MODE=$2; TMO=$3; TAG=$4
export FOIR_LOGFILE=$D/logs/T_$TAG.foir FOIR_THREADS=3
rm -f $FOIR_LOGFILE $D/logs/T_$TAG.foirt $D/logs/T_$TAG.rspt
export RSPT_LOG=$D/logs/T_$TAG.rspt
case "$MODE" in
  hle)      export RSPT_TARGET=$D/plugins/mupen64plus-rsp-hle.so; RSPARGS="--rsp $D/plugins/mupen64plus-rsp-timing.so --set rsp-hle[DisplayListToGraphicsPlugin]=False --set rsp-hle[AudioListToAudioPlugin]=False --set rsp-hle[RspFallback]=$D/plugins/mupen64plus-rsp-parallel.so" ;;
  parallel) export RSPT_TARGET=$D/plugins/mupen64plus-rsp-parallel.so; RSPARGS="--rsp $D/plugins/mupen64plus-rsp-timing.so" ;;
  *)        export RSPT_TARGET=$D/plugins/mupen64plus-rsp-cxd4.so; RSPARGS="--rsp $D/plugins/mupen64plus-rsp-timing.so --set rsp-cxd4[DisplayListToGraphicsPlugin]=False --set rsp-cxd4[AudioListToAudioPlugin]=False" ;;
esac
( tail -F $FOIR_LOGFILE 2>/dev/null | while read l; do echo "$(cut -d' ' -f1 /proc/uptime) $l"; done > $D/logs/T_$TAG.foirt ) &
TP=$!
timeout -s KILL $TMO /usr/local/bin/mupen64plus --configdir $D/cfg --datadir /usr/local/share/mupen64plus \
  --plugindir $D/plugins --gfx mupen64plus-rdp-foir.so $RSPARGS \
  --audio dummy --input mupen64plus-input-sdl.so --noosd --emumode 2 "$ROM" > $D/logs/T_$TAG.log 2>&1
pkill -x mupen64plus; pkill -x tail; kill $TP 2>/dev/null
true
