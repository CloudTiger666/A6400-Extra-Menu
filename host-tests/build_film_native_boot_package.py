#!/usr/bin/env python3
"""Build only: a pinned, read-only native-SD cold-start package.

This verifies inputs and archive structure, not an ELF's behavior or ABI.
No connection, installation, SD mounting, or execution occurs here.
"""
import gzip
import io
import json
from pathlib import Path
import re
import sys
import tarfile

import build_film_runtime_package as base

BUNDLE_PATH = 'A6400/RUNTIME/BOOT0014.TAR'
MAX_BUNDLE = 262144
TEMPLATE_NAMES = ('bootstrap-native-sd.sh', 'prepare.sh', 'runtime-start.sh')


def bundle_bytes(entries):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w', format=tarfile.USTAR_FORMAT) as archive:
        for name, (data, mode) in sorted(entries.items()):
            base.require(re.fullmatch(r'[a-z0-9._-]+', name) is not None,
                         'archive entry must be a fixed basename')
            member = tarfile.TarInfo(name)
            member.size = len(data)
            member.mode = mode
            member.uid = member.gid = member.mtime = 0
            member.uname = member.gname = ''
            member.type = tarfile.REGTYPE
            archive.addfile(member, io.BytesIO(data))
    result = gzip.compress(stream.getvalue(), compresslevel=9, mtime=0)
    base.require(len(result) <= MAX_BUNDLE, 'bundle exceeds RAM staging budget')
    return result


def build(spec_path, out):
    spec = json.loads(spec_path.read_text())
    artifacts, pins, capture_pins = base.load_artifacts(spec, spec_path.parent)
    base.require(not out.exists() and not out.is_symlink(), 'output already exists')
    reader = Path(spec['boot_reader_file'])
    if not reader.is_absolute():
        reader = spec_path.parent / reader
    base.require(reader.is_file() and not reader.is_symlink(),
                 'boot reader must be a regular file')
    reader_data = reader.read_bytes()
    reader_sha = spec.get('boot_reader_sha256', '')
    base.require(re.fullmatch('[0-9a-f]{64}', reader_sha) is not None,
                 'invalid boot reader SHA-256')
    base.require(base.digest(reader_data) == reader_sha, 'boot reader SHA-256 mismatch')
    base.require(0 < len(reader_data) <= 65536, 'boot reader exceeds size budget')
    template_pins = {name: base.digest((base.TEMPLATES / name).read_bytes())
                     for name in TEMPLATE_NAMES}
    identity = {'schema': 3, 'bundle_path': BUNDLE_PATH,
                'artifacts': {k: v['sha256'] for k, v in sorted(artifacts.items())},
                'module_pins': pins, 'capture_pins': capture_pins,
                'reader_sha256': reader_sha, 'templates': template_pins}
    package_id = base.digest(json.dumps(identity, sort_keys=True).encode())[:16]
    rows = '\n'.join(f"{v['dest']} {v['dest']} {v['sha256']} {v['mode']:o}"
                     for v in artifacts.values())
    values = {'PACKAGE_ID': package_id, 'ARTIFACT_ROWS': rows,
              'GUARD_SHA': artifacts['guard']['sha256'],
              'CHECK_SHA': artifacts['check']['sha256'],
              'INSTALL_SHA': artifacts['install']['sha256']}
    runtime = base.render('runtime-start.sh', values)
    stage_rows = rows + '\n' + f'runtime-start.sh a6400-film-runtime-start.sh {base.digest(runtime)} 755'
    prepare = base.render('prepare.sh', dict(values, ROWS=stage_rows))
    old_root = b'ROOT=/mnt/A6400/RUNTIME\n'
    base.require(prepare.count(old_root) == 1, 'unexpected prepare source root')
    prepare = prepare.replace(old_root, b'ROOT=/tmp_bt/a6400-film-boot-stage\n')
    entries = {v['dest']: (v['data'], v['mode']) for v in artifacts.values()}
    entries.update({'runtime-start.sh': (runtime, 0o755), 'prepare.sh': (prepare, 0o755)})
    bundle = bundle_bytes(entries)
    bootstrap = base.render('bootstrap-native-sd.sh', dict(
        values, READER_SHA=reader_sha, BUNDLE_BYTES=len(bundle),
        BUNDLE_SHA=base.digest(bundle), PREPARE_SHA=base.digest(prepare),
        RUNTIME_SHA=base.digest(runtime)))
    files = {'bootstrap-native-sd.sh': (bootstrap, 0o755),
             'a6400-film-boot-reader.elf': (reader_data, 0o755),
             BUNDLE_PATH: (bundle, 0o644)}
    manifest = {
        'schema': 3, 'package_id': package_id,
        'deployment_status': 'BUILT_ONLY_NOT_INSTALLED',
        'native_sd_read': True, 'raw_sd_mount': False, 'default_enabled': False,
        'capture_persistent': True, 'natural_touch_release_required': True,
        'module_pins': pins, 'capture_pins': capture_pins,
        'template_pins': template_pins,
        'boot_reader_source': str(reader.resolve()),
        'boot_reader_sha256': reader_sha,
        'bundle_path': BUNDLE_PATH, 'bundle_max_bytes': MAX_BUNDLE,
        'bundle_entries': [{'path': name, 'sha256': base.digest(data),
                            'bytes': len(data), 'mode': f'{mode:o}'}
                           for name, (data, mode) in sorted(entries.items())],
        'files': [{'path': name, 'sha256': base.digest(data),
                   'bytes': len(data), 'mode': f'{mode:o}'}
                  for name, (data, mode) in sorted(files.items())],
        'disable': {
            'persistent_root_marker': '/tmp/disable_hook',
            'ram_markers': ['/tmp_bt/a6400-film.DISABLE', '/tmp_bt/a6400-film-capture.stop'],
            'sd_next_boot': 'Remove or rename A6400/RUNTIME/BOOT0014.TAR while the camera is powered off or in exclusive host mass-storage mode.',
            'unsupported_sd_markers': ['/disable.txt', '/A6400/DISABLE'],
            'note': 'This native bootstrap does not probe SD disable markers. Removing the bundle prevents the next cold start; it does not stop a running photo operation.'},
        'verification_scope': 'Input hashes and deterministic regular-file USTAR only. ELF ABI, transport, and physical cold boot require separate verification.',
        'artifacts': [{k: v for k, v in item.items() if k != 'data'} | {'role': role}
                      for role, item in artifacts.items()]}
    out.mkdir()
    for name, (data, mode) in files.items():
        path = out / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    print(f'FILM_NATIVE_BOOT_PACKAGE_BUILT out={out} package={package_id} bytes={len(bundle)}')


def main():
    if len(sys.argv) != 3:
        print('usage: build_film_native_boot_package.py ARTIFACTS.json NEW_OUTPUT_DIRECTORY',
              file=sys.stderr)
        return 2
    try:
        build(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).absolute())
    except (ValueError, KeyError, TypeError, OSError) as error:
        print('FILM_NATIVE_BOOT_PACKAGE_FAIL ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
