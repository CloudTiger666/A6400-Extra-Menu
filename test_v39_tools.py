"""Regression tests for V3.9 SSLF tooling, hook builder, deployer, bootstrap, and JPEG worker."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent / 'tools'))
sys.path.insert(0, str(Path(__file__).resolve().parent / 'host-tests'))
import build_autoexec_hook as hook_builder
import pmca_deploy_v3_clite as deployer


class FakeDev:
    def __init__(self, script):
        self.script = list(script)
        self.current = []
        self.writes = []

    def writeTerminal(self, data):
        self.writes.append(data)
        if data != b'\x03\n' and self.script:
            self.current = list(self.script.pop(0))

    def readTerminal(self):
        return self.current.pop(0) if self.current else b''


class FakeCam:
    def __init__(self, script):
        self.dev = FakeDev(script)


class V39ToolsTests(unittest.TestCase):
    def test_autoexec_hook_stub_contains_tmpfs_fallback_and_err_log(self):
        asm = hook_builder.ASM_TEMPLATE
        self.assertIn('str_tmpfs', asm)
        self.assertIn('str_tmp_bt', asm)
        self.assertIn('size=2048k', asm)
        self.assertIn('/tmp/a6400-hook-err.log', asm)
        self.assertIn('mov r7, #21', asm)
        self.assertIn('cmn r0, #17', asm)
        self.assertIn('mov r1, #512', asm)
        self.assertIn('add r1, r1, #65', asm)
        self.assertNotIn('mov r1, #577', asm)

    def test_patch_libak8975_rejects_rodata_overrun_and_injects_dt_init(self):
        rodata_off = 0xB88
        dyn_off = 0xC00
        shstr_off = 0xC80
        shoff = 0xD00
        shstr = b'\x00.text\x00.rodata\x00.dynamic\x00.shstrtab\x00'
        buf = bytearray(shoff + 5 * 40)
        buf[0:4] = b'\x7fELF'
        struct.pack_into('<I', buf, 0x20, shoff)
        struct.pack_into('<HHH', buf, 0x2E, 40, 5, 4)
        struct.pack_into('<IIIIII', buf, shoff + 1 * 40, 1, 1, 6, 0x988, 0x988, 0x200)
        struct.pack_into('<IIIIII', buf, shoff + 2 * 40, 7, 1, 2, rodata_off, rodata_off, 0x20)
        struct.pack_into('<IIIIII', buf, shoff + 3 * 40, 15, 6, 3, dyn_off, dyn_off, 0x40)
        struct.pack_into('<IIIIII', buf, shoff + 4 * 40, 24, 3, 0, 0, shstr_off, len(shstr))
        buf[shstr_off:shstr_off + len(shstr)] = shstr

        payload = b'\x00\xf0\x20\xe3' * 16
        patched = hook_builder.patch_libak8975(bytes(buf), payload)
        self.assertEqual(patched[0x988:0x988 + len(payload)], payload)
        dt_tag, dt_val = struct.unpack_from('<II', patched, dyn_off)
        self.assertEqual((dt_tag, dt_val), (12, 0x988))

        with self.assertRaises(ValueError):
            hook_builder.patch_libak8975(bytes(buf), b'A' * 0x200)

    def test_patch_libak8975_supports_dynamic_text_offset(self):
        text_off = 0x1040
        rodata_off = 0x1340
        dyn_off = 0x1380
        shstr_off = 0x1400
        shoff = 0x1480
        shstr = b'\x00.text\x00.rodata\x00.dynamic\x00.shstrtab\x00'
        buf = bytearray(shoff + 5 * 40)
        buf[0:4] = b'\x7fELF'
        struct.pack_into('<I', buf, 0x20, shoff)
        struct.pack_into('<HHH', buf, 0x2E, 40, 5, 4)
        struct.pack_into('<IIIIII', buf, shoff + 1 * 40, 1, 1, 6, text_off, text_off, 0x300)
        struct.pack_into('<IIIIII', buf, shoff + 2 * 40, 7, 1, 2, rodata_off, rodata_off, 0x20)
        struct.pack_into('<IIIIII', buf, shoff + 3 * 40, 15, 6, 3, dyn_off, dyn_off, 0x40)
        struct.pack_into('<IIIIII', buf, shoff + 4 * 40, 24, 3, 0, 0, shstr_off, len(shstr))
        buf[shstr_off:shstr_off + len(shstr)] = shstr
        payload = b'\x00\xf0\x20\xe3' * 16
        patched = hook_builder.patch_libak8975(bytes(buf), payload)
        self.assertEqual(patched[text_off:text_off + len(payload)], payload)
        self.assertEqual(struct.unpack_from('<II', patched, dyn_off), (12, text_off))

    def test_deployer_commands_stay_under_180_chars_and_retry_rc_none(self):
        cmds = []
        cmds.extend(deployer.build_backup_steps())
        cmds.extend(deployer.build_install_steps(True, [
            ('a6400-charo-bind.elf', '755'),
            ('a6400-charo-shutter.pcm', '644'),
            ('a6400-charo-connect.pcm', '644'),
            ('a6400-charo-push.pcm', '644'),
        ]))
        cmds.extend(deployer.build_rollback_steps())
        cmds.extend(cmd for _, cmd in deployer.build_diag_commands())
        cmds.extend(deployer.build_upload_chunks('A' * 400))
        self.assertTrue(cmds)
        for c in cmds:
            self.assertLessEqual(len(c), deployer.MAX_SHELL_CMD_LEN, f'Command too long ({len(c)}): {c}')

        install = '\n'.join(deployer.build_install_steps(True, []))
        self.assertIn('/etc/a6400-autoexec.so', install)
        self.assertNotIn('/tmp/a6400-autoexec.so', install)
        self.assertNotIn('\\etc', install)
        self.assertNotIn('\\tmp', install)
        self.assertNotIn('printf', install)

        cam = FakeCam([[b'> '], [b'__PMCA_RC:0__\n']])
        out, rc = deployer.run_cmd(cam, 'echo ok', timeout=0.05, fast=True, retry_on_none=True, safe_serial=False)
        self.assertEqual(rc, 0, out)
        self.assertNotIn(b'printf', cam.dev.writes[0])
        self.assertIn(b'R=$?;echo;echo __PMCA_RC:${R}__', cam.dev.writes[0])
        self.assertGreaterEqual(len(cam.dev.writes), 3)
        self.assertIn(b'\x03\n', cam.dev.writes)

        msg = deployer.format_usb_help(RuntimeError('No devices found'), platform='win32')
        self.assertIn('不支援 WinUSB', msg)
        self.assertIn('完全關閉並重開 Zadig', msg)
        self.assertNotIn('若失敗改 WinUSB', msg)
        rb = '\n'.join(deployer.build_rollback_steps())
        self.assertIn('/tmp/a6400-charo-connect.pcm', rb)
        self.assertIn('/tmp/a6400-charo-push.pcm', rb)
        self.assertIn('/etc/ld.so.preload.pre-v3c-backup', rb)
        self.assertIn('/tmp/autoexec.sh.pre-v3c-backup', rb)
        self.assertNotIn('mv $B $P', rb)
        self.assertNotIn('/tmp/a6400-autoexec.so', rb)
        watch_cmd = (Path(__file__).resolve().parent / 'tools/usbipd_watch.cmd').read_text()
        self.assertIn('_LINE:Attached=', watch_cmd)
        self.assertIn('_LINE:Not shared=', watch_cmd)
        src_deploy = (Path(__file__).resolve().parent / 'tools/pmca_deploy_v3_clite.py').read_text()
        self.assertNotIn('CLEAN_LEGACY_HOOK', src_deploy)

    def test_deployer_safe_serial_chunking_and_idempotent_upload(self):
        # 1. safe_serial=True splits >48B commands into <=48B (< 0x40=64B USB packet) slices and picks latest __PMCA_RC
        long_cmd = 'echo ' + ('A' * 50)
        cam_chunked = FakeCam([
            [],
            [b'echo __PMCA_RC:99__ noise\n__PMCA_RC:0__\n'],
        ])
        out, rc = deployer.run_cmd(cam_chunked, long_cmd, timeout=0.5, fast=False, safe_serial=True)
        self.assertEqual(rc, 0, out)
        self.assertEqual(len(cam_chunked.dev.writes), 2)
        self.assertTrue(all(len(w) <= 48 for w in cam_chunked.dev.writes))

        # 2. upload_b64_chunks uses unique per-chunk tag (__PMCA_RC_C{idx}:0__), accepts rc=None when wc -l == idx+1,
        #    and safely re-sends once when wc -l == idx
        chunks = deployer.build_upload_chunks('QUJD' * 40, step=120)
        self.assertEqual(len(chunks), 2)
        self.assertTrue(all(c.startswith('echo ') for c in chunks))
        cam_up = FakeCam([
            [b'> '],
            [b'__B64_IGNORE__\n__L__\n1\n__E__\n__PMCA_RC:0__\n'],
            [b'__PMCA_RC_C0:0__\n'],  # stale C0 marker ignored by C1!
            [b'__L__\n1\n__E__\n__PMCA_RC:0__\n'],  # C1 did not land yet (lines=1 == idx)
            [b'__PMCA_RC_C1R:0__\n'],  # C1 safely re-sent once
            [b'__L__\n2\n__E__\n__PMCA_RC:0__\n'],  # now lines=2 == idx+1
        ])
        deployer.upload_b64_chunks(cam_up, chunks, safe_serial=False)

        # 3. upload_b64_chunks fails closed if wc -l still does not match idx+1 after safe retry
        cam_up_bad = FakeCam([
            [b'> '],
            [b'__L__\n0\n__E__\n__PMCA_RC:0__\n'],
            [b'> '],
            [b'__L__\n0\n__E__\n__PMCA_RC:0__\n'],
        ])
        with self.assertRaises(RuntimeError):
            deployer.upload_b64_chunks(cam_up_bad, chunks, safe_serial=False)

        # 4. verify_uploaded_b64_lines parses markers and rejects mismatched line count
        cam_pre = FakeCam([[b'__B64_L__\n2\n__B64_C__\n162\n__B64_E__\n__PMCA_RC:0__\n']])
        self.assertEqual(deployer.verify_uploaded_b64_lines(cam_pre, 2, safe_serial=False), (2, 162))
        cam_pre_bad = FakeCam([[b'__B64_L__\n1\n__B64_C__\n120\n__B64_E__\n__PMCA_RC:0__\n']])
        with self.assertRaises(RuntimeError):
            deployer.verify_uploaded_b64_lines(cam_pre_bad, 2, safe_serial=False)

        # 5. check_package_charo_notice passes silently when shutter.pcm is present, and prompts/skips when absent
        import io
        from contextlib import redirect_stdout
        from unittest import mock

        self.assertTrue(deployer.check_package_charo_notice([(None, 'a6400-charo-shutter.pcm', '0'*64, '644')]))
        with redirect_stdout(io.StringIO()):
            self.assertTrue(deployer.check_package_charo_notice([], assume_yes=True, interactive=True))
            with mock.patch('builtins.input', return_value='n'):
                self.assertFalse(deployer.check_package_charo_notice([], assume_yes=False, interactive=True))
            with mock.patch('builtins.input', return_value=''):
                self.assertTrue(deployer.check_package_charo_notice([], assume_yes=False, interactive=True))

    def test_encode_sony_sds_adpcm_encodes_and_verifies_slot_limits(self):
        import math
        import wave
        import encode_sony_sds_adpcm as sds

        sr = sds.SAMPLE_RATE
        # 0.25s 440 Hz sine wave in 24kHz mono WAV
        samples = [int(18000 * math.sin(2.0 * math.pi * 440.0 * i / sr)) for i in range(sr // 4)]
        with tempfile.TemporaryDirectory() as td:
            wav_p = Path(td) / 'tone.wav'
            with wave.open(str(wav_p), 'wb') as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)
                wf.setframerate(sr)
                wf.writeframes(struct.pack(f'<{len(samples)}h', *samples))
            dec = sds.decode_to_24k_mono_pcm16(wav_p)
            self.assertEqual(len(dec), len(samples))

        encoded, enc_info = sds.encode_samples_to_sds(samples, 930)
        self.assertEqual(len(encoded), 29760)
        self.assertGreaterEqual(enc_info['snr_db'], 35.0)
        ver = sds.verify_sds_bytes(encoded)
        self.assertEqual(ver['bad_hdr'], 0)
        self.assertEqual(ver['frames'], 930)

        too_long = [1000] * (930 * 31 + 100)
        with self.assertRaises(ValueError):
            sds.encode_samples_to_sds(too_long, 930, trim_lead=False, truncate=False)

    def test_win_usb_driver_uses_libusb_win32_and_parses_pnputil(self):
        import win_usb_driver as windrv

        cmd = windrv.build_wdi_simple_cmd('wdi-simple.exe', 0x054C, 0x0336, 'Sony Camera Service', dest_dir='C:\\tmp')
        self.assertEqual(cmd, [
            'wdi-simple.exe', '--vid', '0x054C', '--pid', '0x0336', '--type', '1',
            '--timeout', '60000', '--name', 'Sony Camera Service', '--dest', 'C:\\tmp',
        ])
        self.assertEqual(
            windrv.parse_pnputil_connected_sony_pids('USB\\VID_054C&PID_0D12\\1234\nUSB\\VID_054C&PID_0336\\5678'),
            {0x0D12, 0x0336},
        )

        sample_pnputil = """
Microsoft PnP Utility

Published Name :            oem14.inf
Original Name :             sony_camera_a6400_mass_storage_(libusb-win32).inf
Provider Name :             libusb-win32
Class Name :                libusb-win32 devices

Published Name :            oem22.inf
Original Name :             nv_dispi.inf
Provider Name :             NVIDIA
Class Name :                Display adapters

Published Name :            oem31.inf
Original Name :             sony_camera_service_mode_(libusb-win32).inf
Provider Name :             libusb-win32
Class Name :                libusb-win32 devices
"""
        self.assertEqual(windrv.parse_pnputil_sony_libusb_infs(sample_pnputil), ['oem14.inf', 'oem31.inf'])
        self.assertEqual(windrv.parse_pnputil_sony_libusb_infs(sample_pnputil, role='mass'), ['oem14.inf'])
        self.assertEqual(windrv.parse_pnputil_sony_libusb_infs(sample_pnputil, role='service'), ['oem31.inf'])

        from unittest import mock
        with mock.patch.object(windrv, 'is_libusb_win32_ready', return_value=True), \
             mock.patch.object(windrv.subprocess, 'run') as m_run:
            self.assertTrue(windrv.run_wdi_install('wdi-simple.exe', 0x0CA8, 'Sony A6400'))
            m_run.assert_not_called()

    def test_auto_hook_from_camera_pulls_verifies_sha_and_patches(self):
        import base64
        rodata_off = 0xB88
        dyn_off = 0xC00
        shstr_off = 0xC80
        shoff = 0xD00
        shstr = b'\x00.text\x00.rodata\x00.dynamic\x00.shstrtab\x00'
        buf = bytearray(shoff + 5 * 40)
        buf[0:4] = b'\x7fELF'
        struct.pack_into('<I', buf, 0x20, shoff)
        struct.pack_into('<HHH', buf, 0x2E, 40, 5, 4)
        struct.pack_into('<IIIIII', buf, shoff + 1 * 40, 1, 1, 6, 0x988, 0x988, 0x200)
        struct.pack_into('<IIIIII', buf, shoff + 2 * 40, 7, 1, 2, rodata_off, rodata_off, 0x20)
        struct.pack_into('<IIIIII', buf, shoff + 3 * 40, 15, 6, 3, dyn_off, dyn_off, 0x40)
        struct.pack_into('<IIIIII', buf, shoff + 4 * 40, 24, 3, 0, 0, shstr_off, len(shstr))
        buf[shstr_off:shstr_off + len(shstr)] = shstr

        raw_so = bytes(buf)
        raw_sha = hashlib.sha256(raw_so).hexdigest()
        c0 = raw_so[:3072]
        c1 = raw_so[3072:]
        cam = FakeCam([
            [f'__SZ__\n{len(raw_so)}\n__SH__\n{raw_sha}  /usr/lib/libAK8975.so\n__PMCA_RC:0__\n'.encode()],
            [f'__FB__\n{base64.b64encode(c0).decode()}\n__FE__\n__PMCA_RC:0__\n'.encode()],
            [f'__FB__\n{base64.b64encode(c1).decode()}\n__FE__\n__PMCA_RC:0__\n'.encode()],
        ])
        stub = b'\x00\xf0\x20\xe3' * 16
        patched, psha = deployer.build_hook_from_camera(cam, stub)
        self.assertEqual(psha, hashlib.sha256(patched).hexdigest())
        self.assertEqual(patched[0x988:0x988 + len(stub)], stub)
        self.assertEqual(struct.unpack_from('<II', patched, dyn_off), (12, 0x988))

    def test_win_oneclick_entry_selects_tc_or_sc_package(self):
        import io
        from contextlib import redirect_stdout
        from unittest import mock
        import win_oneclick_entry as entry

        self.assertEqual(entry.parse_lang_arg(['--lang', 'sc', '--diag']), ('sc', ['--diag']))
        self.assertEqual(entry.parse_lang_arg(['--lang=tc']), ('tc', []))

        with tempfile.TemporaryDirectory() as td:
            r = Path(td)
            (r / 'local-boot-package-tc').mkdir()
            (r / 'local-boot-package-sc').mkdir()
            self.assertEqual(entry.select_package_dir(r, lang='sc').name, 'local-boot-package-sc')
            self.assertEqual(entry.select_package_dir(r, lang='tc').name, 'local-boot-package-tc')
            with redirect_stdout(io.StringIO()):
                with mock.patch('builtins.input', return_value='2'):
                    self.assertEqual(entry.select_package_dir(r, interactive=True).name, 'local-boot-package-sc')
                with mock.patch('builtins.input', return_value=''):
                    self.assertEqual(entry.select_package_dir(r, interactive=True).name, 'local-boot-package-tc')

    def test_multi_charo_local_boot_package_optional_slots(self):
        root = Path(__file__).resolve().parent
        sys.path.insert(0, str(root / 'host-tests'))
        import build_film_local_boot_package as pkg_builder
        import build_film_runtime_package as rt_pkg
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            artifacts = []
            data_map = {}
            for role, name in (
                ('ui', 'a6400-film-ui-v3-persist.so'),
                ('menu_job', 'a6400-film-menu-job.elf'),
                ('worker', 'a6400-film-job.elf'),
                ('native', 'a6400-film-native-command.elf'),
                ('ram_job', 'a6400-film-ram-job.sh'),
                ('guard', 'a6400-ui-load-guard.elf'),
                ('check', 'a6400-check-persist.req'),
                ('install', 'a6400-install-persist.req'),
                ('remove', 'a6400-remove-persist.req'),
                ('ui_stop', 'a6400-film-ui-stop.elf'),
                ('capture_daemon', 'a6400-film-capture-daemon.elf'),
            ):
                p = d / name
                data = b'#!/bin/sh\necho ok\n' if role == 'ram_job' else f'dummy-{role}'.encode()
                p.write_bytes(data)
                data_map[role] = data
                artifacts.append({'role': role, 'file': str(p), 'dest': name, 'sha256': hashlib.sha256(data).hexdigest()})
            menu_pins = d / 'menu_pins.json'
            menu_pins.write_text(json.dumps({
                'path': '/tmp_bt/a6400-film-ui-v3-persist.so',
                'sha256': hashlib.sha256(data_map['ui']).hexdigest(),
                'bytes': len(data_map['ui']),
                'fnv': rt_pkg.fnv(data_map['ui']),
                'lifetime_ms': 0,
                'build_defines': {'FILM_SETTINGS_PERSISTENT': 1},
                'ui_bytes': 1120,
                'ui_rva': '0x15544',
            }))
            cap_pins = d / 'cap_pins.json'
            cap_pins.write_text(json.dumps({
                'persistent': True,
                'runtime': '/tmp_bt/a6400-film-capture-daemon.elf',
                'arguments': ['watch'],
                'daemon_sha256': hashlib.sha256(data_map['capture_daemon']).hexdigest(),
                'daemon_bytes': len(data_map['capture_daemon']),
                'script': {
                    'runtime': '/tmp_bt/a6400-film-ram-job.sh',
                    'sha256': hashlib.sha256(data_map['ram_job']).hexdigest(),
                    'bytes': len(data_map['ram_job']),
                    'fnv': rt_pkg.fnv(data_map['ram_job']),
                },
                'settings': {
                    'runtime': '/tmp_bt/a6400-film-menu-job.elf',
                    'sha256': hashlib.sha256(data_map['menu_job']).hexdigest(),
                    'bytes': len(data_map['menu_job']),
                    'fnv': rt_pkg.fnv(data_map['menu_job']),
                },
            }))
            spec = d / 'artifacts.json'
            spec.write_text(json.dumps({
                'schema': 1, 'default_enabled': False, 'capture_persistent': True,
                'menu_pins_file': str(menu_pins), 'capture_pins_file': str(cap_pins),
                'artifacts': artifacts,
            }))
            cdir = d / 'charo'
            cdir.mkdir()
            (cdir / 'a6400-charo-bind.elf').write_bytes(b'\x7fELFbind')
            (cdir / 'a6400-charo-shutter.pcm').write_bytes(b'\x01' * 29760)
            (cdir / 'a6400-charo-connect.pcm').write_bytes(b'\x02' * 24768)
            (cdir / 'a6400-charo-push.pcm').write_bytes(b'\x03' * 18560)
            out = d / 'pkg'
            old = os.environ.get('A6400_CHARO_DIR')
            os.environ['A6400_CHARO_DIR'] = str(cdir)
            try:
                pkg_builder.build(spec, out)
            finally:
                if old is None:
                    os.environ.pop('A6400_CHARO_DIR', None)
                else:
                    os.environ['A6400_CHARO_DIR'] = old
            mf = json.loads((out / 'manifest.json').read_text())
            cam_paths = [e['camera_path'] for e in mf['extra_persistent_files']]
            self.assertEqual(cam_paths, [
                '/tmp/a6400-charo-bind.elf',
                '/tmp/a6400-charo-shutter.pcm',
                '/tmp/a6400-charo-connect.pcm',
                '/tmp/a6400-charo-push.pcm',
            ])
            boot_sh = (out / 'bootstrap-local.sh').read_text()
            self.assertIn('/tmp/a6400-charo-connect.pcm', boot_sh)
            self.assertIn('/tmp/a6400-charo-push.pcm', boot_sh)

    def test_bootstrap_local_template_supports_self_mounted_tmpfs_on_ro_dir(self):
        root = Path(__file__).resolve().parent
        tpl = (root / 'host-tests/film-runtime-v2-templates/bootstrap-local.sh').read_text()
        self.assertIn('mount -t tmpfs -o size=2048k tmpfs "$RAM"', tpl)
        self.assertIn('version=V4.2.1-DEXP-hany', tpl)

        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            ram = d / 'ram'
            ram.mkdir()
            ram.chmod(0o555)  # Read-only /tmp_bt until fake busybox mount makes it writable
            self.addCleanup(lambda: ram.chmod(0o755) if ram.exists() else None)
            proc = d / 'proc'
            proc.mkdir()
            (proc / 'mounts').write_text('')
            bb = d / 'bb'
            bb.write_text(
                '#!/usr/bin/env python3\n'
                'import os, sys\n'
                'from pathlib import Path\n'
                'cmd = sys.argv[1]; args = sys.argv[2:]\n'
                'if cmd == "mount":\n'
                '    target = Path(args[-1])\n'
                '    target.chmod(0o755)\n'
                '    (target / "mounted_tmpfs").write_text(" ".join(args))\n'
                '    sys.exit(0)\n'
                'os.execvp(cmd, [cmd] + args)\n'
            )
            bb.chmod(0o755)
            tar_dummy = d / 'boot.tar'
            tar_dummy.write_bytes(b'wrong')
            script = (
                tpl.replace('/bin/busybox', str(bb))
                .replace('/tmp_bt', str(ram))
                .replace('/proc', str(proc))
                .replace('/tmp/a6400-film-boot-v15.tar', str(tar_dummy))
                .replace('@@BUNDLE_BYTES@@', '5')
                .replace('@@PACKAGE_ID@@', 'v39testpkg')
                .replace('@@BUNDLE_SHA@@', '0' * 64)
            )
            sh_path = d / 'boot.sh'
            sh_path.write_text(script)
            # Without ae.lock it must mount tmpfs, log begin, and stop cleanly at missing_constructor_lock
            res = subprocess.run(['sh', str(sh_path)], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(res.returncode, 0)
            self.assertTrue((ram / 'mounted_tmpfs').is_file())
            log = (ram / 'a6400-film-boot.log').read_text()
            self.assertIn('FILM_LOCAL_BOOT_BEGIN package=v39testpkg', log)
            self.assertIn('FILM_LOCAL_BOOT_FAIL stage=missing_constructor_lock', log)

    def test_picojpeg_host_job_downsample2_and_bad_input_rejection(self):
        root = Path(__file__).resolve().parent
        cam = root / 'camera'
        vendor = root / 'vendor'
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            objs = []
            for name, src, lang, extra in (
                ('film_jpeg.o', cam / 'film_jpeg.cpp', 'c++', ['-O2', '-std=c++11', '-fno-exceptions', '-fno-rtti']),
                ('author_film.o', cam / 'author_film.c', 'cc', ['-O3', '-std=c11']),
                ('picojpeg.o', vendor / 'picojpeg.c', 'cc', ['-O2', '-std=c11', '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-but-set-variable']),
                ('jpge.o', vendor / 'jpge.cpp', 'c++', ['-O3', '-std=c++11', '-fno-exceptions', '-fno-rtti']),
            ):
                obj = d / name
                subprocess.run([lang, '-DA6400_FILM_HOST', '-DA6400_FILM_JOB', '-Wall', '-Wextra', '-Werror',
                                '-I', str(cam), '-I', str(vendor), *extra, '-c', str(src), '-o', str(obj)],
                               check=True, capture_output=True, text=True)
                objs.append(str(obj))
            binary = d / 'film_job'
            subprocess.run(['c++', *objs, '-o', str(binary)], check=True, capture_output=True, text=True)
            # Compile tiny helper using vendor/jpge.cpp to emit a synthetic 2404x24 JPEG (triggers downsample2)
            gen_cpp = d / 'gen.cpp'
            gen_cpp.write_text(
                '#include <vector>\n'
                '#include "jpge.h"\n'
                'int main(int argc, char **argv) {\n'
                '  int w = 2404, h = 24;\n'
                '  std::vector<unsigned char> rgb(w * h * 3);\n'
                '  for (int y = 0; y < h; ++y)\n'
                '    for (int x = 0; x < w; ++x) {\n'
                '      int i = (y * w + x) * 3;\n'
                '      rgb[i] = (unsigned char)((x * 3 + y * 5) & 255);\n'
                '      rgb[i+1] = (unsigned char)((x + y * 11) & 255);\n'
                '      rgb[i+2] = (unsigned char)((128 + x - y) & 255);\n'
                '    }\n'
                '  jpge::params p; p.m_quality = 90;\n'
                '  return jpge::compress_image_to_jpeg_file(argv[1], w, h, 3, rgb.data(), p) ? 0 : 1;\n'
                '}\n'
            )
            gen_bin = d / 'gen'
            subprocess.run(['c++', '-O2', '-std=c++11', '-I', str(root / 'vendor'),
                            str(gen_cpp), str(root / 'vendor/jpge.cpp'), '-o', str(gen_bin)],
                           check=True, capture_output=True, text=True)
            src = d / 'wide.jpg'
            subprocess.run([str(gen_bin), str(src)], check=True)
            out = d / 'rendered.jpg'
            res = subprocess.run([str(binary), 'kodak_gold', str(src), str(out), '2', '0', '--hires'],
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertIn('FILM_SOURCE kind=downsample2 source_width=2404 source_height=24 width=1202 height=12', res.stdout)
            self.assertIn('FILM_DONE width=1202 height=12 ', res.stdout)
            raw = out.read_bytes()
            self.assertTrue(raw.startswith(b'\xff\xd8') and raw.endswith(b'\xff\xd9'))

            bad = d / 'bad.jpg'
            bad.write_bytes(b'\xff\xd8\xff\xd9')
            r_bad = subprocess.run([str(binary), 'kodak_gold', str(bad), str(d / 'bad_out.jpg'), '2', '0', '--hires'],
                                   capture_output=True, text=True)
            self.assertNotEqual(r_bad.returncode, 0)
            self.assertFalse((d / 'bad_out.jpg').exists())

    def test_camera_hash_algo_fallback_and_cksum(self):
        import base64
        sample = b'Sony A6100/A6400 checksum test payload 1234567890!'
        crc, n = deployer.posix_cksum(sample)
        self.assertEqual(n, len(sample))
        res = subprocess.run(['cksum'], input=sample, capture_output=True, check=True)
        sys_crc, sys_n = res.stdout.decode().strip().split()[:2]
        self.assertEqual((crc, n), (int(sys_crc), int(sys_n)))

        # Simulate camera without sha256sum, with /usr/bin/openssl dgst -sha256
        cam_ossl = FakeCam([
            [b'sha256sum: applet not found\n__PMCA_RC:127__\n'],
            [f'SHA256(/dev/null)= {deployer.EMPTY_SHA256}\n__PMCA_RC:0__\n'.encode()],
        ])
        self.assertEqual(deployer.detect_camera_hash_algo(cam_ossl), 'openssl_sha256')

        # Simulate camera without sha256sum or openssl, with md5sum
        cam = FakeCam([
            [b'sha256sum: applet not found\n__PMCA_RC:127__\n'],
            [b'ash: /usr/bin/openssl: not found\n__PMCA_RC:127__\n'],
            [b'd41d8cd98f00b204e9800998ecf8427e  /dev/null\n__PMCA_RC:0__\n'],
        ])
        self.assertEqual(deployer.detect_camera_hash_algo(cam), 'md5')

        # Simulate b64 mode fallback to openssl when busybox base64 is missing
        cam_b64 = FakeCam([
            [b'base64: applet not found\n__PMCA_RC:127__\n'],
        ])
        self.assertEqual(deployer.detect_camera_b64_mode(cam_b64), 'openssl')

        # Simulate pulling file with openssl_sha256 and openssl base64 fallback (including OpenSSL stderr warning noise)
        sha_hex = hashlib.sha256(sample).hexdigest()
        b64_s = base64.b64encode(sample).decode('ascii')
        cam_pull_ossl = FakeCam([
            [f'__SZ__\n{len(sample)}\n__SH__\nWARNING: can\'t open config file: /usr/local/ssl/openssl.cnf\nSHA256(/usr/lib/libAK8975.so)= {sha_hex}\n__PMCA_RC:0__\n'.encode()],
            [f'echo __FB__; dd ...; echo __FE__\n__FB__\nWARNING: can\'t open config file: /usr/local/ssl/openssl.cnf\n{b64_s}\n__FE__\n__PMCA_RC:0__\n'.encode()],
        ])
        got_ossl = deployer.read_camera_file_b64(
            cam_pull_ossl, '/usr/lib/libAK8975.so', chunk_bytes=1024, hash_algo='openssl_sha256', b64_mode='openssl'
        )
        self.assertEqual(got_ossl, sample)

        # Simulate pulling file with md5 fallback
        md5_hex = hashlib.md5(sample).hexdigest()
        b64_s = base64.b64encode(sample).decode('ascii')
        cam_pull = FakeCam([
            [f'__SZ__\n{len(sample)}\n__SH__\n{md5_hex}  /usr/lib/libAK8975.so\n__PMCA_RC:0__\n'.encode()],
            [f'__FB__\n{b64_s}\n__FE__\n__PMCA_RC:0__\n'.encode()],
        ])
        got = deployer.read_camera_file_b64(cam_pull, '/usr/lib/libAK8975.so', chunk_bytes=1024, hash_algo='md5')
        self.assertEqual(got, sample)

        # Simulate size-only fallback verify
        cam_sz = FakeCam([
            [f'__SZ__\n{len(sample)}\n__SE__\n__PMCA_RC:0__\n'.encode()],
        ])
        deployer.verify_camera_file(cam_sz, '/tmp/test.bin', sample, 'VERIFY_TEST', hash_algo='size')

    def test_wsl_offline_deployer_scripts(self):
        root = Path(__file__).resolve().parent
        ps1 = root / 'tools' / 'wsl_deploy.ps1'
        raw = ps1.read_bytes()
        self.assertTrue(raw.startswith(b'\xef\xbb\xbf'), 'wsl_deploy.ps1 must have UTF-8 BOM for PowerShell 5.1')
        txt = raw.decode('utf-8-sig')
        self.assertIn('dism.exe', txt)
        self.assertIn('wsl_update_x64.msi', txt)
        self.assertIn('usbipd-win.msi', txt)
        self.assertIn('a6400-alpine-wsl.tar.gz', txt)
        self.assertIn('054c:(0ca8|0d12|0336)', txt)
        self.assertNotIn('wdi-simple', txt)

        sh = root / 'tools' / 'build_alpine_wsl_rootfs.sh'
        res = subprocess.run(['bash', '-n', str(sh)], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn('ALPINE_WSL_SMOKE_OK', sh.read_text())

        cmd = root / 'tools' / 'wsl_deploy.cmd'
        cmd_raw = cmd.read_bytes()
        self.assertFalse(cmd_raw.startswith(b'\xef\xbb\xbf'), 'wsl_deploy.cmd must NOT have UTF-8 BOM for cmd.exe')
        self.assertTrue(cmd_raw.startswith(b'@echo off\r\n'), 'wsl_deploy.cmd must start with @echo off CRLF')
        cmd_txt = cmd_raw.decode('ascii')  # Must be 100% pure ASCII so CP950/CP936 cmd.exe never splits UTF-8 CJK bytes
        for needle in (
            '[Check 1/6]',
            '[Check 6/6]',
            'fltmc',
            'hypervisorlaunchtype',
            'vmcompute',
            '/LimitAccess',
            'wsl.2.4.13.0.x64.msi',
            'usbipd-win.msi',
            'PersistedGuid',
            'chr(92)',
        ):
            self.assertIn(needle, cmd_txt)

        watch = root / 'tools' / 'usbipd_watch.cmd'
        watch_raw = watch.read_bytes()
        self.assertFalse(watch_raw.startswith(b'\xef\xbb\xbf'))
        self.assertTrue(watch_raw.startswith(b'@echo off\r\n'))
        self.assertIn('054c:0336', watch_raw.decode('ascii'))


if __name__ == '__main__':
    unittest.main(verbosity=2)
