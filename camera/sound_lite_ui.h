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

    /* Lean native widgets (24 panels + 12 labels total -> zero compositor lag, 100% touch) */
    void *outer_bdr, *left_bg, *right_bg, *v_div, *hdr_div;
    void *row_bgs[SL_VIS_ROWS];
    void *row_acc; /* single cream accent bar on the active row */
    void *pg_prev_bg, *pg_next_bg;
    void *wave_bdr, *wave_bg;
    void *wave_bars[SL_WAVE_BARS];
    void *btn1_bdr, *btn1_bg, *btn2_bdr, *btn2_bg, *btn3_bdr, *btn3_bg;

    void *l_hdr, *l_pager;
    void *row_lbl[SL_VIS_ROWS];
    void *r_name, *r_btn1, *r_btn2, *r_btn3;
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
#define SL_S_BTN_CLOSE    "✕ 关闭菜单"
#define SL_S_PAGER        "◀ 上一页        下一页 ▶"
#else
#define SL_S_BOOT_TOAST   "Sound Lite 已就緒"
#define SL_S_OFF_NAME     "關閉快門音效"
#define SL_S_B1_NAME      "經典快門 1"
#define SL_S_B2_NAME      "清脆快門 2"
#define SL_S_B3_NAME      "輕快快門 3"
#define SL_S_BTN_PLAY     "▶ 試聽快門音"
#define SL_S_BTN_HI_ON    "Hi+ 連拍：開"
#define SL_S_BTN_HI_OFF   "Hi+ 連拍：關"
#define SL_S_BTN_CLOSE    "✕ 關閉選單"
#define SL_S_PAGER        "◀ 上一頁        下一頁 ▶"
#endif

static void sl_analyze_sds(const unsigned char *raw, int bytes, unsigned char out_wave[SL_WAVE_BARS], char out_dur[10]) {
    int total_frames = (raw && bytes >= 32) ? (bytes / 32) : 0;
    if (total_frames <= 0) {
        for (int i = 0; i < SL_WAVE_BARS; i++) out_wave[i] = 8;
        strcpy(out_dur, "0.25s");
        return;
    }
    static short dec[930 * 31];
    int max_f = total_frames > 930 ? 930 : total_frames;
    int prev = 0;
    int active_samples = 0;
    int idx = 0;
    for (int f = 0; f < max_f; f++) {
        unsigned char hdr = raw[f * 32];
        int mode = (hdr >> 4) & 0xF;
        int sh = hdr & 0xF;
        if (sh > 8) sh = 8;
        for (int j = 1; j < 32; j++) {
            int v = (signed char)raw[f * 32 + j];
            int pred = (mode == 0) ? 0 : ((prev * 15) / 16);
            int s = pred + (v << sh);
            if (s < -32768) s = -32768;
            if (s > 32767) s = 32767;
            dec[idx++] = (short)s;
            prev = s;
            if (s > 120 || s < -120) active_samples = idx;
        }
    }
    if (active_samples < 31) active_samples = idx;
    int centisec = (active_samples * 100 + 12000) / 24000;
    if (centisec < 1) centisec = 1;
    snprintf(out_dur, 10, "%d.%02ds", centisec / 100, centisec % 100);

    int step = active_samples / SL_WAVE_BARS;
    if (step < 1) step = 1;
    int max_pk = 1;
    int peaks[SL_WAVE_BARS];
    for (int b = 0; b < SL_WAVE_BARS; b++) {
        int s0 = b * step;
        int s1 = (b == SL_WAVE_BARS - 1) ? active_samples : (s0 + step);
        if (s1 > active_samples) s1 = active_samples;
        int pk = 0;
        for (int s = s0; s < s1; s++) {
            int av = dec[s] < 0 ? -dec[s] : dec[s];
            if (av > pk) pk = av;
        }
        peaks[b] = pk;
        if (pk > max_pk) max_pk = pk;
    }
    for (int b = 0; b < SL_WAVE_BARS; b++) {
        int h = 8 + (peaks[b] * 46) / max_pk;
        if (h < 8) h = 8;
        if (h > 54) h = 54;
        out_wave[b] = (unsigned char)h;
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

static int sl_ends_with_pcm(const char *name) {
    size_t n = strlen(name);
    if (n < 5 || name[0] == '.') return 0;
    const char *ext = name + n - 4;
    return (ext[0] == '.' &&
            (ext[1] == 'p' || ext[1] == 'P') &&
            (ext[2] == 'c' || ext[2] == 'C') &&
            (ext[3] == 'm' || ext[3] == 'M'));
}

/* Dedicated 1-based appBeepId per SD slot (matching sl_sd_bins[] in film_capture_daemon.c)
 * so Sony's ObjSystemSound per-BeepID RAM cache never collides across custom sounds. */
static const int sl_sd_beep_ids[12] = {
    0x31, /* 0: Dslr_Emnt_Wifi_Toggle.bin */
    0x32, /* 1: Dslr_Emnt_Wifi_Toggle_Off.bin */
    0x2D, /* 2: Dslr_Emnt_SelfTimer_2sec.bin */
    0x0E, /* 3: Dslr_Emnt_SelfTimer_RemoteRelease.bin */
    0x1D, /* 4: Dslr_Emnt_Select_SwitchMode.bin */
    0x11, /* 5: Dslr_Emnt_Menu_Open_Option_On.bin */
    0x12, /* 6: Dslr_Emnt_Menu_Close_Option_Off.bin */
    0x27, /* 7: Dslr_Emnt_Back_Cancel.bin */
    0x28, /* 8: Dslr_Emnt_T_Back_Cancel.bin */
    0x29, /* 9: Dslr_Emnt_T_On.bin */
    0x2A, /* 10: Dslr_Emnt_T_Off.bin */
    0x2B  /* 11: Dslr_Emnt_Move_Up_Wheel_Up.bin */
};

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
        snprintf(s->path, sizeof(s->path), "/tmp_bt/a6400-charo-shutter.pcm");
        sl_analyze_sds(sl.base_pcm, sl.has_base_pcm, s->wave, s->dur);
    }
#if defined(A6400_CHARO_MULTI) && A6400_CHARO_MULTI
    {
        int n = sl_read_file_exact("/tmp_bt/a6400-charo-connect.pcm", tmp, SL_PCM_MAX);
        if (n > 0) {
            struct sl_slot *s = &sl.slots[sl.count++];
            s->kind = SL_KIND_BUILTIN_ID;
            s->sound_id = 0x33;
            snprintf(s->name, sizeof(s->name), "%s", SL_S_B2_NAME);
            s->path[0] = 0;
            sl_analyze_sds(tmp, n, s->wave, s->dur);
        }
        n = sl_read_file_exact("/tmp_bt/a6400-charo-push.pcm", tmp, SL_PCM_MAX);
        if (n > 0) {
            struct sl_slot *s = &sl.slots[sl.count++];
            s->kind = SL_KIND_BUILTIN_ID;
            s->sound_id = 0x35;
            snprintf(s->name, sizeof(s->name), "%s", SL_S_B3_NAME);
            s->path[0] = 0;
            sl_analyze_sds(tmp, n, s->wave, s->dur);
        }
    }
#endif

    if (scan_sd && sl.count < SL_MAX_SLOTS) {
        DIR *dir = opendir("/tmp_bt/a6400-sounds");
        if (dir) {
            char sd_names[12][64];
            int n_sd = 0;
            struct dirent *ent;
            while ((ent = readdir(dir)) != 0 && n_sd < 12) {
                if (!sl_ends_with_pcm(ent->d_name)) continue;
                size_t nl = strlen(ent->d_name);
                if (nl >= sizeof(sd_names[0])) continue;
                memcpy(sd_names[n_sd], ent->d_name, nl + 1);
                n_sd++;
            }
            closedir(dir);
            for (int a = 0; a < n_sd; ++a) {
                for (int b = a + 1; b < n_sd; ++b) {
                    if (strcmp(sd_names[a], sd_names[b]) > 0) {
                        char sw[64];
                        memcpy(sw, sd_names[a], sizeof(sw));
                        memcpy(sd_names[a], sd_names[b], sizeof(sw));
                        memcpy(sd_names[b], sw, sizeof(sw));
                    }
                }
            }
            for (int i = 0; i < n_sd && sl.count < SL_MAX_SLOTS; ++i) {
                char full[300];
                snprintf(full, sizeof(full), "/tmp_bt/a6400-sounds/%.63s", sd_names[i]);
                int n = sl_read_file_exact(full, tmp, SL_PCM_MAX);
                if (n <= 0) continue;
                struct sl_slot *s = &sl.slots[sl.count++];
                s->kind = SL_KIND_SD_PCM;
                s->sound_id = sl_sd_beep_ids[i];
                size_t len = strlen(sd_names[i]);
                if (len > 4) len -= 4;
                if (len > sizeof(s->name) - 1) len = sizeof(s->name) - 1;
                memcpy(s->name, sd_names[i], len);
                s->name[len] = 0;
                snprintf(s->path, sizeof(s->path), "%s", full);
                sl_analyze_sds(tmp, n, s->wave, s->dur);
            }
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
    return s->sound_id;
}

static int sl_current_sound_id(void) {
    if (sl.active <= 0 || sl.active >= sl.count) return 0;
    return sl.slots[sl.active].sound_id;
}

static void sl_hide_widgets(void) {
    static const unsigned char clear_c[4] = {0, 0, 0, 0};
    void *panels[] = {
        sl.outer_bdr, sl.left_bg, sl.right_bg, sl.v_div, sl.hdr_div,
        sl.row_acc, sl.pg_prev_bg, sl.pg_next_bg,
        sl.wave_bdr, sl.wave_bg,
        sl.btn1_bdr, sl.btn1_bg, sl.btn2_bdr, sl.btn2_bg, sl.btn3_bdr, sl.btn3_bg
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
    void *lbls[] = { sl.l_hdr, sl.l_pager, sl.r_name, sl.r_btn1, sl.r_btn2, sl.r_btn3 };
    for (unsigned i = 0; i < sizeof(lbls) / sizeof(lbls[0]); i++) {
        if (lbls[i]) { place(lbls[i], 1, 0, 0, 0, 0); write_lbl(lbls[i], ""); }
    }
}

static int sl_build_widgets(void *(*make_fn)(int)) {
    sl.outer_bdr  = make_fn(0);
    sl.left_bg    = make_fn(0);
    sl.right_bg   = make_fn(0);
    sl.v_div      = make_fn(0);
    sl.hdr_div    = make_fn(0);
    sl.row_acc    = make_fn(0);
    sl.pg_prev_bg = make_fn(0);
    sl.pg_next_bg = make_fn(0);
    sl.wave_bdr   = make_fn(0);
    sl.wave_bg    = make_fn(0);
    sl.btn1_bdr   = make_fn(0);
    sl.btn1_bg    = make_fn(0);
    sl.btn2_bdr   = make_fn(0);
    sl.btn2_bg    = make_fn(0);
    sl.btn3_bdr   = make_fn(0);
    sl.btn3_bg    = make_fn(0);
    if (!sl.outer_bdr || !sl.left_bg || !sl.right_bg || !sl.v_div || !sl.hdr_div ||
        !sl.row_acc || !sl.pg_prev_bg || !sl.pg_next_bg ||
        !sl.wave_bdr || !sl.wave_bg || !sl.btn1_bdr || !sl.btn1_bg ||
        !sl.btn2_bdr || !sl.btn2_bg || !sl.btn3_bdr || !sl.btn3_bg)
        return -5;
    for (int i = 0; i < SL_VIS_ROWS; i++) {
        sl.row_bgs[i] = make_fn(0);
        if (!sl.row_bgs[i]) return -5;
    }
    for (int i = 0; i < SL_WAVE_BARS; i++) {
        sl.wave_bars[i] = make_fn(0);
        if (!sl.wave_bars[i]) return -5;
    }
    sl.l_hdr   = make_fn(1);
    sl.l_pager = make_fn(1);
    sl.r_name  = make_fn(1);
    sl.r_btn1  = make_fn(1);
    sl.r_btn2  = make_fn(1);
    sl.r_btn3  = make_fn(1);
    if (!sl.l_hdr || !sl.l_pager || !sl.r_name || !sl.r_btn1 || !sl.r_btn2 || !sl.r_btn3) return -5;
    for (int i = 0; i < SL_VIS_ROWS; i++) {
        sl.row_lbl[i] = make_fn(1);
        if (!sl.row_lbl[i]) return -5;
    }
    sl.hi_repeat = 1;
    sl_scan_slots_ex(0);
    return 0;
}

static void sl_draw_deck(void) {
    /* Minimalist Warm Cream (#F2EDE4) + Matte Obsidian (#121213), 100% Touch-Only */
    static const unsigned char c_scrim[4]     = {  0,   0,   0, 175};
    static const unsigned char c_cream[4]     = {242, 237, 228, 255}; /* #F2EDE4 warm cream */
    static const unsigned char c_cream_dim[4] = {150, 144, 134, 220}; /* muted warm stone */
    static const unsigned char c_bg_left[4]   = { 18,  18,  20, 248}; /* #121214 matte black */
    static const unsigned char c_bg_right[4]  = { 12,  12,  14, 248}; /* #0C0C0E deep black */
    static const unsigned char c_row_idle[4]  = { 24,  24,  27, 255};
    static const unsigned char c_row_sel[4]   = { 44,  42,  38, 255};
    static const unsigned char c_btn_bg[4]    = { 34,  33,  30, 255};
    static const unsigned char c_close_bg[4]  = { 54,  26,  26, 255};
    static const unsigned char c_close_bdr[4] = {220, 110, 100, 255};
    static const unsigned char c_div[4]       = { 68,  65,  58, 235};
    static const unsigned char c_clear[4]     = {  0,   0,   0,   0};

    place(ui.background, 0, 0, 0, 640, 480);  paint(ui.background, c_scrim);
    place(sl.outer_bdr,  0, 22, 26, 596, 428); paint(sl.outer_bdr, c_cream);
    place(sl.left_bg,    0, 24, 28, 342, 424); paint(sl.left_bg, c_bg_left);
    place(sl.right_bg,   0, 366, 28, 250, 424); paint(sl.right_bg, c_bg_right);
    place(sl.v_div,      0, 366, 28, 1, 424);   paint(sl.v_div, c_div);
    place(sl.hdr_div,    0, 36, 66, 318, 1);    paint(sl.hdr_div, c_div);

    char hdr[48];
    snprintf(hdr, sizeof(hdr), "SOUND DECK   %02d / %02d", sl.active, sl.count > 0 ? sl.count - 1 : 0);
    place(sl.l_hdr, 1, 40, 35, 312, 26);
    write_lbl(sl.l_hdr, hdr);

    int acc_drawn = 0;
    for (int r = 0; r < SL_VIS_ROWS; r++) {
        int idx = sl.scroll + r;
        int ry = 74 + r * 52;
        if (idx >= sl.count) {
            place(sl.row_bgs[r], 0, 0, 0, 0, 0); paint(sl.row_bgs[r], c_clear);
            place(sl.row_lbl[r], 1, 0, 0, 0, 0); write_lbl(sl.row_lbl[r], "");
            continue;
        }
        int sel = (idx == sl.active);
        const struct sl_slot *s = &sl.slots[idx];
        place(sl.row_bgs[r], 0, 36, ry, 318, 46);
        paint(sl.row_bgs[r], sel ? c_row_sel : c_row_idle);
        if (sel) {
            place(sl.row_acc, 0, 36, ry, 6, 46);
            paint(sl.row_acc, c_cream);
            acc_drawn = 1;
        }
        char line[48];
        snprintf(line, sizeof(line), "%02d  %s (%s)", idx, s->name, s->dur);
        place(sl.row_lbl[r], 1, 50, ry + 10, 296, 26);
        write_lbl(sl.row_lbl[r], line);
    }
    if (!acc_drawn) { place(sl.row_acc, 0, 0, 0, 0, 0); paint(sl.row_acc, c_clear); }

    /* Left bottom touch pager (prev / next page) */
    place(sl.pg_prev_bg, 0, 36, 396, 152, 42); paint(sl.pg_prev_bg, sl.scroll > 0 ? c_btn_bg : c_row_idle);
    place(sl.pg_next_bg, 0, 202, 396, 152, 42); paint(sl.pg_next_bg, (sl.scroll + SL_VIS_ROWS < sl.count) ? c_btn_bg : c_row_idle);
    place(sl.l_pager,    1, 52, 404, 290, 26); write_lbl(sl.l_pager, SL_S_PAGER);

    /* Right Panel: Active Slot Name + Warm Cream Waveform + 3 Touch Buttons (Play / Hi+ / Close) */
    const struct sl_slot *cur = (sl.active >= 0 && sl.active < sl.count) ? &sl.slots[sl.active] : &sl.slots[0];
    char rtitle[48];
    snprintf(rtitle, sizeof(rtitle), "%02d  %s", sl.active, cur->name);
    place(sl.r_name, 1, 384, 46, 218, 32);
    write_lbl(sl.r_name, rtitle);

    place(sl.wave_bdr, 0, 384, 96, 216, 76); paint(sl.wave_bdr, c_cream_dim);
    place(sl.wave_bg,  0, 385, 97, 214, 74); paint(sl.wave_bg, c_bg_left);
    for (int b = 0; b < SL_WAVE_BARS; b++) {
        int bh = cur->wave[b];
        int bx = 400 + b * 24;
        int by = 97 + (74 - bh) / 2;
        place(sl.wave_bars[b], 0, bx, by, 10, bh);
        paint(sl.wave_bars[b], (sl.active > 0 && bh >= 16) ? c_cream : c_cream_dim);
    }

    place(sl.btn1_bdr, 0, 384, 210, 216, 52); paint(sl.btn1_bdr, c_cream);
    place(sl.btn1_bg,  0, 386, 212, 212, 48); paint(sl.btn1_bg, c_btn_bg);
    place(sl.r_btn1,   1, 398, 223, 192, 26); write_lbl(sl.r_btn1, SL_S_BTN_PLAY);

    place(sl.btn2_bdr, 0, 384, 276, 216, 52); paint(sl.btn2_bdr, sl.hi_repeat ? c_cream : c_div);
    place(sl.btn2_bg,  0, 386, 278, 212, 48); paint(sl.btn2_bg, c_row_idle);
    place(sl.r_btn2,   1, 398, 289, 192, 26); write_lbl(sl.r_btn2, sl.hi_repeat ? SL_S_BTN_HI_ON : SL_S_BTN_HI_OFF);

    place(sl.btn3_bdr, 0, 384, 386, 216, 52); paint(sl.btn3_bdr, c_close_bdr);
    place(sl.btn3_bg,  0, 386, 388, 212, 48); paint(sl.btn3_bg, c_close_bg);
    place(sl.r_btn3,   1, 412, 399, 168, 26); write_lbl(sl.r_btn3, SL_S_BTN_CLOSE);
}

#endif
