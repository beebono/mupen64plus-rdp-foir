# sourced on the device; private dev dir (never touches /storage/foir-dev)
. /etc/profile >/dev/null 2>&1
export XDG_RUNTIME_DIR=/var/run/0-runtime-dir WAYLAND_DISPLAY=wayland-1 SDL_VIDEODRIVER=wayland SDL_AUDIODRIVER=dummy HOME=/storage
D=/storage/prsp-dev
mkdir -p $D/logs $D/shots $D/hash
# rsp_args MODE RSPSO  -> sets RSPARGS, RSPT_TARGET (MODE: hle | parallel). RSPSO is a file name under $D/plugins.
rsp_args() {
  MODE=$1; RSPSO=$2
  case "$MODE" in
    hle)      export RSPT_TARGET=$D/plugins/mupen64plus-rsp-hle.so
              RSPARGS="--rsp $D/plugins/mupen64plus-rsp-timing.so --set rsp-hle[DisplayListToGraphicsPlugin]=False --set rsp-hle[AudioListToAudioPlugin]=False --set rsp-hle[RspFallback]=$D/plugins/$RSPSO" ;;
    parallel) export RSPT_TARGET=$D/plugins/$RSPSO
              RSPARGS="--rsp $D/plugins/mupen64plus-rsp-timing.so" ;;
  esac
}
foreign_check() {
  if pgrep -x mupen64plus >/dev/null 2>&1; then echo "FOREIGN mupen64plus running"; pgrep -a mupen64plus; return 1; fi
  return 0
}
