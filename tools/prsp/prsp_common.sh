# sourced on the device; private dev dir (never touches /storage/foir-dev)
. /etc/profile >/dev/null 2>&1
export XDG_RUNTIME_DIR=/var/run/0-runtime-dir WAYLAND_DISPLAY=wayland-1 SDL_VIDEODRIVER=wayland SDL_AUDIODRIVER=dummy
D=/storage/prsp-dev
mkdir -p $D/logs $D/shots $D/hash $D/home/cache
# HOME is /storage on ROCKNIX, where the CFW's own mupen64plus keeps its saves: keep ours private
export HOME=$D/home XDG_DATA_HOME=$D/home XDG_CONFIG_HOME=$D/home XDG_CACHE_HOME=$D/home/cache
# rsp_args MODE RSPSO  -> sets RSPARGS, RSPT_TARGET (MODE: hle | parallel | armlet). RSPSO is a file name under $D/plugins.
#   hle      rsp-hle with RspFallback = RSPSO (parallel-rsp)
#   parallel RSPSO as the RSP plugin (parallel-rsp alone)
#   armlet   RSPSO = mupen64plus-rsp-armlet.so as the RSP plugin
rsp_args() {
  MODE=$1; RSPSO=$2
  case "$MODE" in
    hle)      export RSPT_TARGET=$D/plugins/mupen64plus-rsp-hle.so
              RSPARGS="--rsp $D/plugins/mupen64plus-rsp-timing.so --set rsp-hle[DisplayListToGraphicsPlugin]=False --set rsp-hle[AudioListToAudioPlugin]=False --set rsp-hle[RspFallback]=$D/plugins/$RSPSO" ;;
    parallel|armlet) export RSPT_TARGET=$D/plugins/$RSPSO
              RSPARGS="--rsp $D/plugins/mupen64plus-rsp-timing.so" ;;
  esac
}
foreign_check() {
  if pgrep -x mupen64plus >/dev/null 2>&1; then echo "FOREIGN mupen64plus running"; pgrep -a mupen64plus; return 1; fi
  return 0
}
