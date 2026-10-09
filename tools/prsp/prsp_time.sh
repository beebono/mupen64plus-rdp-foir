#!/bin/sh
# on device. usage: prsp_time.sh ROMFILE MODE RSPSO WINDOW_END TAG [TMO]
#  Runs until the foir log shows "frame WINDOW_END:" (then kills), writes logs/T_<TAG>.{foirt,rspt,log}
D=/storage/prsp-dev; . $D/prsp_common.sh
ROM="/storage/roms/n64/$1"; rsp_args $2 $3; WEND=$4; TAG=$5; TMO=${6:-70}
export FOIR_LOGFILE=$D/logs/T_$TAG.foir FOIR_THREADS=3 RSPT_LOG=$D/logs/T_$TAG.rspt
therm() { echo "$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null) $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null) $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"; }
echo "start $(therm)" > $D/logs/T_$TAG.therm
rm -f $FOIR_LOGFILE $D/logs/T_$TAG.foirt $RSPT_LOG
( tail -F $FOIR_LOGFILE 2>/dev/null | while read l; do echo "$(cut -d' ' -f1 /proc/uptime) $l"; done > $D/logs/T_$TAG.foirt ) &
TP=$!
timeout -s KILL $TMO /usr/local/bin/mupen64plus --configdir $D/cfg --datadir /usr/local/share/mupen64plus \
  --plugindir $D/plugins --gfx mupen64plus-rdp-foir.so $RSPARGS \
  --audio dummy --input mupen64plus-input-sdl.so --noosd --emumode 2 "$ROM" > $D/logs/T_$TAG.log 2>&1 &
MP=$!
i=0
while [ $i -lt $((TMO*2)) ]; do
  sleep 0.5; i=$((i+1))
  grep -q "frame $WEND:" $FOIR_LOGFILE 2>/dev/null && { sleep 1; break; }
  kill -0 $MP 2>/dev/null || break
done
echo "end $(therm)" >> $D/logs/T_$TAG.therm
pkill -KILL -x mupen64plus; sleep 0.3; pkill -x tail; kill $TP 2>/dev/null
cp $FOIR_LOGFILE $D/logs/T_$TAG.foir.keep 2>/dev/null
true
