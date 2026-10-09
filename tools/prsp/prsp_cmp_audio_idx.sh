#!/bin/sh
# on device. prsp_cmp_audio_idx.sh N REFSO NEWSO SECONDS [reuse]
D=/storage/prsp-dev
ROMF=$(ls /storage/roms/n64/*.z64 | sed -n "${1}p" | sed 's|.*/||')
TAG=$(echo "$ROMF" | sed 's|\.z64$||; s|[^A-Za-z0-9]|_|g')
shift
$D/prsp_cmp_audio.sh "$ROMF" "$1" "$2" "$TAG" "$3" "$4"
