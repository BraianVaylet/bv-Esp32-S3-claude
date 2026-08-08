#include <Arduino.h>
#include <ctype.h>
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

// "API VALUE", not "COST": on a subscription these dollars are never billed.
// See the note rendered on that tile.
static const char *TILE_TITLES[TILE_COUNT] = {"PLAN", "API VALUE", "TOKENS", "SYSTEM"};

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

// PLAN — one row per limit window, laid out like the Claude app's usage panel:
// name on the left, percentage on the right, a full-width bar underneath, and
// the reset countdown below that.
struct LimitRow {
    lv_obj_t *name  = nullptr;
    lv_obj_t *pct   = nullptr;
    lv_obj_t *fill  = nullptr;
    lv_obj_t *reset = nullptr;
    int       y     = 0;
};
static lv_obj_t *lbl_plan_head = nullptr;
static LimitRow  row_5h;
static LimitRow  row_week;

// COST
static constexpr int CHART_H = 78;
static lv_obj_t *lbl_today   = nullptr;
static lv_obj_t *lbl_month   = nullptr;
static lv_obj_t *lbl_session = nullptr;
static lv_obj_t *lbl_cost_note = nullptr;
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

    lv_obj_t *logo = claude_code_logo_create(scr_splash, 9, CC_CLAUDE);  // 144 x 81
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

    lv_obj_t *logo = claude_code_logo_create(hdr, 2, CC_CLAUDE);  // 32 x 18
    if (logo) lv_obj_align(logo, LV_ALIGN_LEFT_MID, PAD, 0);

    hdr_title = mk_caption(hdr, TILE_TITLES[0]);
    lv_obj_set_style_text_letter_space(hdr_title, 2, 0);
    lv_obj_set_style_text_color(hdr_title, cc(CC_DIM), 0);
    lv_obj_set_style_text_font(hdr_title, F_SM, 0);
    lv_obj_align(hdr_title, LV_ALIGN_LEFT_MID, PAD + 40, 0);

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
// logo 4..49 | heading 56..69 | 5h row 78..121 | weekly row 130..173

static void build_limit_row(lv_obj_t *t, LimitRow &r, const char *name, int y)
{
    r.y    = y;
    r.name = mk_label(t, name, F_SM, CC_TEXT);
    lv_obj_align(r.name, LV_ALIGN_TOP_LEFT, 0, y);

    r.pct = mk_label(t, "--", F_SM, CC_TEXT);
    lv_obj_align(r.pct, LV_ALIGN_TOP_RIGHT, 0, y);

    r.fill = mk_bar(t, CONTENT_W, 8);
    lv_obj_align(lv_obj_get_parent(r.fill), LV_ALIGN_TOP_LEFT, 0, y + 18);

    r.reset = mk_caption(t, "");
    lv_obj_align(r.reset, LV_ALIGN_TOP_LEFT, 0, y + 30);
}

static void build_tile_plan(lv_obj_t *t)
{
    lv_obj_t *logo = claude_code_logo_create(t, 5, CC_CLAUDE);  // 80 x 45
    if (logo) lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 4);

    lbl_plan_head = mk_caption(t, "PLAN LIMITS");
    lv_obj_align(lbl_plan_head, LV_ALIGN_TOP_MID, 0, 56);

    build_limit_row(t, row_5h,   "5-hour limit", 78);
    build_limit_row(t, row_week, "Weekly, all models", 130);
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
    lv_obj_align(lbl_session, LV_ALIGN_TOP_LEFT, 0, 50);

    // These are API list prices applied to local transcript token counts. On a
    // subscription nobody is charged them, so the screen says so rather than
    // letting the reader assume it is a bill.
    lbl_cost_note = mk_caption(t, "");
    lv_obj_align(lbl_cost_note, LV_ALIGN_TOP_LEFT, 0, 67);

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
                                 "\nsettings: open this IP in a browser"
                                 "\nhold PWR 1s to power off", F_XS, CC_MUTED);
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

static void update_limit_row(LimitRow &r, const Window &w, const char *fallback,
                             uint32_t fetchedMs, uint32_t now, const char *suffix)
{
    char buf[48], d[16];

    if (w.valid) {
        snprintf(buf, sizeof(buf), "%d%%", (int)(w.pct + 0.5f));
        lv_label_set_text(r.pct, buf);
        lv_obj_set_style_text_color(r.pct, cc(CC_TEXT), 0);
        set_bar(r.fill, w.pct, CONTENT_W);

        fmt_dur(d, sizeof(d), window_remaining(w, fetchedMs, now));
        snprintf(buf, sizeof(buf), "resets in %s%s", d, suffix ? suffix : "");
        lv_label_set_text(r.reset, buf);
    } else {
        lv_label_set_text(r.pct, "--");
        lv_obj_set_style_text_color(r.pct, cc(CC_MUTED), 0);
        set_bar(r.fill, 0, CONTENT_W);
        lv_label_set_text(r.reset, fallback ? fallback : "");
    }
    lv_obj_align(r.pct, LV_ALIGN_TOP_RIGHT, 0, r.y);
}

static void update_plan(const UsageSnapshot &s, uint32_t now)
{
    char head[40];
    if (s.plan[0]) {
        char plan[20];
        strncpy(plan, s.plan, sizeof(plan) - 1);
        plan[sizeof(plan) - 1] = 0;
        for (char *p = plan; *p; p++) *p = toupper((unsigned char)*p);
        snprintf(head, sizeof(head), "%s PLAN LIMITS", plan);
    } else {
        snprintf(head, sizeof(head), "PLAN LIMITS");
    }
    lv_label_set_text(lbl_plan_head, head);
    lv_obj_align(lbl_plan_head, LV_ALIGN_TOP_MID, 0, 56);

    const char *why = s.error[0] ? s.error : "no limit data";
    update_limit_row(row_5h, s.session, why, s.fetchedMs, now, nullptr);

    // Max plans report a separate Opus weekly window; Pro does not send one, so
    // it rides along on the weekly row instead of claiming a row it rarely has.
    char opus[20] = {0};
    if (s.weekOpus.valid && s.weekOpus.pct >= 0.5f)
        snprintf(opus, sizeof(opus), "   opus %d%%", (int)(s.weekOpus.pct + 0.5f));
    update_limit_row(row_week, s.week, "", s.fetchedMs, now, opus);
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

        if (s.plan[0]) {
            char plan[20];
            strncpy(plan, s.plan, sizeof(plan) - 1);
            plan[sizeof(plan) - 1] = 0;
            for (char *p = plan; *p; p++) *p = toupper((unsigned char)*p);
            snprintf(buf, sizeof(buf), "API LIST PRICES - NOT BILLED ON %s", plan);
        } else {
            snprintf(buf, sizeof(buf), "API LIST PRICES, NOT AN INVOICE");
        }
        lv_label_set_text(lbl_cost_note, buf);
    } else {
        lv_label_set_text(lbl_session, "");
        lv_label_set_text(lbl_cost_note, "");
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
