#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include "logos.h"
#include "icons.h"
#include "hal/board_caps.h"

// Both provider marks must share geometry: row_center_y() centers the top row
// against LOGO_CLAUDE_HEIGHT for the Claude *and* Codex screens.
static_assert(LOGO_CLAUDE_WIDTH == LOGO_OPENAI_WIDTH && LOGO_CLAUDE_HEIGHT == LOGO_OPENAI_HEIGHT,
              "Claude and OpenAI logos must be the same size");

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI).
// Inventory lives in fonts.h — the single source of truth for declared sizes.
#include "fonts.h"

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t logo_y;                // logo top; title + battery center against this row
    int16_t content_y;
    int16_t content_w;
    const lv_font_t* title_font;   // shared by both screen titles

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    const lv_font_t* usage_pct_font;
    const lv_font_t* usage_pill_font;
    const lv_font_t* usage_reset_font;

    // WiFi screen
    int16_t bt_info_panel_h;
    uint16_t bt_icon_scale;  // LVGL image zoom for the status icon (256 = 100%, == LV_SCALE_NONE)
    int16_t bt_status_x;     // x of status text, after the (possibly scaled) icon
    int16_t bt_status_y;     // shared y for the status icon and the status label
    int16_t bt_device_y;
    int16_t bt_mac_y;
    int16_t bt_rssi_y;
    int16_t bt_daemon_y;
    int16_t bt_age_y;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;  // also used for sub-lines
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;
    L.logo_y = L.title_y - 10;   // logo top; the title/battery row centers against this

    // Values shared by both breakpoints — kept out of the if/else so only the
    // genuinely size-dependent values differ between the two branches.
    L.content_y        = 110;    // clears the 80px logo (bottom at logo_y + LOGO_CLAUDE_HEIGHT) with padding
    L.usage_panel_gap  = 16;
    L.usage_pill_font  = &font_styrene_28;
    L.usage_reset_font = &font_styrene_28;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.title_font       = &font_tiempos_56;
        L.usage_panel_h = 150;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.usage_pct_font   = &font_styrene_48;
        // Panel extends down to just above the credit lines (≈396 bottom),
        // so the grey fills the screen like the usage page rather than
        // floating as a short box. It stops ~30px shy of where the usage
        // panels end because, unlike them, this screen has credits below.
        L.bt_info_panel_h = 286;
        L.bt_icon_scale = 256;   // status icon at native 48px (LV_SCALE_NONE — no transform)
        L.bt_status_x = 56;
        L.bt_status_y = 2;
        L.bt_device_y = 68;
        L.bt_mac_y = 106;
        L.bt_rssi_y = 144;
        L.bt_daemon_y = 182;
        L.bt_age_y = 220;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else {
        // Compact layout — tuned for 368x448 (AMOLED-1.8). The panel is only
        // ~7% shorter than the square, so heights/fonts stay close to the
        // large layout; the title shrinks only because "WiFi" at 56px would
        // overrun the narrower (368px) width. Panel holds the status row +
        // five diagnostic rows (SSID, IP, signal, daemon, age) + credits.
        L.title_font       = &font_tiempos_34;
        L.usage_panel_h = 134;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;    // ends ~2px above the panel content bottom (clearance for descenders)
        L.usage_pct_font   = &font_styrene_36;   // headline %, a step down from 48 so the row breathes
        // Extends to ≈368 bottom — just above the credit lines — so the grey
        // matches the usage page; stops short of the usage-panel bottom (394)
        // to leave room for the two credit lines this screen has below it.
        L.bt_info_panel_h = 258;
        L.bt_icon_scale = 160;   // scale status icon 48px -> ~30px to match the 28px status text
        L.bt_status_x = 44;
        L.bt_status_y = 4;
        L.bt_device_y = 56;
        L.bt_mac_y = 92;
        L.bt_rssi_y = 128;
        L.bt_daemon_y = 164;
        L.bt_age_y = 200;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;   // sub-lines; 24px overflowed the 296px inner width
        L.bt_credit_1_font = &font_styrene_20;
        L.bt_credit_2_font = &font_styrene_16;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

// Vertically center a top-row item of pixel height `item_h` against the logo,
// whose box is [logo_y, logo_y + LOGO_CLAUDE_HEIGHT]. Keeps the title and battery
// aligned with the (taller) logo instead of riding high in the row.
static int16_t row_center_y(int item_h) {
    return (int16_t)(L.logo_y + (LOGO_CLAUDE_HEIGHT - item_h) / 2);
}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG           THEME_BG
#define COL_PANEL        THEME_PANEL
#define COL_TEXT         THEME_TEXT
#define COL_DIM          THEME_DIM
#define COL_ACCENT       THEME_ACCENT
#define COL_GREEN        THEME_GREEN
#define COL_AMBER        THEME_AMBER
#define COL_RED          THEME_RED
#define COL_BAR_BG       THEME_BAR_BG
#define COL_ACCENT_CODEX THEME_ACCENT_CODEX
#define COL_STALE        THEME_STALE

// ---- Provider widget bundle — one instance per provider panel ----
struct ProviderWidgets {
    lv_obj_t* container;
    lv_obj_t* title;
    lv_obj_t* pct_session;    lv_obj_t* label_session;  lv_obj_t* bar_session;  lv_obj_t* reset_session;
    lv_obj_t* pct_weekly;     lv_obj_t* label_weekly;   lv_obj_t* bar_weekly;   lv_obj_t* reset_weekly;
    lv_obj_t* anim;
};

// ---- Usage screen widgets ----
static ProviderWidgets claude_w;
static ProviderWidgets codex_w;

// ---- WiFi screen widgets ----
static lv_obj_t* wifi_container;
static lv_obj_t* lbl_wifi_status;
static lv_obj_t* lbl_wifi_ssid;
static lv_obj_t* lbl_wifi_ip;
static lv_obj_t* lbl_wifi_rssi;
static lv_obj_t* lbl_wifi_daemon;
static lv_obj_t* lbl_wifi_age;

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Shared ----
static lv_image_dsc_t logo_claude_dsc;
static lv_image_dsc_t logo_openai_dsc;
static screen_t current_screen = SCREEN_USAGE;

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

// Codex's TUI has no whimsical catalog — its status header is just "Working"
// (default) / "Thinking" (reasoning). Mirror that on the Codex screen.
static const char* const codex_messages[] = { "Working", "Thinking" };
#define CODEX_MSG_COUNT (sizeof(codex_messages) / sizeof(codex_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 16, 0);
    lv_obj_set_style_pad_right(panel, 16, 0);
    lv_obj_set_style_pad_top(panel, 12, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

// Screen title in the top row. The +16 x-offset centers it horizontally in the
// gap between the logo (left) and the battery icon (right); the y centers it
// vertically against the logo. Shared by the usage and wifi screens.
static lv_obj_t* make_title(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, L.title_font, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 16, row_center_y(lv_font_get_line_height(L.title_font)));
    return lbl;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

// Fill an LVGL image descriptor. RGB565 is 2 bytes/px; RGB565A8 appends a
// w*h alpha plane (3 bytes/px total — see gotcha #8). stride is the RGB565
// row stride in both formats since the alpha plane is appended, not interleaved.
// Contract: only RGB565 and RGB565A8 are supported — any other cf is treated
// as 2 B/px and would produce a wrong data_size. All call sites pass one of
// these two constants.
static void init_icon_dsc(lv_image_dsc_t* dsc, int w, int h,
                          const void* data, lv_color_format_t cf) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = cf;
    dsc->header.stride = w * 2;
    dsc->data = (const uint8_t*)data;
    dsc->data_size = w * h * (cf == LV_COLOR_FORMAT_RGB565A8 ? 3 : 2);
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text, const lv_font_t* font) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, 18, 0);
    lv_obj_set_style_pad_right(lbl, 18, 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_bottom(lbl, 6, 0);
    return lbl;
}

// Order must match the index logic in ui_update_battery(): empty, low, medium,
// full, charging. All RGB565A8 so the alpha plane blends over the splash art.
static void init_battery_icons(void) {
    static const struct { uint16_t w, h; const uint8_t* data; } icons[] = {
        { ICON_BATTERY_W,          ICON_BATTERY_H,          icon_battery_data },
        { ICON_BATTERY_LOW_W,      ICON_BATTERY_LOW_H,      icon_battery_low_data },
        { ICON_BATTERY_MEDIUM_W,   ICON_BATTERY_MEDIUM_H,   icon_battery_medium_data },
        { ICON_BATTERY_FULL_W,     ICON_BATTERY_FULL_H,     icon_battery_full_data },
        { ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data },
    };
    static_assert(sizeof(icons) / sizeof(icons[0]) == sizeof(battery_dscs) / sizeof(battery_dscs[0]),
                  "battery_dscs and the icon table must stay in sync");
    for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); i++)
        init_icon_dsc(&battery_dscs[i], icons[i].w, icons[i].h,
                      icons[i].data, LV_COLOR_FORMAT_RGB565A8);
}

// ======== Usage Screen ========

static void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, L.usage_pct_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text, L.usage_pill_font);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y, L.content_w - 32, 24);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.usage_reset_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);
}

static void init_provider_screen(lv_obj_t* scr, const char* title, lv_color_t accent, ProviderWidgets* w) {
    w->container = lv_obj_create(scr);
    lv_obj_set_size(w->container, L.scr_w, L.scr_h);
    lv_obj_set_pos(w->container, 0, 0);
    lv_obj_set_style_bg_opa(w->container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(w->container, 0, 0);
    lv_obj_set_style_pad_all(w->container, 0, 0);
    lv_obj_clear_flag(w->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(w->container, global_click_cb, LV_EVENT_CLICKED, NULL);

    w->title = make_title(w->container, title);

    make_usage_panel(w->container, L.content_y, "Current",
                     &w->pct_session, &w->label_session,
                     &w->bar_session, &w->reset_session);
    make_usage_panel(w->container,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &w->pct_weekly, &w->label_weekly,
                     &w->bar_weekly, &w->reset_weekly);

    w->anim = lv_label_create(w->container);
    lv_label_set_text(w->anim, "");
    lv_obj_set_style_text_font(w->anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(w->anim, accent, 0);
    lv_obj_align(w->anim, LV_ALIGN_BOTTOM_MID, 0, -12);
}

// ======== WiFi Screen ========

static void init_wifi_screen(lv_obj_t* scr) {
    wifi_container = lv_obj_create(scr);
    lv_obj_set_size(wifi_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(wifi_container, 0, 0);
    lv_obj_set_style_bg_opa(wifi_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_container, 0, 0);
    lv_obj_set_style_pad_all(wifi_container, 0, 0);
    lv_obj_clear_flag(wifi_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(wifi_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    make_title(wifi_container, "WiFi");

    lv_obj_t* p_info = make_panel(wifi_container, L.margin, L.content_y,
                                  L.content_w, L.bt_info_panel_h);

    static lv_image_dsc_t icon_wifi_dsc;
    init_icon_dsc(&icon_wifi_dsc, ICON_WIFI_W, ICON_WIFI_H, icon_wifi_data, LV_COLOR_FORMAT_RGB565);

    lv_obj_t* wifi_img = lv_image_create(p_info);
    lv_image_set_src(wifi_img, &icon_wifi_dsc);
    // Only scale when the layout actually shrinks the icon (compact). Skipping
    // the call at native size avoids LVGL's per-redraw transform path on the
    // large layout; the compact layout pays it by design to match the 28px text.
    if (L.bt_icon_scale != LV_SCALE_NONE) {
        lv_image_set_pivot(wifi_img, 0, 0);        // scale toward top-left so pos stays predictable
        lv_image_set_scale(wifi_img, L.bt_icon_scale);
    }
    lv_obj_set_pos(wifi_img, 0, L.bt_status_y);

    lbl_wifi_status = lv_label_create(p_info);
    lv_label_set_text(lbl_wifi_status, "Connecting\xe2\x80\xa6");
    lv_obj_set_style_text_font(lbl_wifi_status, L.bt_status_font, 0);
    lv_obj_set_style_text_color(lbl_wifi_status, COL_DIM, 0);
    lv_obj_set_pos(lbl_wifi_status, L.bt_status_x, L.bt_status_y);

    lbl_wifi_ssid = lv_label_create(p_info);
    lv_label_set_text(lbl_wifi_ssid, "SSID: ---");
    lv_obj_set_style_text_font(lbl_wifi_ssid, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_wifi_ssid, COL_DIM, 0);
    lv_obj_set_pos(lbl_wifi_ssid, 0, L.bt_device_y);

    lbl_wifi_ip = lv_label_create(p_info);
    lv_label_set_text(lbl_wifi_ip, "IP: ---");
    lv_obj_set_style_text_font(lbl_wifi_ip, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_wifi_ip, COL_DIM, 0);
    lv_obj_set_pos(lbl_wifi_ip, 0, L.bt_mac_y);

    lbl_wifi_rssi = lv_label_create(p_info);
    lv_label_set_text(lbl_wifi_rssi, "Signal: ---");
    lv_obj_set_style_text_font(lbl_wifi_rssi, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_wifi_rssi, COL_DIM, 0);
    lv_obj_set_pos(lbl_wifi_rssi, 0, L.bt_rssi_y);

    lbl_wifi_daemon = lv_label_create(p_info);
    lv_label_set_text(lbl_wifi_daemon, "Daemon: ---");
    lv_obj_set_style_text_font(lbl_wifi_daemon, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_wifi_daemon, COL_DIM, 0);
    lv_obj_set_pos(lbl_wifi_daemon, 0, L.bt_daemon_y);

    lbl_wifi_age = lv_label_create(p_info);
    lv_label_set_text(lbl_wifi_age, "Updated: \xe2\x80\x94");
    lv_obj_set_style_text_font(lbl_wifi_age, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_wifi_age, COL_DIM, 0);
    lv_obj_set_pos(lbl_wifi_age, 0, L.bt_age_y);

    lv_obj_t* lbl_credit = lv_label_create(wifi_container);
    lv_label_set_text(lbl_credit, "Built by @hermannbjorgvin");
    lv_obj_set_style_text_font(lbl_credit, L.bt_credit_1_font, 0);
    lv_obj_set_style_text_color(lbl_credit, COL_DIM, 0);
    lv_obj_align(lbl_credit, LV_ALIGN_BOTTOM_MID, 0, -46);

    lv_obj_t* lbl_credit2 = lv_label_create(wifi_container);
    lv_label_set_text(lbl_credit2, "Clawd animation by @marciogranzotto");
    lv_obj_set_style_text_font(lbl_credit2, L.bt_credit_2_font, 0);
    lv_obj_set_style_text_color(lbl_credit2, COL_DIM, 0);
    lv_obj_align(lbl_credit2, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_obj_add_flag(wifi_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_icon_dsc(&logo_claude_dsc, LOGO_CLAUDE_WIDTH, LOGO_CLAUDE_HEIGHT, logo_claude_data, LV_COLOR_FORMAT_RGB565A8);
    init_icon_dsc(&logo_openai_dsc, LOGO_OPENAI_WIDTH, LOGO_OPENAI_HEIGHT, logo_openai_data, LV_COLOR_FORMAT_RGB565A8);
    init_battery_icons();

    init_provider_screen(scr, "Claude", COL_ACCENT, &claude_w);
    init_provider_screen(scr, "Codex", COL_ACCENT_CODEX, &codex_w);
    lv_obj_add_flag(codex_w.container, LV_OBJ_FLAG_HIDDEN);
    init_wifi_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_claude_dsc);
    lv_obj_set_pos(logo_img, L.margin, L.logo_y);

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    // Center the battery against the logo, matching the title (see row_center_y).
    lv_obj_set_pos(battery_img, L.scr_w - ICON_BATTERY_W - L.margin, row_center_y(ICON_BATTERY_H));
}

static void ui_update_provider(ProviderWidgets* w,
                               float session_pct, int session_reset,
                               float weekly_pct,  int weekly_reset,
                               bool present, bool ok, const char* absent_msg) {
    char buf[48];

    if (!present) {
        // Provider not subscribed — show placeholder in both windows.
        lv_label_set_text(w->pct_session, "--%");
        lv_obj_set_style_text_color(w->pct_session, COL_DIM, 0);
        lv_bar_set_value(w->bar_session, 0, LV_ANIM_OFF);
        lv_label_set_text(w->reset_session, absent_msg);
        lv_obj_set_style_text_color(w->reset_session, COL_DIM, 0);

        lv_label_set_text(w->pct_weekly, "--%");
        lv_obj_set_style_text_color(w->pct_weekly, COL_DIM, 0);
        lv_bar_set_value(w->bar_weekly, 0, LV_ANIM_OFF);
        lv_label_set_text(w->reset_weekly, absent_msg);
        lv_obj_set_style_text_color(w->reset_weekly, COL_DIM, 0);
    } else if (session_pct < 0) {
        // Present but never polled successfully yet — connecting.
        lv_label_set_text(w->pct_session, "--%");
        lv_obj_set_style_text_color(w->pct_session, COL_DIM, 0);
        lv_bar_set_value(w->bar_session, 0, LV_ANIM_OFF);
        lv_label_set_text(w->reset_session, "Connecting...");
        lv_obj_set_style_text_color(w->reset_session, COL_DIM, 0);

        lv_label_set_text(w->pct_weekly, "--%");
        lv_obj_set_style_text_color(w->pct_weekly, COL_DIM, 0);
        lv_bar_set_value(w->bar_weekly, 0, LV_ANIM_OFF);
        lv_label_set_text(w->reset_weekly, "Connecting...");
        lv_obj_set_style_text_color(w->reset_weekly, COL_DIM, 0);
    } else {
        // Has data — render fresh numbers.
        lv_color_t reset_color = ok ? COL_DIM : COL_STALE;

        int s_pct = (int)(session_pct + 0.5f);
        lv_label_set_text_fmt(w->pct_session, "%d%%", s_pct);
        lv_obj_set_style_text_color(w->pct_session, COL_TEXT, 0);
        lv_bar_set_value(w->bar_session, s_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(w->bar_session, pct_color(session_pct), LV_PART_INDICATOR);
        format_reset_time(session_reset, buf, sizeof(buf));
        lv_label_set_text(w->reset_session, buf);
        lv_obj_set_style_text_color(w->reset_session, reset_color, 0);

        if (weekly_pct < 0) {
            // Defensive: session has data but weekly is the -1 sentinel.
            lv_label_set_text(w->pct_weekly, "--%");
            lv_obj_set_style_text_color(w->pct_weekly, COL_DIM, 0);
            lv_bar_set_value(w->bar_weekly, 0, LV_ANIM_OFF);
            lv_label_set_text(w->reset_weekly, "Connecting...");
            lv_obj_set_style_text_color(w->reset_weekly, COL_DIM, 0);
        } else {
            int wk_pct = (int)(weekly_pct + 0.5f);
            lv_label_set_text_fmt(w->pct_weekly, "%d%%", wk_pct);
            lv_obj_set_style_text_color(w->pct_weekly, COL_TEXT, 0);
            lv_bar_set_value(w->bar_weekly, wk_pct, LV_ANIM_ON);
            lv_obj_set_style_bg_color(w->bar_weekly, pct_color(weekly_pct), LV_PART_INDICATOR);
            format_reset_time(weekly_reset, buf, sizeof(buf));
            lv_label_set_text(w->reset_weekly, buf);
            lv_obj_set_style_text_color(w->reset_weekly, reset_color, 0);
        }
    }
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    ui_update_provider(&claude_w,
                       data->session_pct,       data->session_reset_mins,
                       data->weekly_pct,        data->weekly_reset_mins,
                       data->claude_present,    data->ok,
                       "No Claude account");
    ui_update_provider(&codex_w,
                       data->codex_session_pct,       data->codex_session_reset_mins,
                       data->codex_weekly_pct,        data->codex_weekly_reset_mins,
                       data->codex_present,           data->codex_ok,
                       "No OpenAI account");
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_USAGE && current_screen != SCREEN_CODEX) return;

    uint32_t now = lv_tick_get();
    const char* const* msgs = (current_screen == SCREEN_CODEX) ? codex_messages : anim_messages;
    size_t msg_count = (current_screen == SCREEN_CODEX) ? CODEX_MSG_COUNT : ANIM_MSG_COUNT;

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % msg_count;
        anim_msg_start = now;
    }

    if (now - anim_last_ms >= spinner_ms[anim_spinner_idx]) {
        anim_last_ms = now;
        anim_phase = (anim_phase + 1) % SPINNER_PHASES;
        anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                        : (SPINNER_PHASES - anim_phase);

        static char buf[80];
        snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
                 spinner_frames[anim_spinner_idx],
                 msgs[anim_msg_idx % msg_count]);
        lv_obj_t* anim_lbl = (current_screen == SCREEN_CODEX) ? codex_w.anim : claude_w.anim;
        lv_label_set_text(anim_lbl, buf);
    }
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(claude_w.container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(codex_w.container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:  splash_show(); break;
    case SCREEN_USAGE:   lv_obj_clear_flag(claude_w.container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_CODEX:   lv_obj_clear_flag(codex_w.container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_WIFI:    lv_obj_clear_flag(wifi_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    if (logo_img) {
        if (screen == SCREEN_SPLASH) {
            lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
            if (screen == SCREEN_CODEX) lv_image_set_src(logo_img, &logo_openai_dsc);
            else                        lv_image_set_src(logo_img, &logo_claude_dsc);
        }
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_cycle_screen(void) {
    screen_t next;
    switch (current_screen) {
    case SCREEN_USAGE:  next = SCREEN_CODEX;  break;
    case SCREEN_CODEX:  next = SCREEN_WIFI;   break;
    case SCREEN_WIFI:   next = SCREEN_USAGE;  break;
    default:            next = SCREEN_USAGE;  break;
    }
    ui_show_screen(next);
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_wifi_status(net_state_t state, const char* ssid, const char* ip,
                           int rssi, uint32_t last_update_ms) {
    // Status label + color
    switch (state) {
    case NET_ONLINE:
        lv_label_set_text(lbl_wifi_status, "Connected");
        lv_obj_set_style_text_color(lbl_wifi_status, COL_GREEN, 0);
        break;
    case NET_CONNECTING:
        lv_label_set_text(lbl_wifi_status, "Connecting\xe2\x80\xa6");
        lv_obj_set_style_text_color(lbl_wifi_status, COL_AMBER, 0);
        break;
    case NET_DISCONNECTED:
    default:
        lv_label_set_text(lbl_wifi_status, "Offline");
        lv_obj_set_style_text_color(lbl_wifi_status, COL_RED, 0);
        break;
    }

    // SSID
    {
        static char sbuf[64];
        snprintf(sbuf, sizeof(sbuf), "SSID: %s", (ssid && *ssid) ? ssid : "---");
        lv_label_set_text(lbl_wifi_ssid, sbuf);
    }

    // IP
    {
        static char ibuf[48];
        snprintf(ibuf, sizeof(ibuf), "IP: %s", (ip && *ip) ? ip : "---");
        lv_label_set_text(lbl_wifi_ip, ibuf);
    }

    // RSSI
    {
        static char rbuf[32];
        if (state == NET_ONLINE)
            snprintf(rbuf, sizeof(rbuf), "Signal: %d dBm", rssi);
        else
            snprintf(rbuf, sizeof(rbuf), "Signal: ---");
        lv_label_set_text(lbl_wifi_rssi, rbuf);
    }

    // Age of the last good GET, computed once and shared by the daemon-health
    // verdict and the "Updated: …" line below. 0 means "never fetched".
    uint32_t age_ms = (last_update_ms == 0) ? 0 : (millis() - last_update_ms);

    // Daemon reachability — inferred from whether a good GET has landed and how
    // stale it is. Distinguishes "WiFi up but daemon unreachable" (the common
    // failure: daemon down, wrong host, or mDNS not resolving) from a healthy link.
    // The 120 s threshold is ~2.7× the 45 s device fetch interval (FETCH_INTERVAL_MS),
    // so it trips only after ~3 missed fetches — not on normal inter-fetch quiet.
    {
        if (state != NET_ONLINE) {
            lv_label_set_text(lbl_wifi_daemon, "Daemon: ---");
            lv_obj_set_style_text_color(lbl_wifi_daemon, COL_DIM, 0);
        } else if (last_update_ms == 0) {
            // No good /usage body yet. Covers both "daemon unreachable" and
            // "daemon up but still returning 503 {no data yet}" — we can't tell
            // them apart from last_update_ms alone, so don't claim "no response".
            lv_label_set_text(lbl_wifi_daemon, "Daemon: no data");
            lv_obj_set_style_text_color(lbl_wifi_daemon, COL_RED, 0);
        } else if (age_ms <= 120000UL) {
            lv_label_set_text(lbl_wifi_daemon, "Daemon: connected");
            lv_obj_set_style_text_color(lbl_wifi_daemon, COL_GREEN, 0);
        } else {
            lv_label_set_text(lbl_wifi_daemon, "Daemon: stale");
            lv_obj_set_style_text_color(lbl_wifi_daemon, COL_AMBER, 0);
        }
    }

    // Last-update age
    {
        static char abuf[48];
        if (last_update_ms == 0) {
            snprintf(abuf, sizeof(abuf), "Updated: \xe2\x80\x94");
        } else {
            uint32_t age_s = age_ms / 1000;
            if (age_s < 60)
                snprintf(abuf, sizeof(abuf), "Updated: %lus ago", (unsigned long)age_s);
            else
                snprintf(abuf, sizeof(abuf), "Updated: %lum ago", (unsigned long)(age_s / 60));
        }
        lv_label_set_text(lbl_wifi_age, abuf);
    }
}

void ui_update_battery(int percent, bool charging) {
    int idx;
    if (charging) {
        idx = 4;
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}
