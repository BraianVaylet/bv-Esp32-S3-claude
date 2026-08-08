#include <Arduino.h>
#include <lvgl.h>

#include "app_config.h"
#include "board.h"
#include "claude_logo.h"
#include "net.h"
#include "theme.h"
#include "ui.h"

// ---------------------------------------------------------------------------
// Layout. The panel is 240x240. Header and footer are fixed chrome; each tile
// gets a 224x182 content box (240 - 2*PAD wide, TILE_H - 2*PAD tall) and every
// vertical position below is chosen to land inside it exactly.
// ---------------------------------------------------------------------------
static constexpr int W          = 240;
static constexpr int HEADER_H   = 26;
static constexpr int FOOTER_H   = 16;
static constexpr int TILE_H     = 240 - HEADER_H - FOOTER_H;  // 198
static constexpr int PAD        = 8;
static constexpr int CONTENT_W  = W - PAD * 2;                // 224
static constexpr int CONTENT_H  = TILE_H - PAD * 2;           // 182
static constexpr int TILE_COUNT = 4;

static const char *TILE_TITLES[TILE_COUNT] = {"PLAN", "COST", "TOKENS", "SYSTEM"};

// ------------------------------------------------------------------ state --
static lv_obj_t *scr_splash  = nullptr;
static lv_obj_t *scr_main    = nullptr;
static lv_obj_t *splash_msg  = nullptr;
static bool      splash_done = false;

static lv_obj_t *hdr_title = nullptr;
static lv_obj_t *hdr_dot   = nullptr;
static lv_obj_t *tileview  = nullptr;
static lv_obj_t *dots[TILE_COUNT] = {nullptr};
static lv_obj_t *toast       = nullptr;
static uint32_t  toast_until = 0;

// PLAN
static lv_obj_t *arc_session  = nullptr;
static lv_obj_t *lbl_sess_pct = nullptr;
static lv_obj_t *lbl_sess_sym = nullptr;
static lv_obj_t *lbl_sess_rst = nullptr;
static lv_obj_t *bar_week     = nullptr;
static lv_obj_t *lbl_week_pct = nullptr;
static lv_obj_t *lbl_week_rst = nullptr;
static lv_obj_t *lbl_plan     = nullptr;

// COST
static constexpr int CHART_H = 78;
static lv_obj_t *lbl_today   = nullptr;
static lv_obj_t *lbl_month   = nullptr;
static lv_obj_t *lbl_session = nullptr;
static lv_obj_t *day_fill[MAX_DAYS]  = {nullptr};
static lv_obj_t *day_label[MAX_DAYS] = {nullptr};

// TOKENS
static lv_obj_t *tok_val[4] = {nullptr};
static lv_obj_t *mdl_row[MAX_MODELS]  = {nullptr};
static lv_obj_t *mdl_fill[MAX_MODELS] = {nullptr};
static lv_obj_t *mdl_name[MAX_MODELS] = {nullptr};
static lv_obj_t *mdl_val[MAX_MODELS]  = {nullptr};

// SYSTEM
static lv_obj_t *sys_val[5] = {nullptr};

// ------------------------------------------------------------- formatting --

static void fmt_money(char *out, size_t n, float usd)
{
    if (usd >= 1000.0f)     snprintf(out, n, "$%.1fk", usd / 1000.0f);
    else if (usd >= 100.0f) snprintf(out, n, "$%.0f", usd);
    else                    snprintf(out, n, "$%.2f", usd);
}

static void fmt_tokens(char *out, size_t n, uint64_t t)
{
    // Everything above 1k goes through %f, so the integer branch never needs
    // 64-bit printf support (which newlib-nano would not give us).
    if (t >= 1000000000ULL)   snprintf(out, n, "%.1fB", t / 1e9);
    else if (t >= 1000000ULL) snprintf(out, n, "%.1fM", t / 1e6);
    else if (t >= 1000ULL)    snprintf(out, n, "%.1fk", t / 1e3);
    else                      snprintf(out, n, "%lu", (unsigned long)t);
}

static void fmt_dur(char *out, size_t n, uint32_t sec)
{
    if (!sec)             snprintf(out, n, "now");
    else if (sec < 3600)  snprintf(out, n, "%lum", (unsigned long)(sec / 60));
    else if (sec < 86400) snprintf(out, n, "%luh %lum", (unsigned long)(sec / 3600),
                                                        (unsigned long)((sec % 3600) / 60));
    else                  snprintf(out, n, "%lud %luh", (unsigned long)(sec / 86400),
                                                        (unsigned long)((sec % 86400) / 3600));
}

// ------------------------------------------------------------- primitives --

static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                          uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, cc(color), 0);
    return l;
}

static lv_obj_t *mk_caption(lv_obj_t *parent, const char *txt)
{
    lv_obj_t *l = mk_label(parent, txt, F_XS, CC_MUTED);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    return l;
}

// A bare rectangle with no LVGL theme baggage.
static lv_obj_t *mk_box(lv_obj_t *parent, int w, int h, uint32_t bg, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, cc(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *mk_panel(lv_obj_t *parent, int w, int h, int pad)
{
    lv_obj_t *p = mk_box(parent, w, h, CC_SURFACE, 8);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, cc(CC_BORDER), 0);
    lv_obj_set_style_pad_all(p, pad, 0);
    return p;
}

// Track + fill pair. Returns the fill; its parent is the track, which is what
// the caller positions.
static lv_obj_t *mk_bar(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *track = mk_box(parent, w, h, CC_SURFACE_HI, h / 2);
    lv_obj_t *fill  = mk_box(track, 0, h, CC_CLAUDE, h / 2);
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
    return fill;
}

static void set_bar(lv_obj_t *fill, float pct, int trackW)
{
    if (!fill) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int w = (int)(trackW * pct / 100.0f + 0.5f);
    if (pct > 0 && w < 3) w = 3;
    lv_obj_set_width(fill, w);
    lv_obj_set_style_bg_color(fill, cc(cc_level_color(pct)), 0);
}

// ---------------------------------------------------------------- splash ---

static void build_splash()
{
    scr_splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_splash, cc(CC_BG), 0);
    lv_obj_remove_flag(scr_splash, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *logo = claude_logo_create(scr_splash, 104, CC_CLAUDE);
    if (logo) lv_obj_align(logo, LV_ALIGN_CENTER, 0, -26);

    lv_obj_t *t = mk_label(scr_splash, APP_NAME, F_XL, CC_TEXT);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 48);

    splash_msg = mk_label(scr_splash, "starting", F_SM, CC_MUTED);
    lv_obj_align(splash_msg, LV_ALIGN_CENTER, 0, 74);
}

// ---------------------------------------------------------------- chrome ---

static int active_tile_index()
{
    lv_obj_t *active = lv_tileview_get_tile_active(tileview);
    for (int i = 0; i < TILE_COUNT; i++)
        if (lv_obj_get_child(tileview, i) == active) return i;
    return 0;
}

static void sync_chrome()
{
    const int idx = active_tile_index();
    lv_label_set_text(hdr_title, TILE_TITLES[idx]);
    for (int i = 0; i < TILE_COUNT; i++)
        lv_obj_set_style_bg_color(dots[i], cc(i == idx ? CC_CLAUDE : CC_BORDER), 0);
}

static void on_tile_changed(lv_event_t *) { sync_chrome(); }

static void build_header()
{
    lv_obj_t *hdr = mk_box(scr_main, W, HEADER_H, CC_BG, 0);
    lv_obj_set_pos(hdr, 0, 0);

    lv_obj_t *logo = claude_logo_create(hdr, 16, CC_CLAUDE, 8);
    if (logo) lv_obj_align(logo, LV_ALIGN_LEFT_MID, PAD, 0);

    hdr_title = mk_caption(hdr, TILE_TITLES[0]);
    lv_obj_set_style_text_letter_space(hdr_title, 2, 0);
    lv_obj_set_style_text_color(hdr_title, cc(CC_DIM), 0);
    lv_obj_set_style_text_font(hdr_title, F_SM, 0);
    lv_obj_align(hdr_title, LV_ALIGN_LEFT_MID, PAD + 24, 0);

    hdr_dot = mk_box(hdr, 8, 8, CC_MUTED, 4);
    lv_obj_align(hdr_dot, LV_ALIGN_RIGHT_MID, -PAD, 0);
}

static void build_footer()
{
    lv_obj_t *bar = mk_box(scr_main, W, FOOTER_H, CC_BG, 0);
    lv_obj_set_pos(bar, 0, 240 - FOOTER_H);

    const int spacing = 14;
    for (int i = 0; i < TILE_COUNT; i++) {
        dots[i] = mk_box(bar, 6, 6, i == 0 ? CC_CLAUDE : CC_BORDER, 3);
        lv_obj_align(dots[i], LV_ALIGN_CENTER,
                     (int)((i - (TILE_COUNT - 1) * 0.5f) * spacing), 0);
    }
}

// ------------------------------------------------------------ tile: PLAN ---
// arc 0..112 | reset line 114..129 | weekly panel 130..182

static constexpr int WEEK_BAR_W = CONTENT_W - 12 - 2;  // panel width - 2*pad - border

static void build_tile_plan(lv_obj_t *t)
{
    arc_session = lv_arc_create(t);
    lv_obj_set_size(arc_session, 112, 112);
    lv_obj_align(arc_session, LV_ALIGN_TOP_MID, 0, 0);
    lv_arc_set_rotation(arc_session, 135);
    lv_arc_set_bg_angles(arc_session, 0, 270);
    lv_arc_set_range(arc_session, 0, 100);
    lv_arc_set_value(arc_session, 0);
    lv_obj_remove_style(arc_session, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc_session, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc_session, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_session, cc(CC_SURFACE_HI), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_session, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_session, cc(CC_CLAUDE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_session, true, LV_PART_INDICATOR);

    // Big number, small percent sign — the number alone always fits the ring.
    lbl_sess_pct = mk_label(t, "--", F_HERO, CC_TEXT);
    lv_obj_align_to(lbl_sess_pct, arc_session, LV_ALIGN_CENTER, -7, -6);

    lbl_sess_sym = mk_label(t, "%", F_LG, CC_DIM);
    lv_obj_align_to(lbl_sess_sym, lbl_sess_pct, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -5);

    lv_obj_t *cap = mk_caption(t, "5H WINDOW");
    lv_obj_align_to(cap, arc_session, LV_ALIGN_CENTER, 0, 22);

    lbl_sess_rst = mk_label(t, "", F_SM, CC_DIM);
    lv_obj_align(lbl_sess_rst, LV_ALIGN_TOP_MID, 0, 114);

    lv_obj_t *p = mk_panel(t, CONTENT_W, 52, 6);
    lv_obj_align(p, LV_ALIGN_TOP_MID, 0, 130);

    lv_obj_t *wl = mk_caption(p, "WEEKLY");
    lv_obj_align(wl, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_week_pct = mk_label(p, "--", F_MD, CC_TEXT);
    lv_obj_align(lbl_week_pct, LV_ALIGN_TOP_RIGHT, 0, -3);

    bar_week = mk_bar(p, WEEK_BAR_W, 6);
    lv_obj_align(lv_obj_get_parent(bar_week), LV_ALIGN_TOP_LEFT, 0, 17);

    lbl_week_rst = mk_caption(p, "");
    lv_obj_align(lbl_week_rst, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lbl_plan = mk_label(p, "", F_XS, CC_KRAFT);
    lv_obj_align(lbl_plan, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

// ------------------------------------------------------------ tile: COST ---
// today/month 2..48 | session line 52..67 | chart 84..178

static void build_tile_cost(lv_obj_t *t)
{
    lv_obj_t *c1 = mk_caption(t, "TODAY");
    lv_obj_align(c1, LV_ALIGN_TOP_LEFT, 0, 2);

    lbl_today = mk_label(t, "--", F_2XL, CC_CLAUDE);
    lv_obj_align(lbl_today, LV_ALIGN_TOP_LEFT, 0, 14);

    lv_obj_t *c2 = mk_caption(t, "MONTH");
    lv_obj_align(c2, LV_ALIGN_TOP_RIGHT, 0, 2);

    lbl_month = mk_label(t, "--", F_XL, CC_SUBTLE);
    lv_obj_align(lbl_month, LV_ALIGN_TOP_RIGHT, 0, 18);

    lbl_session = mk_label(t, "", F_SM, CC_DIM);
    lv_obj_align(lbl_session, LV_ALIGN_TOP_LEFT, 0, 52);

    lv_obj_t *chart = mk_box(t, CONTENT_W, CHART_H + 16, CC_BG, 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_align(chart, LV_ALIGN_TOP_LEFT, 0, 84);

    const int gap = 6;
    const int bw  = (CONTENT_W - gap * (MAX_DAYS - 1)) / MAX_DAYS;
    for (int i = 0; i < MAX_DAYS; i++) {
        const int x = i * (bw + gap);

        lv_obj_t *track = mk_box(chart, bw, CHART_H, CC_SURFACE, 3);
        lv_obj_set_pos(track, x, 0);

        day_fill[i] = mk_box(track, bw, 2, CC_CLAUDE, 3);
        lv_obj_align(day_fill[i], LV_ALIGN_BOTTOM_MID, 0, 0);

        day_label[i] = mk_caption(chart, "");
        lv_obj_set_width(day_label[i], bw);
        lv_obj_set_style_text_align(day_label[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(day_label[i], x, CHART_H + 3);
    }
}

// ---------------------------------------------------------- tile: TOKENS ---
// 2x2 token cards 0..94 | "BY MODEL" 98..111 | 4 rows 114..181

static constexpr int MDL_ROW_H = 16;

static void build_tile_tokens(lv_obj_t *t)
{
    static const char *CAPS[4] = {"INPUT", "OUTPUT", "CACHE R", "CACHE W"};
    const int cw = (CONTENT_W - 6) / 2;
    const int ch = 44;

    for (int i = 0; i < 4; i++) {
        lv_obj_t *p = mk_panel(t, cw, ch, 5);
        lv_obj_set_pos(p, (i % 2) * (cw + 6), (i / 2) * (ch + 6));

        lv_obj_t *c = mk_caption(p, CAPS[i]);
        lv_obj_align(c, LV_ALIGN_TOP_LEFT, 0, 0);

        tok_val[i] = mk_label(p, "--", F_MD, CC_TEXT);
        lv_obj_align(tok_val[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    lv_obj_t *hdr = mk_caption(t, "BY MODEL");
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 98);

    for (int i = 0; i < MAX_MODELS; i++) {
        const int y = 114 + i * (MDL_ROW_H + 1);

        // The bar lives behind the text: one row, no separate gauge line.
        mdl_row[i] = mk_box(t, CONTENT_W, MDL_ROW_H, CC_SURFACE, 4);
        lv_obj_set_pos(mdl_row[i], 0, y);

        mdl_fill[i] = mk_box(mdl_row[i], 0, MDL_ROW_H, CC_CLAUDE, 4);
        lv_obj_align(mdl_fill[i], LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_opa(mdl_fill[i], LV_OPA_40, 0);

        mdl_name[i] = mk_label(mdl_row[i], "", F_SM, CC_TEXT);
        lv_obj_align(mdl_name[i], LV_ALIGN_LEFT_MID, 5, 0);

        mdl_val[i] = mk_label(mdl_row[i], "", F_SM, CC_SUBTLE);
        lv_obj_align(mdl_val[i], LV_ALIGN_RIGHT_MID, -5, 0);
    }
}

// ---------------------------------------------------------- tile: SYSTEM ---

static void build_tile_system(lv_obj_t *t)
{
    static const char *CAPS[5] = {"WI-FI", "IP", "BRIDGE", "BATTERY", "UPTIME"};

    for (int i = 0; i < 5; i++) {
        const int y = i * 26;

        lv_obj_t *c = mk_caption(t, CAPS[i]);
        lv_obj_set_pos(c, 0, y + 4);

        sys_val[i] = mk_label(t, "--", F_SM, CC_TEXT);
        lv_obj_set_width(sys_val[i], 152);
        lv_obj_set_style_text_align(sys_val[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(sys_val[i], LV_ALIGN_TOP_RIGHT, 0, y + 2);

        lv_obj_t *rule = mk_box(t, CONTENT_W, 1, CC_BORDER, 0);
        lv_obj_set_pos(rule, 0, y + 22);
    }

    lv_obj_t *hint = mk_label(t, APP_NAME "  v" APP_VERSION
                                 "\nhold PWR 1s to reset Wi-Fi", F_XS, CC_MUTED);
    lv_obj_set_width(hint, CONTENT_W);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
}

// ------------------------------------------------------------------ build --

void ui_begin()
{
    build_splash();
    lv_screen_load(scr_splash);

    scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_main, cc(CC_BG), 0);
    lv_obj_set_style_pad_all(scr_main, 0, 0);
    lv_obj_set_style_border_width(scr_main, 0, 0);
    lv_obj_remove_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);

    build_header();
    build_footer();

    tileview = lv_tileview_create(scr_main);
    lv_obj_set_size(tileview, W, TILE_H);
    lv_obj_set_pos(tileview, 0, HEADER_H);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tileview, 0, 0);
    lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(tileview, on_tile_changed, LV_EVENT_VALUE_CHANGED, NULL);

    for (int i = 0; i < TILE_COUNT; i++) {
        lv_obj_t *tile = lv_tileview_add_tile(tileview, i, 0, LV_DIR_HOR);
        lv_obj_set_style_pad_all(tile, PAD, 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        switch (i) {
            case 0: build_tile_plan(tile);   break;
            case 1: build_tile_cost(tile);   break;
            case 2: build_tile_tokens(tile); break;
            case 3: build_tile_system(tile); break;
        }
    }

    toast = mk_label(scr_main, "", F_SM, CC_TEXT);
    lv_obj_set_style_bg_color(toast, cc(CC_SURFACE_HI), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(toast, 6, 0);
    lv_obj_set_style_radius(toast, 6, 0);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -FOOTER_H - 4);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
}

// ----------------------------------------------------------------- update --

static void update_splash(const UsageSnapshot &snap)
{
    const char *msg = "starting";
    switch (net_state()) {
        case NetState::Portal:     msg = "setup: join " AP_SSID;   break;
        case NetState::Connecting: msg = "connecting to Wi-Fi";    break;
        case NetState::Failed:     msg = "Wi-Fi failed, retrying"; break;
        case NetState::Online:     msg = snap.error[0] ? snap.error : "reading bridge"; break;
        default: break;
    }
    lv_label_set_text(splash_msg, msg);
    lv_obj_align(splash_msg, LV_ALIGN_CENTER, 0, 74);

    // Leave as soon as real data lands, and never sit here past 12 s so the
    // SYSTEM tile stays reachable when the bridge is down.
    if (!splash_done && (snap.fetchedMs != 0 || millis() > 12000)) {
        splash_done = true;
        lv_screen_load_anim(scr_main, LV_SCR_LOAD_ANIM_FADE_IN, 350, 0, false);
    }
}

static void update_plan(const UsageSnapshot &s, uint32_t now)
{
    char buf[48], d[16];

    if (s.session.valid) {
        const int pct = (int)(s.session.pct + 0.5f);
        lv_arc_set_value(arc_session, pct);
        lv_obj_set_style_arc_color(arc_session, cc(cc_level_color(s.session.pct)),
                                   LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "%d", pct);
        lv_label_set_text(lbl_sess_pct, buf);
        lv_obj_set_style_text_color(lbl_sess_pct, cc(CC_TEXT), 0);
        lv_obj_remove_flag(lbl_sess_sym, LV_OBJ_FLAG_HIDDEN);

        fmt_dur(d, sizeof(d), window_remaining(s.session, s.fetchedMs, now));
        snprintf(buf, sizeof(buf), "resets in %s", d);
        lv_label_set_text(lbl_sess_rst, buf);
    } else {
        lv_arc_set_value(arc_session, 0);
        lv_label_set_text(lbl_sess_pct, "--");
        lv_obj_set_style_text_color(lbl_sess_pct, cc(CC_MUTED), 0);
        lv_obj_add_flag(lbl_sess_sym, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_sess_rst, s.error[0] ? s.error : "no limit data");
    }
    lv_obj_align_to(lbl_sess_pct, arc_session, LV_ALIGN_CENTER, -7, -6);
    lv_obj_align_to(lbl_sess_sym, lbl_sess_pct, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -5);
    lv_obj_align(lbl_sess_rst, LV_ALIGN_TOP_MID, 0, 114);

    if (s.week.valid) {
        set_bar(bar_week, s.week.pct, WEEK_BAR_W);
        snprintf(buf, sizeof(buf), "%d%%", (int)(s.week.pct + 0.5f));
        lv_label_set_text(lbl_week_pct, buf);

        fmt_dur(d, sizeof(d), window_remaining(s.week, s.fetchedMs, now));
        snprintf(buf, sizeof(buf), "resets %s", d);
        lv_label_set_text(lbl_week_rst, buf);
    } else {
        set_bar(bar_week, 0, WEEK_BAR_W);
        lv_label_set_text(lbl_week_pct, "--");
        lv_label_set_text(lbl_week_rst, "");
    }

    if (s.weekOpus.valid && s.weekOpus.pct >= 0.5f) {
        snprintf(buf, sizeof(buf), "opus %d%%", (int)(s.weekOpus.pct + 0.5f));
        lv_label_set_text(lbl_plan, buf);
    } else {
        lv_label_set_text(lbl_plan, s.plan);
    }
    lv_obj_align(lbl_week_pct, LV_ALIGN_TOP_RIGHT, 0, -3);
    lv_obj_align(lbl_plan, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

static void update_cost(const UsageSnapshot &s)
{
    char buf[48], m[16];

    fmt_money(m, sizeof(m), s.todayUsd);
    lv_label_set_text(lbl_today, s.costOk ? m : "--");

    fmt_money(m, sizeof(m), s.monthUsd);
    lv_label_set_text(lbl_month, s.costOk ? m : "--");
    lv_obj_align(lbl_month, LV_ALIGN_TOP_RIGHT, 0, 18);

    if (s.costOk) {
        fmt_money(m, sizeof(m), s.sessionUsd);
        snprintf(buf, sizeof(buf), "%s this session", m);
        lv_label_set_text(lbl_session, buf);
    } else {
        lv_label_set_text(lbl_session, "");
    }

    float peak = 0.01f;
    for (int i = 0; i < s.dayCount; i++)
        if (s.days[i].usd > peak) peak = s.days[i].usd;

    for (int i = 0; i < MAX_DAYS; i++) {
        if (i < s.dayCount) {
            int h = (int)((s.days[i].usd / peak) * CHART_H + 0.5f);
            if (h < 2) h = 2;
            lv_obj_set_height(day_fill[i], h);
            const bool last = (i == s.dayCount - 1);  // most recent day = today
            lv_obj_set_style_bg_color(day_fill[i], cc(last ? CC_CLAUDE : CC_BOOKCLOTH), 0);
            lv_obj_set_style_bg_opa(day_fill[i], last ? LV_OPA_COVER : LV_OPA_60, 0);
            lv_label_set_text(day_label[i], s.days[i].label);
        } else {
            lv_obj_set_height(day_fill[i], 2);
            lv_label_set_text(day_label[i], "");
        }
    }
}

static void update_tokens(const UsageSnapshot &s)
{
    const uint64_t vals[4] = {s.tokIn, s.tokOut, s.tokCacheRead, s.tokCacheWrite};
    char buf[32], money[16];

    for (int i = 0; i < 4; i++) {
        if (s.costOk) {
            fmt_tokens(buf, sizeof(buf), vals[i]);
            lv_label_set_text(tok_val[i], buf);
        } else {
            lv_label_set_text(tok_val[i], "--");
        }
        lv_obj_align(tok_val[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    for (int i = 0; i < MAX_MODELS; i++) {
        if (i < s.modelCount) {
            lv_obj_remove_flag(mdl_row[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(mdl_name[i], s.models[i].name);
            fmt_money(money, sizeof(money), s.models[i].usd);
            snprintf(buf, sizeof(buf), "%s  %d%%", money, (int)(s.models[i].pct + 0.5f));
            lv_label_set_text(mdl_val[i], buf);
            lv_obj_align(mdl_val[i], LV_ALIGN_RIGHT_MID, -5, 0);

            int w = (int)(CONTENT_W * s.models[i].pct / 100.0f + 0.5f);
            if (w > CONTENT_W) w = CONTENT_W;
            lv_obj_set_width(mdl_fill[i], w);
            lv_obj_set_style_bg_color(mdl_fill[i], cc(CC_CLAUDE), 0);
        } else {
            lv_obj_add_flag(mdl_row[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void update_system(const UsageSnapshot &s, uint32_t now)
{
    char buf[48], d[16];

    switch (net_state()) {
        case NetState::Online:
            snprintf(buf, sizeof(buf), "%s  %d dBm", net_ssid().c_str(), net_rssi());
            break;
        case NetState::Portal:
            snprintf(buf, sizeof(buf), "setup AP");
            break;
        default:
            snprintf(buf, sizeof(buf), "offline");
            break;
    }
    lv_label_set_text(sys_val[0], buf);
    lv_label_set_text(sys_val[1], net_ip().c_str());

    if (s.ok) {
        snprintf(buf, sizeof(buf), "ok  %lus ago",
                 (unsigned long)((now - s.fetchedMs) / 1000));
        lv_obj_set_style_text_color(sys_val[2], cc(CC_TEXT), 0);
    } else {
        snprintf(buf, sizeof(buf), "%s", s.error[0] ? s.error : "no data");
        lv_obj_set_style_text_color(sys_val[2], cc(CC_CRAIL), 0);
    }
    lv_label_set_text(sys_val[2], buf);

    snprintf(buf, sizeof(buf), "%d%%  %.2fV%s", board_battery_pct(),
             board_battery_mv() / 1000.0f, board_on_usb() ? "  USB" : "");
    lv_label_set_text(sys_val[3], buf);

    fmt_dur(d, sizeof(d), now / 1000);
    lv_label_set_text(sys_val[4], d);
}

void ui_update(const UsageSnapshot &snap)
{
    const uint32_t now = millis();

    if (!splash_done) { update_splash(snap); return; }

    lv_obj_set_style_bg_color(hdr_dot,
        cc(snap.ok ? CC_CLAUDE : (snap.fetchedMs ? CC_KRAFT : CC_CRAIL)), 0);

    update_plan(snap, now);
    update_cost(snap);
    update_tokens(snap);
    update_system(snap, now);

    if (toast_until && now > toast_until) {
        toast_until = 0;
        lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_next_screen()
{
    if (!splash_done || !tileview) return;
    const int next = (active_tile_index() + 1) % TILE_COUNT;
    lv_tileview_set_tile_by_index(tileview, next, 0, LV_ANIM_ON);
    sync_chrome();
}

void ui_show_toast(const char *msg)
{
    if (!toast) return;
    lv_label_set_text(toast, msg);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -FOOTER_H - 4);
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_HIDDEN);
    toast_until = millis() + 1800;
}
