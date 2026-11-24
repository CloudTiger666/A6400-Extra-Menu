#!/bin/sh
# One-click build for A6400 Sound Lite Edition (Pure Shutter Sound + C2 Cream/Black UI).
# Zero LUTs, zero SD scan, zero album, zero JPEG worker — instant boot & <1MB RAM.
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 CAMERA_LIBS_DIR [OUT_DIR]" >&2
    exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LIBS=$(CDPATH= cd -- "$1" && pwd)
OUT=${2:-"$ROOT/build-sound-lite"}
mkdir -p "$OUT"
OUT=$(CDPATH= cd -- "$OUT" && pwd)

[ -f "$LIBS/libc-2.11.2.so" ] && [ -f "$LIBS/libdl-2.11.2.so" ] || {
    echo "missing libc-2.11.2.so or libdl-2.11.2.so in $LIBS" >&2
    exit 2
}

CHARO_ENABLE=0
CHARO_MULTI=0
if [ -f "$ROOT/assets/a6400-charo-shutter.pcm" ]; then
    CHARO_ENABLE=1
    if [ -f "$ROOT/assets/a6400-charo-connect.pcm" ] && [ -f "$ROOT/assets/a6400-charo-push.pcm" ]; then
        CHARO_MULTI=1
    fi
fi

echo "[1/4] Building Sound Lite UI (C2 Cream/Black, charo=$CHARO_ENABLE multi=$CHARO_MULTI sc=${A6400_UI_LANG_SC:-0} mtf2=${A6400_MTF2_ENABLE:-1})..."
rm -rf "$OUT/ui"
A6400_TRIAL_MS=0 \
A6400_DISPLAY_MS=0 \
A6400_SOUND_LITE=1 \
A6400_CHARO_SHUTTER="$CHARO_ENABLE" \
A6400_CHARO_MULTI="$CHARO_MULTI" \
A6400_UI_LANG_SC="${A6400_UI_LANG_SC:-0}" \
A6400_MTF2_ENABLE="${A6400_MTF2_ENABLE:-1}" \
UI_MODULE_PATH=/tmp_bt/a6400-film-ui-v3-persist.so \
    sh "$ROOT/host-tests/build_ui_trial3.sh" "$OUT/ui" "$LIBS"

echo "[2/4] Building charo_bind.elf..."
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

echo "[3/4] Building UI load guard and .req payloads..."
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
out = Path(sys.argv[1 + 1])
for op, name in ((0, 'check-persist.req'), (1, 'install-persist.req'), (2, 'remove-persist.req')):
    hdr = struct.pack('<8I', 0x4d544852, 1, op, len(payload), 1, fnv, 1184, 0)
    (out / name).write_bytes(hdr + payload)
PY

echo "[4/4] Assembling Sound Lite local-boot-package..."
python3 - "$OUT" <<'PY'
import hashlib, io, json, os, tarfile, sys
from pathlib import Path

out = Path(sys.argv[1])
pkg = out / 'local-boot-package'
pdir = pkg / 'persistent'
pdir.mkdir(parents=True, exist_ok=True)

entries = [
    (out / 'ui/a6400-film-ui-v3.so', 'a6400-film-ui-v3-persist.so', 0o755),
    (out / 'guard/a6400-ui-load-guard.elf', 'a6400-ui-load-guard.elf', 0o755),
    (out / 'guard/check-persist.req', 'a6400-check-persist.req', 0o644),
    (out / 'guard/install-persist.req', 'a6400-install-persist.req', 0o644),
    (out / 'guard/remove-persist.req', 'a6400-remove-persist.req', 0o644),
]

charo_dir = out / 'charo-assets'
charo_files = []
if charo_dir.is_dir():
    for fname, mode in (
        ('a6400-charo-bind.elf', 0o755),
        ('a6400-charo-shutter.pcm', 0o644),
        ('a6400-charo-connect.pcm', 0o644),
        ('a6400-charo-push.pcm', 0o644),
    ):
        fp = charo_dir / fname
        if fp.is_file():
            entries.append((fp, fname, mode))
            charo_files.append((fname, hashlib.sha256(fp.read_bytes()).hexdigest()))

tar_path = pdir / 'a6400-film-boot-v15.tar'
with tarfile.open(tar_path, 'w:gz') as tf:
    for src, name, mode in entries:
        data = src.read_bytes()
        ti = tarfile.TarInfo(name)
        ti.size = len(data)
        ti.mode = mode
        ti.uid = ti.gid = 0
        ti.uname = ti.gname = 'root'
        ti.mtime = 0
        tf.addfile(ti, io.BytesIO(data))

def sha_of(name):
    for src, n, _ in entries:
        if n == name:
            return hashlib.sha256(src.read_bytes()).hexdigest()
    return ''

charo_block = ''
if charo_files:
    checks = ' &&\n       '.join(f'hash_is "/tmp/{n}" "{h}"' for n, h in charo_files)
    copies = ' &&\n       '.join(f'"$BB" cp "/tmp/{n}" "$RAM/{n}"' for n, _ in charo_files)
    charo_block = f'''if test ! -e /tmp/disable_charo && test -f /tmp/a6400-charo-bind.elf; then
    if {checks} &&
       {copies} &&
       "$BB" chmod 755 "$RAM/a6400-charo-bind.elf" &&
       "$RAM/a6400-charo-bind.elf"; then
        :
    fi
    "$BB" rm -f "$RAM/a6400-charo-bind.elf"
fi
'''

autoexec_sh = f'''#!/bin/sh
BB=/bin/busybox
RAM=/tmp_bt
LOG=/tmp_bt/a6400-sound-lite-boot.log
"$BB" rm -f "$LOG" 2>/dev/null || true
hash_is() {{
    "$BB" test -f "$1" || return 1
    got=$(/usr/bin/openssl dgst -sha256 "$1" 2>/dev/null | "$BB" awk '{{print $NF}}')
    test "$got" = "$2"
}}
"$BB" mkdir -p "$RAM"
"$BB" mount -t tmpfs -o size=2048k tmpfs "$RAM" 2>/dev/null || true
if ! hash_is "/tmp/a6400-film-ui-v3-persist.so" "{sha_of('a6400-film-ui-v3-persist.so')}" ||
   ! hash_is "/tmp/a6400-ui-load-guard.elf" "{sha_of('a6400-ui-load-guard.elf')}" ||
   ! hash_is "/tmp/a6400-install-persist.req" "{sha_of('a6400-install-persist.req')}"; then
    "$BB" tar -xzf /tmp/a6400-film-boot-v15.tar -C /tmp 2>/dev/null || exit 0
fi
"$BB" cp "/tmp/a6400-film-ui-v3-persist.so" "$RAM/a6400-film-ui-v3-persist.so" || exit 0
"$BB" chmod 755 "$RAM/a6400-film-ui-v3-persist.so"
"$BB" cp "/tmp/a6400-ui-load-guard.elf" "$RAM/a6400-ui-load-guard.elf" || exit 0
"$BB" chmod 755 "$RAM/a6400-ui-load-guard.elf"
"$BB" cp "/tmp/a6400-install-persist.req" "$RAM/a6400-install-persist.req" || exit 0
{charo_block}
"$RAM/a6400-ui-load-guard.elf" "$RAM/a6400-install-persist.req" >>"$LOG" 2>&1 &
'''
(pkg / 'bootstrap-local.sh').write_text(autoexec_sh)
(pkg / 'bootstrap-local.sh').chmod(0o755)

manifest = {
    'package_id': hashlib.sha256(tar_path.read_bytes()).hexdigest()[:16],
    'edition': 'A6400-Sound-Lite-V4.3',
    'ui_theme': 'C2-Cream-Black',
    'charo': {n: h for n, h in charo_files},
    'extra_persistent_files': [],
}
(pkg / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print('SOUND_LITE_PACKAGE_OK', manifest['package_id'], 'tar_bytes=', tar_path.stat().st_size)
PY
