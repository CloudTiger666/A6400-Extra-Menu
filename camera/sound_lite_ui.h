#ifndef A6400_SOUND_LITE_UI_H
#define A6400_SOUND_LITE_UI_H

/* Minimalist Matte Black (#121213) + Warm Cream (#F2EDE4) Sound Deck UI for Sound Lite.
 * Designed strictly for Sony's native white GEN_Label + lightweight widget budget:
 *   - Only 10 GEN_Labels & 20 GEN_Panels total (fewer than V4.3 SLEM -> 0 lag!)
 *   - High-contrast white text on matte obsidian, framed & accented in warm cream (#F2EDE4).
 *   - Supports up to 16 slots (OFF + 3 built-in + SD card .pcm files in /mnt/A6400/SOUNDS). */

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#define SL_MAX_SLOTS 16
#define SL_VIS_ROWS  6
#define SL_WAVE_BARS 8
#define SL_PCM_MAX   29760

enum sl_slot_kind {
    SL_KIND_OFF = 0,
    SL_KIND_BUILTIN_ID = 1,
    SL_KIND_SD_PCM = 2
};

struct sl_slot {
    int kind;
    int sound_id; /* 0x34, 0x33, 0x35 */
    char name[28];
    char dur[10];
    char path[300];
    unsigned char wave[SL_WAVE_BARS]; /* 8..54 px bar height */
};

struct sl_state {
    int initialized;
    int count;
    int active;      /* currently active slot index (0 = OFF) */
    int scroll;      /* top visible row index */
    int hi_repeat;   /* 1 = Hi+ continuous repeat ON, 0 = single shot */
    int has_base_pcm;
    unsigned char base_pcm[SL_PCM_MAX];
    struct sl_slot slots[SL_MAX_SLOTS];

    /* Touch & key edge tracking */
    int touch_down;
    short touch_x, touch_y;
    int last_prev, last_next, last_ok, last_c1;

    /* Lean native widgets (20 panels + 10 labels total -> zero compositor lag) */
    void *outer_bdr, *left_bg, *right_bg, *v_div, *hdr_div;
    void *row_bgs[SL_VIS_ROWS];
    void *row_acc; /* single cream accent bar on the active row */
    void *wave_bdr, *wave_bg;
    void *wave_bars[SL_WAVE_BARS];
    void *btn1_bdr, *btn1_bg, *btn2_bdr, *btn2_bg;

    void *l_hdr;
    void *row_lbl[SL_VIS_ROWS];
    void *r_name, *r_btn1, *r_btn2;
};

static struct sl_state sl;

#if defined(A6400_UI_LANG_SC) && A6400_UI_LANG_SC
#define SL_S_BOOT_TOAST   "Sound Lite 已就绪"
#define SL_S_OFF_NAME     "关闭快门音效"
#define SL_S_B1_NAME      "经典快门 1"
#define SL_S_B2_NAME      "清脆快门 2"
#define SL_S_B3_NAME      "轻快快门 3"
#define SL_S_BTN_PLAY     "▶ 试听快门音"
#define SL_S_BTN_HI_ON    "Hi+ 连拍：开"
#define SL_S_BTN_HI_OFF   "Hi+ 连拍：关"
#else
#define SL_S_BOOT_TOAST   "Sound Lite 已就緒"
#define SL_S_OFF_NAME     "關閉快門音效"
#define SL_S_B1_NAME      "經典快門 1"
#define SL_S_B2_NAME      "清脆快門 2"
#define SL_S_B3_NAME      "輕快快門 3"
#define SL_S_BTN_PLAY     "▶ 試聽快門音"
#define SL_S_BTN_HI_ON    "Hi+ 連拍：開"
#define SL_S_BTN_HI_OFF   "Hi+ 連拍：關"
#endif

static void sl_compute_wave(const unsigned char *pcm, int bytes, unsigned char out[SL_WAVE_BARS]) {
    if (!pcm || bytes < 64) {
        for (int i = 0; i < SL_WAVE_BARS; i++) out[i] = 8;
        return;
    }
    int samples = bytes / 2;
    int step = samples / SL_WAVE_BARS;
    if (step < 1) step = 1;
    int max_pk = 1;
    int peaks[SL_WAVE_BARS];
    for (int b = 0; b < SL_WAVE_BARS; b++) {
        int s0 = b * step;
        int s1 = s0 + step;
        if (s1 > samples) s1 = samples;
        int pk = 0;
        for (int s = s0; s < s1; s++) {
            int16_t v = (int16_t)((unsigned)pcm[s * 2] | ((unsigned)pcm[s * 2 + 1] << 8));
            int av = v < 0 ? -v : v;
            if (av > pk) pk = av;
        }
        peaks[b] = pk;
        if (pk > max_pk) max_pk = pk;
    }
    for (int b = 0; b < SL_WAVE_BARS; b++) {
        int h = 8 + (peaks[b] * 46) / max_pk;
        if (h < 8) h = 8;
        if (h > 54) h = 54;
        out[b] = (unsigned char)h;
    }
}

static int sl_read_file_exact(const char *path, unsigned char *buf, int max_bytes) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    int total = 0;
    while (total < max_bytes) {
        ssize_t r = read(fd, buf + total, (size_t)(max_bytes - total));
        if (r < 0) { close(fd); return -1; }
        if (r == 0) break;
        total += (int)r;
    }
    close(fd);
    return total;
}

static int sl_write_wps_pbc_inplace(const unsigned char *buf, int valid_bytes) {
    static unsigned char padded[SL_PCM_MAX];
    if (valid_bytes <= 0 || valid_bytes > SL_PCM_MAX) return -1;
    for (int i = 0; i < valid_bytes; i++) padded[i] = buf[i];
    for (int i = valid_bytes; i < SL_PCM_MAX; i++) padded[i] = 0;
    int dfd = open("/tmp_bt/a6400-charo-shutter.pcm", O_WRONLY, 0);
    if (dfd < 0) return -1;
    int wrote = 0;
    while (wrote < SL_PCM_MAX) {
        ssize_t w = write(dfd, padded + wrote, (size_t)(SL_PCM_MAX - wrote));
        if (w <= 0) { close(dfd); return -1; }
        wrote += (int)w;
    }
    close(dfd);
    return 0;
}

static void sl_format_dur(int bytes, char out[10]) {
    if (bytes <= 0) { strcpy(out, "0.25s"); return; }
    int ms = (bytes * 100) / 32000;
    if (ms < 1) ms = 1;
    snprintf(out, 10, "%d.%02ds", ms / 100, ms % 100);
}

static int sl_ends_with_pcm(const char *name) {
    size_t n = strlen(name);
    if (n < 5 || name[0] == '.') return 0;
    const char *ext = name + n - 4;
    return (ext[0] == '.' &&
            (ext[1] == 'p' || ext[1] == 'P') &&
            (ext[2] == 'c' || ext[2] == 'C') &&
            (ext[3] == 'm' || ext[3] == 'M'));
}

static void sl_scan_slots_ex(int scan_sd) {
    unsigned char tmp[SL_PCM_MAX];
    sl.count = 0;

    /* Slot 0: OFF */
    {
        struct sl_slot *s = &sl.slots[sl.count++];
        s->kind = SL_KIND_OFF;
        s->sound_id = 0;
        snprintf(s->name, sizeof(s->name), "%s", SL_S_OFF_NAME);
        snprintf(s->dur, sizeof(s->dur), "OFF");
        s->path[0] = 0;
        for (int i = 0; i < SL_WAVE_BARS; i++) s->wave[i] = 8;
    }

    if (!sl.has_base_pcm) {
        int n = sl_read_file_exact("/tmp_bt/a6400-charo-shutter.pcm", sl.base_pcm, SL_PCM_MAX);
        if (n > 0) sl.has_base_pcm = n;
    }
    if (sl.has_base_pcm > 0) {
        struct sl_slot *s = &sl.slots[sl.count++];
        s->kind = SL_KIND_BUILTIN_ID;
        s->sound_id = 0x34;
        snprintf(s->name, sizeof(s->name), "%s", SL_S_B1_NAME);
        sl_format_dur(sl.has_base_pcm, s->dur);
        snprintf(s->path, sizeof(s->path), "/tmp_bt/a6400-charo-shutter.pcm");
        sl_compute_wave(sl.base_pcm, sl.has_base_pcm, s->wave);
    }
#if defined(A6400_CHARO_MULTI) && A6400_CHARO_MULTI
    {
        int n = sl_read_file_exact("/tmp_bt/a6400-charo-connect.pcm", tmp, SL_PCM_MAX);
        if (n > 0) {
            struct sl_slot *s = &sl.slots[sl.count++];
            s->kind = SL_KIND_BUILTIN_ID;
            s->sound_id = 0x33;
            snprintf(s->name, sizeof(s->name), "%s", SL_S_B2_NAME);
            sl_format_dur(n, s->dur);
            s->path[0] = 0;
            sl_compute_wave(tmp, n, s->wave);
        }
        n = sl_read_file_exact("/tmp_bt/a6400-charo-push.pcm", tmp, SL_PCM_MAX);
        if (n > 0) {
            struct sl_slot *s = &sl.slots[sl.count++];
            s->kind = SL_KIND_BUILTIN_ID;
            s->sound_id = 0x35;
            snprintf(s->name, sizeof(s->name), "%s", SL_S_B3_NAME);
            sl_format_dur(n, s->dur);
            s->path[0] = 0;
            sl_compute_wave(tmp, n, s->wave);
        }
    }
#endif

    if (scan_sd && sl.count < SL_MAX_SLOTS) {
        DIR *dir = opendir("/tmp_bt/a6400-sounds");
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != 0 && sl.count < SL_MAX_SLOTS) {
                if (!sl_ends_with_pcm(ent->d_name)) continue;
                char full[300];
                snprintf(full, sizeof(full), "/tmp_bt/a6400-sounds/%s", ent->d_name);
                int n = sl_read_file_exact(full, tmp, SL_PCM_MAX);
                if (n <= 0) continue;
                struct sl_slot *s = &sl.slots[sl.count++];
                s->kind = SL_KIND_SD_PCM;
                s->sound_id = 0x34;
                size_t len = strlen(ent->d_name);
                if (len > 4) len -= 4;
                if (len > sizeof(s->name) - 1) len = sizeof(s->name) - 1;
                memcpy(s->name, ent->d_name, len);
                s->name[len] = 0;
                sl_format_dur(n, s->dur);
                snprintf(s->path, sizeof(s->path), "%s", full);
                sl_compute_wave(tmp, n, s->wave);
            }
            closedir(dir);
        }
    }

    if (sl.active >= sl.count) sl.active = sl.count > 1 ? 1 : 0;
    if (sl.active == 0 && sl.count > 1 && !sl.initialized) sl.active = 1;
    sl.initialized = 1;
}

static void sl_scan_slots(void) {
    sl_scan_slots_ex(1);
}

static int sl_activate_slot(int idx) {
    if (idx < 0 || idx >= sl.count) return 0;
    sl.active = idx;
    if (sl.active < sl.scroll) sl.scroll = sl.active;
    if (sl.active >= sl.scroll + SL_VIS_ROWS) sl.scroll = sl.active - SL_VIS_ROWS + 1;
    if (sl.scroll < 0) sl.scroll = 0;

    const struct sl_slot *s = &sl.slots[idx];
    if (s->kind == SL_KIND_OFF) return 0;
    if (s->kind == SL_KIND_BUILTIN_ID) {
        if (s->sound_id == 0x34 && sl.has_base_pcm > 0) {
            sl_write_wps_pbc_inplace(sl.base_pcm, sl.has_base_pcm);
        }
        return s->sound_id;
    }
    if (s->kind == SL_KIND_SD_PCM) {
        unsigned char tmp[SL_PCM_MAX];
        int n = sl_read_file_exact(s->path, tmp, SL_PCM_MAX);
        if (n > 0) {
            sl_write_wps_pbc_inplace(tmp, n);
            return 0x34;
        }
    }
    return 0;
}

static int sl_current_sound_id(void) {
    if (sl.active <= 0 || sl.active >= sl.count) return 0;
    return sl.slots[sl.active].sound_id;
}

static void sl_hide_widgets(void) {
    static const unsigned char clear_c[4] = {0, 0, 0, 0};
    void *panels[] = {
        sl.outer_bdr, sl.left_bg, sl.right_bg, sl.v_div, sl.hdr_div,
        sl.row_acc, sl.wave_bdr, sl.wave_bg, sl.btn1_bdr, sl.btn1_bg, sl.btn2_bdr, sl.btn2_bg
    };
    for (unsigned i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
        if (panels[i]) { place(panels[i], 0, 0, 0, 0, 0); paint(panels[i], clear_c); }
    }
    for (int i = 0; i < SL_VIS_ROWS; i++) {
        if (sl.row_bgs[i]) { place(sl.row_bgs[i], 0, 0, 0, 0, 0); paint(sl.row_bgs[i], clear_c); }
        if (sl.row_lbl[i]) { place(sl.row_lbl[i], 1, 0, 0, 0, 0); write_lbl(sl.row_lbl[i], ""); }
    }
    for (int i = 0; i < SL_WAVE_BARS; i++) {
        if (sl.wave_bars[i]) { place(sl.wave_bars[i], 0, 0, 0, 0, 0); paint(sl.wave_bars[i], clear_c); }
    }
    void *lbls[] = { sl.l_hdr, sl.r_name, sl.r_btn1, sl.r_btn2 };
    for (unsigned i = 0; i < sizeof(lbls) / sizeof(lbls[0]); i++) {
        if (lbls[i]) { place(lbls[i], 1, 0, 0, 0, 0); write_lbl(lbls[i], ""); }
    }
}

static int sl_build_widgets(void *(*make_fn)(int)) {
    sl.outer_bdr = make_fn(0);
    sl.left_bg   = make_fn(0);
    sl.right_bg  = make_fn(0);
    sl.v_div     = make_fn(0);
    sl.hdr_div   = make_fn(0);
    sl.row_acc   = make_fn(0);
    sl.wave_bdr  = make_fn(0);
    sl.wave_bg   = make_fn(0);
    sl.btn1_bdr  = make_fn(0);
    sl.btn1_bg   = make_fn(0);
    sl.btn2_bdr  = make_fn(0);
    sl.btn2_bg   = make_fn(0);
    if (!sl.outer_bdr || !sl.left_bg || !sl.right_bg || !sl.v_div || !sl.hdr_div ||
        !sl.row_acc || !sl.wave_bdr || !sl.wave_bg || !sl.btn1_bdr || !sl.btn1_bg ||
        !sl.btn2_bdr || !sl.btn2_bg)
        return -5;
    for (int i = 0; i < SL_VIS_ROWS; i++) {
        sl.row_bgs[i] = make_fn(0);
        if (!sl.row_bgs[i]) return -5;
    }
    for (int i = 0; i < SL_WAVE_BARS; i++) {
        sl.wave_bars[i] = make_fn(0);
        if (!sl.wave_bars[i]) return -5;
    }
    sl.l_hdr  = make_fn(1);
    sl.r_name = make_fn(1);
    sl.r_btn1 = make_fn(1);
    sl.r_btn2 = make_fn(1);
    if (!sl.l_hdr || !sl.r_name || !sl.r_btn1 || !sl.r_btn2) return -5;
    for (int i = 0; i < SL_VIS_ROWS; i++) {
        sl.row_lbl[i] = make_fn(1);
        if (!sl.row_lbl[i]) return -5;
    }
    sl.hi_repeat = 1;
    sl_scan_slots_ex(0);
    return 0;
}

static void sl_draw_deck(void) {
    /* Minimalist Warm Cream (#F2EDE4) + Matte Obsidian (#121213)
     * 100% compatible with Sony's native white GEN_Label -> crystal clear & zero lag! */
    static const unsigned char c_scrim[4]     = {  0,   0,   0, 175};
    static const unsigned char c_cream[4]     = {242, 237, 228, 255}; /* #F2EDE4 warm cream */
    static const unsigned char c_cream_dim[4] = {150, 144, 134, 220}; /* muted warm stone */
    static const unsigned char c_bg_left[4]   = { 18,  18,  20, 248}; /* #121214 matte black */
    static const unsigned char c_bg_right[4]  = { 12,  12,  14, 248}; /* #0C0C0E deep black */
    static const unsigned char c_row_idle[4]  = { 24,  24,  27, 255}; /* subtle card */
    static const unsigned char c_row_sel[4]   = { 44,  42,  38, 255}; /* warm charcoal active */
    static const unsigned char c_btn_bg[4]    = { 34,  33,  30, 255}; /* dark warm button */
    static const unsigned char c_div[4]       = { 68,  65,  58, 235}; /* hairline */
    static const unsigned char c_clear[4]     = {  0,   0,   0,   0};

    place(ui.background, 0, 0, 0, 640, 480);  paint(ui.background, c_scrim);
    /* Crisp 2px Warm Cream outer frame around Matte Black Sound Deck */
    place(sl.outer_bdr,  0, 22, 26, 596, 428); paint(sl.outer_bdr, c_cream);
    place(sl.left_bg,    0, 24, 28, 342, 424); paint(sl.left_bg, c_bg_left);
    place(sl.right_bg,   0, 366, 28, 250, 424); paint(sl.right_bg, c_bg_right);
    place(sl.v_div,      0, 366, 28, 1, 424);   paint(sl.v_div, c_div);
    place(sl.hdr_div,    0, 36, 68, 318, 1);    paint(sl.hdr_div, c_div);

    char hdr[48];
    snprintf(hdr, sizeof(hdr), "SOUND DECK   %02d / %02d", sl.active, sl.count > 0 ? sl.count - 1 : 0);
    place(sl.l_hdr, 1, 40, 36, 312, 28);
    write_lbl(sl.l_hdr, hdr);

    int acc_drawn = 0;
    for (int r = 0; r < SL_VIS_ROWS; r++) {
        int idx = sl.scroll + r;
        int ry = 78 + r * 58;
        if (idx >= sl.count) {
            place(sl.row_bgs[r], 0, 0, 0, 0, 0); paint(sl.row_bgs[r], c_clear);
            place(sl.row_lbl[r], 1, 0, 0, 0, 0); write_lbl(sl.row_lbl[r], "");
            continue;
        }
        int sel = (idx == sl.active);
        const struct sl_slot *s = &sl.slots[idx];
        place(sl.row_bgs[r], 0, 36, ry, 318, 50);
        paint(sl.row_bgs[r], sel ? c_row_sel : c_row_idle);
        if (sel) {
            place(sl.row_acc, 0, 36, ry, 6, 50);
            paint(sl.row_acc, c_cream);
            acc_drawn = 1;
        }
        char line[48];
        snprintf(line, sizeof(line), "%02d  %s  (%s)", idx, s->name, s->dur);
        place(sl.row_lbl[r], 1, 52, ry + 11, 294, 28);
        write_lbl(sl.row_lbl[r], line);
    }
    if (!acc_drawn) { place(sl.row_acc, 0, 0, 0, 0, 0); paint(sl.row_acc, c_clear); }

    /* Right Panel: Active Slot Name + Warm Cream Waveform + 2 Cream-Bordered Buttons */
    const struct sl_slot *cur = (sl.active >= 0 && sl.active < sl.count) ? &sl.slots[sl.active] : &sl.slots[0];
    char rtitle[48];
    snprintf(rtitle, sizeof(rtitle), "%02d  %s", sl.active, cur->name);
    place(sl.r_name, 1, 384, 52, 218, 34);
    write_lbl(sl.r_name, rtitle);

    place(sl.wave_bdr, 0, 384, 114, 216, 78); paint(sl.wave_bdr, c_cream_dim);
    place(sl.wave_bg,  0, 385, 115, 214, 76); paint(sl.wave_bg, c_bg_left);
    for (int b = 0; b < SL_WAVE_BARS; b++) {
        int bh = cur->wave[b];
        int bx = 400 + b * 24;
        int by = 115 + (76 - bh) / 2;
        place(sl.wave_bars[b], 0, bx, by, 10, bh);
        paint(sl.wave_bars[b], (sl.active > 0 && bh >= 16) ? c_cream : c_cream_dim);
    }

    place(sl.btn1_bdr, 0, 384, 250, 216, 52); paint(sl.btn1_bdr, c_cream);
    place(sl.btn1_bg,  0, 386, 252, 212, 48); paint(sl.btn1_bg, c_btn_bg);
    place(sl.r_btn1,   1, 398, 263, 192, 28); write_lbl(sl.r_btn1, SL_S_BTN_PLAY);

    place(sl.btn2_bdr, 0, 384, 320, 216, 52); paint(sl.btn2_bdr, sl.hi_repeat ? c_cream : c_div);
    place(sl.btn2_bg,  0, 386, 322, 212, 48); paint(sl.btn2_bg, c_row_idle);
    place(sl.r_btn2,   1, 398, 333, 192, 28); write_lbl(sl.r_btn2, sl.hi_repeat ? SL_S_BTN_HI_ON : SL_S_BTN_HI_OFF);
}

#endif
