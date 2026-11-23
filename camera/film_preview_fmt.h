/* Shared V4.0 preview format and UI/daemon IPC ABI.
 * Kept strictly separate from the pinned 1120-byte `a6400_ui_state` ABI.
 */
#ifndef A6400_FILM_PREVIEW_FMT_H
#define A6400_FILM_PREVIEW_FMT_H

#include <stdint.h>

#define A6400_V4_MAGIC          0x34563441u /* "A4V4" */
#define A6400_V4_VERSION        1u
#define A6400_PV_MAGIC          0x56503441u /* "A4PV" */

/* Instant filter preview pop-up card (bottom-right, clear of corner toast y=390). */
#define A6400_POPUP_W           240u
#define A6400_POPUP_H           160u
#define A6400_POPUP_X           376u
#define A6400_POPUP_Y           212u
#define A6400_POPUP_BYTES       (A6400_POPUP_W * A6400_POPUP_H * 2u)

/* In-camera graded album full-screen preview (centered between top/bottom HUD bars). */
#define A6400_ALBUM_W           600u
#define A6400_ALBUM_H           400u
#define A6400_ALBUM_X           20u
#define A6400_ALBUM_Y           40u
#define A6400_ALBUM_BAND_Y0     36u
#define A6400_ALBUM_BAND_H      408u
#define A6400_ALBUM_BYTES       (A6400_ALBUM_W * A6400_ALBUM_H * 2u)

#define A6400_ALBUM_SLOTS       16u
#define A6400_SESSION_SHOTS_CAP 64u

#define A6400_PV_DIR            "/tmp_bt/a6400-pv"
#define A6400_PV_STAGED         "/tmp_bt/a6400-pv/latest.pkg"
#define A6400_PV_POPUP          "/tmp_bt/a6400-pv/popup.bin"
#define A6400_PV_CURRENT        "/tmp_bt/a6400-pv/current.bin"
#define A6400_PV_ALBUM_INDEX    "/tmp_bt/a6400-pv/album.idx"
#define A6400_PV_JOB_FILE       "/tmp_bt/a6400-film-job/preview.bin"
#define A6400_PV_JOB_INPUT      "/tmp_bt/a6400-film-job/input.jpg"

#define A6400_DISP_NONE         0u
#define A6400_DISP_POPUP        1u
#define A6400_DISP_ALBUM        2u

#define A6400_DEXP_OFF          0u
#define A6400_DEXP_SCREEN       1u /* 濾色: 1 - (1-A)(1-B) */
#define A6400_DEXP_LIGHTEN      2u /* 變亮: max(A, B) */
#define A6400_DEXP_AVERAGE      3u /* 平均: (A + B) / 2 */

struct a6400_preview_hdr {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t seq;
    char label[48];
};

/* Total size of one preview package written by a6400-film-job.elf:
 * [hdr(popup) + 240*160*2] + [hdr(album) + 600*400*2] = 556,928 bytes. */
#define A6400_PV_POPUP_FILE_BYTES (sizeof(struct a6400_preview_hdr) + A6400_POPUP_BYTES)
#define A6400_PV_ALBUM_FILE_BYTES (sizeof(struct a6400_preview_hdr) + A6400_ALBUM_BYTES)
#define A6400_PV_PKG_BYTES        (A6400_PV_POPUP_FILE_BYTES + A6400_PV_ALBUM_FILE_BYTES)

/* Exported V4.0 control, preview, and native-touch IPC block in a6400-film-ui-v3.so.
 * Fields 0..7 are owned by the UI thread; fields 8..13 + slot metadata are updated
 * by film_capture_daemon via /proc/<im_pid>/mem with latest_seq written last. */
struct a6400_ui_v4_v1 {
    uint32_t magic;            /* A6400_V4_MAGIC */
    uint32_t version;          /* A6400_V4_VERSION */
    uint32_t preview_popup_en; /* 1=ON (default), 0=OFF */
    uint32_t album_preview_en; /* 1=ON (default), 0=OFF */
    uint32_t native_touch_en;  /* 1=ON (default), 0=OFF */
    uint32_t display_mode;     /* A6400_DISP_NONE / POPUP / ALBUM */
    uint32_t display_seq;      /* bumped by UI on every display change */
    uint32_t album_slot;       /* 0..15 slot displayed when display_mode == ALBUM */
    uint32_t native_touch_ok;  /* 1 if MTF2 A6400 fingerprints verified, else 0 */
    uint32_t native_touch_hits;/* diagnostic count of native MENU touch actions */
    /* Daemon-published fields (written before latest_seq): */
    uint32_t latest_seq;       /* bumped by daemon after publishing popup + slot */
    uint32_t album_count;      /* TEST index length, 0..1024 */
    uint32_t album_head;       /* 0..15 index of most recent instant-preview slot */
    uint32_t session_shots;    /* total shots (APPLIED + OFF) since boot */
    int8_t   shot_slots[A6400_SESSION_SHOTS_CAP]; /* -1 if LUT OFF, else 0..15 */
    char     slot_labels[A6400_ALBUM_SLOTS][48];  /* per-slot UTF-8 caption */
    /* Double Exposure IPC stays where upstream appended it, so dexp offsets do not move. */
    uint32_t dexp_mode;        /* UI-owned: A6400_DEXP_OFF/SCREEN/LIGHTEN/AVERAGE */
    uint32_t dexp_seq;         /* UI-owned: bumped whenever dexp_mode changes */
    uint32_t dexp_pending;     /* Daemon-owned: 0=idle (0/2), 1=Shot1 (1/2), 2=blending (2/2) */
    /* TEST album index and the one-photo request. After dexp so those three fields stay put. */
    uint32_t album_truncated;  /* 1 if TEST has more graded JPEGs than the index cap */
    uint32_t album_ready_seq;  /* daemon: matches album_view_seq when current.bin is ready */
    uint32_t album_ready_slot; /* daemon: index within the sorted TEST list */
    uint32_t album_scan_done;  /* daemon: echoes album_scan_req after a TEST listing attempt */
    char     album_label[48];  /* daemon: filename of current.bin */
    uint32_t album_view_seq;   /* UI: bumped when the visible TEST index changes */
    uint32_t album_view_slot;  /* UI: sorted-index of the JPEG to decode */
    uint32_t album_scan_req;   /* UI: bumped to ask the daemon to list TEST again */
};

/* Pack 8-bit RGB to little-endian A6400 OSD RGBA4444 (R:15..12 G:11..8 B:7..4 A:3..0)
 * using 4x4 Bayer ordered dithering to eliminate 4-bit gradient banding. */
static inline uint16_t a6400_rgb_to_rgba4444_dither(unsigned r, unsigned g, unsigned b,
                                                    unsigned x, unsigned y) {
    static const uint8_t bayer4[16] = {
         0,  8,  2, 10,
        12,  4, 14,  6,
         3, 11,  1,  9,
        15,  7, 13,  5
    };
    unsigned d = bayer4[((y & 3u) << 2) | (x & 3u)];
    unsigned r4 = (r + d) >> 4;
    unsigned g4 = (g + d) >> 4;
    unsigned b4 = (b + d) >> 4;
    if (r4 > 15u) r4 = 15u;
    if (g4 > 15u) g4 = 15u;
    if (b4 > 15u) b4 = 15u;
    return (uint16_t)((r4 << 12) | (g4 << 8) | (b4 << 4) | 0xFu);
}

#endif /* A6400_FILM_PREVIEW_FMT_H */
