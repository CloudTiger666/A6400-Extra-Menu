#!/usr/bin/env python3
"""Deploy, inspect (--diag), or restore (--rollback) V3.9 SSLF on Sony A6400 / A6100 via Sony-PMCA-RE."""
import argparse
import base64
import hashlib
import io
import json
import posixpath
import re
import sys
import tarfile
import time
from datetime import datetime
from pathlib import Path

MAX_SHELL_CMD_LEN = 180
HOOK_CAMERA_PATH = "/etc/a6400-autoexec.so"
LEGACY_HOOK_PATH = "/tmp/a6400-autoexec.so"
SAFE_SERIAL_MODE = sys.platform == "win32"


def use_safe_serial(safe_serial=None):
    return SAFE_SERIAL_MODE if safe_serial is None else bool(safe_serial)


def run_cmd(cam, cmd, timeout=15.0, fast=False, retry_on_none=True, safe_serial=None, tag=None):
    dev = cam.dev
    attempts = 3 if retry_on_none else 1
    out_bytes = b""
    rc = None
    last_exc = None
    marker_prefix = f"__PMCA_RC_{tag}:" if tag else "__PMCA_RC:"
    pat = re.compile(re.escape(marker_prefix.encode()) + rb"(\d+)__")
    chunked_write = use_safe_serial(safe_serial)
    for attempt in range(attempts):
        if attempt > 0:
            # Recover from a potentially truncated line or transient USB glitch before retrying.
            try:
                dev.writeTerminal(b"\x03\n")
                time.sleep(0.25)
                dev.readTerminal()
            except Exception:
                time.sleep(0.25)
        payload = f"{cmd};R=$?;echo;echo {marker_prefix}${{R}}__\n".encode()
        out = bytearray()
        try:
            if chunked_write and len(payload) > 48:
                # SonySenserDevice.writeTerminal loops at 0x40 (64B) with 0ms delay.
                # Slicing at 48B (< 64B) guarantees 1 USB packet per write + echo drain.
                for i in range(0, len(payload), 48):
                    dev.writeTerminal(payload[i:i + 48])
                    time.sleep(0.02)
                    if i + 48 < len(payload):
                        try:
                            ech = dev.readTerminal()
                            if ech:
                                out.extend(ech)
                        except Exception:
                            pass
            else:
                dev.writeTerminal(payload)
        except Exception as exc:
            last_exc = exc
            time.sleep(0.25)
            continue
        end = time.monotonic() + timeout
        post_rc_deadline = None
        rc = None
        quiet = 0
        read_errs = 0
        while time.monotonic() < end:
            try:
                chunk = dev.readTerminal()
                read_errs = 0
            except Exception as exc:
                last_exc = exc
                read_errs += 1
                if read_errs >= 3:
                    break
                time.sleep(0.05)
                continue
            if chunk:
                out.extend(chunk)
                quiet = 0
                ms = list(pat.finditer(bytes(out)))
                if ms:
                    rc = int(ms[-1].group(1))
                    if fast:
                        break
                    if post_rc_deadline is None:
                        post_rc_deadline = time.monotonic() + (0.15 if chunked_write else 0.03)
                    elif time.monotonic() >= post_rc_deadline:
                        break
            else:
                if rc is not None:
                    quiet += 1
                    if quiet >= 6 or (post_rc_deadline is not None and time.monotonic() >= post_rc_deadline):
                        break
                time.sleep(0.005)
        out_bytes = bytes(out)
        if rc is not None:
            break
    if rc is None and not out_bytes and last_exc is not None:
        return f"[usb_err={last_exc}]", None
    return out_bytes.decode("latin1", errors="replace"), rc


def must_cmd(cam, cmd, label, timeout=15.0, safe_serial=None):
    out, rc = run_cmd(cam, cmd, timeout=timeout, retry_on_none=True, safe_serial=safe_serial)
    if rc is None:
        raise RuntimeError(
            f"{label} failed rc=None (終端命令可能被截斷或超時，已重試一次仍無回應): {out[-400:]}"
        )
    if rc != 0:
        raise RuntimeError(f"{label} failed rc={rc}: {out[-400:]}")
    return out


def atomic_copy_steps(src, dst, mode):
    parent = posixpath.dirname(dst)
    name = posixpath.basename(dst)
    tmp_dst = f"{parent}/{name}.new"
    return [
        f"/bin/busybox cp {src} {tmp_dst}",
        f"/bin/busybox chmod {mode} {tmp_dst} && /bin/busybox sync",
        f"/bin/busybox mv {tmp_dst} {dst} && /bin/busybox sync",
    ]


def build_backup_steps():
    targets = (
        "/etc/ld.so.preload",
        "/tmp/autoexec.sh",
        "/tmp/a6400-film-boot-v15.tar",
        HOOK_CAMERA_PATH,
    )
    steps = []
    for t in targets:
        steps.append(f"P={t}; B=$P.pre-v3c-backup")
        steps.append("test ! -f $P || test -f $B || /bin/busybox cp -p $P $B")
    steps.append("/bin/busybox sync")
    return steps


def build_install_steps(has_hook, extra_entries):
    d = "/tmp_bt/v3c_up"
    steps = []
    if has_hook:
        steps.extend(atomic_copy_steps(f"{d}/new-autoexec.so", HOOK_CAMERA_PATH, "755"))
    for item in extra_entries:
        name, mode = item if isinstance(item, tuple) else (item, "755" if str(item).endswith(".elf") else "644")
        steps.extend(atomic_copy_steps(f"{d}/{name}", f"/tmp/{name}", mode))
    steps.extend(atomic_copy_steps(f"{d}/new-boot.tar", "/tmp/a6400-film-boot-v15.tar", "644"))
    steps.extend(atomic_copy_steps(f"{d}/new-autoexec.sh", "/tmp/autoexec.sh", "755"))
    if has_hook:
        steps.extend([
            f"echo {HOOK_CAMERA_PATH} > /etc/ld.so.preload.new",
            "/bin/busybox chmod 644 /etc/ld.so.preload.new && /bin/busybox sync",
            "/bin/busybox mv /etc/ld.so.preload.new /etc/ld.so.preload && /bin/busybox sync",
        ])
    steps.append("/bin/busybox rm -f /tmp/disable_hook && /bin/busybox sync")
    return steps


def build_rollback_steps():
    # True factory restore: strip any a6400-autoexec entry from /etc/ld.so.preload
    # (removing the file if empty, matching stock Sony) and remove all mod files
    # plus any .pre-v3c-backup copies created by previous upgrades (e.g. V3.9 -> V4.1).
    steps = [
        "/bin/busybox grep -v a6400-autoexec /etc/ld.so.preload > /etc/ld.so.preload.tmp 2>/dev/null || true",
        "test -s /etc/ld.so.preload.tmp && /bin/busybox mv /etc/ld.so.preload.tmp /etc/ld.so.preload || /bin/busybox rm -f /etc/ld.so.preload /etc/ld.so.preload.tmp",
        "/bin/busybox rm -f /etc/ld.so.preload.pre-v3c-backup",
    ]
    for path in (
        HOOK_CAMERA_PATH,
        f"{HOOK_CAMERA_PATH}.pre-v3c-backup",
        "/tmp/autoexec.sh",
        "/tmp/autoexec.sh.pre-v3c-backup",
        "/tmp/a6400-film-boot-v15.tar",
        "/tmp/a6400-film-boot-v15.tar.pre-v3c-backup",
        "/tmp/a6400-charo-bind.elf",
        "/tmp/a6400-charo-bind.elf.pre-v3c-backup",
        "/tmp/a6400-charo-shutter.pcm",
        "/tmp/a6400-charo-shutter.pcm.pre-v3c-backup",
        "/tmp/a6400-charo-connect.pcm",
        "/tmp/a6400-charo-push.pcm",
        "/tmp/a6400-hook-err.log",
        "/tmp/a6400-ui-hang.log",
    ):
        steps.append(f"/bin/busybox rm -f {path}")
    steps.append("/bin/busybox sync")
    return steps


def build_diag_commands():
    return [
        ("VERSION", "/bin/busybox uname -a"),
        ("MOUNTS", "/bin/busybox cat /proc/mounts"),
        ("PRELOAD", "/bin/busybox cat /etc/ld.so.preload 2>&1 || true"),
        ("FILES", f'/bin/busybox ls -la {HOOK_CAMERA_PATH} /tmp/autoexec.sh /tmp/a6400-* 2>&1'),
        ("SHA256", f'/bin/busybox sha256sum {HOOK_CAMERA_PATH} /tmp/autoexec.sh /tmp/a6400-film-boot-v15.tar 2>&1'),
        ("HOOK_ERR", "/bin/busybox cat /tmp/a6400-hook-err.log 2>&1 || true"),
        ("BOOT_LOG", "/bin/busybox cat /tmp_bt/a6400-film-boot.log 2>&1 || true"),
        ("TMP_BT_LS", "/bin/busybox ls -la /tmp_bt /tmp_bt/a6400-film-runtime.lock 2>&1 || true"),
        ("PS", "/bin/busybox ps 2>&1 || true"),
        ("RUNTIME_LOG", "/bin/busybox tail -n 80 /tmp_bt/a6400-film-capture.runtime.log 2>&1 || true"),
        ("MANIFEST", "/bin/busybox cat /tmp_bt/a6400-film-capture.manifest 2>&1 || true"),
        ("JOB_LOG", "/bin/busybox tail -n 60 /tmp_bt/a6400-film-capture.job.log 2>&1 || true"),
        ("IM_MAPS", "/bin/busybox grep -E 'libObj|viewUnified2|CautionConfig' /proc/152/maps 2>&1 || true"),
    ]


def build_upload_chunks(b64_text, step=120):
    # Use newline-delimited echo so wc -l can verify every chunk on camera (busybox base64 -d ignores newlines).
    return [f'echo {b64_text[i:i + step]} >> /tmp_bt/v3c_up/up.b64' for i in range(0, len(b64_text), step)]


def upload_b64_chunks(cam, chunks, safe_serial=None, progress_cb=None):
    total = len(chunks)
    for idx, chunk_cmd in enumerate(chunks):
        # Use a unique per-chunk confirmation tag (C{idx}) so delayed echoes from earlier chunks never match.
        tag = f"C{idx}"
        _, rc = run_cmd(cam, chunk_cmd, timeout=8.0, fast=True, retry_on_none=False, safe_serial=safe_serial, tag=tag)
        if rc != 0:
            for retry in range(2):
                diag = must_cmd(
                    cam,
                    'echo __L__; /bin/busybox wc -l < /tmp_bt/v3c_up/up.b64; echo __E__',
                    f'UP_DIAG_{idx}',
                    safe_serial=safe_serial,
                )
                m = re.search(r'__L__\s*(\d+)\s*__E__', diag)
                lines = int(m.group(1)) if m else -1
                if lines == idx + 1:
                    break
                if lines == idx and retry == 0:
                    # Chunk did not land at all -> safe to re-send once without duplicate append.
                    run_cmd(
                        cam,
                        chunk_cmd,
                        timeout=8.0,
                        fast=True,
                        retry_on_none=False,
                        safe_serial=safe_serial,
                        tag=f"{tag}R",
                    )
                    continue
                raise RuntimeError(
                    f"upload chunk {idx}/{total} failed rc={rc}, lines={lines}, expected={idx + 1}: {diag[-300:]}"
                )
        if progress_cb is not None:
            progress_cb(idx + 1, total)


def verify_uploaded_b64_lines(cam, expected_lines, safe_serial=None):
    pre_dec = must_cmd(
        cam,
        'echo __B64_L__; /bin/busybox wc -l < /tmp_bt/v3c_up/up.b64; '
        'echo __B64_C__; /bin/busybox wc -c < /tmp_bt/v3c_up/up.b64; echo __B64_E__',
        'B64_PRECHECK',
        safe_serial=safe_serial,
    )
    ml = re.search(r'__B64_L__\s*(\d+)\s*__B64_C__', pre_dec)
    mc = re.search(r'__B64_C__\s*(\d+)\s*__B64_E__', pre_dec)
    lines_n = int(ml.group(1)) if ml else -1
    bytes_n = int(mc.group(1)) if mc else -1
    if lines_n != expected_lines:
        raise RuntimeError(
            f"up.b64 line count mismatch: expected={expected_lines}, actual={lines_n}: {pre_dec[-300:]}"
        )
    return lines_n, bytes_n


def check_package_charo_notice(extra_items, assume_yes=False, interactive=None):
    """Warn (and optionally confirm) when deploying a charo=0 package without custom shutter sounds."""
    if any(ename == "a6400-charo-shutter.pcm" for _, ename, _, _ in extra_items):
        return True
    print(
        "[!] 注意：此開機包未包含自訂快門音效（charo=0 公開純淨版）。\n"
        "    - 若不需要自訂快門音，可直接略過並繼續部署。\n"
        "    - 若需要導入（最多 3 組：0x34<=29760B、0x33<=24768B、0x35<=18560B），\n"
        "      請先將 MP3 轉為單聲道 24 kHz Sony 32-byte SDS ADPCM 放入 assets/ 並重新建置後再刷入。",
        flush=True,
    )
    if assume_yes:
        return True
    is_tty = sys.stdin.isatty() if interactive is None else bool(interactive)
    if not is_tty:
        return True
    ans = input("是否略過快門音效並繼續部署無音效版？[Y/n] ").strip().lower()
    if ans in ("n", "no"):
        print("已取消部署；請先備妥 24 kHz Sony SDS ADPCM (.pcm) 並重新打包。", flush=True)
        return False
    return True


def posix_cksum(data: bytes):
    """Compute POSIX cksum (CRC-32 IEEE 802.3 MSB-first + length) matching busybox cksum."""
    crc = 0
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) if (crc & 0x80000000) else (crc << 1)
            crc &= 0xFFFFFFFF
    n = len(data)
    while n:
        crc ^= (n & 0xFF) << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) if (crc & 0x80000000) else (crc << 1)
            crc &= 0xFFFFFFFF
        n >>= 8
    return (~crc) & 0xFFFFFFFF, len(data)


EMPTY_SHA256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
EMPTY_MD5 = "d41d8cd98f00b204e9800998ecf8427e"
EMPTY_CKSUM = "4294967295 0"


def detect_camera_hash_algo(cam):
    """Detect the strongest checksum tool supported by camera (/bin/busybox or /usr/bin/openssl)."""
    probes = (
        ("sha256", "/bin/busybox sha256sum /dev/null 2>&1", EMPTY_SHA256),
        ("openssl_sha256", "/usr/bin/openssl dgst -sha256 /dev/null 2>&1", EMPTY_SHA256),
        ("md5", "/bin/busybox md5sum /dev/null 2>&1", EMPTY_MD5),
        ("cksum", "/bin/busybox cksum /dev/null 2>&1", EMPTY_CKSUM),
    )
    for algo, cmd, expected in probes:
        out, rc = run_cmd(cam, cmd, timeout=8.0)
        if rc == 0 and expected in out.lower():
            return algo
    return "size"


def detect_camera_b64_mode(cam):
    """Return 'busybox' if /bin/busybox base64 works, else 'openssl' (/usr/bin/openssl enc -base64)."""
    out, rc = run_cmd(cam, "echo A | /bin/busybox base64 2>&1", timeout=8.0)
    if rc == 0 and "QQo=" in out:
        return "busybox"
    return "openssl"


def local_digest(data: bytes, algo: str) -> str:
    if algo in ("sha256", "openssl_sha256"):
        return hashlib.sha256(data).hexdigest().lower()
    if algo == "md5":
        return hashlib.md5(data).hexdigest().lower()
    if algo == "cksum":
        crc, n = posix_cksum(data)
        return f"{crc} {n}"
    return str(len(data))


def verify_camera_file(cam, remote_path: str, expected_bytes: bytes, label: str, hash_algo: str = "sha256"):
    """Verify remote file integrity on camera using the detected hash algorithm."""
    exp = local_digest(expected_bytes, hash_algo)
    if hash_algo == "sha256":
        out = must_cmd(cam, f'/bin/busybox sha256sum "{remote_path}"', label)
        ok = exp in out.lower()
    elif hash_algo == "openssl_sha256":
        out = must_cmd(cam, f"/usr/bin/openssl dgst -sha256 {remote_path} 2>/dev/null", label)
        ok = exp in out.lower()
    elif hash_algo == "md5":
        out = must_cmd(cam, f'/bin/busybox md5sum "{remote_path}"', label)
        ok = exp in out.lower()
    elif hash_algo == "cksum":
        out = must_cmd(cam, f'/bin/busybox cksum "{remote_path}"', label)
        ok = re.search(rf"\b{re.escape(exp)}\b", out) is not None
    else:
        out = must_cmd(cam, f'echo __SZ__; /bin/busybox wc -c < "{remote_path}"; echo __SE__', label)
        m = re.search(r"__SZ__\s*(\d+)\s*__SE__", out)
        ok = bool(m and m.group(1) == exp)
    if not ok:
        raise RuntimeError(f"{label} {hash_algo} mismatch for {remote_path} (expected {exp})")


def read_camera_file_b64(cam, remote_path, chunk_bytes=3072, hash_algo="sha256", b64_mode="busybox"):
    """Safely read a binary file from camera over serviceshell in bounded base64 chunks + integrity verification."""
    if hash_algo == "sha256":
        hash_cmd = f'/bin/busybox sha256sum "{remote_path}"'
        hash_re = r"__SH__[\s\S]*?([0-9a-fA-F]{64})\b"
    elif hash_algo == "openssl_sha256":
        hash_cmd = f"/usr/bin/openssl dgst -sha256 {remote_path} 2>/dev/null"
        hash_re = r"__SH__[\s\S]*?([0-9a-fA-F]{64})\b"
    elif hash_algo == "md5":
        hash_cmd = f'/bin/busybox md5sum "{remote_path}"'
        hash_re = r"__SH__[\s\S]*?([0-9a-fA-F]{32})\b"
    elif hash_algo == "cksum":
        hash_cmd = f'/bin/busybox cksum "{remote_path}"'
        hash_re = r"__SH__\s*(\d+\s+\d+)\b"
    else:
        hash_cmd = ""
        hash_re = None

    b64_pipe = "/bin/busybox base64 2>/dev/null" if b64_mode == "busybox" else "/usr/bin/openssl enc -base64 2>/dev/null"

    stat_cmd = f"echo __SZ__; /bin/busybox wc -c < {remote_path}; echo __SH__"
    if hash_cmd:
        stat_cmd += f"; {hash_cmd}"
    stat_out = must_cmd(cam, stat_cmd, "STAT_CAM_FILE")
    m_sz = re.search(r"__SZ__\s*(\d+)\s*__SH__", stat_out)
    m_sh = re.search(hash_re, stat_out) if hash_re else True
    if not m_sz or not m_sh:
        raise RuntimeError(f"Failed to stat camera file {remote_path}: {stat_out[-300:]}")
    total_size = int(m_sz.group(1))
    exp_digest = re.sub(r"\s+", " ", m_sh.group(1).strip().lower()) if hash_re else str(total_size)
    if total_size <= 0:
        raise RuntimeError(f"Empty camera file {remote_path}")

    n_chunks = (total_size + chunk_bytes - 1) // chunk_bytes
    collected = bytearray()
    for skip in range(n_chunks):
        expected_len = min(chunk_bytes, total_size - skip * chunk_bytes)
        chunk_data = None
        last_err = ""
        for _ in range(3):
            out = must_cmd(
                cam,
                f"echo __FB__; /bin/busybox dd if={remote_path} bs={chunk_bytes} count=1 skip={skip} 2>/dev/null | {b64_pipe}; echo __FE__",
                f"PULL_CHUNK_{skip}",
                timeout=15.0,
            )
            matches = re.findall(r"__FB__\r?\n([\s\S]*?)\r?\n__FE__", out)
            if not matches:
                last_err = f"missing framing markers: {out[-200:]}"
                continue
            body = matches[-1]
            # Filter line-by-line so any OpenSSL stderr warning or kernel log line cannot break decoding.
            b64_lines = [ln.strip() for ln in body.splitlines() if re.fullmatch(r"[A-Za-z0-9+/=]+", ln.strip())]
            b64_clean = "".join(b64_lines)
            try:
                raw = base64.b64decode(b64_clean, validate=True)
            except Exception as exc:
                last_err = f"base64 decode error: {exc}"
                continue
            if len(raw) != expected_len:
                last_err = f"length {len(raw)} != expected {expected_len}"
                continue
            chunk_data = raw
            break
        if chunk_data is None:
            raise RuntimeError(f"Failed pulling {remote_path} chunk {skip}/{n_chunks}: {last_err}")
        collected.extend(chunk_data)

    data = bytes(collected)
    got_digest = local_digest(data, hash_algo)
    if got_digest != exp_digest:
        raise RuntimeError(f"Pulled {remote_path} {hash_algo} mismatch: got={got_digest} expected={exp_digest}")
    return data


HOOK_HOST_FALLBACKS = (
    "/usr/lib/libcompassutil.so",
    "/usr/lib/libInfraNfc.so",
)


def build_hook_from_camera(cam, stub_bytes, remote_path="/usr/lib/libAK8975.so", hash_algo="sha256", b64_mode="busybox"):
    """Pull camera's stock preload host SO (libAK8975.so first, with A6100/A6400 fallbacks),
    patch with open-source ARM stub, and return (so_bytes, sha256)."""
    import build_autoexec_hook as hook_builder

    candidates = [remote_path] + [p for p in HOOK_HOST_FALLBACKS if p != remote_path]
    last_err = None
    for cand in candidates:
        try:
            src_bytes = read_camera_file_b64(cam, cand, hash_algo=hash_algo, b64_mode=b64_mode)
            patched = hook_builder.patch_libak8975(src_bytes, stub_bytes)
            return patched, hashlib.sha256(patched).hexdigest()
        except Exception as exc:
            last_err = exc
    raise last_err


def build_deploy_tgz(boot_sh, boot_tar, hook_bytes, extra_items):
    """Assemble in-memory upload tar.gz containing scripts, boot tar, optional hook SO, and extra files."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tgz:
        tgz.add(boot_sh, arcname="new-autoexec.sh")
        tgz.add(boot_tar, arcname="new-boot.tar")
        if hook_bytes is not None:
            ti = tarfile.TarInfo("new-autoexec.so")
            ti.size = len(hook_bytes)
            ti.mode = 0o755
            tgz.addfile(ti, io.BytesIO(hook_bytes))
        for ep, ename, _, _ in extra_items:
            tgz.add(ep, arcname=ename)
    tgz_bytes = buf.getvalue()
    return tgz_bytes, hashlib.sha256(tgz_bytes).hexdigest()


def detect_windows_usb_driver_hint():
    if sys.platform != "win32":
        return ""
    try:
        import usb.core
        devs = list(usb.core.find(find_all=True, idVendor=0x054C) or [])
        if devs:
            pids = ", ".join(f"054C:{d.idProduct:04X}" for d in devs)
            return (
                f"偵測到 Sony USB 裝置 ({pids})，但 PMCA 無法以 libusb-win32 開啟；"
                "請確認不是 WinUSB 驅動（PMCA **不支援 WinUSB**，請改用 libusb-win32）。"
            )
    except Exception:
        pass
    return ""


def format_usb_help(exc=None, platform=None, driver_hint=""):
    plat = platform or sys.platform
    lines = []
    if plat == "win32":
        lines.append("\n[!] 尚未完成連線：相機未進入 Mass Storage / Service 模式，或 Windows 驅動不是 libusb-win32。")
        if driver_hint:
            lines.append(f"診斷：{driver_hint}")
        lines.extend([
            "請依以下步驟確認後重跑：",
            "  1. 相機切為 Mass Storage（大容量儲存）模式並以 USB 線接妥（勿選 MTP/PC遙控）",
            "  2. 以系統管理員執行 Zadig -> Options -> List All Devices",
            "  3. 注意勿選到筆電內建視訊攝影機（WebCam / HD Camera）！請選 ILCE-6400",
            "  4. 右側驅動**必須選 libusb-win32**（PMCA 不支援 WinUSB，選 WinUSB 會顯示 No devices found）-> Replace Driver",
            "  5. 切換服務模式（054C:0336）後，Zadig 不會自動刷新清單：**請完全關閉並重開 Zadig** 再確認 054C:0336 也為 libusb-win32",
        ])
    else:
        lines.append("\n[!] 尚未完成連線：請確認相機已切為 Mass Storage 模式並接上 USB（Linux 若無權限請加 sudo）。")
    if exc is not None:
        exc_str = str(exc)
        if "WinUSB" in exc_str or "GenericUsbDriver" in exc_str:
            lines.append("提示：偵測到 WinUSB / 非 libusb 驅動錯誤 — PMCA 僅支援 libusb-win32。")
        lines.append(f"原始錯誤：{type(exc).__name__}: {exc}")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package_dir", nargs="?", default="", help="Path to local-boot-package directory")
    parser.add_argument("--pmca-dir", default="", help="Path to Sony-PMCA-RE checkout")
    parser.add_argument(
        "--install-hook",
        default="",
        help=f"Optional path to a6400-autoexec.so (installed to {HOOK_CAMERA_PATH})",
    )
    parser.add_argument(
        "--auto-hook-stub",
        default="",
        help="Optional assembled ARM hook stub (.bin) to auto-patch camera /usr/lib/libAK8975.so on blank cameras",
    )
    parser.add_argument("--rollback", action="store_true", help="Restore .pre-v3c-backup files or remove hook cleanly")
    parser.add_argument("--diag", action="store_true", help="Collect camera diagnostics without modifying flash")
    parser.add_argument("--diag-out", default="", help="Optional output path for --diag report")
    parser.add_argument("-y", "--yes", action="store_true", help="Skip interactive confirmation prompts")
    serial_group = parser.add_mutually_exclusive_group()
    serial_group.add_argument(
        "--safe-serial",
        action="store_true",
        help="Force 100-byte/20ms chunked serial writes (auto-enabled on Windows; recommended for A6100)",
    )
    serial_group.add_argument(
        "--fast-serial",
        action="store_true",
        help="Disable chunked serial writes even on Windows",
    )
    args = parser.parse_args()

    global SAFE_SERIAL_MODE
    if args.safe_serial:
        SAFE_SERIAL_MODE = True
    elif args.fast_serial:
        SAFE_SERIAL_MODE = False

    if args.rollback and args.diag:
        parser.error("Cannot combine --rollback and --diag")
    if not args.rollback and not args.diag and not args.package_dir:
        parser.error("package_dir is required unless --rollback or --diag is specified")

    if args.pmca_dir:
        sys.path.insert(0, str(Path(args.pmca_dir).resolve()))
    from pmca.commands.usb import senserShellCommand

    if args.diag:
        diag_ok = False

        def collect_diag(cam):
            nonlocal diag_ok
            cam.setTerminalEnable(True)
            time.sleep(0.6)
            try:
                cam.dev.readTerminal()
            except Exception:
                pass
            sections = []
            try:
                for label, cmd in build_diag_commands():
                    out, rc = run_cmd(cam, cmd, timeout=15.0)
                    sections.append(f"=== {label} (rc={rc}) ===\n{out.strip()}\n")
                report = "\n".join(sections)
                print(report, flush=True)
                out_file = Path(args.diag_out) if args.diag_out else Path(
                    f"a6400-diag-{datetime.now().strftime('%Y%m%d-%H%M%S')}.txt"
                )
                out_file.write_text(report, encoding="utf-8")
                print(f"V3_CLITE_DIAG_OK saved={out_file}", flush=True)
                diag_ok = True
            finally:
                try:
                    cam.setTerminalEnable(False)
                except Exception:
                    pass

        try:
            senserShellCommand(driverName="libusb", complete=collect_diag)
        except Exception as exc:
            print(format_usb_help(exc, driver_hint=detect_windows_usb_driver_hint()), file=sys.stderr)
            sys.exit(1)
        if not diag_ok:
            print(format_usb_help(driver_hint=detect_windows_usb_driver_hint()), file=sys.stderr)
            sys.exit(1)
        return

    if args.rollback:
        rb_ok = False

        def rollback(cam):
            nonlocal rb_ok
            cam.setTerminalEnable(True)
            time.sleep(0.6)
            try:
                cam.dev.readTerminal()
            except Exception:
                pass
            try:
                for idx, step in enumerate(build_rollback_steps()):
                    must_cmd(cam, step, f"ROLLBACK_{idx}", timeout=20.0)
                pre_after = must_cmd(cam, "/bin/busybox cat /etc/ld.so.preload 2>/dev/null || true", "VERIFY_ROLLBACK_PRELOAD")
                if "a6400-autoexec" in pre_after:
                    raise RuntimeError(f"ROLLBACK verify failed: a6400-autoexec still in /etc/ld.so.preload: {pre_after[-200:]}")
                must_cmd(cam, "test ! -f /tmp/autoexec.sh && test ! -f /etc/a6400-autoexec.so", "VERIFY_ROLLBACK_CLEAN")
                print("V3_CLITE_ROLLBACK_OK", flush=True)
                rb_ok = True
            finally:
                try:
                    cam.setTerminalEnable(False)
                except Exception:
                    pass

        try:
            senserShellCommand(driverName="libusb", complete=rollback)
        except Exception as exc:
            print(format_usb_help(exc, driver_hint=detect_windows_usb_driver_hint()), file=sys.stderr)
            sys.exit(1)
        if not rb_ok:
            print(format_usb_help(driver_hint=detect_windows_usb_driver_hint()), file=sys.stderr)
            sys.exit(1)
        return

    pkg = Path(args.package_dir).resolve()
    manifest = json.loads((pkg / "manifest.json").read_text())
    boot_sh = pkg / "bootstrap-local.sh"
    boot_tar = pkg / "persistent/a6400-film-boot-v15.tar"
    boot_sh_bytes = boot_sh.read_bytes()
    boot_tar_bytes = boot_tar.read_bytes()
    hook_path = Path(args.install_hook).resolve() if args.install_hook else None
    init_hook_bytes = hook_path.read_bytes() if hook_path else None
    init_hook_sha = hashlib.sha256(init_hook_bytes).hexdigest() if init_hook_bytes is not None else ""
    stub_bytes = Path(args.auto_hook_stub).resolve().read_bytes() if args.auto_hook_stub else None

    extra_items = []
    extra_bytes_map = {}
    for row in manifest.get("extra_persistent_files", []):
        ep = pkg / row["local_path"]
        data = ep.read_bytes()
        sha = hashlib.sha256(data).hexdigest()
        if sha != row["sha256"]:
            raise RuntimeError(f"local extra file SHA mismatch: {ep}")
        ename = posixpath.basename(row["camera_path"])
        extra_items.append((ep, ename, sha, row.get("mode", "644")))
        extra_bytes_map[ename] = data

    entry_items = []
    with tarfile.open(boot_tar, "r") as tf:
        for m in tf.getmembers():
            if m.isfile():
                data = tf.extractfile(m).read()
                entry_items.append((m.name, data))

    if not check_package_charo_notice(extra_items, assume_yes=args.yes):
        return

    deploy_ok = False
    connected = False

    def deploy(cam):
        nonlocal deploy_ok, connected
        connected = True
        cam.setTerminalEnable(True)
        time.sleep(0.6)
        try:
            cam.dev.readTerminal()
        except Exception:
            pass
        try:
            hash_algo = detect_camera_hash_algo(cam)
            if hash_algo != "sha256":
                print(f"[*] 相機 busybox 未內建 sha256sum，已自動切換為 {hash_algo} 校驗模式", flush=True)
            b64_mode = detect_camera_b64_mode(cam)
            if b64_mode != "busybox":
                print(f"[*] 相機 busybox 未內建 base64，已自動切換為 {b64_mode} 編解碼模式", flush=True)
            hook_bytes = init_hook_bytes
            hook_sha = init_hook_sha
            if hook_bytes is None:
                pre = must_cmd(cam, "/bin/busybox cat /etc/ld.so.preload 2>/dev/null || true", "CHECK_PRELOAD")
                if HOOK_CAMERA_PATH in pre:
                    must_cmd(cam, f'/bin/busybox ls -la "{HOOK_CAMERA_PATH}"', "CHECK_HOOK_FILE")
                elif LEGACY_HOOK_PATH in pre:
                    must_cmd(cam, f'/bin/busybox ls -la "{LEGACY_HOOK_PATH}"', "CHECK_LEGACY_HOOK")
                elif stub_bytes is not None:
                    print("[*] 偵測到全新未改機身，正在讀取相機原廠 /usr/lib/libAK8975.so 並自動產生開機掛勾...", flush=True)
                    hook_bytes, hook_sha = build_hook_from_camera(cam, stub_bytes, hash_algo=hash_algo, b64_mode=b64_mode)
                    print(f"[+] 開機掛勾產生完成 ({len(hook_bytes)} B, sha256={hook_sha[:12]}...)", flush=True)
                else:
                    raise RuntimeError(
                        f"Existing /etc/ld.so.preload + {HOOK_CAMERA_PATH} hook required (or pass --install-hook / --auto-hook-stub)"
                    )

            tgz_bytes, tgz_sha = build_deploy_tgz(boot_sh, boot_tar, hook_bytes, extra_items)

            for idx, step in enumerate(build_backup_steps()):
                must_cmd(cam, step, f"BACKUP_{idx}")

            b64 = base64.b64encode(tgz_bytes).decode("ascii")
            must_cmd(cam, "/bin/busybox umount /tmp_bt/v3c_up 2>/dev/null || true", "UMOUNT_UP_DIR")
            must_cmd(cam, "/bin/busybox rm -rf /tmp_bt/v3c_up 2>/dev/null || true", "CLEAN_UP_DIR")
            _, mk_rc = run_cmd(cam, "/bin/busybox mkdir -p /tmp_bt/v3c_up 2>/dev/null")
            if mk_rc != 0:
                must_cmd(cam, "/bin/busybox mount -t tmpfs -o size=4096k tmpfs /tmp_bt", "MOUNT_TMP_BT")
                must_cmd(cam, "/bin/busybox mkdir -p /tmp_bt/v3c_up", "PREP_UP_DIR")
            must_cmd(cam, "/bin/busybox mount -t tmpfs -o size=2048k tmpfs /tmp_bt/v3c_up", "MOUNT_UP_TMPFS")
            must_cmd(cam, ": > /tmp_bt/v3c_up/up.b64", "PREP_UP_FILE")
            # OpenSSL 1.0.x `enc -d -base64` expects <=64-char lines; busybox base64 accepts 120-char lines.
            chunk_step = 120 if b64_mode == "busybox" else 64
            chunks = build_upload_chunks(b64, step=chunk_step)
            total = len(chunks)
            print(f"Uploading {len(tgz_bytes)} B ({total} chunks)...", flush=True)
            upload_b64_chunks(cam, chunks)
            lines_n, bytes_n = verify_uploaded_b64_lines(cam, total)
            print(f"Verified up.b64: {lines_n} lines / {bytes_n} B", flush=True)

            b64_dec_cmd = (
                "/bin/busybox base64 -d /tmp_bt/v3c_up/up.b64 > /tmp_bt/v3c_up/up.tgz"
                if b64_mode == "busybox"
                else "/usr/bin/openssl enc -d -base64 -in /tmp_bt/v3c_up/up.b64 -out /tmp_bt/v3c_up/up.tgz 2>/dev/null"
            )
            must_cmd(cam, b64_dec_cmd, "B64_DECODE", timeout=20.0)
            must_cmd(cam, "/bin/busybox rm -f /tmp_bt/v3c_up/up.b64", "CLEAN_UP_B64")
            verify_camera_file(cam, "/tmp_bt/v3c_up/up.tgz", tgz_bytes, "VERIFY_TGZ", hash_algo)
            must_cmd(
                cam,
                "/bin/busybox tar -xzf /tmp_bt/v3c_up/up.tgz -C /tmp_bt/v3c_up",
                "UNTAR_UP",
                timeout=20.0,
            )
            must_cmd(cam, "/bin/busybox rm -f /tmp_bt/v3c_up/up.tgz", "CLEAN_UP_TGZ")
            must_cmd(cam, "/bin/sh -n /tmp_bt/v3c_up/new-autoexec.sh", "SYNTAX_AUTOEXEC")

            verify_targets = [
                ("/tmp_bt/v3c_up/new-autoexec.sh", boot_sh_bytes),
                ("/tmp_bt/v3c_up/new-boot.tar", boot_tar_bytes),
            ]
            install_hook = hook_bytes is not None
            if install_hook:
                verify_targets.append(("/tmp_bt/v3c_up/new-autoexec.so", hook_bytes))
            for _, ename, _, _ in extra_items:
                verify_targets.append((f"/tmp_bt/v3c_up/{ename}", extra_bytes_map[ename]))
            for vpath, vbytes in verify_targets:
                verify_camera_file(cam, vpath, vbytes, f"VERIFY_{posixpath.basename(vpath)}", hash_algo)

            tar_x_flag = "xzf" if boot_tar_bytes.startswith(b"\x1f\x8b") else "xf"
            for name, exp_bytes in entry_items:
                must_cmd(cam, "/bin/busybox mkdir -p /tmp_bt/v3c_up/ex", f"MKEX_{name}")
                must_cmd(
                    cam,
                    f"/bin/busybox tar -{tar_x_flag} /tmp_bt/v3c_up/new-boot.tar -C /tmp_bt/v3c_up/ex {name}",
                    f"EX_{name}",
                )
                verify_camera_file(cam, f"/tmp_bt/v3c_up/ex/{name}", exp_bytes, f"VERIFY_{name}", hash_algo)
                must_cmd(cam, "/bin/busybox rm -rf /tmp_bt/v3c_up/ex", f"RMEX_{name}")

            extra_entries = [(ename, emode) for _, ename, _, emode in extra_items]
            for idx, step in enumerate(build_install_steps(install_hook, extra_entries)):
                must_cmd(cam, step, f"INSTALL_{idx}", timeout=20.0)

            final_targets = [
                ("/tmp/autoexec.sh", boot_sh_bytes),
                ("/tmp/a6400-film-boot-v15.tar", boot_tar_bytes),
            ]
            if install_hook:
                final_targets.append((HOOK_CAMERA_PATH, hook_bytes))
            for _, ename, _, _ in extra_items:
                final_targets.append((f"/tmp/{ename}", extra_bytes_map[ename]))
            for fpath, fbytes in final_targets:
                verify_camera_file(cam, fpath, fbytes, f"FINAL_{posixpath.basename(fpath)}", hash_algo)

            if install_hook:
                preload_out = must_cmd(cam, "/bin/busybox cat /etc/ld.so.preload", "FINAL_PRELOAD")
                if HOOK_CAMERA_PATH not in preload_out:
                    raise RuntimeError(f"/etc/ld.so.preload missing {HOOK_CAMERA_PATH}")

            must_cmd(cam, "/bin/busybox umount /tmp_bt/v3c_up 2>/dev/null || true", "UMOUNT_UP_STAGE")
            must_cmd(cam, "/bin/busybox rm -rf /tmp_bt/v3c_up && /bin/busybox sync", "CLEAN_UP_STAGE")
            print(f"V3_CLITE_DEPLOY_OK package={manifest['package_id']} version=V4.1-SLEM-hany", flush=True)
            deploy_ok = True
        finally:
            try:
                cam.setTerminalEnable(False)
            except Exception:
                pass

    try:
        senserShellCommand(driverName="libusb", complete=deploy)
    except Exception as exc:
        if connected:
            print(f"\n[!] 已連線相機，但部署過程發生錯誤：{exc}\n", file=sys.stderr)
            sys.exit(2)
        else:
            print(format_usb_help(exc, driver_hint=detect_windows_usb_driver_hint()), file=sys.stderr)
            sys.exit(1)

    if not deploy_ok:
        print(format_usb_help(driver_hint=detect_windows_usb_driver_hint()), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
