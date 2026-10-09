#!/bin/sh
# on device. prsp_cmp_idx.sh N MODE REFSO NEWSO TMO [reuse]: prsp_cmp.sh for the Nth ROM (sorted) of /storage/roms/n64/*.z64
D=/storage/prsp-dev
ROMF=$(ls /storage/roms/n64/*.z64 | sed -n "${1}p" | sed 's|.*/||')
TAG=$(echo "$ROMF" | sed 's|\.z64$||; s|[^A-Za-z0-9]|_|g')
shift
$D/prsp_cmp.sh "$ROMF" "$1" "$2" "$3" "$TAG" "$4" "$5"
