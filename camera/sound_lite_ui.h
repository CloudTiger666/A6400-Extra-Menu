#ifndef A6400_SOUND_LITE_UI_H
#define A6400_SOUND_LITE_UI_H

/* C2 Warm Cream (#F2EDE4) + Deep Charcoal (#161618) Sound Deck UI for Sound Lite.
 * Compiled into native_film_ui_trial3.c only when -DA6400_SOUND_LITE=1.
 * Avoids stdio (fopen/fwrite/fread/rename/access/fprintf) to preserve libc-2.11.2 pins. */

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#define SL_MAX_SLOTS 16
#define SL_VIS_ROWS  8
#define SL_WAVE_BARS 16
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
    char sub[40];
    char path[300];
    unsigned char wave[SL_WAVE_BARS]; /* 6..56 px bar height */
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
    int last_prev, last_next, last_ok, last_exit, last_c1;

    /* Native GEN_Panel / GEN_Text widgets */
    void *left_bg, *right_bg, *v_div, *hdr_div;
    void *row_bgs[SL_VIS_ROWS];
    void *r_badge_bg, *wave_border, *wave_bg, *btn1_bg, *btn2_bg;
    void *wave_bars[SL_WAVE_BARS];

    void *l_title, *l_cnt, *l_hint;
    void *row_l[SL_VIS_ROWS];
    void *row_r[SL_VIS_ROWS];
    void *r_badge, *r_state, *r_slot, *r_name, *r_sub, *r_btn1, *r_btn2, *r_foot;
};

static struct sl_state sl;

#if defined(A6400_UI_LANG_SC) && A6400_UI_LANG_SC
#define SL_S_BOOT_TOAST   "Sound Lite 已就绪"
#define SL_S_OFF_NAME     "关闭快门音效"
#define SL_S_OFF_SUB      "原厂静音 · 不发声"
#define SL_S_B1_NAME      "经典快门 1 (主快门)"
#define SL_S_B1_SUB       "内置主快门声 · 零冷却"
#define SL_S_B2_NAME      "清脆快門 2 (连拍音)"
#define SL_S_B2_SUB       "内置清脆机械 · 零冷却"
#define SL_S_B3_NAME      "轻快快门 3 (轻拍音)"
#define SL_S_B3_SUB       "内置轻盈快门 · 零冷却"
#define SL_S_SD_SUB       "SD 自定义 16kHz PCM"
#define SL_S_STATE_ON     "● 已启用"
#define SL_S_STATE_OFF    "○ 已静音"
#define SL_S_BTN_PLAY     "▶ 试听快门音"
#define SL_S_BTN_HI_ON    "Hi+ 连拍模式：开"
#define SL_S_BTN_HI_OFF   "Hi+ 连拍模式：关"
#define SL_S_SCROLL_HINT  "▲ 拨轮 / 触控切换 (SD卡 /A6400/SOUNDS) ▼"
#define SL_S_FOOT_HINT    "方向键 / 触控选择 · MENU 返回取景"
#else
#define SL_S_BOOT_TOAST   "Sound Lite 已就緒"
#define SL_S_OFF_NAME     "關閉快門音效"
#define SL_S_OFF_SUB      "原廠靜音 · 不發聲"
#define SL_S_B1_NAME      "經典快門 1 (主快門)"
#define SL_S_B1_SUB       "內建主快門聲 · 零冷卻"
#define SL_S_B2_NAME      "清脆快門 2 (連拍音)"
#define SL_S_B2_SUB       "內建清脆機械 · 零冷卻"
#define SL_S_B3_NAME      "輕快快門 3 (輕拍音)"
#define SL_S_B3_SUB       "內建輕盈快門 · 零冷卻"
#define SL_S_SD_SUB       "SD 自訂 16kHz PCM"
#define SL_S_STATE_ON     "● 已啟用"
#define SL_S_STATE_OFF    "○ 已靜音"
#define SL_S_BTN_PLAY     "▶ 試聽快門音"
#define SL_S_BTN_HI_ON    "Hi+ 連拍模式：開"
#define SL_S_BTN_HI_OFF   "Hi+ 連拍模式：關"
#define SL_S_SCROLL_HINT  "▲ 撥輪 / 觸控切換 (SD卡 /A6400/SOUNDS) ▼"
#define SL_S_FOOT_HINT    "方向鍵 / 觸控選擇 · MENU 返回取景"
#endif

static void sl_compute_wave(const unsigned char *pcm, int bytes, unsigned char out[SL_WAVE_BARS]) {
    if (!pcm || bytes < 64) {
        for (int i = 0; i < SL_WAVE_BARS; i++) out[i] = 6;
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
        int h = 6 + (peaks[b] * 50) / max_pk;
        if (h < 6) h = 6;
        if (h > 56) h = 56;
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
    int ms = (bytes * 100) / 32000; /* centiseconds at 16kHz 16-bit mono */
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
        snprintf(s->sub, sizeof(s->sub), "%s", SL_S_OFF_SUB);
        s->path[0] = 0;
        for (int i = 0; i < SL_WAVE_BARS; i++) s->wave[i] = 6;
    }

    /* Built-in shutter slot(s) if mounted by charo_bind */
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
        snprintf(s->sub, sizeof(s->sub), "%s", SL_S_B1_SUB);
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
            snprintf(s->sub, sizeof(s->sub), "%s", SL_S_B2_SUB);
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
            snprintf(s->sub, sizeof(s->sub), "%s", SL_S_B3_SUB);
            s->path[0] = 0;
            sl_compute_wave(tmp, n, s->wave);
        }
    }
#endif

    /* Optional SD card custom .pcm slots in /mnt/A6400/SOUNDS/ */
    if (scan_sd && sl.count < SL_MAX_SLOTS) {
        DIR *dir = opendir("/mnt/A6400/SOUNDS");
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != 0 && sl.count < SL_MAX_SLOTS) {
                if (!sl_ends_with_pcm(ent->d_name)) continue;
                char full[300];
                snprintf(full, sizeof(full), "/mnt/A6400/SOUNDS/%s", ent->d_name);
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
                snprintf(s->sub, sizeof(s->sub), "%s", SL_S_SD_SUB);
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
        sl.left_bg, sl.right_bg, sl.v_div, sl.hdr_div,
        sl.r_badge_bg, sl.wave_border, sl.wave_bg, sl.btn1_bg, sl.btn2_bg
    };
    for (unsigned i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
        if (panels[i]) { place(panels[i], 0, 0, 0, 0, 0); paint(panels[i], clear_c); }
    }
    for (int i = 0; i < SL_VIS_ROWS; i++) {
        if (sl.row_bgs[i]) { place(sl.row_bgs[i], 0, 0, 0, 0, 0); paint(sl.row_bgs[i], clear_c); }
        if (sl.row_l[i]) { place(sl.row_l[i], 1, 0, 0, 0, 0); write_lbl(sl.row_l[i], ""); }
        if (sl.row_r[i]) { place(sl.row_r[i], 1, 0, 0, 0, 0); write_lbl(sl.row_r[i], ""); }
    }
    for (int i = 0; i < SL_WAVE_BARS; i++) {
        if (sl.wave_bars[i]) { place(sl.wave_bars[i], 0, 0, 0, 0, 0); paint(sl.wave_bars[i], clear_c); }
    }
    void *lbls[] = {
        sl.l_title, sl.l_cnt, sl.l_hint,
        sl.r_badge, sl.r_state, sl.r_slot, sl.r_name, sl.r_sub,
        sl.r_btn1, sl.r_btn2, sl.r_foot
    };
    for (unsigned i = 0; i < sizeof(lbls) / sizeof(lbls[0]); i++) {
        if (lbls[i]) { place(lbls[i], 1, 0, 0, 0, 0); write_lbl(lbls[i], ""); }
    }
}

static int sl_build_widgets(void *(*make_fn)(int)) {
    sl.left_bg = make_fn(0);
    sl.right_bg = make_fn(0);
    sl.v_div = make_fn(0);
    sl.hdr_div = make_fn(0);
    sl.r_badge_bg = make_fn(0);
    sl.wave_border = make_fn(0);
    sl.wave_bg = make_fn(0);
    sl.btn1_bg = make_fn(0);
    sl.btn2_bg = make_fn(0);
    if (!sl.left_bg || !sl.right_bg || !sl.v_div || !sl.hdr_div ||
        !sl.r_badge_bg || !sl.wave_border || !sl.wave_bg || !sl.btn1_bg || !sl.btn2_bg)
        return -5;
    for (int i = 0; i < SL_VIS_ROWS; i++) {
        sl.row_bgs[i] = make_fn(0);
        if (!sl.row_bgs[i]) return -5;
    }
    for (int i = 0; i < SL_WAVE_BARS; i++) {
        sl.wave_bars[i] = make_fn(0);
        if (!sl.wave_bars[i]) return -5;
    }
    sl.l_title = make_fn(1);
    sl.l_cnt = make_fn(1);
    sl.l_hint = make_fn(1);
    sl.r_badge = make_fn(1);
    sl.r_state = make_fn(1);
    sl.r_slot = make_fn(1);
    sl.r_name = make_fn(1);
    sl.r_sub = make_fn(1);
    sl.r_btn1 = make_fn(1);
    sl.r_btn2 = make_fn(1);
    sl.r_foot = make_fn(1);
    if (!sl.l_title || !sl.l_cnt || !sl.l_hint || !sl.r_badge || !sl.r_state ||
        !sl.r_slot || !sl.r_name || !sl.r_sub || !sl.r_btn1 || !sl.r_btn2 || !sl.r_foot)
        return -5;
    for (int i = 0; i < SL_VIS_ROWS; i++) {
        sl.row_l[i] = make_fn(1);
        sl.row_r[i] = make_fn(1);
        if (!sl.row_l[i] || !sl.row_r[i]) return -5;
    }
    sl.hi_repeat = 1;
    sl_scan_slots_ex(0);
    return 0;
}

static void sl_draw_deck(void) {
    /* C2 Warm Cream (#F2EDE4) + Deep Charcoal (#161618) palette */
    static const unsigned char c_scrim[4]      = {  8,   9,  10, 198};
    static const unsigned char c_cream[4]      = {242, 237, 228, 255}; /* #F2EDE4 */
    static const unsigned char c_cream_r[4]    = {234, 228, 216, 255}; /* #EAE4D8 */
    static const unsigned char c_cream_hi[4]   = {245, 241, 232, 255}; /* #F5F1E8 */
    static const unsigned char c_divider[4]    = {220, 213, 200, 255}; /* #DCD5C8 */
    static const unsigned char c_pill_bg[4]    = {223, 216, 202, 255}; /* #DFD8CA */
    static const unsigned char c_wave_bdr[4]   = {216, 208, 194, 255}; /* #D8D0C2 */
    static const unsigned char c_btn2_bdr[4]   = {204, 196, 181, 255}; /* #CCC4B5 */
    static const unsigned char c_charcoal[4]   = { 22,  22,  24, 255}; /* #161618 */
    static const unsigned char c_dark_ink[4]   = { 30,  30,  32, 255}; /* #1E1E20 */
    static const unsigned char c_muted_ink[4]  = {108, 102,  92, 255}; /* #6C665C */
    static const unsigned char c_bar_dim[4]    = {203, 195, 180, 255}; /* #CBC3B4 */
    static const unsigned char c_clear[4]      = {  0,   0,   0,   0};
    int has_lc = (ui_label_color != 0);
    const unsigned char *unsel_bg = has_lc ? c_cream : c_charcoal;
    const unsigned char *r_chip_bg = has_lc ? c_pill_bg : c_charcoal;
    const unsigned char *btn2_fill = has_lc ? c_btn2_bdr : c_charcoal;

    place(ui.background, 0, 0, 0, 640, 480); paint(ui.background, c_scrim);
    place(sl.left_bg,    0, 22, 26, 344, 428); paint(sl.left_bg, c_cream);
    place(sl.right_bg,   0, 366, 26, 252, 428); paint(sl.right_bg, c_cream_r);
    place(sl.v_div,      0, 366, 26, 1, 428); paint(sl.v_div, c_divider);
    place(sl.hdr_div,    0, 34, 62, 320, 1); paint(sl.hdr_div, c_divider);

    char hdr_l[40], hdr_r[16];
    snprintf(hdr_l, sizeof(hdr_l), "SOUND DECK · %d SLOTS", sl.count > 0 ? sl.count - 1 : 0);
    snprintf(hdr_r, sizeof(hdr_r), "%02d / %02d", sl.active, sl.count > 0 ? sl.count - 1 : 0);
    place(sl.l_title, 1, 36, 36, 236, 24); write_lbl(sl.l_title, hdr_l);
    place(sl.l_cnt,   1, 274, 36, 80, 24); write_lbl(sl.l_cnt, hdr_r);
    if (has_lc) { ui_label_color(sl.l_title, c_charcoal); ui_label_color(sl.l_cnt, c_muted_ink); }

    for (int r = 0; r < SL_VIS_ROWS; r++) {
        int idx = sl.scroll + r;
        int ry = 70 + r * 44;
        if (idx >= sl.count) {
            place(sl.row_bgs[r], 0, 0, 0, 0, 0); paint(sl.row_bgs[r], c_clear);
            place(sl.row_l[r],   1, 0, 0, 0, 0); write_lbl(sl.row_l[r], "");
            place(sl.row_r[r],   1, 0, 0, 0, 0); write_lbl(sl.row_r[r], "");
            continue;
        }
        int sel = (idx == sl.active);
        const struct sl_slot *s = &sl.slots[idx];
        place(sl.row_bgs[r], 0, 34, ry, 320, 40);
        paint(sl.row_bgs[r], sel ? c_charcoal : unsel_bg);

        char row_txt[40];
        snprintf(row_txt, sizeof(row_txt), "%02d  %s", idx, s->name);
        place(sl.row_l[r], 1, 46, ry + 8, 238, 24);
        write_lbl(sl.row_l[r], row_txt);
        place(sl.row_r[r], 1, 288, ry + 8, 56, 24);
        write_lbl(sl.row_r[r], s->dur);
        if (has_lc) {
            ui_label_color(sl.row_l[r], sel ? c_cream_hi : c_dark_ink);
            ui_label_color(sl.row_r[r], sel ? c_cream_hi : c_muted_ink);
        }
    }

    place(sl.l_hint, 1, 34, 426, 320, 22);
    write_lbl(sl.l_hint, SL_S_SCROLL_HINT);
    if (has_lc) ui_label_color(sl.l_hint, c_muted_ink);

    /* Right Panel */
    const struct sl_slot *cur = (sl.active >= 0 && sl.active < sl.count) ? &sl.slots[sl.active] : &sl.slots[0];
    place(sl.r_badge_bg, 0, 384, 42, 92, 24); paint(sl.r_badge_bg, r_chip_bg);
    place(sl.r_badge,    1, 390, 44, 82, 20); write_lbl(sl.r_badge, "16kHz PCM");
    place(sl.r_state,    1, 514, 44, 86, 22); write_lbl(sl.r_state, sl.active > 0 ? SL_S_STATE_ON : SL_S_STATE_OFF);
    if (has_lc) { ui_label_color(sl.r_badge, c_muted_ink); ui_label_color(sl.r_state, c_charcoal); }

    char slot_hdr[24];
    snprintf(slot_hdr, sizeof(slot_hdr), "SLOT %02d", sl.active);
    place(sl.r_slot, 1, 384, 82, 216, 22); write_lbl(sl.r_slot, slot_hdr);
    place(sl.r_name, 1, 384, 108, 216, 32); write_lbl(sl.r_name, cur->name);
    place(sl.r_sub,  1, 384, 144, 216, 24); write_lbl(sl.r_sub, cur->sub);
    if (has_lc) {
        ui_label_color(sl.r_slot, c_muted_ink);
        ui_label_color(sl.r_name, c_charcoal);
        ui_label_color(sl.r_sub,  c_muted_ink);
    }

    /* Waveform Box + 16 vertical bars */
    place(sl.wave_border, 0, 384, 178, 216, 68); paint(sl.wave_border, c_wave_bdr);
    place(sl.wave_bg,     0, 385, 179, 214, 66); paint(sl.wave_bg, c_cream_hi);
    for (int b = 0; b < SL_WAVE_BARS; b++) {
        int bh = cur->wave[b];
        int bx = 394 + b * 12;
        int by = 179 + (66 - bh) / 2;
        place(sl.wave_bars[b], 0, bx, by, 6, bh);
        paint(sl.wave_bars[b], (sl.active > 0 && bh >= 16) ? c_charcoal : c_bar_dim);
    }

    /* Action Buttons */
    place(sl.btn1_bg, 0, 384, 294, 216, 42); paint(sl.btn1_bg, c_charcoal);
    place(sl.r_btn1,  1, 396, 304, 192, 24); write_lbl(sl.r_btn1, SL_S_BTN_PLAY);
    if (has_lc) ui_label_color(sl.r_btn1, c_cream_hi);

    place(sl.btn2_bg, 0, 384, 346, 216, 38); paint(sl.btn2_bg, btn2_fill);
    place(sl.r_btn2,  1, 396, 354, 192, 24); write_lbl(sl.r_btn2, sl.hi_repeat ? SL_S_BTN_HI_ON : SL_S_BTN_HI_OFF);
    if (has_lc) ui_label_color(sl.r_btn2, c_dark_ink);

    place(sl.r_foot,  1, 384, 426, 216, 22); write_lbl(sl.r_foot, SL_S_FOOT_HINT);
    if (has_lc) ui_label_color(sl.r_foot, c_muted_ink);
}

#endif
