"""Host checks for the TEST album filename index. No camera, no SD mount."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent


class FilmAlbumIndexTests(unittest.TestCase):
    def test_filter_sort_cap_and_browse_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'film_album_index_test'
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'camera'),
                str(ROOT / 'camera/film_album_index.c'),
                str(ROOT / 'camera/test_film_album_index.c'),
                '-o', str(binary),
            ], check=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, cwd=tmp)
        self.assertIn('FILM_ALBUM_INDEX_OK', result.stdout)

    def test_ui_browse_is_not_clamped_to_sixteen(self):
        text = (ROOT / 'camera/native_film_ui_trial3.c').read_text(encoding='utf-8')
        self.assertIn('film_album_browse_count', text)
        self.assertNotIn('if(limit>A6400_ALBUM_SLOTS)limit=A6400_ALBUM_SLOTS;', text)
        self.assertNotIn('if(cnt>A6400_ALBUM_SLOTS)cnt=A6400_ALBUM_SLOTS;', text)

    def test_album_flip_is_served_before_dcim_inventory(self):
        text = (ROOT / 'camera/film_capture_daemon.c').read_text(encoding='utf-8')
        watch = text.split('while(1) {', 1)[1]
        scan_at = watch.find('inventory_dirs_with_hint(&transport')
        self.assertGreater(scan_at, 0)
        self.assertIn('album_flip_waiting()', watch[:scan_at])
        wait = text.split('static void wait_before_scan', 1)[1].split('static int metadata_retry', 1)[0]
        self.assertIn('album_flip_waiting()', wait)

    def test_album_loading_prompt_is_centered_over_a_solid_background(self):
        text = (ROOT / 'camera/native_film_ui_trial3.c').read_text(encoding='utf-8')
        view = text.split('static void v4_album_sync_view', 1)[1].split('static void v4_exit_album', 1)[0]
        self.assertIn('v4_draw_album_hud(top,bot,1)', view)
        self.assertIn('UI_S_ALB_LOADING', view)
        self.assertIn('place(ui.labels[1],1,240,210,160,36)', view)
        self.assertIn('v4_publish_display(A6400_DISP_NONE,slot)', view)
        self.assertNotIn('v4_publish_display(A6400_DISP_ALBUM,slot)', view)


def _rgba4444(buf, x, y):
    off = 64 + (y * 600 + x) * 2
    pixel = int.from_bytes(buf[off:off + 2], 'little')
    return ((pixel >> 12) & 0xF) * 17, ((pixel >> 8) & 0xF) * 17, ((pixel >> 4) & 0xF) * 17


def _exif_orientation(jpeg, orient):
    tiff = bytes([
        0x49, 0x49, 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,
        0x01, 0x00,
        0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, orient & 0xFF, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    ])
    payload = b'Exif\x00\x00' + tiff
    segment = b'\xff\xe1' + (len(payload) + 2).to_bytes(2, 'big') + payload
    return jpeg[:2] + segment + jpeg[2:]


class AlbumPreviewTests(unittest.TestCase):
    def test_preview_uses_exif_orientation_and_dark_margin(self):
        cam = ROOT / 'camera'
        vendor = ROOT / 'vendor'
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            objs = []
            for name, src, lang, extra in (
                ('film_jpeg.o', cam / 'film_jpeg.cpp', 'c++', ['-O1', '-std=c++11', '-fno-exceptions', '-fno-rtti']),
                ('author_film.o', cam / 'author_film.c', 'cc', ['-O1', '-std=c11']),
                ('picojpeg.o', vendor / 'picojpeg.c', 'cc', ['-O1', '-std=c11', '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-but-set-variable']),
                ('jpge.o', vendor / 'jpge.cpp', 'c++', ['-O1', '-std=c++11', '-fno-exceptions', '-fno-rtti']),
            ):
                obj = d / name
                subprocess.run([lang, '-DA6400_FILM_HOST', '-DA6400_FILM_JOB', '-Wall', '-Wextra', '-Werror',
                                '-I', str(cam), '-I', str(vendor), *extra, '-c', str(src), '-o', str(obj)],
                               check=True)
                objs.append(str(obj))
            job = d / 'film_job'
            subprocess.run(['c++', *objs, '-o', str(job)], check=True)
            gen = d / 'gen.cpp'
            gen.write_text(
                '#include <cstdlib>\n#include <vector>\n#include "jpge.h"\n'
                'int main(int argc, char **argv) {\n'
                '  int w = std::atoi(argv[1]), h = std::atoi(argv[2]), mode = std::atoi(argv[3]);\n'
                '  std::vector<unsigned char> rgb((size_t)w * (size_t)h * 3u);\n'
                '  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {\n'
                '    int i = (y * w + x) * 3;\n'
                '    int red = mode == 0 && x < w / 4;\n'
                '    int blue = mode == 0 ? x >= w / 4 : y < h / 2;\n'
                '    int green = mode == 1 && y >= h / 2;\n'
                '    rgb[i] = red ? 255 : 0; rgb[i + 1] = green ? 255 : 0; rgb[i + 2] = blue ? 255 : 0;\n'
                '  }\n'
                '  jpge::params p; p.m_quality = 95; p.m_subsampling = jpge::H2V2;\n'
                '  return jpge::compress_image_to_jpeg_file(argv[4], w, h, 3, rgb.data(), p) ? 0 : 1;\n'
                '}\n'
            )
            gen_bin = d / 'gen'
            subprocess.run(['c++', '-O1', '-std=c++11', '-I', str(vendor), str(gen), str(vendor / 'jpge.cpp'), '-o', str(gen_bin)],
                           check=True)
            turned = d / 'turned.jpg'
            subprocess.run([str(gen_bin), '160', '80', '0', str(turned)], check=True)
            turned.write_bytes(_exif_orientation(turned.read_bytes(), 6))
            preview = d / 'current.bin'
            res = subprocess.run([str(job), '--album-preview', str(turned), str(preview), 'F0000001.JPG'],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            packed = preview.read_bytes()
            red = _rgba4444(packed, 300, 20)
            self.assertGreater(red[0], 200, red)
            self.assertLess(red[1], 40, red)
            margin = _rgba4444(packed, 8, 200)
            self.assertLess(margin[0], 40, margin)
            self.assertLess(max(margin), 50, margin)

            split = d / 'split.jpg'
            subprocess.run([str(gen_bin), '1616', '136', '1', str(split)], check=True)
            res = subprocess.run([str(job), '--album-preview', str(split), str(preview), 'F0000002.JPG'],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            packed = preview.read_bytes()
            top = _rgba4444(packed, 300, 180)
            bottom = _rgba4444(packed, 300, 220)
            self.assertGreater(top[2], 180, top)
            self.assertLess(top[0], 40, top)
            self.assertGreater(bottom[1], 180, bottom)
            self.assertLess(bottom[0], 40, bottom)
            self.assertLess(bottom[2], 80, bottom)

            # Fast 1/8 DC-only path (>= 3600x2400 12MP) + EXIF orient=6 portrait
            big12m = d / 'big12m.jpg'
            subprocess.run([str(gen_bin), '4240', '2826', '0', str(big12m)], check=True)
            big12m.write_bytes(_exif_orientation(big12m.read_bytes(), 6))
            res = subprocess.run([str(job), '--album-preview', str(big12m), str(preview), 'F0000004.JPG'],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            packed12 = preview.read_bytes()
            self.assertEqual(len(packed12), 480064)
            red12 = _rgba4444(packed12, 300, 20)
            self.assertGreater(red12[0], 200, red12)
            self.assertLess(red12[1], 40, red12)
            margin12 = _rgba4444(packed12, 8, 200)
            self.assertLess(max(margin12), 50, margin12)

            cut = d / 'cut.jpg'
            cut.write_bytes(split.read_bytes()[:-400])
            preview.unlink()
            res = subprocess.run([str(job), '--album-preview', str(cut), str(preview), 'F0000003.JPG'],
                                 capture_output=True, text=True)
            self.assertNotEqual(res.returncode, 0, res.stdout)
            self.assertIn('FILM_ALBUM_PREVIEW_FAIL', res.stdout)
            self.assertFalse(preview.exists())

    def test_daemon_copies_into_its_own_ram_mount_and_gives_up(self):
        text = (ROOT / 'camera/film_capture_daemon.c').read_text(encoding='utf-8')
        decode = text.split('static int decode_test_jpeg', 1)[1].split('static void remember_graded_name', 1)[0]
        release = text.split('static void album_release_mount', 1)[1].split('static int decode_test_jpeg', 1)[0]
        self.assertIn('album_input_mount()', decode)
        self.assertIn('album_release_mount()', decode)
        self.assertIn('umount(', release)
        self.assertIn('rmdir(ALBUM_JOB_DIR)', release)
        job = (ROOT / 'camera/film_ram_job.sh').read_text(encoding='utf-8')
        self.assertIn('test ! -e "$RAM"', job)
        self.assertIn('FILM_ALBUM_PREVIEW_GIVEUP', text)
        self.assertIn('album_request_matches', decode)
        self.assertIn('run_exec_until_left', decode)
        self.assertIn('unlink(A6400_PV_CURRENT)', decode)
        self.assertIn('FILM_ALBUM_PREVIEW_CANCEL', text)
        waiting = text.split('static int album_flip_waiting', 1)[1].split('static void service_test_album', 1)[0]
        self.assertIn('view_seq!=0', waiting)
        watch = text.split('while(1) {', 1)[1]
        self.assertLess(watch.find('album_flip_waiting()'), watch.find('transport_start(&transport)'))
        self.assertIn('FILM_ALBUM_CACHE_HIT', text)
        self.assertIn('FILM_ALBUM_PREFETCH_OK', text)
        ui = (ROOT / 'camera/native_film_ui_trial3.c').read_text(encoding='utf-8')
        leave = ui.split('static void v4_exit_album', 1)[1].split('static void visibility', 1)[0]
        self.assertIn('album_view_seq=0', leave)
        browse = ui.split('if(change&FILM_MENU_ALBUM_BROWSE)', 1)[1].split('V4.1 Instant filter preview', 1)[0]
        self.assertLess(browse.find('if(a6400_ui_v4_v1.album_count>0u)'), browse.find('album_scan_req+=1u'))


if __name__ == '__main__':
    unittest.main()
