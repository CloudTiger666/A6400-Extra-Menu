/* Exact-version RAM trial. Invoked on the natural Appframework thread.
 * All drawing uses owned Sony widgets/layer. No framebuffer writes, input
 * consumption, firmware/configuration patch or independent SD mount.
 */
#define _GNU_SOURCE
#include "film_native_view.h"
#include "film_album_index.h"
#include "film_preview_fmt.h"
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "ui_trial_trace.h"
#ifndef A6400_DISABLE_MTF2
#include "mtf2_native_touch.h"
#endif
#ifdef A6400_INPUT_DIAGNOSTIC
#include "ui_input_journal.h"
static volatile struct ui_input_journal a6400_ui_input_journal_v1;
#endif

#if defined(A6400_UI_TOUCH_CLOSE_DIAGNOSTIC) && !defined(A6400_UI_RENDER_DIAGNOSTIC)
#error "touch-close diagnostic requires the bounded render diagnostic"
#endif

#if defined(A6400_UI_COMBO_DIAGNOSTIC) && defined(A6400_UI_RENDER_DIAGNOSTIC)
#error "combo and automatic-render diagnostics are mutually exclusive"
#endif

#if defined(A6400_UI_SETTINGS_DIAGNOSTIC) && \
    (A6400_UI_SETTINGS_DIAGNOSTIC != 1 || !defined(A6400_UI_COMBO_DIAGNOSTIC))
#error "settings diagnostic requires the combo diagnostic"
#endif

typedef void *(*key_inst_fn)(void);
static key_inst_fn get_key_instance_fn;
#ifndef A6400_UI_TRIAL_MS
# ifdef A6400_UI_COMBO_DIAGNOSTIC
#  define A6400_UI_TRIAL_MS 180000u
# else
#  define A6400_UI_TRIAL_MS 30000u
# endif
#endif
#if A6400_UI_TRIAL_MS != 0 && (A6400_UI_TRIAL_MS < 30000 || A6400_UI_TRIAL_MS > 600000)
#error "A6400_UI_TRIAL_MS must be 0 (persistent) or between 30000 and 600000 milliseconds"
#endif
#define TRIAL_MS A6400_UI_TRIAL_MS
#ifndef A6400_UI_INSTANCE_SUFFIX
#define A6400_UI_INSTANCE_SUFFIX ""
#endif
#ifndef A6400_UI_DISPLAY_MS
#define A6400_UI_DISPLAY_MS 5000u
#endif
#if (A6400_UI_DISPLAY_MS != 0 || TRIAL_MS != 0) && \
    (A6400_UI_DISPLAY_MS < 1 || A6400_UI_DISPLAY_MS > 20000 || (TRIAL_MS != 0 && A6400_UI_DISPLAY_MS >= TRIAL_MS))
#error "A6400_UI_DISPLAY_MS must be 0 (no auto-hide, persistent only) or between 1 and 20000 milliseconds"
#endif
static volatile struct ui_trial_trace a6400_ui_trial_trace;
#define TRACE (&a6400_ui_trial_trace)
typedef void (*ctor_fn)(void *);
typedef void (*unary_fn)(void *);
typedef void (*bool_fn)(void *,int);
typedef void (*layer_id_fn)(void *,const uint32_t *);
/* The firmware is base AAPCS soft-float. Compiler/system headers are hard-float:
 * annotate the only native calls with float arguments explicitly. */
typedef void (*size_fn)(void *,float,float) __attribute__((pcs("aapcs")));
typedef void (*position_fn)(void *,const float *);
typedef void (*text_fn)(void *,const char *);
typedef void (*color_fn)(void *,const unsigned char *);
typedef uint32_t (*id_fn)(const char *);
typedef void *(*create_layer_fn)(void *,const uint32_t *);
typedef void (*mouse_position_fn)(const void *,unsigned char,short *,short *);
typedef int (*mouse_state_fn)(const void *,unsigned char,unsigned char);
typedef int (*key_fn)(void *,int,void *);
typedef void (*font_fn)(void *,uint32_t);
typedef void (*cimg_size_fn)(void *,float,float) __attribute__((pcs("aapcs")));
typedef void *(*img_sys_get_fn)(void *);
typedef uint32_t (*get_res_id_fn)(void *);
typedef void (*create_param_ctor_fn)(void *,uint16_t,uint16_t,int);
typedef void (*create_param_arena_fn)(void *,uint8_t);
typedef uint8_t (*widget_get_arena_fn)(void *);
typedef int (*resmgr_create_img_fn)(const void *,void *);
typedef int (*bmp_stride_fn)(void *,int16_t *);
typedef int (*bmp_fmt_fn)(void *,int *);
typedef int (*bmp_lock_fn)(void *,int,void **);
typedef int (*bmp_unlock_fn)(void *);
typedef void (*sprite_set_img_fn)(void *,uint32_t,uint32_t);
typedef int (*pv_open_fn)(const char *,int);
typedef long (*pv_read_fn)(int,void *,size_t);
typedef int (*pv_close_fn)(int);
#ifndef A6400_PV_DIR_RUNTIME
#define A6400_PV_DIR_RUNTIME A6400_PV_DIR
#endif
#define A6400_KEY_LEFT      0x0015
#define A6400_KEY_RIGHT     0x01af
#define A6400_KEY_CENTER    0x01ac
#define A6400_KEY_MENU      0x01ab
#define A6400_KEY_SHUTTER   0x00a7
#define A6400_KEY_TRASH     0x01f8
#define A6400_KEY_PLAY      0x01f4
#define A6400_KEY_WHEEL_CCW 0x10000000
#define A6400_KEY_WHEEL_CW  0x10000001
#define A6400_POPUP_SHOW_MS 3500u

#ifdef A6400_UI_LANG_SC
#define UI_S_ON                 "开启"
#define UI_S_OFF                "关闭"
#define UI_S_QUAL_12M           "画质: 12MP 超清"
#define UI_S_QUAL_6M            "画质: 6MP 高清"
#define UI_S_QUAL_1M7           "画质: 1.7MP 极速"
#define UI_S_HDR_BUSY           "V4.3 SLEM | %s | 转档:%u 已转:%u"
#define UI_S_HDR_IDLE           "V4.3 SLEM | %s | 已转:%u (%u/%u)"
#define UI_S_HDR_INIT           "V4.3 SLEM (hany) | %s"
#define UI_S_HDR_BOOT           "V4.3 SLEM (hany) | 关闭"
#define UI_S_HDR_LUT            "LUT 设置 | %s"
#define UI_S_HDR_V4             "细节设置 | V4.3 SLEM (hany)"
#define UI_S_BOOT_READY_TOAST   "V4.3 SLEM 已就绪"
#define UI_S_HDR_STOCKS         "胶片风格 (%u/2)"
#define UI_S_FOOT_HOME          "hany | 相册:%u张 | 队列 %u/%u"
#define UI_S_ALBUM_TILE_FMT     "套色相册 (%u张) >"
#define UI_S_ALBUM_TILE_MORE    "套色相册 (%u张+) >"
#define UI_S_ALB_LOADING        "读取中"
#define UI_S_ALB_DIR_WAIT       "[套色相册 %u/%u]"
#define UI_S_POPUP_FMT          "即时预览小窗: %s"
#define UI_S_TOUCH_FMT          "原生MENU触控: %s"
#ifdef A6400_DISABLE_MTF2
#define UI_S_TOUCH_UNSUP        "未启用 (稳定版)"
#else
#define UI_S_TOUCH_UNSUP        "不支持"
#endif
#define UI_S_DEXP_OFF           "双重曝光: 关闭"
#define UI_S_DEXP_FMT           "双重曝光: %s (%s)"
#define UI_S_DEXP_TILE_FMT      "双曝: %s (%s) >"
#define UI_S_DEXP_HDR_FMT       "V4.3 SLEM | 双曝:%s (%s)"
#define UI_S_DEXP_LOCK_LUT      "胶片滤镜: 关闭 (双曝未完锁定)"
#define UI_S_DEXP_BLOCK_HDR     "双曝 1/2 未拍完，不可开启 LUT!"
#define UI_S_DEXP_BLOCK_TOAST   "双曝未拍完，无法开启 LUT"
#define UI_S_DEXP_M_SCREEN      "滤色"
#define UI_S_DEXP_M_LIGHTEN     "变亮"
#define UI_S_DEXP_M_AVERAGE     "平均"
#define UI_S_DEXP_P_IDLE        "0/2"
#define UI_S_DEXP_P_WAIT        "1/2待拍"
#define UI_S_DEXP_P_BUSY        "2/2合成"
#define UI_S_DEXP_CANCEL_TILE   "取消双曝 1/2"
#define UI_S_DEXP_CANCEL_TOAST  "已取消双曝 1/2，可重拍第1张"
#define UI_S_ALB_LUT_DEF        "已套滤镜"
#define UI_S_ALB_DIR_TOP        "[套色相册 %u/%u] %s"
#define UI_S_ALB_DIR_BOT        "< 上一张 | 点中间或按快门退出 | 下一张 >"
#define UI_S_HINT_COMBO         "MENU + 放大镜"
#define UI_S_HINT_COMBO_RAM     "MENU + 放大镜 | RAM"
#define UI_S_HINT_AUTO20        "20秒自动隐藏"
#else
#define UI_S_ON                 "開啟"
#define UI_S_OFF                "關閉"
#define UI_S_QUAL_12M           "畫質: 12MP 超清"
#define UI_S_QUAL_6M            "畫質: 6MP 高清"
#define UI_S_QUAL_1M7           "畫質: 1.7MP 極速"
#define UI_S_HDR_BUSY           "V4.3 SLEM | %s | 轉檔:%u 已轉:%u"
#define UI_S_HDR_IDLE           "V4.3 SLEM | %s | 已轉:%u (%u/%u)"
#define UI_S_HDR_INIT           "V4.3 SLEM (hany) | %s"
#define UI_S_HDR_BOOT           "V4.3 SLEM (hany) | 關閉"
#define UI_S_HDR_LUT            "LUT 設定 | %s"
#define UI_S_HDR_V4             "細節設定 | V4.3 SLEM (hany)"
#define UI_S_BOOT_READY_TOAST   "V4.3 SLEM 已就緒"
#define UI_S_HDR_STOCKS         "膠片風格 (%u/2)"
#define UI_S_FOOT_HOME          "hany | 相冊:%u張 | 佇列 %u/%u"
#define UI_S_ALBUM_TILE_FMT     "套色相冊 (%u張) >"
#define UI_S_ALBUM_TILE_MORE    "套色相冊 (%u張+) >"
#define UI_S_ALB_LOADING        "讀取中"
#define UI_S_ALB_DIR_WAIT       "[套色相冊 %u/%u]"
#define UI_S_POPUP_FMT          "即時預覽小窗: %s"
#define UI_S_TOUCH_FMT          "原生MENU觸控: %s"
#ifdef A6400_DISABLE_MTF2
#define UI_S_TOUCH_UNSUP        "未啟用 (穩定番)"
#else
#define UI_S_TOUCH_UNSUP        "不支援"
#endif
#define UI_S_DEXP_OFF           "雙重曝光: 關閉"
#define UI_S_DEXP_FMT           "雙重曝光: %s (%s)"
#define UI_S_DEXP_TILE_FMT      "雙曝: %s (%s) >"
#define UI_S_DEXP_HDR_FMT       "V4.3 SLEM | 雙曝:%s (%s)"
#define UI_S_DEXP_LOCK_LUT      "膠片濾鏡: 關閉 (雙曝未完鎖定)"
#define UI_S_DEXP_BLOCK_HDR     "雙曝 1/2 未拍完，不可開啟 LUT!"
#define UI_S_DEXP_BLOCK_TOAST   "雙曝未拍完，無法開啟 LUT"
#define UI_S_DEXP_M_SCREEN      "濾色"
#define UI_S_DEXP_M_LIGHTEN     "變亮"
#define UI_S_DEXP_M_AVERAGE     "平均"
#define UI_S_DEXP_P_IDLE        "0/2"
#define UI_S_DEXP_P_WAIT        "1/2待拍"
#define UI_S_DEXP_P_BUSY        "2/2合成"
#define UI_S_DEXP_CANCEL_TILE   "取消雙曝 1/2"
#define UI_S_DEXP_CANCEL_TOAST  "已取消雙曝 1/2，可重拍第1張"
#define UI_S_ALB_LUT_DEF        "已套濾鏡"
#define UI_S_ALB_DIR_TOP        "[套色相冊 %u/%u] %s"
#define UI_S_ALB_DIR_BOT        "< 上一張 | 點中間或按快門退出 | 下一張 >"
#define UI_S_HINT_COMBO         "MENU + 放大鏡"
#define UI_S_HINT_COMBO_RAM     "MENU + 放大鏡 | RAM"
#define UI_S_HINT_AUTO20        "20秒自動隱藏"
#endif

static const char *quality_label(uint32_t q) {
    if(q==2u)return UI_S_QUAL_12M;
    if(q==1u)return UI_S_QUAL_6M;
    return UI_S_QUAL_1M7;
}
static const char *dexp_mode_name(uint32_t m) {
    if(m==A6400_DEXP_SCREEN)return UI_S_DEXP_M_SCREEN;
    if(m==A6400_DEXP_LIGHTEN)return UI_S_DEXP_M_LIGHTEN;
    if(m==A6400_DEXP_AVERAGE)return UI_S_DEXP_M_AVERAGE;
    return UI_S_OFF;
}
static const char *dexp_phase_name(uint32_t p) {
    if(p==1u)return UI_S_DEXP_P_WAIT;
    if(p>=2u)return UI_S_DEXP_P_BUSY;
    return UI_S_DEXP_P_IDLE;
}
#ifdef A6400_CHARO_SHUTTER
typedef void (*beep_play_fn)(void *,int);
#define A6400_CHARO_SHUTTER_KEY 0xa7
#define A6400_CHARO_SOUND_ID 0x34
#define A6400_CHARO_CONNECT_SOUND_ID 0x33
#define A6400_CHARO_PUSH_SOUND_ID 0x35
#ifndef A6400_CHARO_DELAY_MS
#define A6400_CHARO_DELAY_MS 0u
#endif
#ifndef A6400_CHARO_COOLDOWN_MS
#define A6400_CHARO_COOLDOWN_MS 0u
#endif
#ifndef A6400_CHARO_HOLD_DELAY_MS
#define A6400_CHARO_HOLD_DELAY_MS 180u
#endif
#ifndef A6400_CHARO_REPEAT_MS
#define A6400_CHARO_REPEAT_MS 95u
#endif
static __attribute__((unused)) int charo_sound_id(int mode) {
#if defined(A6400_CHARO_MULTI) && A6400_CHARO_MULTI
    if(mode==1)return A6400_CHARO_CONNECT_SOUND_ID;
    if(mode==2)return A6400_CHARO_SOUND_ID;
    if(mode==3)return A6400_CHARO_PUSH_SOUND_ID;
    return 0;
#else
    return mode?A6400_CHARO_SOUND_ID:0;
#endif
}
static const char *charo_mode_label(int mode,int custom) {
#ifdef A6400_UI_LANG_SC
#if defined(A6400_CHARO_MULTI) && A6400_CHARO_MULTI
    if(mode==1)return "快门: 日文 Charo";
    if(mode==2)return custom?"快门: SD自定义":"快门: Bling Bling";
    if(mode==3)return "快门: iPhone 快门";
    return "快门: 静音";
#else
    if(!mode)return "快门: 静音";
    return custom?"快门: Charo (SD)":"快门: Charo";
#endif
#else
#if defined(A6400_CHARO_MULTI) && A6400_CHARO_MULTI
    if(mode==1)return "快門: 日文 Charo";
    if(mode==2)return custom?"快門: SD自訂":"快門: Bling Bling";
    if(mode==3)return "快門: iPhone 快門";
    return "快門: 靜音";
#else
    if(!mode)return "快門: 靜音";
    return custom?"快門: Charo (SD)":"快門: Charo";
#endif
#endif
}
#endif

static struct a6400_ui_state {
    struct film_native_view view;
    void *system,*layer,*ticker,*background,*panels[6],*labels[8],*keys;
    void *ticker_layer;
    uint32_t ticker_layer_id;
    uint32_t layer_id,tid,started,last_tick,ticks;
    uint32_t opened_ms;
    int ever_opened;
#ifdef A6400_UI_TOUCH_CLOSE_DIAGNOSTIC
    int last_touch;
#endif
#ifdef A6400_UI_COMBO_DIAGNOSTIC
    int last_menu,last_zoom,last_other;
#elif defined(A6400_UI_TOUCH_CLOSE_DIAGNOSTIC)
    uint32_t snapshot_abi_padding[2];
#else
    uint32_t snapshot_abi_padding[3];
#endif
    uintptr_t vtable[42];
    int attached,running;
    ctor_fn widget_ctor,panel_ctor,label_ctor;
    layer_id_fn set_layer;
    position_fn set_position;
    size_fn panel_size,label_size;
    text_fn set_text;
    color_fn set_color;
    bool_fn layer_active,layer_clear,set_animate;
    unary_fn move_last;
    id_fn make_id;
    create_layer_fn create_layer;
    mouse_position_fn mouse_position;
    mouse_state_fn mouse_state;
    key_fn key;
    font_fn set_font;
    char texts[8][80];
} ui;
_Static_assert(sizeof(ui)==1120,"UI snapshot ABI must remain 1120 bytes");
_Static_assert(offsetof(struct a6400_ui_state,system)==0x68,"ui.system ABI mismatch");
_Static_assert(offsetof(struct a6400_ui_state,tid)==0xc0,"ui.tid ABI mismatch");
_Static_assert(offsetof(struct a6400_ui_state,started)==0xc4,"ui.started ABI mismatch");
_Static_assert(offsetof(struct a6400_ui_state,last_tick)==0xc8,"ui.last_tick ABI mismatch");
_Static_assert(offsetof(struct a6400_ui_state,attached)==0x18c,"ui.attached ABI mismatch");
_Static_assert(offsetof(struct a6400_ui_state,running)==0x190,"ui.running ABI mismatch");

/* Owned control word, separate from the pinned 1120-byte UI snapshot ABI.
 * A matching external manager may write 1; only the natural UI callback
 * stops owned layers and publishes 2. Retained widgets keep this SO loaded. */
volatile unsigned a6400_ui_stop_requested_v1;
__attribute__((visibility("default")))
volatile uint32_t a6400_ui_hires_v1 = 0u;

/* Owned corner-toast ABI, separate from the 1120-byte UI snapshot ABI. */
struct a6400_ui_toast_v1 {
    uint32_t magic;
    uint32_t seq;
    uint32_t kind;
    uint32_t duration_ms;
    char text[64];
};
__attribute__((visibility("default")))
volatile struct a6400_ui_toast_v1 a6400_ui_toast_v1 = { 0x54534f54u, 0u, 0u, 0u, "" };

struct a6400_ui_status_v1 {
    uint32_t magic;       /* 0x53544156u 'STAV' */
    uint32_t seq;
    uint32_t applied;
    uint32_t scanned;
    uint32_t max_files;
    uint32_t custom_charo;
    uint32_t active_jobs;
};
__attribute__((visibility("default")))
volatile struct a6400_ui_status_v1 a6400_ui_status_v1 = { 0x53544156u, 0u, 0u, 0u, 1024u, 0u, 0u };

__attribute__((visibility("default")))
volatile struct a6400_ui_v4_v1 a6400_ui_v4_v1 = {
    A6400_V4_MAGIC, A6400_V4_VERSION,
    1u, 0u, 1u,          /* preview_popup_en=ON, native Playback hijack=OFF, native_touch_en=ON */
    A6400_DISP_NONE, 0u, 0u,
    0u, 0u,
    0u, 0u, 0u, 0u,
    {0}, {{0}},
    0u, 0u, 0u,          /* dexp_mode=OFF, dexp_seq=0, dexp_pending=0 */
    0u, 0u, 0u, 0u, {0}, /* album_truncated, ready seq/slot, scan_done, label */
    0u, 0u, 0u           /* album_view_seq, album_view_slot, album_scan_req */
};

struct v4_native_slot {
    void *widget;
    void *bmp;
    int w, h, init_failed;
    uint32_t res_id;
};

static struct {
#ifndef A6400_DISABLE_MTF2
    struct mtf2_ctx mtf2;
#endif
    /* Native Sony GEN_CacheInsImg + RGBA4444 (fmt=7) zero-flicker canvas APIs. */
    ctor_fn cache_img_ctor, sprite_invalidate;
    cimg_size_fn cache_img_set_size;
    create_param_ctor_fn create_param_ctor;
    create_param_arena_fn create_param_arena;
    widget_get_arena_fn widget_get_arena;
    resmgr_create_img_fn resmgr_create_img;
    img_sys_get_fn img_sys_get, bmp_get_internal;
    get_res_id_fn get_res_id;
    bmp_stride_fn bmp_stride;
    bmp_fmt_fn bmp_fmt;
    bmp_lock_fn bmp_lock;
    bmp_unlock_fn bmp_unlock;
    sprite_set_img_fn sprite_set_img;
    pv_open_fn pv_open;
    pv_read_fn pv_read;
    pv_close_fn pv_close;
    void *popup_border;
    struct v4_native_slot pop_slot;
    struct v4_native_slot alb_slot;
    uint32_t seen_latest_seq;
    uint32_t seen_album_ready;
    uint32_t seen_album_count;
    uint32_t seen_album_trunc;
    int popup_active;
    int popup_is_native;
    uint32_t popup_until_ms;
    int album_mode;          /* 0=off, 2=direct-menu-browser (never hijacks native Playback) */
    int album_is_native;
    unsigned album_idx;      /* 0..album_count-1 in direct graded album browser */
    int last_nav_left, last_nav_right, last_nav_exit;
    int album_touch_down;
    short album_touch_x, album_touch_y;
    int mtf2_wait_release;
    int boot_toast_done;
    uint32_t mtf2_last_probe_ms;
    uint32_t seen_dexp_pending;
    uint32_t dexp_blocked_until_ms;
    uint32_t next_album_seq;
} ui_v4;

static void record(const char *event,int a,int b);

static __attribute__((unused)) void v4_reprobe_mtf2(uint32_t now) {
#ifdef A6400_DISABLE_MTF2
    (void)now;
#else
    if(a6400_ui_v4_v1.native_touch_ok)return;
    ui_v4.mtf2_last_probe_ms=now;
    a6400_ui_v4_v1.native_touch_ok=(uint32_t)mtf2_probe_and_init(&ui_v4.mtf2);
    if(a6400_ui_v4_v1.native_touch_ok)
        record("V4_MTF2_REPROBE_OK",(int)a6400_ui_v4_v1.native_touch_ok,0);
#endif
}

static struct {
    void *bg, *accent, *label;
    uint32_t seq, until_ms;
    int visible;
    char text[64];
} ui_toast;

/* Decorative theme panels kept outside `ui` so the 1120-byte snapshot ABI stays exact. */
static struct {
    void *card_border, *card_panel, *header_accent, *header_divider, *page_btn;
    void *footer_divider, *status_dot, *page_btn_accent;
    void *grid_h, *grid_vc;
    void *row_accents[6];
    void *chips[6][3];
    uint32_t status_seq;
#ifdef A6400_CHARO_SHUTTER
    beep_play_fn beep_play;
    int shutter_sound_enabled;
    int last_shutter_key;
    int shutter_pending;
    int shutter_repeating;
    uint32_t shutter_arm_ms;
    uint32_t shutter_hold_ms;
    uint32_t last_shutter_beep_ms;
#endif
} ui_theme;

static uint32_t now_ms(void) {
    struct timespec t;
    if(syscall(SYS_clock_gettime,CLOCK_MONOTONIC,&t))return 0;
    return (uint32_t)((uint64_t)t.tv_sec*1000u+(unsigned)t.tv_nsec/1000000u);
}
static void input_snapshot(unsigned stage) {
#ifdef A6400_INPUT_DIAGNOSTIC
    void *owned[UI_INPUT_OWNED]={0};
    owned[0]=ui.background;
    for(unsigned i=0;i<6;i++)owned[i+1]=ui.panels[i];
    for(unsigned i=0;i<8;i++)owned[i+7]=ui.labels[i];
    owned[15]=ui.ticker;
    if(ui.layer)owned[16]=*(void **)((unsigned char *)ui.layer+0x38);
    if(ui.ticker_layer)owned[17]=*(void **)((unsigned char *)ui.ticker_layer+0x38);
    ui_input_capture(&a6400_ui_input_journal_v1,stage,ui.system,owned,
        UI_INPUT_OWNED,now_ms(),(uint32_t)syscall(SYS_gettid));
#else
    (void)stage;
#endif
}
static void record(const char *event,int a,int b) {
    ui_trial_record(TRACE,event,now_ms(),(uint32_t)syscall(SYS_gettid),
                    (uint32_t)a,(uint32_t)b,ui.ticks,
                    (unsigned)ui.running | ((unsigned)ui.view.menu.visible<<1));
}
static void *api(const char *name,uint32_t offset) {
    (void)offset;
    void *p=dlsym(RTLD_DEFAULT,name);
    if(!p){record("API_MISMATCH",(int)offset,0);return 0;}
    return p;
}
static uintptr_t decode_t32_branch(uintptr_t pc_addr,uint16_t hw0,uint16_t hw1) {
    uint32_t S=(hw0>>10)&1u,imm10=hw0&0x3ffu;
    uint32_t J1=(hw1>>13)&1u,J2=(hw1>>11)&1u,imm11=hw1&0x7ffu;
    uint32_t I1=~(J1^S)&1u,I2=~(J2^S)&1u;
    int32_t imm25=(int32_t)((S<<24)|(I1<<23)|(I2<<22)|(imm10<<12)|(imm11<<1));
    if(S)imm25-=(int32_t)(1u<<25);
    return (pc_addr+4u)+(uintptr_t)imm25;
}
static int is_bmp_vtbl_thunk(uintptr_t fn_even,uint16_t vtbl_ldr) {
    const uint16_t *p=(const uint16_t *)fn_even;
    if(p[0]!=0xb580u || p[1]!=0xaf00u)return 0;
    for(unsigned k=8u;k<=19u;k++) {
        if(p[k]==0x6803u && p[k+1]==vtbl_ldr && p[k+2]==0x4798u)return 1;
    }
    return 0;
}
static void *find_bmp_thunk(uintptr_t bmp_base,const char *sym_name,
                            uintptr_t fast_off,uint16_t vtbl_ldr) {
    void *sym=dlsym(RTLD_DEFAULT,sym_name);
    if(sym)return sym;
    uintptr_t even_base=bmp_base & ~1u;
    if(is_bmp_vtbl_thunk(even_base+fast_off,vtbl_ldr))return (void *)((even_base+fast_off)|1u);
    for(uintptr_t off=0u;off<=0xa00u;off+=2u) {
        if(is_bmp_vtbl_thunk(even_base+off,vtbl_ldr))return (void *)((even_base+off)|1u);
    }
    return 0;
}
static __attribute__((noinline)) int resolve(void) {
#define API(member,type,name,offset) do {ui.member=(type)api(name,offset);if(!ui.member)return -1;}while(0)
    API(widget_ctor,ctor_fn,"_ZN2ux6wgtsys6WidgetC1Ev",0x5ed5ad);
    API(panel_ctor,ctor_fn,"_ZN9GEN_PanelC1Ev",0x4149ad);
    API(label_ctor,ctor_fn,"_ZN9GEN_LabelC1Ev",0x3c8d41);
    API(set_layer,layer_id_fn,"_ZN2ux6wgtsys6Widget10setLayerIDERKNS_4core2IDE",0x5ed50d);
    API(set_position,position_fn,"_ZN2ux6wgtsys6Widget11setLocalPosERKNS_4core7Vector2E",0x5ed45d);
    API(panel_size,size_fn,"_ZN9GEN_Panel7setSizeEff",0x414def);
    API(label_size,size_fn,"_ZN9GEN_Label7setSizeEff",0x3c9381);
    API(set_text,text_fn,"_ZN9GEN_Label13setTextDirectEPKc",0x3c88cf);
    API(set_font,font_fn,"_ZN9GEN_Label7setFontEj",0x3c89af);
    API(set_color,color_fn,"_ZN9GEN_Panel14setColorDirectERKN6stella6CColorE",0x4148e5);
    API(layer_active,bool_fn,"_ZN2ux6wgtsys5Layer9setActiveEb",0x5ef12f);
    API(layer_clear,bool_fn,"_ZN2ux6wgtsys5Layer8setClearEb",0x5ef159);
    API(set_animate,bool_fn,"_ZN2ux6wgtsys10WidgetBase10setAnimateEb",0x606951);
    API(move_last,unary_fn,"_ZN2ux6wgtsys9LayerBase8moveLastEv",0x601331);
    API(make_id,id_fn,"_ZN2ux4core2ID5genIDEPKc",0x63dcd9);
    API(create_layer,create_layer_fn,"_ZN2ux6wgtsys12WidgetSystem11createLayerERKNS_4core2IDE",0x5f0f49);
    API(mouse_position,mouse_position_fn,"_ZNK2ux6wgtsys12WidgetSystem24getMousePosInFramebufferEhRsS2_",0x5f1019);
    API(mouse_state,mouse_state_fn,"_ZNK2ux6wgtsys12WidgetSystem13getMouseStateEhh",0x5f1045);
    API(key,key_fn,"_ZN10InfraInput20GetPhyKeyStatusCacheEiP21_InfraInputPhyKeyData",0x860fa9);
    get_key_instance_fn=(key_inst_fn)api("_ZN10InfraInput11GetInstanceEv",0x860385);
    if(!get_key_instance_fn)return -1;
#ifdef A6400_CHARO_SHUTTER
    ui_theme.beep_play=(beep_play_fn)api("_ZN13AppWidgetBase8beepPlayEi",0);
    if(!ui_theme.beep_play)return -1;
#endif
    /* Optional native GEN_CacheInsImg + RGBA4444 (CreateImageParam fmt=7, same as
     * Sony's GEN_QRCode) zero-flicker preview renderer. */
    ui_v4.cache_img_ctor=(ctor_fn)dlsym(RTLD_DEFAULT,"_ZN15GEN_CacheInsImgC1Ev");
    ui_v4.cache_img_set_size=(cimg_size_fn)dlsym(RTLD_DEFAULT,"_ZN15GEN_CacheInsImg7setSizeEff");
    ui_v4.create_param_ctor=(create_param_ctor_fn)dlsym(RTLD_DEFAULT,"_ZN5libra16CreateImageParamC1Etti");
    ui_v4.create_param_arena=(create_param_arena_fn)dlsym(RTLD_DEFAULT,"_ZN5libra16CreateImageParam10setArenaIDEh");
    ui_v4.widget_get_arena=(widget_get_arena_fn)dlsym(RTLD_DEFAULT,"_ZN13AppWidgetBase10getArenaIDEv");
    ui_v4.resmgr_create_img=(resmgr_create_img_fn)dlsym(RTLD_DEFAULT,
        "_ZN2ux6resmgr15ResourceManager11createImageERKN5libra16CreateImageParamEPNS2_9ImageDataE");
    ui_v4.img_sys_get=(img_sys_get_fn)dlsym(RTLD_DEFAULT,"_ZN5libra9ImageData16sys_getImageDataEv");
    ui_v4.bmp_get_internal=(img_sys_get_fn)dlsym(RTLD_DEFAULT,"_ZN2ux4core13GraphicsImage21sys_getInternalObjectEPv");
    ui_v4.get_res_id=(get_res_id_fn)dlsym(RTLD_DEFAULT,"_ZNK5libra9ImageData8getResIDEv");
    ui_v4.pv_open=(pv_open_fn)dlsym(RTLD_DEFAULT,"open");
    ui_v4.pv_read=(pv_read_fn)dlsym(RTLD_DEFAULT,"read");
    ui_v4.pv_close=(pv_close_fn)dlsym(RTLD_DEFAULT,"close");
    {
        void *bmp_create=dlsym(RTLD_DEFAULT,"BMP_Create");
        if(bmp_create) {
            uintptr_t bb=(uintptr_t)bmp_create;
            ui_v4.bmp_stride=(bmp_stride_fn)find_bmp_thunk(bb,"BMP_GetRowBytes",0x3fcu,0x6a1bu);
            ui_v4.bmp_fmt=(bmp_fmt_fn)find_bmp_thunk(bb,"BMP_GetFormat",0x45cu,0x6a9bu);
            ui_v4.bmp_lock=(bmp_lock_fn)find_bmp_thunk(bb,"BMP_WriteLockBits",0x57cu,0x6cdbu);
            ui_v4.bmp_unlock=(bmp_unlock_fn)find_bmp_thunk(bb,"BMP_WriteUnlockBits",0x5acu,0x6d1bu);
        }
        void *cimg_clear=dlsym(RTLD_DEFAULT,"_ZN15GEN_CacheInsImg10clearImageEN6stella6CColorE");
        if(cimg_clear) {
            uintptr_t clr_base=(uintptr_t)cimg_clear & ~1u;
            for(uintptr_t off=0x08u;off<=0x70u;off+=2u) {
                uintptr_t pc=clr_base+off;
                const uint16_t *ins=(const uint16_t *)pc;
                if(!ui_v4.sprite_set_img &&
                   (ins[0]&0xf800u)==0xf000u && (ins[1]&0xd000u)==0xd000u) {
                    const uint16_t *prev=(const uint16_t *)(pc-6u);
                    if(prev[0]==0x2280u || prev[1]==0x2280u || prev[2]==0x2280u) {
                        uintptr_t tgt=decode_t32_branch(pc,ins[0],ins[1]);
                        if(*(const uint16_t *)tgt==0xf1b1u)ui_v4.sprite_set_img=(sprite_set_img_fn)(tgt|1u);
                    }
                } else if(!ui_v4.sprite_invalidate &&
                          (ins[0]&0xf800u)==0xf000u && (ins[1]&0xd000u)==0x9000u) {
                    uintptr_t tgt=decode_t32_branch(pc,ins[0],ins[1]);
                    if(*(const uint16_t *)tgt==0xb5f0u)ui_v4.sprite_invalidate=(ctor_fn)(tgt|1u);
                }
            }
        }
    }
#undef API
    return 0;
}
static void place(void *widget,int label,int x,int y,int w,int h) {
    if(!widget)return;
    const float xy[2]={(float)x,(float)y};
    ui.set_position(widget,xy);
    (label?ui.label_size:ui.panel_size)(widget,(float)w,(float)h);
}
static void paint(void *panel,const unsigned char *c) {
    if(panel)ui.set_color(panel,c);
}
static void write_lbl(void *label,const char *s) {
    if(label)ui.set_text(label,s);
}
#ifdef A6400_SOUND_LITE
#include "sound_lite_ui.h"
#endif
/* Signature 3-color RGBA palette triads for all 10 built-in film stocks. */
static const unsigned char stock_palettes[10][3][4] = {
    { {236,186,134,255}, {214,148,128,255}, {156,166,132,255} }, /* 0 Portra 400 */
    { {232, 68, 56,255}, { 72,184,198,255}, { 44, 68,122,255} }, /* 1 CineStill 800T */
    { { 56,168,124,255}, { 78,152,214,255}, {226,178, 92,255} }, /* 2 Fuji Provia */
    { {242,192, 58,255}, {214,118, 64,255}, {156,108, 72,255} }, /* 3 Kodak Gold */
    { {236,236,234,255}, {124,124,128,255}, { 44, 44, 48,255} }, /* 4 Tri-X 400 */
    { { 88,134,136,255}, {184,132, 86,255}, { 62, 74, 86,255} }, /* 5 Classic Chrome */
    { {228, 66, 86,255}, { 58,118,224,255}, {238,184, 48,255} }, /* 6 Agfa Ultra 100 */
    { {228,214,184,255}, {118,168,162,255}, {146,114, 96,255} }, /* 7 Polaroid 669 */
    { {244,246,248,255}, {148,154,162,255}, { 52, 56, 62,255} }, /* 8 Fuji Acros */
    { {226,174, 94,255}, {136,184,166,255}, {118, 86,112,255} }, /* 9 Polaroid PX680+ */
};
static void clear_chips(unsigned slot) {
    static const unsigned char clear_c[4]={0,0,0,0};
    if(slot>=6u)return;
    for(unsigned k=0;k<3u;k++){
        place(ui_theme.chips[slot][k],0,0,0,0,0);
        paint(ui_theme.chips[slot][k],clear_c);
    }
}
static void draw_swiss_toggle(unsigned slot,int x,int y,int on,const unsigned char *on_c) {
    static const unsigned char track_off[4]={44,48,56,255};
    static const unsigned char knob_on[4]={248,250,255,255};
    static const unsigned char knob_off[4]={148,154,164,255};
    static const unsigned char clear_c[4]={0,0,0,0};
    if(slot>=6u)return;
    place(ui_theme.chips[slot][0],0,x,y,48,24);
    paint(ui_theme.chips[slot][0],on?on_c:track_off);
    place(ui_theme.chips[slot][1],0,on?(x+27):(x+3),y+3,18,18);
    paint(ui_theme.chips[slot][1],on?knob_on:knob_off);
    place(ui_theme.chips[slot][2],0,0,0,0,0);
    paint(ui_theme.chips[slot][2],clear_c);
}
static void draw_film_swatch(unsigned slot,int x,int y,int stock) {
    if(slot>=6u)return;
    if(stock<0 || stock>=10)stock=0;
    for(unsigned k=0;k<3u;k++){
        place(ui_theme.chips[slot][k],0,x+(int)k*10,y,8,28);
        paint(ui_theme.chips[slot][k],stock_palettes[stock][k]);
    }
}
static void draw_gallery_icon(unsigned slot,int x,int y,int has_photos) {
    static const unsigned char back_on[4]={180,56,12,255};
    static const unsigned char back_off[4]={88,94,104,235};
    static const unsigned char frame_on[4]={248,250,255,255};
    static const unsigned char frame_off[4]={74,78,88,235};
    static const unsigned char inner_on[4]={234,78,16,255};
    static const unsigned char inner_off[4]={18,20,24,255};
    if(slot>=6u)return;
    place(ui_theme.chips[slot][0],0,x+8,y,32,22);
    paint(ui_theme.chips[slot][0],has_photos?back_on:back_off);
    place(ui_theme.chips[slot][1],0,x,y+6,32,22);
    paint(ui_theme.chips[slot][1],has_photos?frame_on:frame_off);
    place(ui_theme.chips[slot][2],0,x+3,y+9,26,16);
    paint(ui_theme.chips[slot][2],has_photos?inner_on:inner_off);
}
static void show_menu_chrome(int is_home) {
    static const unsigned char background[4]={0,0,0,148};
    static const unsigned char swiss_border[4]={216,220,226,215};
    static const unsigned char swiss_bg[4]={14,15,18,244};
    static const unsigned char hairline_main[4]={76,80,90,210};
    static const unsigned char hairline_grid[4]={52,56,64,215};
    static const unsigned char clear_c[4]={0,0,0,0};
    place(ui.background,0,0,0,640,480);paint(ui.background,background);
    place(ui_theme.card_border,0,40,24,560,432);paint(ui_theme.card_border,swiss_border);
    place(ui_theme.card_panel,0,41,25,558,430);paint(ui_theme.card_panel,swiss_bg);
    place(ui_theme.header_accent,0,0,0,0,0);paint(ui_theme.header_accent,clear_c);
    place(ui_theme.header_divider,0,41,76,558,1);paint(ui_theme.header_divider,hairline_main);
    place(ui_theme.footer_divider,0,41,388,558,1);paint(ui_theme.footer_divider,hairline_main);
    /* Crisp 1px Hasselblad 田-grid crosshair only on Home page */
    place(ui_theme.grid_h,0,is_home?41:0,is_home?232:0,is_home?558:0,is_home?1:0);
    paint(ui_theme.grid_h,is_home?hairline_grid:clear_c);
    place(ui_theme.grid_vc,0,is_home?320:0,is_home?77:0,is_home?1:0,is_home?311:0);
    paint(ui_theme.grid_vc,is_home?hairline_grid:clear_c);
}
static void hide_menu_widgets(void) {
    static const unsigned char clear_c[4]={0,0,0,0};
    place(ui.background,0,0,0,0,0);paint(ui.background,clear_c);
    place(ui_theme.card_border,0,0,0,0,0);paint(ui_theme.card_border,clear_c);
    place(ui_theme.card_panel,0,0,0,0,0);paint(ui_theme.card_panel,clear_c);
    place(ui_theme.header_accent,0,0,0,0,0);paint(ui_theme.header_accent,clear_c);
    place(ui_theme.header_divider,0,0,0,0,0);paint(ui_theme.header_divider,clear_c);
    place(ui_theme.footer_divider,0,0,0,0,0);paint(ui_theme.footer_divider,clear_c);
    place(ui_theme.status_dot,0,0,0,0,0);paint(ui_theme.status_dot,clear_c);
    place(ui_theme.page_btn,0,0,0,0,0);paint(ui_theme.page_btn,clear_c);
    place(ui_theme.page_btn_accent,0,0,0,0,0);paint(ui_theme.page_btn_accent,clear_c);
    place(ui_theme.grid_h,0,0,0,0,0);paint(ui_theme.grid_h,clear_c);
    place(ui_theme.grid_vc,0,0,0,0,0);paint(ui_theme.grid_vc,clear_c);
    for(unsigned i=0;i<6u;i++){
        place(ui.panels[i],0,0,0,0,0);paint(ui.panels[i],clear_c);
        place(ui_theme.row_accents[i],0,0,0,0,0);paint(ui_theme.row_accents[i],clear_c);
        clear_chips(i);
    }
    for(unsigned i=0;i<8u;i++){
        ui.texts[i][0]=0;
        place(ui.labels[i],1,0,0,0,0);
        write_lbl(ui.labels[i],ui.texts[i]);
    }
#ifdef A6400_SOUND_LITE
    sl_hide_widgets();
#endif
}
static void hide_toast_widgets(void) {
    static const unsigned char clear_c[4]={0,0,0,0};
    if(!ui_toast.bg)return;
    place(ui_toast.bg,0,0,0,0,0);ui.set_color(ui_toast.bg,clear_c);
    place(ui_toast.accent,0,0,0,0,0);ui.set_color(ui_toast.accent,clear_c);
    ui_toast.text[0]=0;
    place(ui_toast.label,1,0,0,0,0);ui.set_text(ui_toast.label,ui_toast.text);
}
static void v4_publish_display(uint32_t mode,uint32_t slot) {
    a6400_ui_v4_v1.album_slot=slot;
    a6400_ui_v4_v1.display_mode=mode;
    __sync_synchronize();
    a6400_ui_v4_v1.display_seq+=1u;
}
static void sync_layer_active(void) {
    if(!ui.layer)return;
    int want=(ui.view.menu.visible || ui_v4.album_mode ||
              (ui_v4.popup_active && ui_v4.popup_is_native) || ui_toast.visible)?1:0;
    ui.layer_active(ui.layer,want);
}
static int v4_pv_read_exact(int fd,void *buf,size_t len) {
    unsigned char *p=(unsigned char *)buf;
    size_t got=0u;
    while(got<len) {
        long r=ui_v4.pv_read(fd,p+got,len-got);
        if(r<=0)return -1;
        got+=(size_t)r;
    }
    return 0;
}
static void v4_hide_slot(struct v4_native_slot *slot) {
    if(slot->widget && ui_v4.cache_img_set_size) {
        const float zxy[2]={0.0f,0.0f};
        ui.set_position(slot->widget,zxy);
        ui_v4.cache_img_set_size(slot->widget,0.0f,0.0f);
        if(ui_v4.sprite_invalidate)ui_v4.sprite_invalidate((unsigned char *)slot->widget+0x164u);
    }
}
static int v4_ensure_native_slot(struct v4_native_slot *slot,const int dims[][2],int ndims) {
    if(slot->widget && slot->bmp)return 1;
    if(slot->init_failed)return 0;
    if(!ui_v4.cache_img_ctor || !ui_v4.cache_img_set_size ||
       !ui_v4.create_param_ctor || !ui_v4.resmgr_create_img ||
       !ui_v4.img_sys_get || !ui_v4.bmp_get_internal || !ui_v4.get_res_id ||
       !ui_v4.bmp_stride || !ui_v4.bmp_lock || !ui_v4.bmp_unlock ||
       !ui_v4.sprite_set_img || !ui_v4.sprite_invalidate ||
       !ui_v4.pv_open || !ui_v4.pv_read || !ui_v4.pv_close) {
        slot->init_failed=1;
        return 0;
    }
    for(int t=0;t<ndims;t++) {
        int cw=dims[t][0],ch=dims[t][1];
        void *cand=calloc(1,0x400u);
        if(!cand)break;
        ui_v4.cache_img_ctor(cand);
        ui.set_layer(cand,&ui.layer_id);
        const float zxy[2]={0.0f,0.0f};
        ui.set_position(cand,zxy);
        ui_v4.cache_img_set_size(cand,(float)cw,(float)ch);
        *(uint8_t *)((unsigned char *)cand+0x20cu)=1u;
        uint32_t param[12];
        memset(param,0,sizeof(param));
        /* Format 7 = native 16-bit RGBA4444 (0xRGBA), exact match to Sony GEN_QRCode. */
        ui_v4.create_param_ctor(param,(uint16_t)cw,(uint16_t)ch,7);
        if(ui_v4.create_param_arena && ui_v4.widget_get_arena)
            ui_v4.create_param_arena(param,ui_v4.widget_get_arena(cand));
        void *img_data=(unsigned char *)cand+0x15cu;
        int created=ui_v4.resmgr_create_img(param,img_data);
        if(!created && ui_v4.create_param_arena) {
            ui_v4.create_param_ctor(param,(uint16_t)cw,(uint16_t)ch,7);
            created=ui_v4.resmgr_create_img(param,img_data);
        }
        if(created) {
            void *internal_img=ui_v4.img_sys_get(img_data);
            void *bmp=internal_img?ui_v4.bmp_get_internal(internal_img):0;
            if(bmp) {
                uint32_t rid=ui_v4.get_res_id(img_data);
                ui_v4.sprite_set_img((unsigned char *)cand+0x164u,rid,0x80u);
                *(uint8_t *)((unsigned char *)cand+0x20eu)=1u;
                *(uint8_t *)((unsigned char *)cand+0x20du)=1u;
                slot->widget=cand;
                slot->bmp=bmp;
                slot->w=cw;
                slot->h=ch;
                slot->res_id=rid;
                ui_v4.cache_img_set_size(cand,0.0f,0.0f);
                ui_v4.sprite_invalidate((unsigned char *)cand+0x164u);
                record("V4_NATIVE_IMG_OK",cw,ch);
                return 1;
            }
        }
        ui_v4.cache_img_set_size(cand,0.0f,0.0f);
    }
    slot->init_failed=1;
    record("V4_NATIVE_IMG_FAIL",dims[0][0],dims[0][1]);
    return 0;
}
static void v4_hide_native_img(void) {
    static const unsigned char clear_c[4]={0,0,0,0};
    if(ui_v4.popup_border) {
        place(ui_v4.popup_border,0,0,0,0,0);
        paint(ui_v4.popup_border,clear_c);
    }
    v4_hide_slot(&ui_v4.pop_slot);
    v4_hide_slot(&ui_v4.alb_slot);
}
static void v4_write_rgba4444_row(unsigned char *dst_row,int is32,unsigned slot_w,
                                  const uint16_t *src_row,unsigned src_w) {
    unsigned dst_w=(src_w<=slot_w)?src_w:slot_w;
    unsigned pad_l=(slot_w>dst_w)?((slot_w-dst_w)/2u):0u;
    if(is32) {
        uint32_t *d=(uint32_t *)dst_row;
        for(unsigned x=0;x<slot_w;x++)d[x]=0xff12100eu;
        for(unsigned dx=0;dx<dst_w;dx++) {
            uint16_t q=src_row[(dst_w==src_w)?dx:((dx*src_w)/dst_w)];
            uint32_t r=((q>>12)&0xfu)*0x11u;
            uint32_t g=((q>>8)&0xfu)*0x11u;
            uint32_t b=((q>>4)&0xfu)*0x11u;
            d[pad_l+dx]=r | (g<<8) | (b<<16) | 0xff000000u;
        }
    } else {
        uint16_t *d=(uint16_t *)dst_row;
        for(unsigned x=0;x<pad_l;x++)d[x]=0x112Fu;
        if(dst_w==src_w) {
            memcpy(d+pad_l,src_row,(size_t)dst_w*2u);
        } else {
            for(unsigned dx=0;dx<dst_w;dx++)d[pad_l+dx]=src_row[(dx*src_w)/dst_w];
        }
        for(unsigned x=pad_l+dst_w;x<slot_w;x++)d[x]=0x112Fu;
    }
}
static int v4_load_bin_to_slot(struct v4_native_slot *slot,const int dims[][2],int ndims,
                               const char *path,unsigned want_w,unsigned want_h) {
    if(!v4_ensure_native_slot(slot,dims,ndims))return -1;
    if(want_w==0u || want_h==0u || want_w>A6400_ALBUM_W)return -1;
    int fd=ui_v4.pv_open(path,0);
    if(fd<0)return -1;
    struct a6400_preview_hdr hdr;
    if(v4_pv_read_exact(fd,&hdr,sizeof(hdr))!=0 ||
       hdr.magic!=A6400_PV_MAGIC || hdr.width!=want_w || hdr.height!=want_h) {
        ui_v4.pv_close(fd);
        return -1;
    }
    int16_t stride=0;
    void *dst=0;
    ui_v4.bmp_stride(slot->bmp,&stride);
    if(stride<slot->w*2 || ui_v4.bmp_lock(slot->bmp,-1,&dst)!=0 || !dst) {
        ui_v4.pv_close(fd);
        return -1;
    }
    int is32=(stride>=slot->w*4);
    unsigned dst_h=(unsigned)slot->h;
    for(unsigned y=0;y<dst_h;y++) {
        unsigned char *drow=(unsigned char *)dst+(size_t)y*(size_t)stride;
        if(is32) {
            uint32_t *d=(uint32_t *)drow;
            for(unsigned x=0;x<(unsigned)slot->w;x++)d[x]=0xff12100eu;
        } else {
            uint16_t *d=(uint16_t *)drow;
            for(unsigned x=0;x<(unsigned)slot->w;x++)d[x]=0x112Fu;
        }
    }
    uint16_t row_buf[A6400_ALBUM_W];
    unsigned next_dy=0u;
    int ok=1;
    for(unsigned sy=0;sy<want_h;sy++) {
        if(v4_pv_read_exact(fd,row_buf,(size_t)want_w*2u)!=0){ok=0;break;}
        while(next_dy<dst_h && ((next_dy*want_h)/dst_h)==sy) {
            unsigned char *drow=(unsigned char *)dst+(size_t)next_dy*(size_t)stride;
            v4_write_rgba4444_row(drow,is32,(unsigned)slot->w,row_buf,want_w);
            ++next_dy;
        }
    }
    ui_v4.bmp_unlock(slot->bmp);
    ui_v4.pv_close(fd);
    if(!ok)return -1;
    ui_v4.sprite_set_img((unsigned char *)slot->widget+0x164u,slot->res_id,0x80u);
    return 0;
}
static __attribute__((unused)) void v4_show_popup(uint32_t now) {
    static const int pop_dims[1][2]={{(int)A6400_POPUP_W,(int)A6400_POPUP_H}};
    char path[160];
    snprintf(path,sizeof(path),"%s/popup.bin",A6400_PV_DIR_RUNTIME);
    ui_v4.popup_active=1;
    ui_v4.popup_until_ms=now+A6400_POPUP_SHOW_MS;
    if(v4_load_bin_to_slot(&ui_v4.pop_slot,pop_dims,1,path,A6400_POPUP_W,A6400_POPUP_H)==0) {
        static const unsigned char frame_c[4]={234,78,16,255};
        hide_menu_widgets();
        v4_hide_slot(&ui_v4.alb_slot);
        if(ui_v4.popup_border) {
            place(ui_v4.popup_border,0,(int)A6400_POPUP_X-2,(int)A6400_POPUP_Y-2,
                  ui_v4.pop_slot.w+4,ui_v4.pop_slot.h+4);
            paint(ui_v4.popup_border,frame_c);
        }
        const float xy[2]={(float)A6400_POPUP_X,(float)A6400_POPUP_Y};
        ui.set_position(ui_v4.pop_slot.widget,xy);
        ui_v4.cache_img_set_size(ui_v4.pop_slot.widget,(float)ui_v4.pop_slot.w,(float)ui_v4.pop_slot.h);
        ui_v4.sprite_invalidate((unsigned char *)ui_v4.pop_slot.widget+0x164u);
        ui_v4.popup_is_native=1;
        v4_publish_display(A6400_DISP_NONE,0u);
        ui.move_last(ui.layer);
        sync_layer_active();
    } else {
        ui_v4.popup_is_native=0;
        v4_publish_display(A6400_DISP_POPUP,a6400_ui_v4_v1.album_head);
    }
}
static void v4_dismiss_popup(void) {
    if(!ui_v4.popup_active)return;
    int was_native=ui_v4.popup_is_native;
    ui_v4.popup_active=0;
    ui_v4.popup_is_native=0;
    ui_v4.popup_until_ms=0;
    if(was_native && !ui_v4.album_mode)v4_hide_native_img();
    if(!ui_v4.album_mode) {
        v4_publish_display(A6400_DISP_NONE,0u);
        sync_layer_active();
    }
}
static void v4_draw_album_hud(const char *top_text,const char *bot_text,int opaque_bg) {
    static const unsigned char bg_solid[4]={14,15,18,255};
    static const unsigned char bar_lut[4]={14,15,18,242};
    static const unsigned char accent_c[4]={245,86,18,255};
    static const unsigned char clear_c[4]={0,0,0,0};
    hide_menu_widgets();
    hide_toast_widgets();
    ui_toast.visible=0;
    v4_hide_slot(&ui_v4.pop_slot);
    if(ui_v4.popup_border) {
        place(ui_v4.popup_border,0,0,0,0,0);
        paint(ui_v4.popup_border,clear_c);
    }
    if(opaque_bg) {
        place(ui.background,0,0,0,640,480);
        paint(ui.background,bg_solid);
    }
    place(ui.panels[0],0,0,0,640,34);
    paint(ui.panels[0],bar_lut);
    place(ui_theme.header_accent,0,0,34,640,2);
    paint(ui_theme.header_accent,accent_c);
    place(ui.panels[1],0,0,444,640,36);
    paint(ui.panels[1],bar_lut);
    snprintf(ui.texts[0],sizeof(ui.texts[0]),"%s",top_text?top_text:"");
    snprintf(ui.texts[7],sizeof(ui.texts[7]),"%s",bot_text?bot_text:"");
    place(ui.labels[0],1,14,3,612,28);
    ui.set_text(ui.labels[0],ui.texts[0]);
    place(ui.labels[7],1,14,448,612,28);
    ui.set_text(ui.labels[7],ui.texts[7]);
    ui.move_last(ui.layer);
    sync_layer_active();
}
static __attribute__((unused)) void v4_album_sync_view(void) {
    if(!ui_v4.album_mode)return;
    uint32_t cnt=a6400_ui_v4_v1.album_count;
    if(cnt==0u) {
        ui_v4.album_mode=0;
        ui_v4.album_is_native=0;
        a6400_ui_v4_v1.album_view_seq=0;
        __sync_synchronize();
        v4_hide_native_img();
        v4_publish_display(A6400_DISP_NONE,0u);
        sync_layer_active();
        return;
    }
    cnt=film_album_browse_count(cnt);
    if(ui_v4.album_idx>=cnt)ui_v4.album_idx=cnt-1u;
    uint32_t slot=cnt-1u-ui_v4.album_idx;
    if(a6400_ui_v4_v1.album_view_seq==0u || a6400_ui_v4_v1.album_view_slot!=slot) {
        if(ui_v4.next_album_seq<a6400_ui_v4_v1.album_view_seq)
            ui_v4.next_album_seq=a6400_ui_v4_v1.album_view_seq;
        if(ui_v4.next_album_seq<a6400_ui_v4_v1.album_ready_seq)
            ui_v4.next_album_seq=a6400_ui_v4_v1.album_ready_seq;
        ui_v4.next_album_seq+=1u;
        if(ui_v4.next_album_seq==0u)ui_v4.next_album_seq=1u;
        a6400_ui_v4_v1.album_view_slot=slot;
        __sync_synchronize();
        a6400_ui_v4_v1.album_view_seq=ui_v4.next_album_seq;
    }
    int ready=a6400_ui_v4_v1.album_ready_seq==a6400_ui_v4_v1.album_view_seq &&
        a6400_ui_v4_v1.album_ready_slot==slot && a6400_ui_v4_v1.album_label[0];
    char path[160];
    snprintf(path,sizeof(path),"%s/current.bin",A6400_PV_DIR_RUNTIME);
    /* Widths stay multiples of 16 for GEN_QRCode RGBA4444. The 16:9 LCD is a
     * 640x480 buffer, so one pixel is 4/3 as wide as it is tall and a 3:2
     * file drawn 1:1 looks stretched. These slots take that width down to
     * 3/4; the bars on the left and right are the uncovered background. */
    static const int alb_dims[3][2]={{448,400},{368,320},{224,200}};
    int native_ok=ready && v4_load_bin_to_slot(&ui_v4.alb_slot,alb_dims,3,path,A6400_ALBUM_W,A6400_ALBUM_H)==0;
    char top[80],bot[80];
    if(native_ok) {
        snprintf(top,sizeof(top),UI_S_ALB_DIR_TOP,
                 (unsigned)(ui_v4.album_idx+1u),(unsigned)cnt,a6400_ui_v4_v1.album_label);
    } else {
        snprintf(top,sizeof(top),UI_S_ALB_DIR_WAIT,
                 (unsigned)(ui_v4.album_idx+1u),(unsigned)cnt);
    }
    snprintf(bot,sizeof(bot),UI_S_ALB_DIR_BOT);
    v4_draw_album_hud(top,bot,1);
    if(!native_ok) {
        char mid[48];
        if(!ready || !a6400_ui_v4_v1.album_label[0])snprintf(mid,sizeof(mid),"%s",UI_S_ALB_LOADING);
        else snprintf(mid,sizeof(mid),"%s",a6400_ui_v4_v1.album_label);
        snprintf(ui.texts[1],sizeof(ui.texts[1]),"%s",mid);
        place(ui.labels[1],1,240,210,160,36);
        ui.set_text(ui.labels[1],ui.texts[1]);
    }
    if(native_ok) {
        int px=(640-ui_v4.alb_slot.w)/2;
        int py=(int)A6400_ALBUM_BAND_Y0+((int)A6400_ALBUM_BAND_H-ui_v4.alb_slot.h)/2;
        const float xy[2]={(float)px,(float)py};
        ui.set_position(ui_v4.alb_slot.widget,xy);
        ui_v4.cache_img_set_size(ui_v4.alb_slot.widget,(float)ui_v4.alb_slot.w,(float)ui_v4.alb_slot.h);
        ui_v4.sprite_invalidate((unsigned char *)ui_v4.alb_slot.widget+0x164u);
        ui_v4.album_is_native=1;
        /* Keep external OSD blitter idle while native GEN_CacheInsImg renders zero-flicker. */
        v4_publish_display(A6400_DISP_NONE,slot);
    } else {
        ui_v4.album_is_native=0;
        v4_hide_native_img();
        v4_publish_display(A6400_DISP_NONE,slot);
    }
}
static void v4_exit_album(void) {
    if(!ui_v4.album_mode)return;
    ui_v4.album_mode=0;
    ui_v4.album_is_native=0;
    ui_v4.album_touch_down=0;
    a6400_ui_v4_v1.album_view_seq=0;
    __sync_synchronize();
    v4_hide_native_img();
    v4_publish_display(A6400_DISP_NONE,0u);
    if(!ui.view.menu.visible) {
        hide_menu_widgets();
        sync_layer_active();
    }
    record("ALBUM_EXIT",0,0);
}
static void visibility(void *unused,int active) {
    (void)unused;
    input_snapshot(active?3:5);
    record(active?"SHOW_BEFORE_MOVE":"HIDE_BEFORE_ACTIVE",active,0);
    if(active){
        ui.move_last(ui.layer);
        record("SHOW_AFTER_MOVE",0,0);
        if(ui.ticker_layer)ui.layer_active(ui.ticker_layer,1);
        if(ui.ticker)ui.set_animate(ui.ticker,1);
    }
    ui.layer_active(ui.layer,active);
    input_snapshot(active?4:6);
    if(active){ui.opened_ms=now_ms();ui.ever_opened=1;}
#if defined(A6400_UI_COMBO_DIAGNOSTIC) || (TRIAL_MS == 0 && !defined(A6400_UI_RENDER_DIAGNOSTIC))
    else ui.opened_ms=0;
#endif
    record(active?"FILM_OPEN":"FILM_CLOSE",ui.view.menu.settings.enabled,ui.view.menu.settings.stock);
}
static void draw(void *unused,const struct film_menu *menu) {
    (void)unused;
    record("DRAW_BEGIN",menu->page,menu->selected);
    v4_dismiss_popup();
    if(ui_v4.album_mode) {
        ui_v4.album_mode=0;
        ui_v4.album_is_native=0;
        ui_v4.album_touch_down=0;
        a6400_ui_v4_v1.album_view_seq=0;
        __sync_synchronize();
        v4_publish_display(A6400_DISP_NONE,0u);
    }
    v4_hide_native_img();
    hide_toast_widgets();
    ui_toast.visible=0;
#ifdef A6400_SOUND_LITE
    hide_menu_widgets();
    if(menu->visible)sl_draw_deck();
    return;
#endif
    int is_home=(menu->page==FILM_MENU_HOME);
    int is_lut=(menu->page==FILM_MENU_DETAILS);
    int is_v4=(menu->page==FILM_MENU_V4);
    int is_stocks=(menu->page==FILM_MENU_STOCKS || menu->page==FILM_MENU_STOCKS_P2);
    show_menu_chrome(is_home);

    static const unsigned char clear_c[4]={0,0,0,0};
    static const unsigned char h_orange[4]={234,78,16,255};
    static const unsigned char h_orange_dark[4]={168,48,8,255};
    static const unsigned char h_orange_bright[4]={250,96,24,255};
    static const unsigned char h_ember[4]={42,22,14,245};
    static const unsigned char h_cell[4]={24,26,31,245};
    static const unsigned char h_pill[4]={32,35,42,245};
    static const unsigned char h_white[4]={245,247,250,255};
    static const unsigned char h_silver[4]={130,136,148,240};
    static const unsigned char h_muted[4]={62,66,76,220};
    static const unsigned char h_sky[4]={76,186,238,255};

    /* 1. Single clean Hasselblad Header Line + Left Orange LED */
    unsigned qmax=(a6400_ui_status_v1.magic==0x53544156u && a6400_ui_status_v1.max_files)
                  ?(unsigned)a6400_ui_status_v1.max_files:1024u;
    unsigned qcur=(a6400_ui_status_v1.magic==0x53544156u)?(unsigned)a6400_ui_status_v1.scanned:0u;
    if(a6400_ui_status_v1.magic==0x53544156u && a6400_ui_status_v1.seq>0u)
        ui_theme.status_seq=a6400_ui_status_v1.seq;

    uint32_t dm=a6400_ui_v4_v1.dexp_mode, dp=a6400_ui_v4_v1.dexp_pending;
    uint32_t draw_now=now_ms();
    int dexp_warn=(ui_v4.dexp_blocked_until_ms && (int32_t)(ui_v4.dexp_blocked_until_ms-draw_now)>0);
    if(!dexp_warn)ui_v4.dexp_blocked_until_ms=0u;

    if(dexp_warn) {
        snprintf(ui.texts[0],sizeof(ui.texts[0]),"%s",UI_S_DEXP_BLOCK_HDR);
    } else if(is_lut) {
        snprintf(ui.texts[0],sizeof(ui.texts[0]),UI_S_HDR_LUT,
                 film_stock_display_name(menu->settings.stock));
    } else if(is_v4) {
        snprintf(ui.texts[0],sizeof(ui.texts[0]),"%s",UI_S_HDR_V4);
    } else if(is_stocks) {
        snprintf(ui.texts[0],sizeof(ui.texts[0]),UI_S_HDR_STOCKS,
                 menu->page==FILM_MENU_STOCKS_P2?2u:1u);
    } else if(dm!=0u || dp!=0u) {
        snprintf(ui.texts[0],sizeof(ui.texts[0]),UI_S_DEXP_HDR_FMT,
                 dexp_mode_name(dm?dm:A6400_DEXP_SCREEN),dexp_phase_name(dp));
    } else if(a6400_ui_status_v1.magic==0x53544156u && a6400_ui_status_v1.seq>0u) {
        if(a6400_ui_status_v1.active_jobs>0u) {
            snprintf(ui.texts[0],sizeof(ui.texts[0]),UI_S_HDR_BUSY,
                     menu->settings.enabled?UI_S_ON:UI_S_OFF,
                     (unsigned)a6400_ui_status_v1.active_jobs,(unsigned)a6400_ui_status_v1.applied);
        } else {
            snprintf(ui.texts[0],sizeof(ui.texts[0]),UI_S_HDR_IDLE,
                     menu->settings.enabled?UI_S_ON:UI_S_OFF,
                     (unsigned)a6400_ui_status_v1.applied,qcur,qmax);
        }
    } else {
        snprintf(ui.texts[0],sizeof(ui.texts[0]),UI_S_HDR_INIT,
                 menu->settings.enabled?UI_S_ON:UI_S_OFF);
    }
    place(ui_theme.status_dot,0,56,46,10,10);
    paint(ui_theme.status_dot,
          dexp_warn?h_orange_bright:
          (a6400_ui_status_v1.magic==0x53544156u && a6400_ui_status_v1.active_jobs>0u)?h_white:
          ((dm!=0u || dp!=0u)?h_sky:(menu->settings.enabled?h_orange:h_muted)));
    place(ui.labels[0],1,76,34,508,34);
    write_lbl(ui.labels[0],ui.texts[0]);

    /* 2. Page Content (slots 0..5 -> ui.panels[0..5] & ui.labels[1..6]) */
    for(unsigned row=0;row<6u;row++) {
        int x=0,y=0,w=0,h=0;
        int used=film_native_view_row(menu,row,ui.texts[row+1u],sizeof(ui.texts[0]),&x,&y,&w,&h);
        if(used<=0) {
            ui.texts[row+1u][0]=0;
            place(ui.panels[row],0,0,0,0,0);paint(ui.panels[row],clear_c);
            place(ui_theme.row_accents[row],0,0,0,0,0);paint(ui_theme.row_accents[row],clear_c);
            clear_chips(row);
            place(ui.labels[row+1u],1,0,0,0,0);write_lbl(ui.labels[row+1u],"");
            continue;
        }
        /* Live RAM text overrides */
        if(is_home) {
            if(row==0u && (dm!=0u || dp!=0u)) {
                snprintf(ui.texts[row+1u],sizeof(ui.texts[0]),UI_S_DEXP_TILE_FMT,
                         dexp_mode_name(dm?dm:A6400_DEXP_SCREEN),dexp_phase_name(dp));
            } else if(row==1u) {
                if(dp==1u) {
                    snprintf(ui.texts[row+1u],sizeof(ui.texts[0]),"%s",UI_S_DEXP_CANCEL_TILE);
                } else {
                    unsigned shown=film_album_browse_count(a6400_ui_v4_v1.album_count);
                    snprintf(ui.texts[row+1u],sizeof(ui.texts[0]),
                             a6400_ui_v4_v1.album_truncated?UI_S_ALBUM_TILE_MORE:UI_S_ALBUM_TILE_FMT,shown);
                }
            } else if(row==2u) {
#ifdef A6400_CHARO_SHUTTER
                int custom=(a6400_ui_status_v1.magic==0x53544156u && a6400_ui_status_v1.custom_charo);
                snprintf(ui.texts[row+1u],sizeof(ui.texts[0]),"%s",
                         charo_mode_label(ui_theme.shutter_sound_enabled,custom));
#endif
            } else if(row==3u) {
                snprintf(ui.texts[row+1u],sizeof(ui.texts[0]),"%s",quality_label(a6400_ui_hires_v1));
            }
        } else if(is_lut && row==0u && dp!=0u && !menu->settings.enabled) {
            snprintf(ui.texts[row+1u],sizeof(ui.texts[0]),"%s",UI_S_DEXP_LOCK_LUT);
        } else if(is_v4) {
            if(row==0u)snprintf(ui.texts[row+1u],sizeof(ui.texts[0]),UI_S_POPUP_FMT,
                                a6400_ui_v4_v1.preview_popup_en?UI_S_ON:UI_S_OFF);
            else if(row==1u)snprintf(ui.texts[row+1u],sizeof(ui.texts[0]),UI_S_TOUCH_FMT,
                                     !a6400_ui_v4_v1.native_touch_ok?UI_S_TOUCH_UNSUP:
                                     (a6400_ui_v4_v1.native_touch_en?UI_S_ON:UI_S_OFF));
            else if(row==3u) {
                if(dm==0u && dp==0u)snprintf(ui.texts[row+1u],sizeof(ui.texts[0]),"%s",UI_S_DEXP_OFF);
                else snprintf(ui.texts[row+1u],sizeof(ui.texts[0]),UI_S_DEXP_FMT,
                              dexp_mode_name(dm?dm:A6400_DEXP_SCREEN),dexp_phase_name(dp));
            }
        }

        /* Map visual slot to hit ID for press highlight */
        int hit_id=(int)row;
        if(is_home) {
            static const int hm[6]={FILM_HIT_OPEN_LUT,FILM_HIT_ALBUM_HOME,3,7,-1,FILM_HIT_CLOSE};
            hit_id=hm[row];
        } else if(is_lut || is_v4) {
            static const int sm[6]={0,1,3,4,-1,5};
            hit_id=sm[row];
        }
        int is_pressed=(hit_id>=0 && menu->selected==hit_id);
        int is_footer_btn=(row==5u);

        const unsigned char *bg=is_footer_btn?h_pill:h_cell;
        if(is_pressed) bg=h_orange_bright;
        else if(is_home && row==0u && menu->settings.enabled) bg=h_orange;
        else if(is_home && row==0u && (dm!=0u || dp!=0u)) bg=h_ember;
        else if(is_home && row==1u && dp==1u) bg=h_orange_dark;
        else if(is_home && row==1u && a6400_ui_v4_v1.album_count>0u) bg=h_ember;
        else if(is_home && row==3u && a6400_ui_hires_v1==2u) bg=h_ember;
        else if(is_lut && row==0u && menu->settings.enabled) bg=h_orange;
        else if(is_v4 && row==3u && (dm!=0u || dp!=0u)) bg=h_ember;
        else if(is_stocks && row<5u && ((menu->page==FILM_MENU_STOCKS_P2?5:0)+(int)row)==menu->settings.stock) bg=h_ember;

        record("ROW_PANEL_GEOMETRY",(int)row,used);
        place(ui.panels[row],0,x,y,w,h);
        paint(ui.panels[row],bg);

        int lx=x+16,ly=y+8,lw=w-28,lh=h-16;
        if(is_footer_btn) {
            place(ui_theme.row_accents[row],0,x,y,3,h);
            paint(ui_theme.row_accents[row],h_silver);
            clear_chips(row);
            lx=x+12;ly=y+7;lw=w-26;lh=28;
        } else if(is_home) {
            /* Ultra-clean Hasselblad 2x2 Tian-grid tile: single top-left square pip + clean text, no "---" bars. */
            const unsigned char *pip=h_muted;
            clear_chips(row);
            if(row==0u) {
                pip=menu->settings.enabled?h_white:((dm!=0u || dp!=0u)?h_sky:h_muted);
                place(ui_theme.row_accents[row],0,x+16,y+18,10,10);
                paint(ui_theme.row_accents[row],pip);
            } else if(row==1u) {
                if(dp==1u) {
                    place(ui_theme.row_accents[row],0,x+16,y+18,10,10);
                    paint(ui_theme.row_accents[row],h_white);
                } else {
                    int has=a6400_ui_v4_v1.album_count>0u;
                    place(ui_theme.row_accents[row],0,0,0,0,0);
                    draw_gallery_icon(row,x+16,y+16,has);
                }
            } else if(row==2u) {
#ifdef A6400_CHARO_SHUTTER
                pip=ui_theme.shutter_sound_enabled?h_sky:h_muted;
#else
                pip=h_orange;
#endif
                place(ui_theme.row_accents[row],0,x+16,y+18,10,10);
                paint(ui_theme.row_accents[row],pip);
            } else {
                pip=a6400_ui_hires_v1?h_orange:h_silver;
                place(ui_theme.row_accents[row],0,x+16,y+18,10,10);
                paint(ui_theme.row_accents[row],pip);
            }
            lx=x+14;ly=y+52;lw=w-30;lh=36;
        } else if(is_lut) {
            if(row==0u) {
                place(ui_theme.row_accents[row],0,0,0,0,0);
                draw_swiss_toggle(row,x+18,y+18,menu->settings.enabled,h_orange_dark);
                lx=x+84;ly=y+12;lw=w-102;lh=36;
            } else if(row==1u) {
                int s=(menu->settings.stock>=0 && menu->settings.stock<10)?menu->settings.stock:0;
                place(ui_theme.row_accents[row],0,x,y,4,h);paint(ui_theme.row_accents[row],stock_palettes[s][0]);
                draw_film_swatch(row,x+18,y+16,s);
                lx=x+64;ly=y+12;lw=w-82;lh=36;
            } else if(row==2u) {
                place(ui_theme.row_accents[row],0,x,y,4,h);paint(ui_theme.row_accents[row],menu->settings.grain?h_orange:h_muted);
                clear_chips(row);
                lx=x+24;ly=y+12;lw=w-42;lh=36;
            } else {
                place(ui_theme.row_accents[row],0,x,y,4,h);paint(ui_theme.row_accents[row],menu->settings.halation?h_orange:h_muted);
                draw_swiss_toggle(row,x+18,y+18,menu->settings.halation,h_orange);
                lx=x+84;ly=y+12;lw=w-102;lh=36;
            }
        } else if(is_v4) {
            if(row<2u || row==3u) {
                int on=(row==0u)?(a6400_ui_v4_v1.preview_popup_en!=0u):
                       (row==1u)?(a6400_ui_v4_v1.native_touch_ok && a6400_ui_v4_v1.native_touch_en):
                       (dm!=0u || dp!=0u);
                const unsigned char *ac=(row==0u)?h_orange:h_sky;
                place(ui_theme.row_accents[row],0,x,y,4,h);
                paint(ui_theme.row_accents[row],on?ac:h_muted);
                draw_swiss_toggle(row,x+18,y+18,on,ac);
                lx=x+84;ly=y+12;lw=w-102;lh=36;
            } else {
                place(ui_theme.row_accents[row],0,x,y,4,h);
                paint(ui_theme.row_accents[row],h_silver);
                clear_chips(row);
                lx=x+24;ly=y+12;lw=w-42;lh=36;
            }
        } else {
            /* 10-Film Stock list (5 clean rows per page with 3-color swatches) */
            int stock=(menu->page==FILM_MENU_STOCKS_P2?5:0)+(int)row;
            int cur=(stock==menu->settings.stock);
            place(ui_theme.row_accents[row],0,x,y,cur?6:3,h);
            paint(ui_theme.row_accents[row],cur?h_orange:h_muted);
            draw_film_swatch(row,x+18,y+10,stock);
            lx=x+60;ly=y+7;lw=w-78;lh=34;
        }
        place(ui.labels[row+1u],1,lx,ly,lw,lh);
        write_lbl(ui.labels[row+1u],ui.texts[row+1u]);
    }

    /* 3. Bottom-Left Secondary Action (Reset / Pager) OR Subtle Home Telemetry */
    int px=0,py=0,pw=0,ph=0;
    if(film_native_view_row(menu,6u,ui.texts[7],sizeof(ui.texts[0]),&px,&py,&pw,&ph)>0) {
        place(ui_theme.page_btn,0,px,py,pw,ph);
        paint(ui_theme.page_btn,menu->selected==6?h_orange_bright:h_pill);
        place(ui_theme.page_btn_accent,0,px,py,3,ph);
        paint(ui_theme.page_btn_accent,h_silver);
        place(ui.labels[7],1,px+12,py+7,pw-24,28);
    } else if(is_home) {
        place(ui_theme.page_btn,0,0,0,0,0);paint(ui_theme.page_btn,clear_c);
        place(ui_theme.page_btn_accent,0,0,0,0,0);paint(ui_theme.page_btn_accent,clear_c);
        snprintf(ui.texts[7],sizeof(ui.texts[7]),UI_S_FOOT_HOME,
                 (unsigned)a6400_ui_v4_v1.album_count,qcur,qmax);
        place(ui.labels[7],1,54,405,368,28);
    } else {
        place(ui_theme.page_btn,0,0,0,0,0);paint(ui_theme.page_btn,clear_c);
        place(ui_theme.page_btn_accent,0,0,0,0,0);paint(ui_theme.page_btn_accent,clear_c);
        ui.texts[7][0]=0;
        place(ui.labels[7],1,0,0,0,0);
    }
    write_lbl(ui.labels[7],ui.texts[7]);
    record("FILM_DRAW",menu->page,menu->selected);
}
static void settings(void *unused,const struct film_settings *s) {
    (void)unused;
    /* The persistent reader samples ui.view.menu.settings directly. Keep the
     * callback native and allocation-free; photo work stays in the daemon. */
    record("FILM_SETTINGS_SNAPSHOT",s->enabled,s->stock);
}
static void stop_trial(const char *why) {
    record("STOP_BEGIN",0,0);
    v4_dismiss_popup();
    v4_exit_album();
    film_native_view_hide(&ui.view);
    record("STOP_MENU_HIDDEN",0,0);
    if(ui.layer)ui.layer_active(ui.layer,0);
    ui_toast.visible=0;
#ifdef A6400_CHARO_SHUTTER
    ui_theme.shutter_pending=0;
    ui_theme.shutter_repeating=0;
#endif
    ui.running=0;
    input_snapshot(7);
    /* setAnimate only changes the flag/counter; retain allocated native nodes. */
    record("STOP_TICKER_BEFORE",0,0);
    if(ui.ticker)ui.set_animate(ui.ticker,0);
    record("STOP_LAYER_BEFORE",0,0);
    if(ui.ticker_layer)ui.layer_active(ui.ticker_layer,0);
    input_snapshot(8);
    record(why,(int)ui.ticks,0);
}
int a6400_ui_stop_v2(void) {
    if(!ui.attached || (uint32_t)syscall(SYS_gettid)!=ui.tid)return -1;
    stop_trial("FILM_EXPLICIT_STOP");return 0;
}
static int stop_old_v1(void) {
    void *old=dlopen("/tmp_bt/a6400-film-ui-v1.so",RTLD_NOW|RTLD_NOLOAD);
    if(!old)return 0;
    uintptr_t entry=(uintptr_t)dlsym(old,"a6400_ui_attach_v1");
    if(!entry){dlclose(old);return -1;}
    uintptr_t base=entry-0x12e0u;
    if(base!=0x10a73000u){dlclose(old);return -2;}
    /* This cleanup is solely for the hash-pinned v1 already observed in RAM.
     * Validate its owned state/vtable before calling native setters on it. */
    uintptr_t *old_ui=(uintptr_t *)(base+0x14088u);
    void *ticker=(void *)old_ui[0x70/4],*layer=(void *)old_ui[0x6c/4];
    if(old_ui[0x68/4]!=(uintptr_t)ui.system ||
       old_ui[0xb8/4]!=ui.tid || old_ui[0x170/4]!=1 || !ticker || !layer ||
       *(uintptr_t *)ticker!=(uintptr_t)old_ui+0xd0u ||
       old_ui[(0xd0+32*4)/4]!=base+0xaa4u) {
        dlclose(old);return -2;
    }
    old_ui[0x174/4]=0;
    ui.set_animate(ticker,0);ui.layer_active(layer,0);
    record("FILM_OLD_V1_STOPPED",(int)(uintptr_t)ticker,0);
    dlclose(old);return 0;
}
static void animate(void *self) {
    if(!ui.running)return;
    uint32_t cur_tid=(uint32_t)syscall(SYS_gettid);
    if(self!=ui.ticker) {
        ui.running=0;record("FILM_BAD_TICK_CONTEXT",0,0);return;
    }
    if(cur_tid!=ui.tid) {
        record("FILM_TICK_TID_RECOVER",(int)ui.tid,(int)cur_tid);
        ui.tid=cur_tid;
    }
    if(a6400_ui_stop_requested_v1==1u) {
        stop_trial("FILM_STOP_REQUESTED");
        __sync_synchronize();a6400_ui_stop_requested_v1=2u;return;
    }
    uint32_t now=now_ms();
    if(!now){stop_trial("FILM_CLOCK_ERROR");return;}
    if(!ui.started) {ui.started=now;record("FILM_TICK_READY",(int)(uintptr_t)self,0);}
    ++ui.ticks;
#if TRIAL_MS != 0
    if(now-ui.started>=TRIAL_MS) {
        stop_trial("FILM_TRIAL_STOPPED");return;
    }
#endif
#if A6400_UI_DISPLAY_MS != 0
    if(ui.opened_ms && now-ui.opened_ms>=A6400_UI_DISPLAY_MS) {
#if defined(A6400_UI_COMBO_DIAGNOSTIC) || (TRIAL_MS == 0 && !defined(A6400_UI_RENDER_DIAGNOSTIC))
        /* Hide only: the separate active ticker still watches the chord. */
        film_native_view_hide(&ui.view);
        record("COMBO_AUTO_HIDE",0,0);return;
#else
        stop_trial("TRIAL_AUTO_HIDE");return;
#endif
    }
#endif
#if !defined(A6400_UI_COMBO_DIAGNOSTIC) && !(TRIAL_MS == 0 && !defined(A6400_UI_RENDER_DIAGNOSTIC))
    if(ui.ever_opened && !ui.view.menu.visible) {
        stop_trial("TRIAL_CLOSE_DONE");return;
    }
#endif
    if(now-ui.last_tick<15u)return;
    ui.last_tick=now;
    if(ui_toast.bg && ui.layer) {
        if(ui.view.menu.visible || ui_v4.album_mode) {
            ui_toast.visible=0;
        } else {
#if !defined(A6400_UI_RENDER_DIAGNOSTIC) && TRIAL_MS == 0
            if(!ui_v4.boot_toast_done) {
                ui_v4.boot_toast_done=1;
                if(a6400_ui_toast_v1.seq==ui_toast.seq) {
                    static const unsigned char boot_bg[4]={18,22,29,215};
                    static const unsigned char boot_ok[4]={46,196,96,255};
                    hide_menu_widgets();
                    place(ui_toast.bg,0,408,16,216,28);
                    place(ui_toast.accent,0,408,16,6,28);
                    place(ui_toast.label,1,420,18,200,24);
                    ui.set_color(ui_toast.bg,boot_bg);
                    ui.set_color(ui_toast.accent,boot_ok);
#ifdef A6400_SOUND_LITE
                    ui.set_text(ui_toast.label,SL_S_BOOT_TOAST);
#else
                    ui.set_text(ui_toast.label,UI_S_BOOT_READY_TOAST);
#endif
                    ui.move_last(ui.layer);
                    ui_toast.visible=1;
                    ui_toast.until_ms=now+1200u;
                    sync_layer_active();
                    record("V4_BOOT_READY_TOAST",0,0);
                }
            }
#endif
            uint32_t seq=a6400_ui_toast_v1.seq;
            if(seq!=ui_toast.seq && a6400_ui_toast_v1.magic==0x54534f54u) {
                for(unsigned i=0;i<63;i++)ui_toast.text[i]=a6400_ui_toast_v1.text[i];
                ui_toast.text[63]=0;
                uint32_t kind=a6400_ui_toast_v1.kind;
                uint32_t dur=a6400_ui_toast_v1.duration_ms;
                ui_toast.seq=seq;
                if(ui_toast.text[0]) {
                    static const unsigned char bg_c[4]={18,22,29,235};
                    static const unsigned char k1_c[4]={234,158,28,255};
                    static const unsigned char k2_c[4]={46,196,96,255};
                    static const unsigned char k3_c[4]={220,62,62,255};
                    hide_menu_widgets();
                    place(ui_toast.bg,0,24,390,480,36);
                    place(ui_toast.accent,0,24,390,8,36);
                    place(ui_toast.label,1,40,394,456,28);
                    ui.set_color(ui_toast.bg,bg_c);
                    ui.set_color(ui_toast.accent,kind==2?k2_c:kind==3?k3_c:k1_c);
                    ui.set_text(ui_toast.label,ui_toast.text);
                    ui.move_last(ui.layer);
                    ui_toast.visible=1;
                    ui_toast.until_ms=now+(dur?dur:3000u);
                    sync_layer_active();
                } else if(ui_toast.visible) {
                    hide_toast_widgets();
                    ui_toast.visible=0;
                    sync_layer_active();
                }
            }
            if(ui_toast.visible && (int32_t)(now-ui_toast.until_ms)>=0) {
                hide_toast_widgets();
                ui_toast.visible=0;
                sync_layer_active();
            }
        }
    }
#ifdef A6400_UI_RENDER_DIAGNOSTIC
    /* Isolate rendering from the entire input state machine. A simulated
     * chord can lose its release latch after a startup dispatch gap and
     * therefore never open. No synthetic events reach Sony. */
    if(!ui.ever_opened && now-ui.started>=1000u) {
        record("RENDER_DIAGNOSTIC_BEGIN",(int)(now-ui.started),0);
        ui.view.menu.visible=1;
        draw(0,&ui.view.menu);
        visibility(0,1);
    }
#ifdef A6400_UI_TOUCH_CLOSE_DIAGNOSTIC
    /* Read-only native getters: no capture, focus, mode or key setters.
     * The enum is 5=pressed, 6=released. Other rows remain inert in this
     * diagnostic; only a complete Close gesture can stop our owned UI. */
    if(ui.view.menu.visible) {
        short x=32767,y=32767;
        ui.mouse_position(ui.system,0,&x,&y);
        int state=ui.mouse_state(ui.system,0,0);
        if(state!=ui.last_touch) {
            record(state==5?"CLOSE_TOUCH_DOWN":state==6?"CLOSE_TOUCH_UP":"CLOSE_TOUCH_INVALID",x,y);
            ui.last_touch=state;
        }
        if(state==5 && film_menu_hit(ui.view.menu.page,x,y)!=2)x=y=32767;
        unsigned change=film_native_view_step(&ui.view,now,0,0,0,state,x,y);
        if(change&FILM_MENU_CLOSED) {
            stop_trial("TRIAL_TOUCH_CLOSE");return;
        }
    }
#endif
    return;
#endif
    if(!get_key_instance_fn || get_key_instance_fn()!=ui.keys){stop_trial("FILM_KEY_INSTANCE_CHANGED");return;}
    int menu=ui.key(ui.keys,0x1ab,0),zoom=ui.key(ui.keys,0x70dc,0),other=0;
#ifdef A6400_SOUND_LITE
    if(ui.view.menu.visible) {
        if(ui.key(ui.keys,A6400_KEY_SHUTTER,0)==1 ||
           ui.key(ui.keys,A6400_KEY_PLAY,0)==1 ||
           ui.key(ui.keys,A6400_KEY_TRASH,0)==1)other=1;
    } else {
        static const int dismiss_keys[]={0x3,0x15,0x30,0x66,0xa7,0x1ac,0x1af,
            0x1f0,0x1f4,0x1f7,0x1f8,0x1f9,0x7095,0x70a9,0x70aa,0x70ab,
            0x70ad,0x70c4,0x10000000,0x10000001,0x10000003,0x10000004};
        for(unsigned i=0;i<sizeof(dismiss_keys)/sizeof(*dismiss_keys);i++)
            if(ui.key(ui.keys,dismiss_keys[i],0)==1)other=1;
    }
    short x=32767,y=32767;
    ui.mouse_position(ui.system,0,&x,&y);
    int touch=ui.mouse_state(ui.system,0,0);
    int v4_busy_before=(ui.view.menu.visible);
    /* Pass real touch state (5/6) so film_native_view_step never auto-closes on invalid state,
     * while (32767,32767) keeps film_menu's default hitboxes inert so Sound Lite owns touch. */
    unsigned change=film_native_view_step(&ui.view,now,menu,zoom,other,touch,32767,32767);
    if(change&FILM_MENU_OPENED) {
        sl_scan_slots();
        draw(0,&ui.view.menu);
    }
    if(ui.view.menu.visible) {
        int dirty=0, play_now=0;
        if(touch==5) {
            if(!sl.touch_down && x>=0 && x<640 && y>=0 && y<480) {
                sl.touch_down=1; sl.touch_x=x; sl.touch_y=y;
            }
        } else if(touch==6 && sl.touch_down) {
            short tx=sl.touch_x, ty=sl.touch_y;
            sl.touch_down=0;
            if(tx<22 || tx>=618 || ty<26 || ty>=454 ||
               (tx>=384 && tx<600 && ty>=386 && ty<438)) {
                film_native_view_hide(&ui.view);
            } else if(tx>=36 && tx<354 && ty>=74 && ty<74+SL_VIS_ROWS*52) {
                int r=(ty-74)/52;
                int idx=sl.scroll+r;
                if(idx>=0 && idx<sl.count) {
                    sl_activate_slot(idx);
                    dirty=1; play_now=1;
                }
            } else if(tx>=36 && tx<192 && ty>=394 && ty<442) {
                if(sl.scroll>0) {
                    sl.scroll-=SL_VIS_ROWS;
                    if(sl.scroll<0)sl.scroll=0;
                    dirty=1;
                }
            } else if(tx>=198 && tx<354 && ty>=394 && ty<442) {
                if(sl.scroll+SL_VIS_ROWS<sl.count) {
                    sl.scroll+=SL_VIS_ROWS;
                    dirty=1;
                }
            } else if(tx>=384 && tx<600 && ty>=210 && ty<262) {
                play_now=1;
            } else if(tx>=384 && tx<600 && ty>=276 && ty<328) {
                sl.hi_repeat^=1;
                dirty=1;
            }
        }
        if(dirty && ui.view.menu.visible)draw(0,&ui.view.menu);
        if(play_now && ui_theme.beep_play) {
            int pid=sl_current_sound_id();
            if(pid) {
                ui_theme.last_shutter_beep_ms=now;
                ui_theme.beep_play(ui.background,pid);
            }
        }
    } else {
        sl.touch_down=0;
    }
#else
    static const int dismiss_keys[]={0x3,0x15,0x30,0x66,0xa7,0x1ac,0x1af,
        0x1f0,0x1f4,0x1f7,0x1f8,0x1f9,0x7095,0x70a9,0x70aa,0x70ab,
        0x70ad,0x70c4,0x10000000,0x10000001,0x10000003,0x10000004};
    for(unsigned i=0;i<sizeof(dismiss_keys)/sizeof(*dismiss_keys);i++)
        if(ui.key(ui.keys,dismiss_keys[i],0)==1)other=1;
    short x=32767,y=32767;
    ui.mouse_position(ui.system,0,&x,&y);
    int touch=ui.mouse_state(ui.system,0,0);
#ifdef A6400_UI_COMBO_DIAGNOSTIC
    /* Like the validated Close-only round, this does not consume native keys
     * or touch. Observed physical actions dismiss our layer; native redraw
     * ordering is not guaranteed because Sony still receives those events.
     * Settings are enabled only in the separate RAM-settings diagnostic. */
    if(ui.view.menu.have_sample && now-ui.view.menu.last_sample>250u)
        record("COMBO_GAP",(int)(now-ui.view.menu.last_sample),
            ui.view.menu.released | (ui.view.menu.pending<<1) |
            (ui.view.menu.fired<<2) | ((menu==1)<<3) |
            ((zoom==1)<<4) | ((other!=0)<<5));
    if(menu!=ui.last_menu || zoom!=ui.last_zoom || other!=ui.last_other) {
        record("COMBO_KEYS",menu,zoom);
        if(other)record("COMBO_OTHER",other,0);
        ui.last_menu=menu;ui.last_zoom=zoom;ui.last_other=other;
    }
#ifndef A6400_UI_SETTINGS_DIAGNOSTIC
    if(touch==5 && film_menu_hit(ui.view.menu.page,x,y)!=2)x=y=32767;
#endif
#endif
    int v4_busy_before=(ui.view.menu.visible || ui_v4.album_mode || ui_v4.popup_active);
    unsigned change=film_native_view_step(&ui.view,now,menu,zoom,other,touch,x,y);
    if(change&FILM_MENU_SETTINGS_CHANGED) {
        if(ui.view.menu.settings.enabled) {
            if(a6400_ui_v4_v1.dexp_pending!=0u) {
                /* Mutual exclusion: cannot enable LUT while Double Exposure has an unfinished shot (1/2 or blending 2/2). */
                ui.view.menu.settings.enabled=0;
                ui.view.drawn_enabled=0;
                settings(0,&ui.view.menu.settings);
                ui_v4.dexp_blocked_until_ms=now+3500u;
                a6400_ui_toast_v1.kind=3u;
                a6400_ui_toast_v1.duration_ms=3500u;
                snprintf((char *)a6400_ui_toast_v1.text,sizeof(a6400_ui_toast_v1.text),"%s",UI_S_DEXP_BLOCK_TOAST);
                a6400_ui_toast_v1.seq+=1u;
                draw(0,&ui.view.menu);
                record("V4_DEXP_BLOCK_LUT",(int)a6400_ui_v4_v1.dexp_mode,(int)a6400_ui_v4_v1.dexp_pending);
            } else if(a6400_ui_v4_v1.dexp_mode!=0u) {
                /* Mutual exclusion: enabling LUT when Double Exposure is idle (0/2) auto-disables Double Exposure. */
                a6400_ui_v4_v1.dexp_mode=0u;
                a6400_ui_v4_v1.dexp_seq+=1u;
                draw(0,&ui.view.menu);
                record("V4_LUT_AUTO_OFF_DEXP",0,0);
            }
        }
        record("FILM_TAP",x,y);
    }
    if(change&FILM_MENU_DEXP_TOGGLED) {
        uint32_t next_mode=(a6400_ui_v4_v1.dexp_mode+1u)&3u;
        a6400_ui_v4_v1.dexp_mode=next_mode;
        a6400_ui_v4_v1.dexp_seq+=1u;
        ui_v4.dexp_blocked_until_ms=0u;
        if(next_mode!=0u && ui.view.menu.settings.enabled) {
            /* Mutual exclusion: enabling Double Exposure automatically turns OFF LUT to reduce CPU load. */
            ui.view.menu.settings.enabled=0;
            ui.view.drawn_enabled=0;
            settings(0,&ui.view.menu.settings);
            record("V4_DEXP_AUTO_OFF_LUT",(int)next_mode,0);
        } else if(next_mode==0u && a6400_ui_v4_v1.dexp_pending==1u) {
            a6400_ui_v4_v1.dexp_pending=0u;
        }
        draw(0,&ui.view.menu);
        record("V4_DEXP_TOGGLE",(int)next_mode,(int)a6400_ui_v4_v1.dexp_pending);
    }
    if(change&FILM_MENU_HIRES_TOGGLED) {
        a6400_ui_hires_v1=(a6400_ui_hires_v1+1u)%3u;
        draw(0,&ui.view.menu);
        record("FILM_HIRES_TOGGLE",(int)a6400_ui_hires_v1,0);
    }
    if(change&FILM_MENU_POPUP_TOGGLED) {
        a6400_ui_v4_v1.preview_popup_en^=1u;
        if(!a6400_ui_v4_v1.preview_popup_en)v4_dismiss_popup();
        draw(0,&ui.view.menu);
        record("V4_POPUP_TOGGLE",(int)a6400_ui_v4_v1.preview_popup_en,0);
    }
    if(ui.view.menu.visible && ui.view.menu.page==FILM_MENU_V4 &&
       !a6400_ui_v4_v1.native_touch_ok &&
       (uint32_t)(now-ui_v4.mtf2_last_probe_ms)>=1000u) {
        v4_reprobe_mtf2(now);
        if(a6400_ui_v4_v1.native_touch_ok)draw(0,&ui.view.menu);
    }
    if(change&FILM_MENU_TOUCH_TOGGLED) {
        if(!a6400_ui_v4_v1.native_touch_ok) {
            v4_reprobe_mtf2(now);
            if(a6400_ui_v4_v1.native_touch_ok)a6400_ui_v4_v1.native_touch_en=1u;
        } else {
            a6400_ui_v4_v1.native_touch_en^=1u;
        }
        draw(0,&ui.view.menu);
        record("V4_TOUCH_TOGGLE",(int)a6400_ui_v4_v1.native_touch_en,(int)a6400_ui_v4_v1.native_touch_ok);
    }
    if(change&FILM_MENU_ALBUM_BROWSE) {
        if(a6400_ui_v4_v1.dexp_pending==1u) {
            /* Cancel pending Shot 1/2 directly from Home Top-Right tile while keeping Double Exposure mode ON. */
            a6400_ui_v4_v1.dexp_pending=0u;
            a6400_ui_v4_v1.dexp_seq+=1u;
            ui_v4.dexp_blocked_until_ms=0u;
            a6400_ui_toast_v1.kind=2u;
            a6400_ui_toast_v1.duration_ms=2500u;
            snprintf((char *)a6400_ui_toast_v1.text,sizeof(a6400_ui_toast_v1.text),"%s",UI_S_DEXP_CANCEL_TOAST);
            a6400_ui_toast_v1.seq+=1u;
            draw(0,&ui.view.menu);
            record("V4_DEXP_CANCEL_SHOT1",(int)a6400_ui_v4_v1.dexp_mode,0);
        } else if(a6400_ui_v4_v1.album_count>0u) {
            film_native_view_hide(&ui.view);
            v4_dismiss_popup();
            ui_v4.album_mode=2;
            ui_v4.album_idx=0u;
            ui_v4.album_touch_down=0;
            v4_album_sync_view();
            record("V4_ALBUM_BROWSE_OPEN",(int)a6400_ui_v4_v1.album_count,ui_v4.album_is_native);
        } else {
            a6400_ui_v4_v1.album_scan_req+=1u;
            draw(0,&ui.view.menu);
        }
    }
    /* V4.1 Instant filter preview pop-up card after shot finishes (native zero-flicker). */
    if(a6400_ui_v4_v1.magic==A6400_V4_MAGIC && a6400_ui_v4_v1.latest_seq!=ui_v4.seen_latest_seq) {
        ui_v4.seen_latest_seq=a6400_ui_v4_v1.latest_seq;
        if(a6400_ui_v4_v1.preview_popup_en && !ui.view.menu.visible && !ui_v4.album_mode) {
            v4_show_popup(now);
            record("V4_POPUP_SHOW",(int)ui_v4.seen_latest_seq,ui_v4.popup_is_native);
        }
    }
    if(ui_v4.popup_active) {
        if((int32_t)(now-ui_v4.popup_until_ms)>=0 || touch==5 || other || menu==1 ||
           ui.view.menu.visible || ui_v4.album_mode) {
            v4_dismiss_popup();
            record("V4_POPUP_HIDE",0,0);
        }
    }
    /* V4.1 Graded Album direct browser (opened ONLY via custom menu's Top-Right tile;
     * Sony's native Playback button is never hijacked or intercepted). */
    if(ui_v4.album_mode && !ui.view.menu.visible) {
        if(a6400_ui_v4_v1.album_ready_seq!=ui_v4.seen_album_ready ||
           a6400_ui_v4_v1.album_count!=ui_v4.seen_album_count) {
            ui_v4.seen_album_ready=a6400_ui_v4_v1.album_ready_seq;
            ui_v4.seen_album_count=a6400_ui_v4_v1.album_count;
            v4_album_sync_view();
        }
        int nav_left=(ui.key(ui.keys,A6400_KEY_LEFT,0)==1 || ui.key(ui.keys,A6400_KEY_WHEEL_CCW,0)==1);
        int nav_right=(ui.key(ui.keys,A6400_KEY_RIGHT,0)==1 || ui.key(ui.keys,A6400_KEY_WHEEL_CW,0)==1);
        int nav_exit=(menu==1 || ui.key(ui.keys,A6400_KEY_SHUTTER,0)==1 ||
                      ui.key(ui.keys,A6400_KEY_TRASH,0)==1 ||
                      ui.key(ui.keys,A6400_KEY_PLAY,0)==1);
        int dirty=0;
        if(nav_exit && !ui_v4.last_nav_exit) {
            v4_exit_album();
        } else {
            if(nav_left && !ui_v4.last_nav_left) {
                unsigned limit=film_album_browse_count(a6400_ui_v4_v1.album_count);
                if(limit>0u && ui_v4.album_idx+1u<limit) {
                    ui_v4.album_idx+=1u;
                    dirty=1;
                }
            }
            if(nav_right && !ui_v4.last_nav_right) {
                if(ui_v4.album_idx>0u) {
                    ui_v4.album_idx-=1u;
                    dirty=1;
                }
            }
            if(touch==5) {
                if(!ui_v4.album_touch_down && x>=0 && x<640 && y>=0 && y<480) {
                    ui_v4.album_touch_down=1;
                    ui_v4.album_touch_x=x;
                    ui_v4.album_touch_y=y;
                }
            } else if(touch==6 && ui_v4.album_touch_down) {
                short tx=ui_v4.album_touch_x;
                ui_v4.album_touch_down=0;
                unsigned limit=film_album_browse_count(a6400_ui_v4_v1.album_count);
                if(tx<200) {
                    if(limit>0u && ui_v4.album_idx+1u<limit){ui_v4.album_idx+=1u;dirty=1;}
                } else if(tx>440) {
                    if(ui_v4.album_idx>0u){ui_v4.album_idx-=1u;dirty=1;}
                } else {
                    v4_exit_album();
                }
            }
            if(dirty && ui_v4.album_mode)v4_album_sync_view();
        }
        ui_v4.last_nav_left=nav_left;
        ui_v4.last_nav_right=nav_right;
        ui_v4.last_nav_exit=nav_exit;
    } else {
        ui_v4.last_nav_left=ui_v4.last_nav_right=ui_v4.last_nav_exit=0;
        ui_v4.album_touch_down=0;
        if(ui.view.menu.visible && ui.view.menu.page==FILM_MENU_HOME &&
           (a6400_ui_v4_v1.album_count!=ui_v4.seen_album_count ||
            a6400_ui_v4_v1.album_truncated!=ui_v4.seen_album_trunc)) {
            ui_v4.seen_album_count=a6400_ui_v4_v1.album_count;
            ui_v4.seen_album_trunc=a6400_ui_v4_v1.album_truncated;
            draw(0,&ui.view.menu);
        }
    }
#endif /* !A6400_SOUND_LITE */
    /* V4.0 Native MENU Full Touch (MTF2, credit: ivica00). Suppressed whenever our
     * Film Menu, Album Preview, or Instant Pop-up is active (or until the finger
     * lifts after closing one of them), or when toggled OFF. */
    int v4_busy_now=(ui.view.menu.visible || ui_v4.album_mode || ui_v4.popup_active);
    if(v4_busy_now) {
        ui_v4.mtf2_wait_release=1;
    } else if(v4_busy_before) {
        ui_v4.mtf2_wait_release=(touch==5)?1:0;
    } else if(ui_v4.mtf2_wait_release && touch==6) {
        ui_v4.mtf2_wait_release=0;
    }
#ifndef A6400_DISABLE_MTF2
    int allow_mtf2=(a6400_ui_v4_v1.native_touch_en!=0u) &&
                   !v4_busy_before && !v4_busy_now && !ui_v4.mtf2_wait_release;
    if(allow_mtf2 && !a6400_ui_v4_v1.native_touch_ok && touch==5 &&
       (uint32_t)(now-ui_v4.mtf2_last_probe_ms)>=2000u) {
        v4_reprobe_mtf2(now);
    }
    if(mtf2_on_touch_poll(&ui_v4.mtf2,allow_mtf2,touch,x,y)!=MTF2_ACT_NONE) {
        a6400_ui_v4_v1.native_touch_hits=ui_v4.mtf2.hits;
        record("MTF2_ACT",(int)ui_v4.mtf2.last_action,(int)ui_v4.mtf2.hits);
    }
#else
    (void)v4_busy_before; (void)v4_busy_now;
#endif
#ifdef A6400_CHARO_SHUTTER
#ifndef A6400_SOUND_LITE
    if(change&FILM_MENU_SOUND_TOGGLED) {
#if defined(A6400_CHARO_MULTI) && A6400_CHARO_MULTI
        ui_theme.shutter_sound_enabled=(ui_theme.shutter_sound_enabled+1)&3;
#else
        ui_theme.shutter_sound_enabled^=1;
#endif
        if(!ui_theme.shutter_sound_enabled){ui_theme.shutter_pending=0;ui_theme.shutter_repeating=0;}
        draw(0,&ui.view.menu);
        record("CHARO_SOUND_TOGGLE",ui_theme.shutter_sound_enabled,0);
        int preview_id=charo_sound_id(ui_theme.shutter_sound_enabled);
        if(preview_id && ui_theme.beep_play) {
            ui_theme.last_shutter_beep_ms=now;
            ui_theme.beep_play(ui.background,preview_id);
            record("CHARO_MENU_BEEP_PREVIEW",preview_id,(int)now);
        }
    }
#endif
    int shutter_down=ui.key(ui.keys,A6400_CHARO_SHUTTER_KEY,0);
#ifdef A6400_SOUND_LITE
    int cur_sound_id=sl_current_sound_id();
    int allow_repeat=sl.hi_repeat;
#else
    int cur_sound_id=charo_sound_id(ui_theme.shutter_sound_enabled);
    int allow_repeat=1;
#endif
    if(shutter_down==1 && cur_sound_id) {
        if(ui_theme.last_shutter_key==0) {
            ui_theme.shutter_pending=1;
            ui_theme.shutter_repeating=0;
            ui_theme.shutter_arm_ms=now;
            ui_theme.shutter_hold_ms=now;
            record("CHARO_SHUTTER_ARMED",cur_sound_id,(int)now);
        } else if(allow_repeat && !ui_theme.shutter_pending) {
            uint32_t wait_ms=ui_theme.shutter_repeating?A6400_CHARO_REPEAT_MS:A6400_CHARO_HOLD_DELAY_MS;
            if((uint32_t)(now-ui_theme.shutter_hold_ms)>=wait_ms) {
                ui_theme.shutter_pending=1;
                ui_theme.shutter_repeating=1;
                ui_theme.shutter_arm_ms=now;
                ui_theme.shutter_hold_ms=now;
            }
        }
    } else {
        ui_theme.shutter_repeating=0;
    }
    ui_theme.last_shutter_key=(shutter_down==1)?1:0;
    if(ui_theme.shutter_pending &&
       cur_sound_id &&
       ui_theme.beep_play
#if A6400_CHARO_DELAY_MS > 0
       && (uint32_t)(now-ui_theme.shutter_arm_ms)>=A6400_CHARO_DELAY_MS
#endif
       ) {
        ui_theme.shutter_pending=0;
        int active_id=cur_sound_id;
        if(active_id
#if A6400_CHARO_COOLDOWN_MS > 0
           && (!ui_theme.last_shutter_beep_ms ||
               (uint32_t)(now-ui_theme.last_shutter_beep_ms)>=A6400_CHARO_COOLDOWN_MS)
#endif
           ) {
            ui_theme.last_shutter_beep_ms=now;
            ui_theme.beep_play(ui.background,active_id);
            record("CHARO_SHUTTER_BEEP",active_id,(int)now);
        }
    }
#endif
    if(ui.view.menu.visible && (
       (a6400_ui_status_v1.magic==0x53544156u && a6400_ui_status_v1.seq!=ui_theme.status_seq) ||
       a6400_ui_v4_v1.dexp_pending!=ui_v4.seen_dexp_pending ||
       (ui_v4.dexp_blocked_until_ms && (int32_t)(now-ui_v4.dexp_blocked_until_ms)>=0))) {
        ui_v4.seen_dexp_pending=a6400_ui_v4_v1.dexp_pending;
        draw(0,&ui.view.menu);
    }
    if(ui.ticks%300u==0)record("FILM_ALIVE",(int)ui.ticks,ui.view.menu.visible);
}
static void *make(int label) {
    void *p=calloc(1,label?0x400u:0x200u);
    if(p){
        record(label?"LABEL_CTOR_BEFORE":"PANEL_CTOR_BEFORE",0,0);
        (label?ui.label_ctor:ui.panel_ctor)(p);
        if(label){
            /* The first five IDs are static firmware resources. The native
             * Label's sixth entry was 0x0100061d, a runtime override from a
             * previous boot: it is absent now and the original font resolver
             * rejects it. Retain the constructor's unset sixth entry. */
            static const uint32_t attributes[6]={0x02076000,0x0207a00f,
                0x0207a000,0x02066027,0x02076002,0xffffffff};
            void *ctor_font=*(void **)((unsigned char *)p+0x1f4);
            memcpy((unsigned char *)p+0x2b4,attributes,sizeof(attributes));
            record("LABEL_FONT_BEFORE",(int)(uintptr_t)p,0);
            ui.set_font(p,attributes[0]);
            if(!*(void **)((unsigned char *)p+0x1f4)) {
                static const uint32_t fb_fonts[3]={0x0207a00f,0x0207a000,0x02076002};
                for(unsigned fi=0;fi<3u && !*(void **)((unsigned char *)p+0x1f4);fi++)
                    ui.set_font(p,fb_fonts[fi]);
                if(!*(void **)((unsigned char *)p+0x1f4) && ctor_font)
                    *(void **)((unsigned char *)p+0x1f4)=ctor_font;
            }
            record("LABEL_FONT_AFTER",*(int *)((unsigned char *)p+0x1f4),0);
            if(!*(void **)((unsigned char *)p+0x1f4))return 0;
        }
        record("WIDGET_LAYER_BEFORE",label,0);
        ui.set_layer(p,&ui.layer_id);
        record("WIDGET_LAYER_AFTER",label,0);
    }
    return p;
}
int a6400_ui_attach_v1(void *system,unsigned tid) {
    record("FILM_ATTACH_BEGIN",(int)tid,(int)(uintptr_t)system);
    if(ui.attached || !system || (uintptr_t)system<0x10000u || tid!=(uint32_t)syscall(SYS_gettid))return -1;
    if(resolve())return -2;
    ui.system=system;ui.tid=tid;ui.keys=get_key_instance_fn();
    if(!ui.keys || (uintptr_t)ui.keys<0x10000u || ui.key(ui.keys,0x1ab,0)<0 || ui.key(ui.keys,0x70dc,0)<0)return -3;
    if(stop_old_v1())return -7;
    input_snapshot(0);
    struct film_settings initial={0,0,2,1};
    const struct film_native_view_ops ops={visibility,draw,settings};
    film_native_view_init(&ui.view,&ops,0,&initial);
#if defined(A6400_UI_COMBO_DIAGNOSTIC)
    ui.layer_id=ui.make_id("A6400_FILM_COMBO_V3" A6400_UI_INSTANCE_SUFFIX);
#elif defined(A6400_UI_TOUCH_CLOSE_DIAGNOSTIC)
    ui.layer_id=ui.make_id("A6400_FILM_CLOSE_TOUCH_V3");
#elif defined(A6400_INPUT_DIAGNOSTIC) && A6400_INPUT_DIAGNOSTIC == 2
    ui.layer_id=ui.make_id("A6400_FILM_INPUT2_V3");
#elif defined(A6400_INPUT_DIAGNOSTIC)
    ui.layer_id=ui.make_id("A6400_FILM_INPUT1_V3");
#elif defined(A6400_UI_RENDER_DIAGNOSTIC)
    ui.layer_id=ui.make_id("A6400_FILM_NATIVE_FONTFIX3_V3");
#else
    ui.layer_id=ui.make_id("A6400_FILM_NATIVE_TRIAL_V3");
#endif
    record("CREATE_LAYER_BEFORE",0,0);
    ui.layer=ui.create_layer(system,&ui.layer_id);
    if(!ui.layer)return -4;
    ui.layer_active(ui.layer,0);ui.layer_clear(ui.layer,1);
    record("FILM_LAYER_CREATED",(int)(uintptr_t)ui.layer,0);
    /* Z-order: backdrop -> Swiss frame/crosshair -> panels -> accents/chips -> footer -> labels -> toast */
    ui.background=make(0);
    if(!ui.background)return -5;
    ui_theme.card_border=make(0);
    ui_theme.card_panel=make(0);
    ui_theme.header_accent=make(0);
    ui_theme.header_divider=make(0);
    ui_theme.footer_divider=make(0);
    ui_theme.status_dot=make(0);
    ui_theme.grid_h=make(0);
    ui_theme.grid_vc=make(0);
    if(!ui_theme.card_border || !ui_theme.card_panel ||
       !ui_theme.header_accent || !ui_theme.header_divider ||
       !ui_theme.footer_divider || !ui_theme.status_dot ||
       !ui_theme.grid_h || !ui_theme.grid_vc)return -5;
    for(unsigned i=0;i<6u;i++){
        ui.panels[i]=make(0);
        if(!ui.panels[i])return -5;
    }
#ifndef A6400_SOUND_LITE
    for(unsigned i=0;i<6u;i++){
        ui_theme.row_accents[i]=make(0);
        if(!ui_theme.row_accents[i])return -5;
    }
    for(unsigned i=0;i<6u;i++){
        for(unsigned k=0;k<3u;k++){
            ui_theme.chips[i][k]=make(0);
            if(!ui_theme.chips[i][k])return -5;
        }
    }
#endif
    ui_theme.page_btn=make(0);
    ui_theme.page_btn_accent=make(0);
    if(!ui_theme.page_btn || !ui_theme.page_btn_accent)return -5;
    for(unsigned i=0;i<8u;i++){ui.labels[i]=make(1);if(!ui.labels[i])return -5;}
    ui_toast.bg=make(0);
    ui_toast.accent=make(0);
    ui_toast.label=make(1);
    if(ui_toast.bg && ui_toast.accent && ui_toast.label) {
        hide_toast_widgets();
    } else {
        ui_toast.bg=ui_toast.accent=ui_toast.label=0;
    }
    ui_v4.popup_border=make(0);
    v4_hide_native_img();
#ifdef A6400_SOUND_LITE
    if(sl_build_widgets(make)!=0)return -5;
#endif
    hide_menu_widgets();
#ifdef A6400_CHARO_SHUTTER
    ui_theme.shutter_sound_enabled=1;
    ui_theme.last_shutter_key=0;
    ui_theme.shutter_pending=0;
    ui_theme.shutter_arm_ms=0;
    ui_theme.last_shutter_beep_ms=0;
#endif
    strcpy(ui.texts[0],UI_S_HDR_BOOT);
    for(unsigned i=0;i<A6400_SESSION_SHOTS_CAP;i++)a6400_ui_v4_v1.shot_slots[i]=-1;
#ifdef A6400_DISABLE_MTF2
    a6400_ui_v4_v1.native_touch_ok=0u;
    a6400_ui_v4_v1.native_touch_en=0u;
#else
    a6400_ui_v4_v1.native_touch_ok=(uint32_t)mtf2_probe_and_init(&ui_v4.mtf2);
#endif
    a6400_ui_v4_v1.album_scan_req+=1u;
    record("V4_MTF2_INIT",(int)a6400_ui_v4_v1.native_touch_ok,(int)(ui_v4.cache_img_ctor!=0));
#ifdef A6400_UI_SETTINGS_DIAGNOSTIC
    strcpy(ui.texts[7],UI_S_HINT_COMBO_RAM);
#elif defined(A6400_UI_COMBO_DIAGNOSTIC)
    strcpy(ui.texts[7],UI_S_HINT_COMBO);
#elif defined(A6400_UI_TOUCH_CLOSE_DIAGNOSTIC)
    strcpy(ui.texts[7],UI_S_HINT_AUTO20);
#elif defined(A6400_UI_RENDER_DIAGNOSTIC) && A6400_UI_DISPLAY_MS % 1000 == 0
    snprintf(ui.texts[7],sizeof(ui.texts[7]),"%u second display test",A6400_UI_DISPLAY_MS/1000u);
#elif defined(A6400_UI_RENDER_DIAGNOSTIC)
    snprintf(ui.texts[7],sizeof(ui.texts[7]),"%u ms display test",(unsigned)A6400_UI_DISPLAY_MS);
#else
    strcpy(ui.texts[7],UI_S_HINT_COMBO);
#endif
    record("TITLE_TEXT_BEFORE",0,0);
    place(ui.labels[0],1,76,48,488,34);ui.set_text(ui.labels[0],ui.texts[0]);
    place(ui.labels[7],1,76,394,312,28);ui.set_text(ui.labels[7],ui.texts[7]);
    record("FILM_WIDGETS_CREATED",7,8);
    input_snapshot(1);
    ui.ticker=calloc(1,0x200u);
    if(!ui.ticker)return -5;
    ui.widget_ctor(ui.ticker);
    uintptr_t *original=*(uintptr_t **)ui.ticker;
    uintptr_t expected_animate=(uintptr_t)dlsym(RTLD_DEFAULT,"_ZN2ux6wgtsys10WidgetBase9onAnimateEv");
    if(!original || !expected_animate || original[32]!=expected_animate)return -6;
    memcpy(ui.vtable,original-2,sizeof(ui.vtable));
    ui.vtable[34]=(uintptr_t)animate;
    *(uintptr_t **)ui.ticker=ui.vtable+2; /* Clone only our new Widget's table. */
#if defined(A6400_UI_COMBO_DIAGNOSTIC)
    ui.ticker_layer_id=ui.make_id("A6400_FILM_COMBO_TICKER_V3" A6400_UI_INSTANCE_SUFFIX);
#elif defined(A6400_UI_TOUCH_CLOSE_DIAGNOSTIC)
    ui.ticker_layer_id=ui.make_id("A6400_FILM_CLOSE_TOUCH_TICKER_V3");
#elif defined(A6400_INPUT_DIAGNOSTIC) && A6400_INPUT_DIAGNOSTIC == 2
    ui.ticker_layer_id=ui.make_id("A6400_FILM_INPUT2_TICKER_V3");
#elif defined(A6400_INPUT_DIAGNOSTIC)
    ui.ticker_layer_id=ui.make_id("A6400_FILM_INPUT1_TICKER_V3");
#elif defined(A6400_UI_RENDER_DIAGNOSTIC)
    ui.ticker_layer_id=ui.make_id("A6400_FILM_NATIVE_FONTFIX3_TICKER_V3");
#else
    ui.ticker_layer_id=ui.make_id("A6400_FILM_NATIVE_TICKER_V3");
#endif
    ui.ticker_layer=ui.create_layer(system,&ui.ticker_layer_id);
    if(!ui.ticker_layer)return -4;
    ui.layer_clear(ui.ticker_layer,0);
    ui.set_layer(ui.ticker,&ui.ticker_layer_id);
    ui.layer_active(ui.ticker_layer,1);
    ui.attached=ui.running=1;
    ui.set_animate(ui.ticker,1);
    input_snapshot(2);
    record("FILM_ATTACHED_NATIVE",(int)ui.layer_id,(int)(uintptr_t)ui.ticker);
    return 0;
}
