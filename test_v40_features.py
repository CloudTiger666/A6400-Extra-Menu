"""Comprehensive host tests for A6400 Extra-Menu V4.0 features:
1. 12MP streaming strip pipeline (--ultra12m) & EXIF/preview generation
2. Instant filter preview pop-up state machine (Poll #5)
3. In-camera graded album preview: Playback auto-sync + direct browser (Poll #2)
4. Native MENU full touch (MTF2) poll/drag/release & coexistence gating
5. External RGBA4444 OSD preview blitter (film_preview_osd.c) byte-exact verification
"""
from pathlib import Path
import ctypes
import os
import signal
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent


class UiV4V1(ctypes.Structure):
    _fields_ = [
        ('magic', ctypes.c_uint32),
        ('version', ctypes.c_uint32),
        ('preview_popup_en', ctypes.c_uint32),
        ('album_preview_en', ctypes.c_uint32),
        ('native_touch_en', ctypes.c_uint32),
        ('display_mode', ctypes.c_uint32),
        ('display_seq', ctypes.c_uint32),
        ('album_slot', ctypes.c_uint32),
        ('native_touch_ok', ctypes.c_uint32),
        ('native_touch_hits', ctypes.c_uint32),
        ('latest_seq', ctypes.c_uint32),
        ('album_count', ctypes.c_uint32),
        ('album_head', ctypes.c_uint32),
        ('session_shots', ctypes.c_uint32),
        ('shot_slots', ctypes.c_int8 * 64),
        ('slot_labels', (ctypes.c_char * 48) * 16),
        ('dexp_mode', ctypes.c_uint32),
        ('dexp_seq', ctypes.c_uint32),
        ('dexp_pending', ctypes.c_uint32),
        ('album_truncated', ctypes.c_uint32),
        ('album_ready_seq', ctypes.c_uint32),
        ('album_ready_slot', ctypes.c_uint32),
        ('album_scan_done', ctypes.c_uint32),
        ('album_label', ctypes.c_char * 48),
        ('album_view_seq', ctypes.c_uint32),
        ('album_view_slot', ctypes.c_uint32),
        ('album_scan_req', ctypes.c_uint32),
    ]


def _make_synthetic_jpeg(path: Path, width: int, height: int):
    """Build a valid baseline JPEG using our own vendored jpge encoder (no PIL needed)."""
    with tempfile.TemporaryDirectory() as td:
        gen_cpp = Path(td) / 'gen.cpp'
        gen_bin = Path(td) / 'gen'
        gen_cpp.write_text(r'''
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "jpge.h"
int main(int argc, char **argv) {
    int w = atoi(argv[2]), h = atoi(argv[3]);
    std::vector<unsigned char> rgb((size_t)w * (size_t)h * 3u);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t idx = ((size_t)y * (size_t)w + (size_t)x) * 3u;
            rgb[idx]     = (unsigned char)((x * 255) / (w > 1 ? w - 1 : 1));
            rgb[idx + 1] = (unsigned char)((y * 255) / (h > 1 ? h - 1 : 1));
            rgb[idx + 2] = (unsigned char)(((x + y) * 127) / (w + h));
        }
    }
    jpge::params p; p.m_quality = 90; p.m_subsampling = jpge::H2V2;
    return jpge::compress_image_to_jpeg_file(argv[1], w, h, 3, rgb.data(), p) ? 0 : 1;
}
''', encoding='utf-8')
        subprocess.run([
            'c++', '-O2', '-std=c++11', '-I', str(ROOT / 'vendor'),
            str(gen_cpp), str(ROOT / 'vendor/jpge.cpp'), '-o', str(gen_bin),
        ], check=True)
        subprocess.run([str(gen_bin), str(path), str(width), str(height)], check=True)


def _jpeg_dims(data: bytes):
    i = 2
    while i + 4 <= len(data):
        if data[i] != 0xFF:
            raise AssertionError('bad JPEG marker')
        m = data[i + 1]
        if m == 0xDA:
            break
        ln = struct.unpack_from('>H', data, i + 2)[0]
        if m in (0xC0, 0xC1, 0xC2):
            h, w = struct.unpack_from('>HH', data, i + 5)
            return w, h
        i += 2 + ln
    raise AssertionError('SOF not found')


class V40FeaturesTests(unittest.TestCase):
    def test_ultra12m_streaming_resize_and_preview_package(self):
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            worker = d / 'film_job'
            subprocess.run([
                'make', '-C', str(ROOT / 'camera'), '-f', 'Makefile.film',
                'HOST=1', 'JOB=1', f'BUILD={d / "build"}', f'TARGET={worker}',
            ], check=True, capture_output=True)
            src24 = d / 'in_6000x4000.jpg'
            _make_synthetic_jpeg(src24, 6000, 4000)
            out12 = d / 'out_12m.jpg'
            pv12 = d / 'pv_12m.bin'
            res = subprocess.run([
                str(worker), 'portra_400', str(src24), str(out12),
                '2', '1', '--ultra12m', '--preview', str(pv12)
            ], capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertIn('FILM_SOURCE kind=ultra12m source_width=6000 source_height=4000 width=4240 height=2826', res.stdout)
            self.assertIn('FILM_PREVIEW_OK bytes=556928', res.stdout)
            self.assertEqual(_jpeg_dims(out12.read_bytes()), (4240, 2826))
            ver = subprocess.run([str(worker), '--verify', str(out12)],
                                 capture_output=True, text=True)
            self.assertEqual(ver.returncode, 0, ver.stdout + ver.stderr)
            self.assertIn('FILM_DECODED width=4240 height=2826', ver.stdout)

            # Verify preview binary package layout and Bayer-dithered content
            raw = pv12.read_bytes()
            self.assertEqual(len(raw), 556928)
            m1, w1, h1, _ = struct.unpack_from('<IIII', raw, 0)
            self.assertEqual((m1, w1, h1), (0x56503441, 240, 160))
            off2 = 64 + 240 * 160 * 2
            m2, w2, h2, _ = struct.unpack_from('<IIII', raw, off2)
            self.assertEqual((m2, w2, h2), (0x56503441, 600, 400))
            # Popup border must be golden accent 0xEA3F
            self.assertEqual(struct.unpack_from('<H', raw, 64)[0], 0xEA3F)
            # Album pixels must be opaque RGBA4444 (low nibble == 0xF) with gradient variety
            sample = struct.unpack_from('<600H', raw, off2 + 64 + 200 * 600 * 2)
            self.assertTrue(all((px & 0xF) == 0xF for px in sample))
            self.assertGreater(len(set(sample)), 10)

            # Smaller image (<= 12.5MP) must pass through 1:1 without upscaling
            src_small = d / 'in_800x600.jpg'
            _make_synthetic_jpeg(src_small, 800, 600)
            out_small = d / 'out_small.jpg'
            res2 = subprocess.run([
                str(worker), 'classic_chrome', str(src_small), str(out_small),
                '1', '0', '--ultra12m'
            ], capture_output=True, text=True)
            self.assertEqual(res2.returncode, 0, res2.stdout + res2.stderr)
            self.assertEqual(_jpeg_dims(out_small.read_bytes()), (800, 600))

    def test_v40_ui_quality_popup_album_and_mtf2_gating(self):
        headers = {
            'dlfcn.h': '#define RTLD_DEFAULT ((void *)0)\n#define RTLD_NOW 2\n#define RTLD_NOLOAD 4\nvoid *dlopen(const char *,int);\nvoid *dlsym(void *,const char *);\nint dlclose(void *);\n',
            'stdio.h': '#include <stddef.h>\nint snprintf(char *,size_t,const char *,...);\n',
            'stdlib.h': '#include <stddef.h>\nvoid *calloc(size_t,size_t);\n',
            'string.h': '#include <stddef.h>\nvoid *memcpy(void *,const void *,size_t);\nvoid *memset(void *,int,size_t);\nchar *strcpy(char *,const char *);\n',
            'sys/syscall.h': '#define SYS_gettid 224\n#define SYS_clock_gettime 263\n',
            'time.h': '#define CLOCK_MONOTONIC 1\nstruct timespec {long tv_sec,tv_nsec;};\n',
            'unistd.h': 'long model_syscall(long,...);\n#define syscall model_syscall\n',
        }
        model_c = r'''
#include <stdarg.h>
static int mtf2_mock_calls = 0;
static int mtf2_mock_last_x = 0, mtf2_mock_last_y = 0, mtf2_mock_last_dx = 0, mtf2_mock_last_dy = 0;
#define A6400_MTF2_HOST_MOCK 1
#include "native_film_ui_trial3.c"
static uint32_t mtf2_host_mock_dispatch(int16_t x, int16_t y, int dx, int dy, int has_press) {
    (void)has_press;
    ++mtf2_mock_calls;
    mtf2_mock_last_x = x; mtf2_mock_last_y = y;
    mtf2_mock_last_dx = dx; mtf2_mock_last_dy = dy;
    return (mtf2_iabs(dx) >= 48) ? MTF2_ACT_SWIPE_PAGE : MTF2_ACT_TAB;
}
unsigned model_now_ms = 1000;
int model_touch = 6, model_x = 32767, model_y = 32767;
int model_key_code = 0;
uint32_t model_pb_val = 0;
int model_layer_is_active = 0;
char model_texts[8][80];
static unsigned char menu_marker[64], ticker_marker[64];
static uint32_t model_pb_base(void) { return model_pb_val; }
static int model_key(void *p, int key, void *data) {
    (void)p; (void)data;
    return (model_key_code != 0 && key == model_key_code) ? 1 : 0;
}
static void model_position(const void *p, unsigned char n, short *x, short *y) {
    (void)p; (void)n; *x = (short)model_x; *y = (short)model_y;
}
static int model_state(const void *p, unsigned char a, unsigned char b) {
    (void)p; (void)a; (void)b; return model_touch;
}
static void model_layer(void *p, int active) { (void)p; model_layer_is_active = active; }
static void model_animation(void *p, int active) { (void)p; (void)active; }
static void model_unary(void *p) { (void)p; }
static void model_place(void *p, const float *xy) { (void)p; (void)xy; }
static void model_size(void *p, float w, float h) { (void)p; (void)w; (void)h; }
static void model_color(void *p, const unsigned char *c) { (void)p; (void)c; }
static void model_text(void *p, const char *s) {
    for (unsigned i = 0; i < 8; i++)
        if (p == ui.labels[i] && s) snprintf(model_texts[i], sizeof(model_texts[i]), "%s", s);
}
long model_syscall(long n, ...) {
    if (n == SYS_gettid) return 380;
    if (n == SYS_clock_gettime) {
        va_list ap; va_start(ap, n);
        int clk = va_arg(ap, int);
        struct timespec *t = va_arg(ap, struct timespec *);
        va_end(ap);
        if (clk == CLOCK_MONOTONIC && t) {
            t->tv_sec = (long)(model_now_ms / 1000u);
            t->tv_nsec = (long)((model_now_ms % 1000u) * 1000000u);
            return 0;
        }
    }
    return 0;
}
static void *model_get_key_inst(void) { return ui.keys; }
void model_init(void) {
    memset(&ui, 0, sizeof(ui));
    memset(&ui_theme, 0, sizeof(ui_theme));
    memset(&ui_toast, 0, sizeof(ui_toast));
    memset(&ui_v4, 0, sizeof(ui_v4));
    struct film_settings initial = {0, 0, 2, 1};
    const struct film_native_view_ops ops = {visibility, draw, settings};
    film_native_view_init(&ui.view, &ops, 0, &initial);
    ui.system = ui_sys_stub; ui.keys = (void *)0x59150; get_key_instance_fn = model_get_key_inst;
    ui.tid = 380; ui.running = ui.attached = 1;
    ui.layer = menu_marker; ui.ticker = ui.ticker_layer = ticker_marker;
    ui.key = model_key; ui.mouse_position = model_position; ui.mouse_state = model_state;
    ui.layer_active = model_layer; ui.set_animate = model_animation; ui.move_last = model_unary;
    ui.set_position = model_place; ui.panel_size = ui.label_size = model_size;
    ui.set_color = model_color; ui.set_text = model_text;
    for (unsigned i = 0; i < 8; i++) ui.labels[i] = (void *)(uintptr_t)(0x3000u + i);
    a6400_ui_v4_v1.native_touch_ok = (uint32_t)mtf2_probe_and_init(&ui_v4.mtf2);
    for (unsigned i = 0; i < A6400_SESSION_SHOTS_CAP; i++) a6400_ui_v4_v1.shot_slots[i] = -1;
    a6400_ui_v4_v1.preview_popup_en = 1u;
    a6400_ui_v4_v1.album_preview_en = 0u;
    a6400_ui_v4_v1.native_touch_en = 1u;
    a6400_ui_v4_v1.display_mode = A6400_DISP_NONE;
    a6400_ui_v4_v1.display_seq = 0u;
    a6400_ui_v4_v1.latest_seq = 0u;
    a6400_ui_v4_v1.album_count = 0u;
    a6400_ui_v4_v1.album_head = 0u;
    a6400_ui_v4_v1.session_shots = 0u;
    a6400_ui_hires_v1 = 0u;
    mtf2_mock_calls = 0;
    model_now_ms = 1000; model_pb_val = 0; model_key_code = 0;
}
void model_tick(unsigned dt, int touch, int x, int y, int key_code, uint32_t pb) {
    model_now_ms += dt; model_touch = touch; model_x = x; model_y = y;
    model_key_code = key_code; model_pb_val = pb;
    animate(ui.ticker);
}
void model_open_home(void) {
    ui.view.menu.visible = 1; ui.view.menu.page = FILM_MENU_HOME;
    ui.view.menu.touch_ready = 1; draw(0, &ui.view.menu);
}
const char *model_text_at(unsigned i) { return i < 8 ? model_texts[i] : ""; }
int model_mtf2_calls(void) { return mtf2_mock_calls; }
int model_mtf2_dx(void) { return mtf2_mock_last_dx; }
int model_album_mode(void) { return ui_v4.album_mode; }
int model_menu_visible(void) { return ui.view.menu.visible; }
'''
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            inc = d / 'inc'
            (inc / 'sys').mkdir(parents=True)
            for name, content in headers.items():
                (inc / name).write_text(content, encoding='utf-8')
            for name in ('film_menu.h', 'film_native_view.h', 'ui_trial_trace.h',
                         'film_preview_fmt.h', 'mtf2_native_touch.h', 'film_album_index.h'):
                (inc / name).write_text((ROOT / 'camera' / name).read_text(encoding='utf-8'), encoding='utf-8')
            src = (ROOT / 'camera/native_film_ui_trial3.c').read_text(encoding='utf-8')
            body = model_c.replace('#include "native_film_ui_trial3.c"', src).replace('__attribute__((pcs("aapcs")))', '')
            preamble = 'static unsigned char ui_sys_stub[4096];\n#define _Static_assert(cond, msg)\n'
            (d / 'model.c').write_text(preamble + body, encoding='utf-8')
            so_file = d / 'v40_model.so'
            subprocess.run([
                'cc', '-std=c11', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-DA6400_UI_TRIAL_MS=0', '-DA6400_UI_DISPLAY_MS=0',
                '-I', str(inc), '-I', str(d),
                str(d / 'model.c'), str(ROOT / 'camera/film_native_view.c'),
                str(ROOT / 'camera/film_menu.c'), '-o', str(so_file),
            ], check=True)
            lib = ctypes.CDLL(str(so_file))
            lib.model_text_at.restype = ctypes.c_char_p
            lib.model_init()
            v4 = UiV4V1.in_dll(lib, 'a6400_ui_v4_v1')
            hires = ctypes.c_uint32.in_dll(lib, 'a6400_ui_hires_v1')

            # 1) 3-way quality cycling: 1.7MP -> 6MP -> 12MP -> 1.7MP (Bottom-Right Tian tile @ x=450, y=310, label 4)
            lib.model_open_home()
            self.assertEqual(lib.model_text_at(4).decode('utf-8'), '畫質: 1.7MP 極速')
            for expected_val, expected_lbl in [
                (1, '畫質: 6MP 高清'),
                (2, '畫質: 12MP 超清'),
                (0, '畫質: 1.7MP 極速'),
            ]:
                lib.model_tick(25, 5, 450, 310, 0, 0)
                lib.model_tick(25, 6, 32767, 32767, 0, 0)
                self.assertEqual(hires.value, expected_val)
                self.assertEqual(lib.model_text_at(4).decode('utf-8'), expected_lbl)
            # While Film Menu is open, MTF2 must NEVER fire
            self.assertEqual(lib.model_mtf2_calls(), 0)
            # Verify NO auto-hide when A6400_UI_DISPLAY_MS=0 even after 120s idle!
            lib.model_tick(120000, 6, 32767, 32767, 0, 0)
            self.assertEqual(lib.model_menu_visible(), 1)
            self.assertIn('V4.3 SLEM', lib.model_text_at(0).decode('utf-8'))
            self.assertIn('hany', lib.model_text_at(7).decode('utf-8'))
            # Close menu via bottom-right Close pill (x=510, y=418)
            lib.model_tick(25, 5, 510, 418, 0, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertEqual(lib.model_menu_visible(), 0)

            # 2) Native MENU touch (MTF2): tap + swipe while Film Menu closed, even when release reports 32767
            lib.model_tick(25, 5, 300, 120, 0, 0)
            lib.model_tick(25, 5, 210, 122, 0, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertEqual(lib.model_mtf2_calls(), 1)
            self.assertEqual(lib.model_mtf2_dx(), -90)
            self.assertEqual(v4.native_touch_hits, 1)
            # Disable native touch -> must not fire
            v4.native_touch_en = 0
            lib.model_tick(25, 5, 150, 150, 0, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertEqual(lib.model_mtf2_calls(), 1)
            v4.native_touch_en = 1

            # 3) Instant filter preview pop-up (Poll #5)
            v4.slot_labels[0].value = b'portra_400 12MP (F9530001.JPG)'
            v4.slot_labels[1].value = b'classic_chrome 6MP (F9532002.JPG)'
            v4.shot_slots[0] = 0   # shot 0: LUT ON -> slot 0
            v4.shot_slots[1] = -1  # shot 1: LUT OFF
            v4.shot_slots[2] = 1   # shot 2 (newest): LUT ON -> slot 1
            v4.album_count = 2
            v4.album_head = 1
            v4.session_shots = 3
            v4.latest_seq = 1
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertEqual(v4.display_mode, 1)  # POPUP active
            # Auto-dismisses after 3.5s
            lib.model_tick(3600, 6, 32767, 32767, 0, 0)
            self.assertEqual(v4.display_mode, 0)
            # Touch immediately dismisses popup without passing through to MTF2
            v4.latest_seq = 2
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertEqual(v4.display_mode, 1)
            lib.model_tick(25, 5, 200, 200, 0, 0)
            self.assertEqual(v4.display_mode, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertEqual(lib.model_mtf2_calls(), 1)  # did NOT leak touch to MTF2

            # 4) Native Sony Playback must NEVER be hijacked!
            lib.model_tick(25, 6, 32767, 32767, 0, 0x10)  # Native Playback active
            self.assertEqual(lib.model_album_mode(), 0)
            self.assertEqual(v4.display_mode, 0)
            lib.model_tick(25, 6, 32767, 32767, 0x15, 0x10)  # Left in native Playback
            self.assertEqual(lib.model_album_mode(), 0)
            self.assertEqual(v4.display_mode, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)

            # 5) Direct Graded Album Browser directly from Home Top-Right tile!
            lib.model_open_home()
            self.assertEqual(lib.model_text_at(2).decode('utf-8'), '套色相冊 (2張) >')
            lib.model_tick(25, 5, 450, 150, 0, 0)  # Top-Right tile -> direct album browse!
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertEqual(lib.model_album_mode(), 2)
            self.assertEqual(v4.display_mode, 0)  # loading covers live view; OSD stays idle
            self.assertEqual(v4.album_slot, 1)  # newest graded slot
            # Tap left third -> older graded slot (slot 0)
            lib.model_tick(25, 5, 80, 240, 0, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertEqual(v4.album_slot, 0)
            # Tap center -> exit cleanly
            lib.model_tick(25, 5, 320, 240, 0, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertEqual(lib.model_album_mode(), 0)
            self.assertEqual(v4.display_mode, 0)

            # 5b) Double Exposure & LUT strict mutual exclusion:
            #     - Enabling Double Exposure automatically turns OFF LUT.
            #     - Enabling LUT while dexp_pending != 0 (Shot 1/2 unfinished) is BLOCKED.
            #     - Enabling LUT when dexp_pending == 0 auto-disables Double Exposure.
            lib.model_open_home()
            # Open LUT Settings (Top-Left tile @ x=180, y=150) and turn LUT ON (row 0 @ x=200, y=120)
            lib.model_tick(25, 5, 180, 150, 0, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            lib.model_tick(25, 5, 200, 120, 0, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertIn('開啟', lib.model_text_at(1).decode('utf-8'))
            self.assertEqual(v4.dexp_mode, 0)
            # Open Details/V4 subpage (bottom-left @ x=140, y=418) -> row 3 is Double Exposure
            lib.model_tick(25, 5, 140, 418, 0, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertIn('雙重曝光: 關閉', lib.model_text_at(4).decode('utf-8'))
            # Tap Double Exposure (row 3 @ x=200, y=336) -> turns ON Screen (1) and auto-disables LUT!
            lib.model_tick(25, 5, 200, 336, 0, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertEqual(v4.dexp_mode, 1)
            self.assertIn('雙重曝光: 濾色 (0/2)', lib.model_text_at(4).decode('utf-8'))
            # Return to LUT Settings (bottom-left @ x=140, y=418) -> LUT must be OFF!
            lib.model_tick(25, 5, 140, 418, 0, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertIn('關閉', lib.model_text_at(1).decode('utf-8'))
            # Simulate Shot 1/2 captured (dexp_pending = 1):
            #   - Home Top-Right tile becomes "取消雙曝 1/2" (replaces Graded Album while 1/2 is pending)
            #   - Trying to turn LUT ON is BLOCKED
            v4.dexp_pending = 1
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertIn('雙曝未完鎖定', lib.model_text_at(1).decode('utf-8'))
            lib.model_tick(25, 5, 200, 120, 0, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertIn('雙曝 1/2 未拍完，不可開啟 LUT!', lib.model_text_at(0).decode('utf-8'))
            self.assertIn('關閉', lib.model_text_at(1).decode('utf-8'))
            # Return to Home -> Top-Right tile shows "取消雙曝 1/2", tapping it cancels 1/2 while keeping dexp_mode ON!
            lib.model_tick(25, 5, 510, 418, 0, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertEqual(lib.model_text_at(2).decode('utf-8'), '取消雙曝 1/2')
            lib.model_tick(25, 5, 450, 150, 0, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertEqual(v4.dexp_pending, 0)
            self.assertEqual(v4.dexp_mode, 1)  # Double Exposure stays ON so user can re-shoot Shot 1/2!
            self.assertEqual(lib.model_text_at(2).decode('utf-8'), '套色相冊 (2張) >')
            # Now enter LUT Settings and turn LUT ON -> succeeds & auto-disables Double Exposure!
            lib.model_tick(25, 5, 180, 150, 0, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            lib.model_tick(25, 5, 200, 120, 0, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertIn('開啟', lib.model_text_at(1).decode('utf-8'))
            self.assertEqual(v4.dexp_mode, 0)
            # Close menu
            lib.model_tick(25, 5, 510, 418, 0, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)
            lib.model_tick(25, 5, 510, 418, 0, 0)
            lib.model_tick(25, 6, 32767, 32767, 0, 0)

            # 6) Simplified Chinese (-DA6400_UI_LANG_SC=1) + 3-sound compile & strings check
            so_sc = d / 'v40_model_sc.so'
            subprocess.run([
                'cc', '-std=c11', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-DA6400_UI_TRIAL_MS=0', '-DA6400_UI_DISPLAY_MS=0',
                '-DA6400_UI_LANG_SC=1', '-DA6400_CHARO_SHUTTER=1', '-DA6400_CHARO_MULTI=1',
                '-I', str(inc), '-I', str(d),
                str(d / 'model.c'), str(ROOT / 'camera/film_native_view.c'),
                str(ROOT / 'camera/film_menu.c'), '-o', str(so_sc),
            ], check=True)
            lib_sc = ctypes.CDLL(str(so_sc))
            lib_sc.model_text_at.restype = ctypes.c_char_p
            lib_sc.model_init()
            lib_sc.model_open_home()
            self.assertEqual(lib_sc.model_text_at(2).decode('utf-8'), '套色相册 (0张) >')
            self.assertEqual(lib_sc.model_text_at(4).decode('utf-8'), '画质: 1.7MP 极速')
            lib_sc.model_tick(25, 5, 450, 310, 0, 0)
            lib_sc.model_tick(25, 6, 32767, 32767, 0, 0)
            lib_sc.model_tick(25, 5, 450, 310, 0, 0)
            lib_sc.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertEqual(lib_sc.model_text_at(4).decode('utf-8'), '画质: 12MP 超清')
            # Check 3-sound SC label on Bottom-Left tile, then LUT (x=180,y=150) -> Details/V4 (x=160,y=418)
            self.assertEqual(lib_sc.model_text_at(3).decode('utf-8'), '快门: 静音')
            lib_sc.model_tick(25, 5, 180, 310, 0, 0)
            lib_sc.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertEqual(lib_sc.model_text_at(3).decode('utf-8'), '快门: 日文 Charo')
            lib_sc.model_tick(25, 5, 180, 150, 0, 0)  # Open LUT Settings
            lib_sc.model_tick(25, 6, 32767, 32767, 0, 0)
            lib_sc.model_tick(25, 5, 160, 418, 0, 0)  # Open Details/V4 inside LUT
            lib_sc.model_tick(25, 6, 32767, 32767, 0, 0)
            self.assertEqual(lib_sc.model_text_at(1).decode('utf-8'), '即时预览小窗: 开启')
            self.assertEqual(lib_sc.model_text_at(2).decode('utf-8'), '原生MENU触控: 开启')
            self.assertEqual(lib_sc.model_text_at(3).decode('utf-8'), '恢复胶片默认 (标准/开)')

    def test_external_osd_preview_blitter_popup_album_and_clear(self):
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            pv_dir = d / 'pv'
            pv_dir.mkdir()
            stop_file = d / 'stop'
            maps_file = d / 'maps'
            mem_file = d / 'mem'

            # Synthetic memory image layout:
            # 0x001000..0x002000: mock ui rw page (a6400_ui_v4_v1 at offset 0x1000)
            # 0x010000..0x0a6000: OSD_A (614400 bytes = 0x96000)
            # 0x0b0000..0x146000: OSD_B (614400 bytes = 0x96000)
            v4_off = 0x1000
            osd_a = 0x10000
            osd_b = 0xb0000
            total_mem = osd_b + 0x96000
            # Fill OSD_A and OSD_B with sentinel 0x55AA so we verify ZERO out-of-bounds writes!
            mem_buf = bytearray(total_mem)
            sentinel = b'\xaa\x55' * (0x96000 // 2)
            mem_buf[osd_a:osd_a + 0x96000] = sentinel
            mem_buf[osd_b:osd_b + 0x96000] = sentinel
            # Write initial a6400_ui_v4_v1 header (mode=NONE)
            struct.pack_into('<8I', mem_buf, v4_off,
                             0x34563441, 1, 1, 1, 1, 0, 0, 0)
            mem_file.write_bytes(mem_buf)

            maps_file.write_text(
                f'00000000-00001000 r-xp 00000000 00:00 0 /tmp_bt/a6400-film-ui-v3-persist.so\n'
                f'00001000-00004000 rw-p 00001000 00:00 0 /tmp_bt/a6400-film-ui-v3-persist.so\n'
                f'{osd_a:08x}-{osd_a + 0x96000:08x} rw-s 23a80000 00:00 0 /dev/stream\n'
                f'{osd_b:08x}-{osd_b + 0x96000:08x} rw-s 23b16000 00:00 0 /dev/stream\n',
                encoding='utf-8',
            )

            # Create popup.bin (0xF00F red opaque) and slot_00.bin (0x0F0F green opaque)
            pop_hdr = struct.pack('<IIII48s', 0x56503441, 240, 160, 1, b'popup')
            (pv_dir / 'popup.bin').write_bytes(pop_hdr + b'\x0f\xf0' * (240 * 160))
            alb_hdr = struct.pack('<IIII48s', 0x56503441, 600, 400, 1, b'album0')
            (pv_dir / 'slot_00.bin').write_bytes(alb_hdr + b'\x0f\x0f' * (600 * 400))

            (d / 'film_preview_osd_build.h').write_text(
                '#define FILM_PREVIEW_UI_MODULE "/tmp_bt/a6400-film-ui-v3-persist.so"\n'
                f'#define FILM_PREVIEW_V4_RVA 0x{v4_off:x}u\n'
                '#define FILM_PREVIEW_FIXED_PID 149\n'
                f'#define FILM_PREVIEW_MAPS_PATH "{maps_file}"\n'
                f'#define FILM_PREVIEW_MEM_PATH "{mem_file}"\n'
                f'#define FILM_PREVIEW_PV_DIR "{pv_dir}"\n'
                f'#define FILM_PREVIEW_STOP_FILE "{stop_file}"\n',
                encoding='utf-8',
            )
            helper = d / 'preview_osd'
            subprocess.run([
                'cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I', str(d), '-I', str(ROOT / 'camera'),
                str(ROOT / 'camera/film_preview_osd.c'), '-o', str(helper),
            ], check=True)

            proc = subprocess.Popen([str(helper), 'watch'])
            try:
                def set_disp(mode, seq, slot=0):
                    with open(mem_file, 'r+b') as f:
                        f.seek(v4_off + 20)
                        f.write(struct.pack('<3I', mode, seq, slot))
                        f.flush()

                def px_at(base, x, y):
                    with open(mem_file, 'rb') as f:
                        f.seek(base + (y * 640 + x) * 2)
                        return struct.unpack('<H', f.read(2))[0]

                # 1) Trigger POPUP
                set_disp(1, 1, 0)
                deadline = time.time() + 2.0
                while time.time() < deadline and px_at(osd_a, 376, 212) != 0xF00F:
                    time.sleep(0.02)
                self.assertEqual(px_at(osd_a, 376, 212), 0xF00F)
                self.assertEqual(px_at(osd_b, 376 + 239, 212 + 159), 0xF00F)
                # Pixel outside popup rect must remain untouched sentinel 0x55AA!
                self.assertEqual(px_at(osd_a, 375, 212), 0x55AA)
                self.assertEqual(px_at(osd_a, 10, 10), 0x55AA)

                # 2) Transition to ALBUM slot 0
                set_disp(2, 2, 0)
                deadline = time.time() + 2.0
                while time.time() < deadline and px_at(osd_a, 20, 40) != 0x0F0F:
                    time.sleep(0.02)
                self.assertEqual(px_at(osd_a, 20, 40), 0x0F0F)
                self.assertEqual(px_at(osd_b, 619, 439), 0x0F0F)
                # Top bar (y=0..35) and bottom bar (y=444..479) must NEVER be touched!
                self.assertEqual(px_at(osd_a, 100, 10), 0x55AA)
                self.assertEqual(px_at(osd_b, 100, 460), 0x55AA)
                # Side pillar inside band (x=5, y=200) must be dark bezel 0x112F
                self.assertEqual(px_at(osd_a, 5, 200), 0x112F)

                # 3) Transition to NONE -> band clears to 0x0000 transparent!
                set_disp(0, 3, 0)
                deadline = time.time() + 2.0
                while time.time() < deadline and px_at(osd_a, 20, 40) != 0x0000:
                    time.sleep(0.02)
                self.assertEqual(px_at(osd_a, 20, 40), 0x0000)
                self.assertEqual(px_at(osd_b, 300, 200), 0x0000)
                self.assertEqual(px_at(osd_a, 100, 10), 0x55AA)
            finally:
                proc.send_signal(signal.SIGTERM)
                proc.wait(timeout=3)
            self.assertEqual(proc.returncode, 0)

    def test_native_cacheinsimg_zero_flicker_popup_and_album(self):
        headers = {
            'dlfcn.h': '#define RTLD_DEFAULT ((void *)0)\n#define RTLD_NOW 2\n#define RTLD_NOLOAD 4\nvoid *dlopen(const char *,int);\nvoid *dlsym(void *,const char *);\nint dlclose(void *);\n',
            'stdio.h': '#include <stddef.h>\nint snprintf(char *,size_t,const char *,...);\n',
            'stdlib.h': '#include <stddef.h>\nvoid *calloc(size_t,size_t);\n',
            'string.h': '#include <stddef.h>\nvoid *memcpy(void *,const void *,size_t);\nvoid *memset(void *,int,size_t);\nchar *strcpy(char *,const char *);\n',
            'sys/syscall.h': '#define SYS_gettid 224\n#define SYS_clock_gettime 263\n',
            'time.h': '#define CLOCK_MONOTONIC 1\nstruct timespec {long tv_sec,tv_nsec;};\n',
            'unistd.h': 'long model_syscall(long,...);\n#define syscall model_syscall\n',
        }
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            pv_dir = d / 'pv'
            pv_dir.mkdir()
            # popup.bin: solid red 0xF00F -> RGBA8888 0xFF0000FF (R=0xFF,G=0,B=0,A=0xFF)
            pop_hdr = struct.pack('<IIII48s', 0x56503441, 240, 160, 1, b'popup')
            (pv_dir / 'popup.bin').write_bytes(pop_hdr + b'\x0f\xf0' * (240 * 160))
            # slot_00.bin stays the instant-preview ring sample. current.bin is the one decoded album image.
            alb_hdr = struct.pack('<IIII48s', 0x56503441, 600, 400, 1, b'album0')
            (pv_dir / 'slot_00.bin').write_bytes(alb_hdr + b'\x0f\x0f' * (600 * 400))
            (pv_dir / 'current.bin').write_bytes(alb_hdr + b'\x0f\x0f' * (600 * 400))

            inc = d / 'inc'
            (inc / 'sys').mkdir(parents=True)
            for name, content in headers.items():
                (inc / name).write_text(content, encoding='utf-8')
            for name in ('film_menu.h', 'film_native_view.h', 'ui_trial_trace.h',
                         'film_preview_fmt.h', 'mtf2_native_touch.h', 'film_album_index.h'):
                (inc / name).write_text((ROOT / 'camera' / name).read_text(encoding='utf-8'), encoding='utf-8')

            src = (ROOT / 'camera/native_film_ui_trial3.c').read_text(encoding='utf-8')
            model_c = r'''
#include <stdarg.h>
#include <stddef.h>
extern int open(const char *, int, ...);
extern long read(int, void *, size_t);
extern int close(int);
#define A6400_MTF2_HOST_MOCK 1
#define A6400_PV_DIR_RUNTIME "''' + str(pv_dir) + r'''"
#include "native_film_ui_trial3.c"
static uint32_t mtf2_host_mock_dispatch(int16_t x, int16_t y, int dx, int dy, int hp) {
    (void)x;(void)y;(void)dx;(void)dy;(void)hp; return MTF2_ACT_NONE;
}
static unsigned model_now_ms = 1000;
static int model_layer_is_active = 0;
static float last_img_w = -1.0f, last_img_h = -1.0f;
static int last_param_fmt = -1;
static int invalidates = 0, unlocks = 0;
static uint16_t pop_rgba4444[240 * 160];
static uint16_t alb_rgba4444[608 * 400];
static unsigned char menu_marker[64], ticker_marker[64], pop_bmp[64], alb_bmp[64], pop_int[64], alb_int[64], border_marker[64];
static uint16_t pending_w = 0, pending_h = 0;
static int model_key(void *p, int k, void *d) { (void)p;(void)k;(void)d; return 0; }
static void model_position(const void *p, unsigned char n, short *x, short *y) { (void)p;(void)n;*x=32767;*y=32767; }
static int model_state(const void *p, unsigned char a, unsigned char b) { (void)p;(void)a;(void)b; return 6; }
static void model_layer(void *p, int active) { (void)p; model_layer_is_active = active; }
static void model_unary(void *p) { (void)p; }
static void model_set_layer(void *p, const uint32_t *id) { (void)p;(void)id; }
static void model_place(void *p, const float *xy) { (void)p;(void)xy; }
static void model_size(void *p, float w, float h) { (void)p;(void)w;(void)h; }
static void model_cimg_size(void *p, float w, float h) { (void)p; last_img_w = w; last_img_h = h; }
static void model_color(void *p, const unsigned char *c) { (void)p;(void)c; }
static void model_text(void *p, const char *s) { (void)p;(void)s; }
static void model_param_ctor(void *p, uint16_t w, uint16_t h, int fmt) {
    (void)p; pending_w = w; pending_h = h; last_param_fmt = fmt;
}
static int model_resmgr_create(const void *param, void *img_data) {
    (void)param;
    *(uint16_t *)img_data = pending_w;
    return 1;
}
static void *model_sys_get(void *p) { return (*(uint16_t *)p == 240u) ? pop_int : alb_int; }
static void *model_bmp_get(void *p) { return (p == pop_int) ? pop_bmp : alb_bmp; }
static uint32_t model_res_id(void *p) { return (*(uint16_t *)p == 240u) ? 0x1001u : 0x1002u; }
static int model_bmp_stride(void *p, int16_t *s) { *s = (p == pop_bmp) ? (240 * 2) : (608 * 2); return 0; }
static int model_bmp_fmt(void *p, int *f) { (void)p; *f = 7; return 0; }
static int model_bmp_lock(void *p, int fl, void **out) { (void)fl; *out = (p == pop_bmp) ? (void *)pop_rgba4444 : (void *)alb_rgba4444; return 0; }
static int model_bmp_unlock(void *p) { (void)p; ++unlocks; return 0; }
static void model_sprite_set(void *p, uint32_t id, uint32_t fl) { (void)p;(void)id;(void)fl; }
static void model_sprite_inv(void *p) { (void)p; ++invalidates; }
static int model_open(const char *p, int f) { return open(p, f); }
static void *model_get_key_inst(void) { return ui.keys; }
long model_syscall(long n, ...) {
    if (n == SYS_gettid) return 380;
    if (n == SYS_clock_gettime) {
        va_list ap; va_start(ap, n);
        int clk = va_arg(ap, int);
        struct timespec *t = va_arg(ap, struct timespec *);
        va_end(ap);
        if (clk == CLOCK_MONOTONIC && t) {
            t->tv_sec = (long)(model_now_ms / 1000u);
            t->tv_nsec = (long)((model_now_ms % 1000u) * 1000000u);
            return 0;
        }
    }
    return 0;
}
void n_init(void) {
    memset(&ui, 0, sizeof(ui));
    memset(&ui_theme, 0, sizeof(ui_theme));
    memset(&ui_toast, 0, sizeof(ui_toast));
    memset(&ui_v4, 0, sizeof(ui_v4));
    memset(pop_rgba4444, 0, sizeof(pop_rgba4444));
    memset(alb_rgba4444, 0, sizeof(alb_rgba4444));
    struct film_settings initial = {0, 0, 2, 1};
    const struct film_native_view_ops ops = {visibility, draw, settings};
    film_native_view_init(&ui.view, &ops, 0, &initial);
    ui.system = ui_sys_stub; ui.keys = (void *)0x59150; get_key_instance_fn = model_get_key_inst;
    ui.tid = 380; ui.running = ui.attached = 1;
    ui.layer = menu_marker; ui.ticker = ui.ticker_layer = ticker_marker;
    ui.key = model_key; ui.mouse_position = model_position; ui.mouse_state = model_state;
    ui.layer_active = model_layer; ui.move_last = model_unary; ui.set_layer = model_set_layer;
    ui.set_position = model_place; ui.panel_size = ui.label_size = model_size;
    ui.set_color = model_color; ui.set_text = model_text;
    ui_v4.popup_border = border_marker;
    ui_v4.cache_img_ctor = model_unary;
    ui_v4.cache_img_set_size = model_cimg_size;
    ui_v4.create_param_ctor = model_param_ctor;
    ui_v4.resmgr_create_img = model_resmgr_create;
    ui_v4.img_sys_get = model_sys_get;
    ui_v4.bmp_get_internal = model_bmp_get;
    ui_v4.get_res_id = model_res_id;
    ui_v4.bmp_stride = model_bmp_stride;
    ui_v4.bmp_fmt = model_bmp_fmt;
    ui_v4.bmp_lock = model_bmp_lock;
    ui_v4.bmp_unlock = model_bmp_unlock;
    ui_v4.sprite_set_img = model_sprite_set;
    ui_v4.sprite_invalidate = model_sprite_inv;
    ui_v4.pv_open = model_open;
    ui_v4.pv_read = read;
    ui_v4.pv_close = close;
    a6400_ui_v4_v1.preview_popup_en = 1u;
    a6400_ui_v4_v1.album_count = 1u;
    a6400_ui_v4_v1.album_head = 0u;
    a6400_ui_v4_v1.latest_seq = 0u;
    a6400_ui_v4_v1.display_mode = A6400_DISP_NONE;
    a6400_ui_v4_v1.album_view_seq = 1u;
    a6400_ui_v4_v1.album_view_slot = 0u;
    a6400_ui_v4_v1.album_ready_seq = 1u;
    a6400_ui_v4_v1.album_ready_slot = 0u;
    for(unsigned i=0;i<13u;i++)a6400_ui_v4_v1.album_label[i]="F0000001.JPG"[i];
    model_now_ms = 1000; model_layer_is_active = 0; last_img_w = -1.0f; last_img_h = -1.0f;
    last_param_fmt = -1; invalidates = 0; unlocks = 0;
}
void n_tick(unsigned dt) { model_now_ms += dt; animate(ui.ticker); }
void n_open_album(void) { ui_v4.album_mode = 2; ui_v4.album_idx = 0; v4_album_sync_view(); }
void n_exit_album(void) { v4_exit_album(); }
uint32_t n_pop_px(unsigned idx) { return pop_rgba4444[idx]; }
uint32_t n_alb_px(unsigned idx) { return alb_rgba4444[idx]; }
int n_param_fmt(void) { return last_param_fmt; }
int n_layer_active(void) { return model_layer_is_active; }
int n_img_w(void) { return (int)last_img_w; }
int n_img_h(void) { return (int)last_img_h; }
int n_popup_native(void) { return ui_v4.popup_is_native; }
int n_album_native(void) { return ui_v4.album_is_native; }
'''
            body = model_c.replace('#include "native_film_ui_trial3.c"', src).replace('__attribute__((pcs("aapcs")))', '')
            preamble = 'static unsigned char ui_sys_stub[4096];\n#define _Static_assert(cond, msg)\n'
            (d / 'nmodel.c').write_text(preamble + body, encoding='utf-8')
            so_file = d / 'native_pv.so'
            subprocess.run([
                'cc', '-std=c11', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-DA6400_UI_TRIAL_MS=0', '-DA6400_UI_DISPLAY_MS=0',
                '-I', str(inc), '-I', str(d),
                str(d / 'nmodel.c'), str(ROOT / 'camera/film_native_view.c'),
                str(ROOT / 'camera/film_menu.c'), '-o', str(so_file),
            ], check=True)
            lib = ctypes.CDLL(str(so_file))
            lib.n_pop_px.restype = ctypes.c_uint32
            lib.n_alb_px.restype = ctypes.c_uint32
            lib.n_init()
            v4 = UiV4V1.in_dll(lib, 'a6400_ui_v4_v1')

            # 1) Trigger popup -> native RGBA4444 (fmt=7) 240x160 exact copy, external OSD stays NONE!
            v4.latest_seq = 1
            lib.n_tick(25)
            self.assertEqual(lib.n_popup_native(), 1)
            self.assertEqual(lib.n_param_fmt(), 7)
            self.assertEqual(v4.display_mode, 0)  # external film_preview_osd stays 100% idle!
            self.assertEqual(lib.n_layer_active(), 1)
            self.assertEqual((lib.n_img_w(), lib.n_img_h()), (240, 160))
            self.assertEqual(lib.n_pop_px(0), 0xF00F)
            self.assertEqual(lib.n_pop_px(240 * 159 + 239), 0xF00F)
            # Expires after 3.5s -> hides canvas (0x0) and deactivates layer!
            lib.n_tick(3600)
            self.assertEqual(lib.n_layer_active(), 0)
            self.assertEqual((lib.n_img_w(), lib.n_img_h()), (0, 0))

            # 2) Album bitmap is 448x400: 600x400 preview with width at 3/4 so a
            #    3:2 photo is 3:2 on the 16:9 LCD. Mock row pitch stays 608.
            lib.n_open_album()
            self.assertEqual(lib.n_album_native(), 1)
            self.assertEqual(lib.n_param_fmt(), 7)
            self.assertEqual(v4.display_mode, 0)
            self.assertEqual(lib.n_layer_active(), 1)
            self.assertEqual((lib.n_img_w(), lib.n_img_h()), (448, 400))
            self.assertEqual(lib.n_alb_px(608 * 200 + 0), 0x0F0F)
            self.assertEqual(lib.n_alb_px(608 * 200 + 447), 0x0F0F)
            lib.n_exit_album()
            self.assertEqual(lib.n_layer_active(), 0)
            self.assertEqual((lib.n_img_w(), lib.n_img_h()), (0, 0))

    def test_double_exposure_blend_modes_and_preview(self):
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            worker = d / 'film_job_dexp'
            subprocess.run([
                'make', '-C', str(ROOT / 'camera'), '-f', 'Makefile.film',
                'HOST=1', 'JOB=1', f'BUILD={d / "build"}', f'TARGET={worker}',
            ], check=True, capture_output=True)
            in1 = d / 'shot1.jpg'
            in2 = d / 'shot2.jpg'
            _make_synthetic_jpeg(in1, 320, 240)
            _make_synthetic_jpeg(in2, 320, 240)
            for idx, mode in enumerate(('screen', 'lighten', 'average'), start=1):
                out_jpg = d / f'out_{mode}.jpg'
                pv_bin = d / f'pv_{mode}.bin'
                res = subprocess.run(
                    [str(worker), '--dexp', mode, str(in1), str(in2), str(out_jpg), '--preview', str(pv_bin)],
                    capture_output=True, text=True, check=True,
                )
                self.assertIn(f'FILM_DONE width=320 height=240', res.stdout)
                self.assertIn(f'dexp={idx}', res.stdout)
                self.assertTrue(out_jpg.is_file() and out_jpg.stat().st_size > 1000)
                self.assertEqual(pv_bin.stat().st_size, 556928)


if __name__ == '__main__':
    unittest.main(verbosity=2)
