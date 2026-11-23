#!/bin/sh
# One bounded native-read -> author-film RAM job; apply saves a separate JPEG.
# Successful render retains its private mount for verified host backup/release.
# An uncertain native operation must retain all scratch even after client exit.
set -u
set -f
umask 077
RAM=/tmp_bt/a6400-film-job
LOCK=/tmp_bt/a6400-film-job.lock
BB=/bin/busybox
NATIVE=/tmp_bt/a6400-film-native-command.elf
WORKER=/tmp_bt/a6400-film-job.elf
SETTINGS=/tmp_bt/a6400-film-menu-job.elf
MEMINFO=/proc/meminfo
MOUNTS=/proc/mounts
BOOTFILE=/proc/sys/kernel/random/boot_id
CAPTURE_STOP=/tmp_bt/a6400-film-capture.stop
CLOCKFILE=/proc/uptime
MEDIA_WAIT_POLLS=60
NATIVE_SHA=__NATIVE_SHA__
WORKER_SHA=__WORKER_SHA__
SETTINGS_SHA=__SETTINGS_SHA__
INPUT=$RAM/input.jpg
INPUT1=$RAM/input1.jpg
OUTPUT=$RAM/output.jpg
PREVIEW=$RAM/preview.bin
PV_DIR=/tmp_bt/a6400-pv
MODE=${1:-}
SOURCE=${2:-}
EXPECTED_BYTES=${3:-}
SOURCE1=
EXPECTED_BYTES1=0
DEXP_MODE=
TARGET=
PRESET=
GRAIN=
HALATION=
INITIAL_PLAN=
BOOT=
PHASE=arguments
INPUT_SHA=
OUTPUT_SHA=
OUTPUT_BYTES=0
TARGET_ATTEMPTED=0
TARGET_VERIFIED=0
ORIGINAL_VERIFIED=0
READBACK_BYTES=0
READBACK_SHA=
NATIVE_UNCERTAIN=0
NATIVE_LAST_STATUS=none
applied=0
retain=0
ram_created=0
native_inflight=0
worker_inflight=0
native_pid=0
worker_pid=0
media_wait_polls=0
settings_pinned=0
JOB_STARTED=-1
CLOCK_MS=-1

# Built-in reads avoid spawning date/awk for every measurement. A missing
# clock only disables telemetry; it must not change native I/O semantics.
clock_ms() {
    CLOCK_MS=-1
    IFS=' ' read -r clock_value clock_rest < "$CLOCKFILE" || return 1
    clock_seconds=${clock_value%%.*}
    clock_fraction=${clock_value#*.}000
    clock_fraction=${clock_fraction%"${clock_fraction#???}"}
    case "$clock_seconds:$clock_fraction" in *[!0-9:]*|:*) return 1;; esac
    test "${#clock_seconds}" -le 8 && test "${#clock_fraction}" = 3 || return 1
    CLOCK_MS=$((clock_seconds*1000+1$clock_fraction-1000))
}
stage_done() {
    clock_ms || return 0
    test "$2" -ge 0 && test "$CLOCK_MS" -ge "$2" || return 0
    echo "RAM_JOB_STAGE stage=$1 start_ms=$2 end_ms=$CLOCK_MS elapsed_ms=$((CLOCK_MS-$2)) completed=1"
}

fail() { echo "RAM_JOB_FAIL stage=$1"; exit "${2:-1}"; }
hash_file() {
    test -f "$1" && test ! -L "$1" || return 1
    if hash_line=$("$BB" sha256sum "$1" 2>/dev/null); then
        HASH=${hash_line%% *}
    else
        hash_line=$(/usr/bin/openssl dgst -sha256 "$1" 2>/dev/null) || return 1
        HASH=${hash_line##* }
    fi
    test "${#HASH}" = 64 || return 1
    case "$HASH" in *[!0-9a-f]*) return 1;; esac
}
hash_is() { hash_file "$1" && test "$HASH" = "$2"; }
file_bytes() {
    test -f "$1" && test ! -L "$1" || return 1
    size_line=$("$BB" ls -ln "$1") || return 1
    set -- $size_line
    test "$#" -ge 5 || return 1
    case "$5" in ''|*[!0-9]*) return 1;; esac
    test "${#5}" -le 8 || return 1
    FILE_BYTES=$5
}
no_linux_sd() {
    test -r "$MOUNTS" || return 1
    while read -r mount_source mount_target mount_type mount_options mount_rest; do
        case "$mount_source" in /dev/mmca*|/dev/mmcblk*) return 1;; esac
    done < "$MOUNTS"
}
# 0=one owned RAM mount;1=no mount at RAM;2=unreadable/other/ambiguous.
ram_mount_state() {
    test -r "$MOUNTS" || return 2
    mount_count=0
    mount_owned=0
    while read -r mount_source mount_target mount_type mount_options mount_rest; do
        if test "$mount_target" = "$RAM"; then
            mount_count=$((mount_count+1))
            if test "$mount_source" = a6400-film-job && test "$mount_type" = tmpfs; then
                mount_owned=$((mount_owned+1))
            fi
        fi
    done < "$MOUNTS"
    test "$mount_count" = 0 && return 1
    test "$mount_count" = 1 && test "$mount_owned" = 1 && return 0
    return 2
}
manifest() {
    {
        echo "boot_id=$BOOT"
        echo "pid=$$"
        echo "mode=$MODE"
        echo "phase=$PHASE"
        echo "source=$SOURCE"
        echo "expected_bytes=$EXPECTED_BYTES"
        echo "target=$TARGET"
        echo "preset=$PRESET"
        echo "grain=$GRAIN"
        echo "halation=$HALATION"
        echo "input_sha256=$INPUT_SHA"
        echo "output_sha256=$OUTPUT_SHA"
        echo "output_bytes=$OUTPUT_BYTES"
        echo "native_pid=$native_pid"
        echo "worker_pid=$worker_pid"
        echo "native_inflight=$native_inflight"
        echo "worker_inflight=$worker_inflight"
        echo "ram=$RAM"
        echo "input=$INPUT"
        echo "output=$OUTPUT"
        echo "native_sha256=$NATIVE_SHA"
        echo "worker_sha256=$WORKER_SHA"
        echo "settings_sha256=$SETTINGS_SHA"
        echo "target_attempted=$TARGET_ATTEMPTED"
        echo "target_verified=$TARGET_VERIFIED"
        echo "original_verified=$ORIGINAL_VERIFIED"
        echo "readback_bytes=$READBACK_BYTES"
        echo "readback_sha256=$READBACK_SHA"
        echo "native_uncertain=$NATIVE_UNCERTAIN"
        echo "native_last_status=$NATIVE_LAST_STATUS"
    } > "$LOCK/manifest"
}
cleanup() {
    test "$native_inflight" = 0 && test "$worker_inflight" = 0 || return 1
    if test "$ram_created" = 1; then
        ram_mount_state
        cleanup_mount=$?
        if test "$cleanup_mount" = 0; then
            # Only these fixed files belong to this job; never delete by glob.
            "$BB" rm -f "$INPUT" "$INPUT1" "$OUTPUT" "$PREVIEW" || return 1
            "$BB" umount "$RAM" || return 1
            ram_mount_state
            test "$?" = 1 || return 1
        elif test "$cleanup_mount" != 1; then
            return 1
        fi
        "$BB" rmdir "$RAM" || return 1
    fi
    "$BB" rm -f "$LOCK/native.log" "$LOCK/native.current.log" "$LOCK/verify.log" "$LOCK/render.log" "$LOCK/manifest" || return 1
    "$BB" rmdir "$LOCK"
}
finish() {
    job_rc=$?
    trap - 0
    if test "$native_inflight" != 0 || test "$worker_inflight" != 0; then
        if test "$native_inflight" != 0; then NATIVE_UNCERTAIN=1; fi
        retain=1
        PHASE=operation_unconfirmed
        job_rc=21
    fi
    if test "$retain" = 0; then
        if ! cleanup; then
            retain=1
            PHASE=cleanup_unconfirmed
            job_rc=21
        fi
    fi
    if test "$retain" = 1; then
        manifest || echo 'RAM_JOB_MANIFEST_WRITE_FAILED'
        echo "RAM_JOB_RETAINED phase=$PHASE ram=$RAM lock=$LOCK result=$job_rc"
    fi
    if clock_ms && test "$JOB_STARTED" -ge 0 && test "$CLOCK_MS" -ge "$JOB_STARTED"; then
        echo "RAM_JOB_TIMING total_ms=$((CLOCK_MS-JOB_STARTED)) result=$job_rc"
    fi
    if test "$job_rc" = 0 && test "$applied" = 1 && test "$retain" = 0; then
        echo "RAM_JOB_APPLIED source=$SOURCE target=$TARGET preset=$PRESET grain=$GRAIN halation=$HALATION source_bytes=$EXPECTED_BYTES source_sha256=$INPUT_SHA output_bytes=$OUTPUT_BYTES output_sha256=$OUTPUT_SHA source_access=read_only target_verified=$TARGET_VERIFIED original_verified=$ORIGINAL_VERIFIED"
    fi
    exit "$job_rc"
}
interrupted() { fail interrupted 21; }
guard() {
    IFS= read -r current_boot < "$BOOTFILE" || return 1
    test "$current_boot" = "$BOOT" && test -r "$MOUNTS" || return 1
    mount_count=0
    mount_owned=0
    while read -r mount_source mount_target mount_type mount_options mount_rest; do
        case "$mount_source" in /dev/mmca*|/dev/mmcblk*) return 1;; esac
        if test "$mount_target" = "$RAM"; then
            mount_count=$((mount_count+1))
            if test "$mount_source" = a6400-film-job && test "$mount_type" = tmpfs; then
                mount_owned=$((mount_owned+1))
            fi
        fi
    done < "$MOUNTS"
    test "$mount_count" = 1 && test "$mount_owned" = 1
}
decimal() {
    test -n "$1" && test "${#1}" -le 10 || return 1
    case "$1" in *[!0-9]*) return 1;; esac
}
# Accept the pinned command's legacy report and its extended history counters.
# A prompt is transport completion only; each copy is independently verified.
native_report_ok() {
    report_count=0
    report_bad=0
    while IFS= read -r report_line; do
        case "$report_line" in
            NATIVE_RETURN\ *|NATIVE_ABSENT\ *|NATIVE_FAIL\ *)
                report_count=$((report_count+1))
                set -- $report_line
                report_fields=$#
                test "$#" = 6 || test "$#" = 9 || { report_bad=1;continue; }
                test "$1" = "$report_kind" && test "$2" = "operation=$native_op" &&
                    test "$3" = "reason=$report_reason" && test "$5" = uncertain=0 &&
                    test "$6" = copy_verified=0 || report_bad=1
                report_number=${4#response_bytes=}
                report_response_bytes=$report_number
                test "$4" = "response_bytes=$report_number" && decimal "$report_number" || report_bad=1
                if test "$#" = 9; then
                    report_number=${7#startup_history_bytes=}
                    report_startup_bytes=$report_number
                    test "$7" = "startup_history_bytes=$report_number" && decimal "$report_number" || report_bad=1
                    report_number=${8#pre_echo_bytes=}
                    report_pre_echo_bytes=$report_number
                    test "$8" = "pre_echo_bytes=$report_number" && decimal "$report_number" || report_bad=1
                    report_number=${9#pre_echo_overflows=}
                    report_pre_echo_overflows=$report_number
                    test "$9" = "pre_echo_overflows=$report_number" && decimal "$report_number" || report_bad=1
                fi;;
        esac
    done < "$LOCK/native.current.log"
    test "$report_count" = 1 && test "$report_bad" = 0
}
native_safe_presend_failure() {
    # A child exit alone says nothing about a Sony operation it already sent.
    # Only the pinned helper's complete, exact pre-send failure receipt is safe.
    case "$native_rc" in
        3) set -- monitor_pin;;
        5) set -- startup startup_busy history_capacity;;
        6) set -- before_send write_command;;
        20) set -- signal;;
        *) return 1;;
    esac
    test ! -e "$INPUT" && test ! -L "$INPUT" || return 1
    test "$native_has_sent" = 0 || return 1
    report_kind=NATIVE_FAIL
    for report_reason do
        if native_report_ok && test "$report_fields" = 9 &&
           test "$report_response_bytes" = 0 && test "$report_pre_echo_bytes" = 0 &&
           test "$report_pre_echo_overflows" = 0; then
            if test "$native_rc" != 3 || test "$report_startup_bytes" = 0; then return 0; fi
        fi
    done
    return 1
}
native_run() {
    native_op=$1
    native_path=$2
    report_kind=NATIVE_RETURN
    report_reason=fresh_prompt
    if test "$native_op" = absent; then report_kind=NATIVE_ABSENT;report_reason=source_open_failed; fi
    PHASE=native_$native_op
    guard || fail pre_native_guard
    clock_ms || :
    native_started=$CLOCK_MS
    native_inflight=1
    "$NATIVE" "$native_op" "$native_path" > "$LOCK/native.current.log" 2>&1 &
    native_pid=$!
    manifest || fail manifest_native
    wait "$native_pid"
    native_rc=$?
    NATIVE_LAST_STATUS=$native_rc
    native_inflight=0
    native_has_uncertain=0
    native_has_sent=0
    exec 3>>"$LOCK/native.log" || { NATIVE_UNCERTAIN=1;retain=1;fail native_log 21; }
    while IFS= read -r native_line || test -n "$native_line"; do
        echo "$native_line"
        echo "$native_line" >&3 || { exec 3>&-; NATIVE_UNCERTAIN=1;retain=1;fail native_log 21; }
        case "$native_line" in *uncertain=1*) native_has_uncertain=1;; esac
        case "$native_line" in NATIVE_SENT\ *) native_has_sent=1;; esac
    done < "$LOCK/native.current.log"
    exec 3>&-
    if (test "$MODE" = apply || test "$MODE" = dexp) && test "$TARGET_ATTEMPTED" = 0 &&
       test "$native_op" = read && test "$native_rc" = 14 &&
       test ! -e "$INPUT" && test ! -L "$INPUT"; then
        report_kind=NATIVE_FAIL
        report_reason=source_unavailable
        if native_report_ok && test "$report_fields" = 9 && guard; then return 14; fi
    fi
    if test "$native_rc" != 0 || test "$native_has_uncertain" = 1; then
        if test "$native_rc" != 20 && (test "$MODE" = apply || test "$MODE" = dexp) && test "$TARGET_ATTEMPTED" = 0 && test "$native_op" = read; then
            return 15
        fi
        if test "$native_rc" = 21 || test "$native_has_uncertain" = 1; then
            NATIVE_UNCERTAIN=1;retain=1;fail native_uncertain 21
        fi
        if native_safe_presend_failure; then fail native_presend "$native_rc"; fi
        NATIVE_UNCERTAIN=1;retain=1;fail native_completion_unconfirmed 21
    fi
    if ! native_report_ok; then NATIVE_UNCERTAIN=1;retain=1;fail native_return_unconfirmed 21; fi
    stage_done "$native_op" "$native_started"
}
decode_input() {
    PHASE=$1
    decode_failure=$2
    guard || fail verify_worker_pin
    clock_ms || :
    decode_started=$CLOCK_MS
    worker_inflight=1
    "$WORKER" --verify "$INPUT" > "$LOCK/verify.log" 2>&1 &
    worker_pid=$!
    manifest || fail manifest_verify
    wait "$worker_pid"
    worker_rc=$?
    worker_inflight=0
    "$BB" cat "$LOCK/verify.log"
    test "$worker_rc" = 0 && "$BB" grep -q '^FILM_DECODED ' "$LOCK/verify.log" || fail "$decode_failure"
    stage_done "$PHASE" "$decode_started"
}
release_input() {
    guard && test -f "$INPUT" && test ! -L "$INPUT" || fail release_input_guard
    "$BB" rm -f "$INPUT" "$INPUT1" || fail release_input
    test ! -e "$INPUT" && test ! -L "$INPUT" && test ! -e "$INPUT1" && test ! -L "$INPUT1" || fail release_input
}
read_settings() {
    if test "$settings_pinned" = 0; then
        hash_is "$SETTINGS" "$SETTINGS_SHA" || return 11
        settings_pinned=1
    fi
    SETTING_LINE=$("$SETTINGS")
    setting_rc=$?
    if test "$setting_rc" = 10 && test "$SETTING_LINE" = FILM_OFF; then return 10; fi
    test "$setting_rc" = 0 || return 11
    set -- $SETTING_LINE
    test "$#" = 4 && test "$1" = FILM_JOB || return 11
    setting_preset=${2#preset=}
    setting_grain=${3#grain=}
    setting_halation=${4#halation=}
    test "$SETTING_LINE" = "FILM_JOB preset=$setting_preset grain=$setting_grain halation=$setting_halation" || return 11
    case "$setting_preset" in portra_400|cinestill_800t|fuji_provia|kodak_gold|tri_x_400|classic_chrome|agfa_ultra_100|polaroid_669|fuji_acros|polaroid_px680) ;; *) return 11;; esac
    case "$setting_grain:$setting_halation" in [0-3]:[01]) ;; *) return 11;; esac
    return 0
}
confirm_settings() {
    read_settings
    confirm_rc=$?
    if test "$confirm_rc" != 0; then fail "$1" "$confirm_rc"; fi
    test "$SETTING_LINE" = "$INITIAL_PLAN" || fail "$1-changed" 12
}
# The camera UDC's configured state can remain stale after physical unplug.
# Probe actual native media access instead. Only a pinned exact source-open
# failure (no input created, fresh completion, target not attempted) may wait.
# Used only for the initial source read. The separate target's fresh absence,
# write, flush and small readback confirm its completion without reopening the
# full original twice. No operation here reserves against a media transition.
quiesce_partial_read_input() {
    if test ! -e "$INPUT" && test ! -L "$INPUT"; then return 0; fi
    prev_bytes=-1
    idle_hits=0
    q_polls=0
    while test "$q_polls" -lt 6; do
        if test ! -e "$INPUT" && test ! -L "$INPUT"; then return 0; fi
        cur_bytes=-1
        if file_bytes "$INPUT" 2>/dev/null; then cur_bytes=$FILE_BYTES; fi
        if test "$cur_bytes" = "$prev_bytes"; then
            idle_hits=$((idle_hits+1))
            if test "$idle_hits" -ge 2; then
                "$BB" rm -f "$INPUT" || return 1
                test ! -e "$INPUT" && test ! -L "$INPUT" && return 0
                return 1
            fi
        else
            idle_hits=0
            prev_bytes=$cur_bytes
        fi
        "$BB" sleep 1 || return 1
        q_polls=$((q_polls+1))
    done
    return 1
}
read_source_ready() {
    media_stage=$1
    read_src=${2:-$SOURCE}
    media_waited_here=0
    read_retries=0
    while :; do
        guard || fail media_wait_guard 13
        test ! -e "$CAPTURE_STOP" || fail media_wait_stopped 20
        if test "$MODE" = apply && (test "$media_waited_here" = 1 || test "$read_retries" -gt 0); then confirm_settings settings_after_media_wait; fi
        native_run read "$read_src"
        media_rc=$?
        if test "$media_rc" = 0; then
            if test "$media_waited_here" = 1; then
                echo "RAM_JOB_MEDIA_RESUMED stage=$media_stage polls=$media_wait_polls"
            fi
            return 0
        fi
        if test "$media_rc" = 15; then
            if ! quiesce_partial_read_input; then
                NATIVE_UNCERTAIN=1;retain=1;fail read_active_unconfirmed 21
            fi
            NATIVE_UNCERTAIN=0
            retain=0
            read_retries=$((read_retries+1))
            echo "RAM_JOB_READ_RETRY attempt=$read_retries rc=$NATIVE_LAST_STATUS"
            if test "$read_retries" -gt 3; then
                fail read_retry_exhausted 13
            fi
            "$BB" sleep 1 || fail read_retry_sleep 13
            continue
        fi
        test "$media_rc" = 14 || fail media_completion_unconfirmed 21
        if test "$media_waited_here" = 0; then
            media_waited_here=1
            PHASE=media_wait_$media_stage
            manifest || fail manifest_media_wait
            echo "RAM_JOB_MEDIA_WAIT stage=$media_stage limit_polls=$MEDIA_WAIT_POLLS"
        fi
        test "$media_wait_polls" -lt "$MEDIA_WAIT_POLLS" || fail media_wait_timeout 13
        media_wait_polls=$((media_wait_polls+1))
        "$BB" sleep 1 || fail media_wait_sleep 13
    done
}
memory_budget() {
    test -r "$MEMINFO" || return 1
    mem_seen=0
    mem_bad=0
    mem_free=0
    mem_buffers=0
    mem_cached=0
    mem_shmem=0
    while read -r mem_key mem_value mem_unit mem_rest; do
        case "$mem_key" in MemFree:) mem_bit=1;; Buffers:) mem_bit=2;; Cached:) mem_bit=4;; Shmem:) mem_bit=8;; *) continue;; esac
        case "$mem_value" in ''|*[!0-9]*) mem_bad=1; continue;; esac
        if test "${#mem_value}" -gt 9 || test "$mem_unit" != kB || test "$((mem_seen & mem_bit))" != 0; then
            mem_bad=1;continue
        fi
        mem_seen=$((mem_seen | mem_bit))
        case "$mem_key" in MemFree:) mem_free=$mem_value;; Buffers:) mem_buffers=$mem_value;; Cached:) mem_cached=$mem_value;; Shmem:) mem_shmem=$mem_value;; esac
    done < "$MEMINFO"
    test "$mem_bad" = 0 && test "$mem_seen" = 15 || return 1
    mem_available=$((mem_free+mem_buffers+mem_cached-mem_shmem))
    # The worker additionally checks the selected image's RGB/mask requirement
    # against current memory before allocating; never reserve 24MP RGB blindly.
    mem_required=$(((EXPECTED_BYTES+1023)/1024+${1:-16384}+16384))
    echo "RAM_JOB_MEMORY available_kib=$mem_available required_kib=$mem_required private_mount_bytes=33554432"
    test "$mem_available" -ge "$mem_required"
}

clock_ms || :
JOB_STARTED=$CLOCK_MS
HIRES_ARG=
case "$MODE:$#" in
    render:6) PRESET=$4;GRAIN=$5;HALATION=$6;;
    apply:4) TARGET=$4;;
    apply:5) TARGET=$4
        case "$5" in
            hires) HIRES_ARG=--hires;;
            ultra12m) HIRES_ARG=--ultra12m;;
            *) fail arguments;;
        esac;;
    dexp:7) SOURCE1=$2;EXPECTED_BYTES1=$3;SOURCE=$4;EXPECTED_BYTES=$5;TARGET=$6;DEXP_MODE=$7;PRESET=dexp_$7;GRAIN=0;HALATION=0;;
    dexp:8) SOURCE1=$2;EXPECTED_BYTES1=$3;SOURCE=$4;EXPECTED_BYTES=$5;TARGET=$6;DEXP_MODE=$7;PRESET=dexp_$7;GRAIN=0;HALATION=0
        case "$8" in
            hires) HIRES_ARG=--hires;;
            ultra12m) HIRES_ARG=--ultra12m;;
            *) fail arguments;;
        esac;;
    *) fail arguments;;
esac
case "$EXPECTED_BYTES" in ''|0|0*|*[!0-9]*) fail expected_size;; esac
test "${#EXPECTED_BYTES}" -le 8 || fail expected_size
if test "$MODE" = dexp; then
    case "$EXPECTED_BYTES1" in ''|0|0*|*[!0-9]*) fail expected_size;; esac
    test "${#EXPECTED_BYTES1}" -le 8 || fail expected_size
    case "$DEXP_MODE" in screen|lighten|average) ;; *) fail arguments;; esac
    test "$((EXPECTED_BYTES+EXPECTED_BYTES1+12582912+65536))" -le 41943040 || fail scratch_capacity
    s1_tail=${SOURCE1#A:/DCIM/}
    s1_dir=${s1_tail%%/*}
    s1_file=${s1_tail#*/}
    s1_name=${s1_file%.JPG}
    test "${#s1_dir}" = 8 && test "${#s1_name}" = 8 &&
        test "$SOURCE1" = "A:/DCIM/$s1_dir/$s1_name.JPG" || fail source_path
    case "$s1_dir$s1_name" in *[!A-Za-z0-9_-]*) fail source_path;; esac
else
    test "$((EXPECTED_BYTES+16777216+65536))" -le 33554432 || fail scratch_capacity
fi
source_tail=${SOURCE#A:/DCIM/}
source_dir=${source_tail%%/*}
source_file=${source_tail#*/}
source_name=${source_file%.JPG}
test "${#source_dir}" = 8 && test "${#source_name}" = 8 &&
    test "$SOURCE" = "A:/DCIM/$source_dir/$source_name.JPG" || fail source_path
case "$source_dir$source_name" in *[!A-Za-z0-9_-]*) fail source_path;; esac
if test "$MODE" = render; then
    case "$PRESET" in portra_400|cinestill_800t|fuji_provia|kodak_gold|tri_x_400) ;; *) fail preset;; esac
    case "$GRAIN:$HALATION" in [0-3]:[01]) ;; *) fail effect_arguments;; esac
else
    target_digits=${TARGET#A:/A6400/TEST/F}
    target_digits=${target_digits%.JPG}
    test "${#target_digits}" = 7 && test "$TARGET" = "A:/A6400/TEST/F$target_digits.JPG" || fail target_path
    case "$target_digits" in *[!0-9]*) fail target_path;; esac
fi
test -x "$NATIVE" && hash_is "$NATIVE" "$NATIVE_SHA" || fail native_pin
test -x "$WORKER" && hash_is "$WORKER" "$WORKER_SHA" || fail worker_pin
if test "$MODE" = apply; then
    read_settings
    initial_rc=$?
    if test "$initial_rc" != 0; then fail settings_initial "$initial_rc"; fi
    INITIAL_PLAN=$SETTING_LINE
    PRESET=$setting_preset;GRAIN=$setting_grain;HALATION=$setting_halation
fi
memory_budget || fail memory_budget
no_linux_sd || fail linux_sd_mounted
IFS= read -r BOOT < "$BOOTFILE" || fail boot_id
test "${#BOOT}" = 36 || fail boot_id
case "$BOOT" in *[!0-9a-f-]*) fail boot_id;; esac
test ! -e "$RAM" && test ! -L "$RAM" || fail existing_ram_directory
test ! -e "$LOCK" && test ! -L "$LOCK" || fail existing_lock
"$BB" mkdir "$LOCK" || fail lock
trap finish 0
trap '' HUP
trap interrupted INT TERM
PHASE=mount
manifest || fail manifest
"$BB" mkdir "$RAM" || fail ram_directory
ram_created=1
MOUNT_BYTES=33554432
if test "$MODE" = dexp; then MOUNT_BYTES=41943040; fi
"$BB" mount -t tmpfs -o size=$MOUNT_BYTES,nr_inodes=16,mode=700,nosuid,nodev,noexec a6400-film-job "$RAM"
mount_rc=$?
ram_mount_state
mount_state=$?
if test "$mount_state" = 2; then retain=1;fail mount_unconfirmed 21; fi
test "$mount_rc" = 0 && test "$mount_state" = 0 || fail mount

if test "$MODE" = dexp; then
    read_source_ready before_read1 "$SOURCE1"
    file_bytes "$INPUT" && test "$FILE_BYTES" = "$EXPECTED_BYTES1" || fail source_size
    "$BB" mv -f "$INPUT" "$INPUT1" || fail source_size
    read_source_ready before_read "$SOURCE"
    file_bytes "$INPUT" && test "$FILE_BYTES" = "$EXPECTED_BYTES" || fail source_size
    INPUT_SHA=size:$EXPECTED_BYTES
elif test "$MODE" = apply; then
    read_source_ready before_read
    file_bytes "$INPUT" && test "$FILE_BYTES" = "$EXPECTED_BYTES" || fail source_size
    INPUT_SHA=size:$EXPECTED_BYTES
else
    native_run read "$SOURCE"
    file_bytes "$INPUT" && test "$FILE_BYTES" = "$EXPECTED_BYTES" || fail source_size
    hash_file "$INPUT" || fail source_hash
    INPUT_SHA=$HASH
    decode_input verify_input input_decode
fi

PHASE=render
guard || fail pre_render_guard
echo "RAM_JOB_RENDER_START preset=$PRESET grain=$GRAIN halation=$HALATION"
worker_inflight=1
clock_ms || :
render_started=$CLOCK_MS
PREVIEW_ARGS=
if (test "$MODE" = apply || test "$MODE" = dexp) && test -d "$PV_DIR" && test ! -L "$PV_DIR"; then
    PREVIEW_ARGS="--preview $PREVIEW"
fi
if test "$MODE" = dexp; then
    "$WORKER" --dexp "$DEXP_MODE" "$INPUT1" "$INPUT" "$OUTPUT" $HIRES_ARG $PREVIEW_ARGS > "$LOCK/render.log" 2>&1 &
else
    "$WORKER" "$PRESET" "$INPUT" "$OUTPUT" "$GRAIN" "$HALATION" $HIRES_ARG $PREVIEW_ARGS > "$LOCK/render.log" 2>&1 &
fi
worker_pid=$!
manifest || fail manifest_worker
wait "$worker_pid"
worker_rc=$?
worker_inflight=0
render_done=0
while IFS= read -r render_line || test -n "$render_line"; do
    echo "$render_line"
    case "$render_line" in FILM_DONE\ *) render_done=1;; esac
done < "$LOCK/render.log"
test "$worker_rc" = 0 && test "$render_done" = 1 || fail render
stage_done render "$render_started"
file_bytes "$OUTPUT" && test "$FILE_BYTES" -gt 0 && test "$FILE_BYTES" -le 16777216 || fail output_size
OUTPUT_BYTES=$FILE_BYTES
if test "$MODE" = apply || test "$MODE" = dexp; then
    OUTPUT_SHA=flush_ok:$OUTPUT_BYTES
    PHASE=release_original_scratch
    release_input
    if test "$MODE" = apply; then confirm_settings settings_before_write; fi
    test ! -e "$CAPTURE_STOP" || fail stopped_before_write 20
    guard || fail pre_write_output
    TARGET_ATTEMPTED=1
    retain=1
    native_run commit "$TARGET"
    TARGET_VERIFIED=1
    if test -f "$PREVIEW" && test ! -L "$PREVIEW" && test -d "$PV_DIR" && test ! -L "$PV_DIR"; then
        "$BB" cp "$PREVIEW" "$PV_DIR/latest.pkg.tmp" && "$BB" mv -f "$PV_DIR/latest.pkg.tmp" "$PV_DIR/latest.pkg" || :
    fi
    PHASE=verified_target
    manifest || fail manifest_verified_target
    PHASE=applied
    retain=0
    applied=1
    exit 0
fi
hash_file "$OUTPUT" || fail output_hash
OUTPUT_SHA=$HASH
guard || fail post_render_guard
PHASE=rendered
retain=1
echo "RAM_JOB_RENDERED input_sha256=$INPUT_SHA output_sha256=$OUTPUT_SHA output_bytes=$OUTPUT_BYTES"
exit 0
