#!/usr/bin/env python3
"""檢查 Git 已追蹤的公開檔案，不讀取憑證或連接任何服務。"""
from pathlib import Path
import hashlib
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
# Only this user-approved, resized, metadata-free documentation image is public.
# Other photographs/media remain forbidden; a replacement needs a new review.
APPROVED_IMAGES = {
    'docs/images/a6400-extra-menu-camera.jpg':
        'd689dc9801bdefaf1c4711665278e28baba223e9b700a2e6e04dbcf94c91b04d',
    'docs/images/a6400-extra-menu-v3-clite.jpg':
        'd275d4e16da33c06a73d9a488f0e4a7125397287f95d577254370da594606116',
    'docs/images/a6400-extra-menu-v32-charo.jpg':
        '4b0fcf70b09ab1370b87f614685033b06b03b483c46d44aff0e9e97e599f8187',
    'docs/images/a6400-extra-menu-v0-early.jpg':
        '5eb2ea263806f734d6778e649011617d8f75368303911441b360bc5d92aca1ee',
    'docs/images/community-a7r4a-film-menu.jpg':
        'cd2925fcb4e14945df6ae9b0b6e97c13c6ff7df218fb3016ad74a8f4415c0e3d',
    'docs/images/a6100-extra-menu-camera.jpg':
        '13968f668b8a32f8ecc5bc03321155a858d4e3bd90be548b143265e07f8b58db',
    'docs/images/a6600-extra-menu-camera.jpg':
        'd482ea9be4ae9b6049651cc4a3e92bb37166be7419de3b07785faf30c2eb993f',
    'docs/images/a6400-extra-menu-v39-sslf.jpg':
        '54af54b01c15b0eb6c208102abc001919b6d6a028477fa146b942599ca118428',
    'docs/images/sponsor-wechat.jpg':
        '0f069a8b81c054daab52bce48ac5fde15b90fd42828f5beba3540019c66bd008',
    'docs/images/sample-bougainvillea-sky.jpg':
        '5c72bc9f173b3968b582ba05566f32f080536a7328e71d770ac6256dca7b19a0',
    'docs/images/sample-clouds-sunlight.jpg':
        '291da6ce909c0e9603a9afe049311f416b9c020ea2352a01eac5bfd50cb3f806',
}
FORBIDDEN_SUFFIXES = {
    '.elf', '.so', '.dylib', '.o', '.a', '.bin', '.dump', '.req',
    '.tar', '.gz', '.zip', '.img', '.dat', '.log', '.sock', '.pem', '.key',
    '.jpg', '.jpeg', '.png', '.mp3', '.wma', '.pcm', '.cube', '.pyc',
}
PATTERNS = [
    (re.compile(rb'/(?:Users|home)/[^\s/]+/'), '私人絕對路徑'),
    (re.compile(rb'gh[pousr]_[A-Za-z0-9]{20,}'), 'GitHub token'),
    (re.compile(rb'github_pat_[A-Za-z0-9_]{20,}'), 'GitHub token'),
    (re.compile(rb'-----BEGIN [A-Z ]*PRIVATE KEY-----'), '私鑰'),
]


def main():
    tracked = subprocess.run(['git', 'ls-files', '-z'], cwd=ROOT,
                             check=True, capture_output=True).stdout
    names = [n.decode('utf-8') for n in tracked.split(b'\0') if n]
    if not names:
        print('尚未有已追蹤檔案；請先建立待發布的 Git 索引。')
        return 1
    errors = []
    images = 0
    for name in names:
        p = ROOT / name
        if p.is_symlink() or not p.is_file():
            errors.append((name, '不是一般檔案'))
            continue
        if name in APPROVED_IMAGES:
            data = p.read_bytes()
            if (len(data) > 300000 or not data.startswith(b'\xff\xd8\xff') or
                    not data.endswith(b'\xff\xd9') or
                    hashlib.sha256(data).hexdigest() != APPROVED_IMAGES[name]):
                errors.append((name, '展示照與已核准的版本不符'))
            images += 1
            continue
        if p.suffix.lower() in FORBIDDEN_SUFFIXES or p.name.startswith('.env'):
            errors.append((name, '不應公開的檔案類型'))
        data = p.read_bytes()
        if b'\0' in data or len(data) > 100000:
            errors.append((name, '二進位內容或超出公開原始碼大小限制'))
        try:
            data.decode('utf-8')
        except UnicodeDecodeError:
            errors.append((name, '不是 UTF-8 文字'))
        for pattern, reason in PATTERNS:
            if pattern.search(data):
                errors.append((name, reason))
    for name, reason in errors:
        print(f'{name}: {reason}')
    if errors:
        return 1
    print(f'公開檔案檢查通過：{len(names)-images} 個文字檔案、{images} 張已核准展示照。')
    print('這是有限範圍的自動檢查，不取代來源及授權審閱。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
