#include "film_native_view.h"
#include <stdio.h>

void film_native_view_init(struct film_native_view *v,const struct film_native_view_ops *ops,
                          void *context,const struct film_settings *settings) {
    *v=(struct film_native_view){0};
    film_menu_init(&v->menu,settings);
    v->ops=*ops;v->context=context;
}
void film_native_view_hide(struct film_native_view *v) {
    if(film_menu_close(&v->menu))v->ops.visibility(v->context,0);
    v->drawn=0;
}
unsigned film_native_view_step(struct film_native_view *v,unsigned now,
                               int menu,int zoom,int other,int touch_state,int x,int y) {
    unsigned change=film_menu_input(&v->menu,now,menu,zoom,other);
    /* Sony's enum is 5=pressed, 6=released, NOT a boolean. Invalid state
     * cancels the gesture by hiding, rather than fabricating a release. */
    if(touch_state!=5 && touch_state!=6) {
        change|=film_menu_close(&v->menu);
    } else change|=film_menu_touch(&v->menu,now,touch_state==5,x,y);
    if(change & FILM_MENU_SETTINGS_CHANGED)v->ops.settings(v->context,&v->menu.settings);
    if(!v->menu.visible) {
        if(change & FILM_MENU_CLOSED)v->ops.visibility(v->context,0);
        v->drawn=0;
        return change;
    }
    if(!v->drawn || (change & FILM_MENU_SETTINGS_CHANGED) ||
       v->drawn_page!=v->menu.page || v->drawn_selected!=v->menu.selected ||
       v->drawn_enabled!=v->menu.settings.enabled || v->drawn_stock!=v->menu.settings.stock) {
        v->ops.draw(v->context,&v->menu);
        v->drawn=1;v->drawn_page=v->menu.page;v->drawn_selected=v->menu.selected;
        v->drawn_enabled=v->menu.settings.enabled;v->drawn_stock=v->menu.settings.stock;
    }
    if(change & FILM_MENU_OPENED)v->ops.visibility(v->context,1);
    return change;
}
#ifdef A6400_UI_LANG_SC
#define FV_ON           "开启"
#define FV_OFF          "关闭"
#define FV_CLOSE        "关闭"
#define FV_BACK         "返回"
#define FV_BACK_LUT     "< LUT 设置"
#define FV_NEXT_PAGE    "下一页 >"
#define FV_PREV_PAGE    "< 上一页"
#define FV_LUT_ON_TILE  "LUT: %s >"
#define FV_LUT_OFF_TILE "LUT 设置 (关) >"
#define FV_ALBUM_TILE   "套色相册 (0张) >"
#define FV_DETAIL_TILE  "细节设置 >"
#define FV_SOUND_TILE   "快门: 日文 Charo"
#define FV_QUAL_TILE    "画质: 1.7MP 极速"
#define FV_LUT_SW_FMT   "胶片滤镜开关: %s"
#define FV_STYLE_FMT    "胶片风格: %s >"
#define FV_GRAIN_FMT    "颗粒强度: %s"
#define FV_HALO_FMT     "底片光晕: %s"
#define FV_HALO_OFF     "关闭 (加速)"
#define FV_MORE_DET     "细节设置 >"
#define FV_RESET_DET    "恢复胶片默认 (标准/开)"
#define FV_V4_POPUP     "即时预览小窗: 开启"
#define FV_V4_TOUCH     "原生MENU触控: 开启"
#define FV_V4_DEXP      "双重曝光: 关闭"
#define FV_GRAIN_DET    {"无 (加速)","轻微","标准","浓厚"}
#define FV_GRAIN_STD    "标准"
#else
#define FV_ON           "開啟"
#define FV_OFF          "關閉"
#define FV_CLOSE        "關閉"
#define FV_BACK         "返回"
#define FV_BACK_LUT     "< LUT 設定"
#define FV_NEXT_PAGE    "下一頁 >"
#define FV_PREV_PAGE    "< 上一頁"
#define FV_LUT_ON_TILE  "LUT: %s >"
#define FV_LUT_OFF_TILE "LUT 設定 (關) >"
#define FV_ALBUM_TILE   "套色相冊 (0張) >"
#define FV_DETAIL_TILE  "細節設定 >"
#define FV_SOUND_TILE   "快門: 日文 Charo"
#define FV_QUAL_TILE    "畫質: 1.7MP 極速"
#define FV_LUT_SW_FMT   "膠片濾鏡開關: %s"
#define FV_STYLE_FMT    "膠片風格: %s >"
#define FV_GRAIN_FMT    "顆粒強度: %s"
#define FV_HALO_FMT     "底片光暈: %s"
#define FV_HALO_OFF     "關閉 (加速)"
#define FV_MORE_DET     "細節設定 >"
#define FV_RESET_DET    "恢復膠片預設 (標準/開)"
#define FV_V4_POPUP     "即時預覽小窗: 開啟"
#define FV_V4_TOUCH     "原生MENU觸控: 開啟"
#define FV_V4_DEXP      "雙重曝光: 關閉"
#define FV_GRAIN_DET    {"無 (加速)","輕微","標準","濃厚"}
#define FV_GRAIN_STD    "標準"
#endif

static const char *grain_detail(int grain) {
    static const char *const names[4]=FV_GRAIN_DET;
    return (grain>=0 && grain<4)?names[grain]:FV_GRAIN_STD;
}
int film_native_view_row(const struct film_menu *m,unsigned row,char *out,unsigned capacity,
                         int *x,int *y,int *width,int *height) {
    if(!m || row>=7 || !out || !capacity)return -1;
    if(!x||!y||!width||!height)return -1;
    int n=0;
    if(m->page==FILM_MENU_STOCKS || m->page==FILM_MENU_STOCKS_P2) {
        if(row<5) {
            int stock=(m->page==FILM_MENU_STOCKS_P2?5:0)+(int)row;
            *x=56;*width=528;*y=90+(int)row*56;*height=48;
            n=snprintf(out,capacity,"%s %s",stock==m->settings.stock?"*":" ",
                       film_stock_display_name(stock));
        } else if(row==5) {
            *x=444;*width=144;*y=398;*height=42;
            n=snprintf(out,capacity,"%s",FV_BACK);
        } else {
            *x=56;*width=180;*y=398;*height=42;
            n=snprintf(out,capacity,"%s",m->page==FILM_MENU_STOCKS?FV_NEXT_PAGE:FV_PREV_PAGE);
        }
    } else if(m->page==FILM_MENU_DETAILS) {
        /* LUT Settings subpage: 0=ON/OFF, 1=Stock, 2=Grain, 3=Halation, 5=Back, 6=Details/V4 */
        if(row==4){out[0]=0;return 0;}
        if(row<4) {
            *x=56;*width=528;*y=90+(int)row*72;*height=60;
            if(row==0)n=snprintf(out,capacity,FV_LUT_SW_FMT,m->settings.enabled?FV_ON:FV_OFF);
            else if(row==1)n=snprintf(out,capacity,FV_STYLE_FMT,film_stock_display_name(m->settings.stock));
            else if(row==2)n=snprintf(out,capacity,FV_GRAIN_FMT,grain_detail(m->settings.grain));
            else n=snprintf(out,capacity,FV_HALO_FMT,m->settings.halation?FV_ON:FV_HALO_OFF);
        } else if(row==5) {
            *x=444;*width=144;*y=398;*height=42;
            n=snprintf(out,capacity,"%s",FV_BACK);
        } else {
            *x=56;*width=220;*y=398;*height=42;
            n=snprintf(out,capacity,"%s",FV_MORE_DET);
        }
    } else if(m->page==FILM_MENU_V4) {
        /* Advanced Details subpage: 0=Popup, 1=Touch, 2=Reset, 3=Double Exposure, 5=Home, 6=Back LUT */
        if(row==4){out[0]=0;return 0;}
        if(row<4) {
            *x=56;*width=528;*y=90+(int)row*72;*height=60;
            if(row==0)n=snprintf(out,capacity,"%s",FV_V4_POPUP);
            else if(row==1)n=snprintf(out,capacity,"%s",FV_V4_TOUCH);
            else if(row==2)n=snprintf(out,capacity,"%s",FV_RESET_DET);
            else n=snprintf(out,capacity,"%s",FV_V4_DEXP);
        } else if(row==5) {
            *x=444;*width=144;*y=398;*height=42;
            n=snprintf(out,capacity,"%s",FV_BACK);
        } else {
            *x=56;*width=180;*y=398;*height=42;
            n=snprintf(out,capacity,"%s",FV_BACK_LUT);
        }
    } else {
        /* FILM_MENU_HOME: Hasselblad 2x2 Tian-grid:
         * 0=TL LUT Settings, 1=TR Graded Album, 2=BL Shutter Sound, 3=BR Quality, 5=Close */
        if(row==4 || row==6){out[0]=0;return 0;}
        if(row<4) {
            int col=(int)(row & 1u);
            int r=(int)(row >> 1u);
            *x=col?330:52;*width=258;*y=r?242:86;*height=136;
            if(row==0) {
                if(m->settings.enabled)n=snprintf(out,capacity,FV_LUT_ON_TILE,film_stock_display_name(m->settings.stock));
                else n=snprintf(out,capacity,"%s",FV_LUT_OFF_TILE);
            } else if(row==1) {
                n=snprintf(out,capacity,"%s",FV_ALBUM_TILE);
            } else if(row==2) {
#ifdef A6400_CHARO_SHUTTER
                n=snprintf(out,capacity,"%s",FV_SOUND_TILE);
#else
                n=snprintf(out,capacity,"%s",FV_DETAIL_TILE);
#endif
            } else {
                n=snprintf(out,capacity,"%s",FV_QUAL_TILE);
            }
        } else {
            *x=444;*width=144;*y=398;*height=42;
            n=snprintf(out,capacity,"%s",FV_CLOSE);
        }
    }
    return n>=0 && (unsigned)n<capacity?1:-1;
}
