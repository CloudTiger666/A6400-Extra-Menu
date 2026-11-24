#!/bin/sh
# One-click build for A6400 V3.9 SSLF (Sony Super LUT FILM, 10-film edition) from source.
# Requires: arm-linux-gnueabihf-gcc/g++, python3, and user-pulled camera inputs.
set -eu

if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
    echo "usage: $0 CAMERA_LIBS_DIR CAMERA_MPR_MONIO_ELF CAMERA_MWF_MONIO_ELF [OUT_DIR]" >&2
    exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LIBS=$(CDPATH= cd -- "$1" && pwd)
MPR_MONIO=$(CDPATH= cd -- "$(dirname -- "$2")" && pwd)/$(basename -- "$2")
MWF_MONIO=$(CDPATH= cd -- "$(dirname -- "$3")" && pwd)/$(basename -- "$3")
OUT=${4:-"$ROOT/build-v3-clite"}
mkdir -p "$OUT"
OUT=$(CDPATH= cd -- "$OUT" && pwd)

[ -f "$LIBS/libc-2.11.2.so" ] && [ -f "$LIBS/libdl-2.11.2.so" ] || {
    echo "missing libc-2.11.2.so or libdl-2.11.2.so in $LIBS" >&2
    exit 2
}
[ -f "$MPR_MONIO" ] || { echo "missing mpr_monio.elf: $MPR_MONIO" >&2; exit 2; }
[ -f "$MWF_MONIO" ] || { echo "missing mwf_monio.elf: $MWF_MONIO" >&2; exit 2; }

STAGE_MPR="$ROOT/host-tests/camera-mpr_monio.elf"
CREATED_MPR=0
cleanup() {
    if [ "$CREATED_MPR" = 1 ]; then
        rm -f "$STAGE_MPR"
    fi
}
trap cleanup EXIT INT TERM
if [ ! -e "$STAGE_MPR" ]; then
    cp "$MPR_MONIO" "$STAGE_MPR"
    CREATED_MPR=1
fi

echo "[1/9] Building 10-film JPEG worker (a6400-film-job.elf)..."
make -C "$ROOT/camera" -f Makefile.film clean >/dev/null 2>&1 || true
make -C "$ROOT/camera" -f Makefile.film JOB=1
cp "$ROOT/camera/film_job.elf" "$OUT/a6400-film-job.elf"
chmod 755 "$OUT/a6400-film-job.elf"
make -C "$ROOT/camera" -f Makefile.film JOB=1 clean >/dev/null 2>&1 || true

CHARO_ENABLE=0
CHARO_MULTI=0
CHARO_DIR=""
if [ -f "$ROOT/assets/a6400-charo-shutter.pcm" ]; then
    CHARO_ENABLE=1
    CHARO_DIR="$OUT/charo-assets"
    if [ -f "$ROOT/assets/a6400-charo-connect.pcm" ] && [ -f "$ROOT/assets/a6400-charo-push.pcm" ]; then
        CHARO_MULTI=1
    fi
fi

echo "[2/9] Building persistent V4.3 SLEM (hany) UI (charo=$CHARO_ENABLE multi=$CHARO_MULTI sc=${A6400_UI_LANG_SC:-0} mtf2=${A6400_MTF2_ENABLE:-1})..."
rm -rf "$OUT/ui"
A6400_TRIAL_MS=0 \
A6400_DISPLAY_MS=0 \
A6400_CHARO_SHUTTER="$CHARO_ENABLE" \
A6400_CHARO_MULTI="$CHARO_MULTI" \
A6400_UI_LANG_SC="${A6400_UI_LANG_SC:-0}" \
A6400_MTF2_ENABLE="${A6400_MTF2_ENABLE:-1}" \
UI_MODULE_PATH=/tmp_bt/a6400-film-ui-v3-persist.so \
    sh "$ROOT/host-tests/build_ui_trial3.sh" "$OUT/ui" "$LIBS"

rm -rf "$OUT/charo-assets"
if [ "$CHARO_ENABLE" = 1 ]; then
    mkdir -p "$OUT/charo-assets"
    cp "$ROOT/assets/a6400-charo-shutter.pcm" "$OUT/charo-assets/a6400-charo-shutter.pcm"
    chmod 644 "$OUT/charo-assets/a6400-charo-shutter.pcm"
    if [ "$CHARO_MULTI" = 1 ]; then
        cp "$ROOT/assets/a6400-charo-connect.pcm" "$OUT/charo-assets/a6400-charo-connect.pcm"
        cp "$ROOT/assets/a6400-charo-push.pcm" "$OUT/charo-assets/a6400-charo-push.pcm"
        chmod 644 "$OUT/charo-assets/a6400-charo-connect.pcm" "$OUT/charo-assets/a6400-charo-push.pcm"
    fi
    arm-linux-gnueabihf-gcc -Os -mcpu=cortex-a5 -mfpu=vfpv3-d16 -mfloat-abi=hard \
        -ffreestanding -fno-builtin -fno-stack-protector -ffunction-sections -fdata-sections \
        -fno-pie -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wall -Wextra -Werror -std=c11 \
        -nostdlib -static -no-pie -Wl,--gc-sections -Wl,-s -Wl,-e,_start \
        "$ROOT/camera/charo_bind.c" "$ROOT/camera/rt.c" -lgcc \
        -o "$OUT/charo-assets/a6400-charo-bind.elf"
    chmod 755 "$OUT/charo-assets/a6400-charo-bind.elf"
fi

echo "[3/9] Building persistent menu job CLI..."
rm -rf "$OUT/menu"
FILM_SETTINGS_PERSISTENT=1 \
UI_MODULE_PATH=/tmp_bt/a6400-film-ui-v3-persist.so \
    sh "$ROOT/host-tests/build_film_menu_job_cli.sh" "$OUT/menu" "$LIBS" "$OUT/ui"

echo "[4/9] Building UI stop CLI and V4.0 OSD preview helper..."
rm -rf "$OUT/ui-stop" "$OUT/preview"
UI_MODULE_PATH=/tmp_bt/a6400-film-ui-v3-persist.so \
    sh "$ROOT/host-tests/build_film_ui_stop_cli.sh" "$OUT/ui-stop" "$LIBS" "$OUT/ui"
sh "$ROOT/host-tests/build_film_preview_osd.sh" "$OUT/preview" "$LIBS" "$OUT/ui"

echo "[5/9] Building native command bridge..."
rm -rf "$OUT/native"
sh "$ROOT/host-tests/build_film_native_command.sh" "$OUT/native" "$LIBS" "$MWF_MONIO"

echo "[6/9] Building UI load guard and .req payloads..."
rm -rf "$OUT/guard"
mkdir -p "$OUT/guard"
arm-linux-gnueabihf-gcc -march=armv7-a+fp -mfpu=vfpv3-d16 -mfloat-abi=hard \
    -Os -Wall -Wextra -Werror -fno-stack-protector -fno-unwind-tables \
    -fno-asynchronous-unwind-tables -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
    -marm -fPIE -c "$ROOT/camera/native_ui_load_guard.c" -o "$OUT/guard/guard.o"
arm-linux-gnueabihf-gcc -march=armv7-a+fp -mfpu=vfpv3-d16 -mfloat-abi=hard \
    -Os -Wall -Wextra -Werror -fno-stack-protector -fno-unwind-tables \
    -fno-asynchronous-unwind-tables -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
    -marm -c "$ROOT/camera/native_start.S" -o "$OUT/guard/start.o"
arm-linux-gnueabihf-gcc -mfloat-abi=hard -nostdlib -pie \
    -Wl,-s,-e,_start,--dynamic-linker=/lib/ld-linux.so.3 \
    "$OUT/guard/start.o" "$OUT/guard/guard.o" -Wl,--no-as-needed "$LIBS/libc-2.11.2.so" -lgcc \
    -o "$OUT/guard/a6400-ui-load-guard.elf"
chmod 755 "$OUT/guard/a6400-ui-load-guard.elf"
python3 - "$OUT/ui/payload.bin" "$OUT/guard" <<'PY'
import struct, sys
from pathlib import Path

payload = Path(sys.argv[1]).read_bytes()
while len(payload) % 4:
    payload += b'\x00'
fnv = 2166136261
for b in payload:
    fnv = ((fnv ^ b) * 16777619) & 0xffffffff
out = Path(sys.argv[2])
for op, name in ((0, 'check-persist.req'), (1, 'install-persist.req'), (2, 'remove-persist.req')):
    hdr = struct.pack('<8I', 0x4d544852, 1, op, len(payload), 1, fnv, 1184, 0)
    (out / name).write_bytes(hdr + payload)
PY

echo "[7/9] Pinning a6400-film-ram-job.sh hashes..."
python3 - "$ROOT/camera/film_ram_job.sh" "$OUT" <<'PY'
import hashlib, sys
from pathlib import Path

tpl = Path(sys.argv[1]).read_text()
out = Path(sys.argv[2])
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
lines = [ln if i == 0 else (ln.lstrip() if ln.strip() and not ln.lstrip().startswith('#') else '') for i, ln in enumerate(tpl.splitlines(True))]
text = (''.join(lines)
    .replace('__NATIVE_SHA__', sha(out / 'native/a6400-film-native-command.elf'))
    .replace('__WORKER_SHA__', sha(out / 'a6400-film-job.elf'))
    .replace('__SETTINGS_SHA__', sha(out / 'menu/a6400-film-menu-job.elf')))
assert '__NATIVE_SHA__' not in text and '__WORKER_SHA__' not in text and '__SETTINGS_SHA__' not in text
dst = out / 'a6400-film-ram-job.sh'
dst.write_text(text)
dst.chmod(0o755)
PY

echo "[8/9] Building persistent capture daemon..."
rm -rf "$OUT/daemon"
FILM_CAPTURE_PERSISTENT=1 \
A6400_UI_LANG_SC="${A6400_UI_LANG_SC:-0}" \
FILM_UI_DIR="$OUT/ui" \
    sh "$ROOT/host-tests/build_film_capture_daemon.sh" \
    "$OUT/daemon" "$LIBS" "$OUT/a6400-film-ram-job.sh" "$OUT/menu/a6400-film-menu-job.elf"

echo "[9/9] Assembling local boot package..."
python3 - "$OUT" <<'PY'
import hashlib, json, sys
from pathlib import Path

out = Path(sys.argv[1])
items = [
    ('ui', out / 'ui/a6400-film-ui-v3.so', 'a6400-film-ui-v3-persist.so'),
    ('menu_job', out / 'menu/a6400-film-menu-job.elf', 'a6400-film-menu-job.elf'),
    ('worker', out / 'a6400-film-job.elf', 'a6400-film-job.elf'),
    ('native', out / 'native/a6400-film-native-command.elf', 'a6400-film-native-command.elf'),
    ('ram_job', out / 'a6400-film-ram-job.sh', 'a6400-film-ram-job.sh'),
    ('guard', out / 'guard/a6400-ui-load-guard.elf', 'a6400-ui-load-guard.elf'),
    ('check', out / 'guard/check-persist.req', 'a6400-check-persist.req'),
    ('install', out / 'guard/install-persist.req', 'a6400-install-persist.req'),
    ('remove', out / 'guard/remove-persist.req', 'a6400-remove-persist.req'),
    ('ui_stop', out / 'ui-stop/a6400-film-ui-stop.elf', 'a6400-film-ui-stop.elf'),
    ('preview', out / 'preview/a6400-film-preview.elf', 'a6400-film-preview.elf'),
    ('capture_daemon', out / 'daemon/a6400-film-capture-daemon.elf', 'a6400-film-capture-daemon.elf'),
]
spec = {
    'schema': 1,
    'default_enabled': False,
    'capture_persistent': True,
    'menu_pins_file': str(out / 'menu/pinned-ui.json'),
    'capture_pins_file': str(out / 'daemon/pinned-inputs.json'),
    'artifacts': [
        {'role': role, 'file': str(path), 'dest': dest,
         'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
        for role, path, dest in items
    ],
}
(out / 'runtime-artifacts.json').write_text(json.dumps(spec, indent=2) + '\n')
PY
rm -rf "$OUT/local-boot-package"
A6400_CHARO_DIR="$CHARO_DIR" \
python3 "$ROOT/host-tests/build_film_local_boot_package.py" \
    "$OUT/runtime-artifacts.json" "$OUT/local-boot-package"

echo "V3_CLITE_BUILD_DONE package_dir=$OUT/local-boot-package"
