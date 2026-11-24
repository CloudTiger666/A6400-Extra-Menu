#!/bin/sh
# One-click build for A6400 Sound Lite Edition (Pure Shutter Sound + C2 Cream/Black UI).
# Uses the exact proven V4.3 SLEM bootstrap/runtime-start/charo_bind chain with A6400_SOUND_LITE=1.
set -eu

if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
    echo "usage: $0 CAMERA_LIBS_DIR CAMERA_MPR_MONIO_ELF CAMERA_MWF_MONIO_ELF [OUT_DIR]" >&2
    exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${4:-"$ROOT/build-sound-lite"}
A6400_SOUND_LITE=1 exec sh "$ROOT/tools/build_v3_clite.sh" "$1" "$2" "$3" "$OUT"
