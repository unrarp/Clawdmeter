#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "data.h"
#include "ui.h"
#include "net.h"
#include "splash.h"
#include "usage_rate.h"
#include "idle.h"
#include "idle_cfg.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"

static UsageData usage = {};

// ---- LVGL draw buffers (partial render mode) ----
// PSRAM-equipped boards (S3) can comfortably hold larger strips. PSRAM-free
// boards (e.g. ESP32-C6) allocate from internal SRAM, so we shrink the strip
// — 480×20 RGB565 = 19 KB × 2 buffers = 38 KB, fits beside everything else.
#ifdef BOARD_HAS_PSRAM
#define BUF_LINES 40
#define LV_BUF_CAPS (MALLOC_CAP_SPIRAM)
#else
#define BUF_LINES 20
#define LV_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    lv_display_flush_ready(disp);
}

static void rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    display_hal_round_area(&area->x1, &area->y1, &area->x2, &area->y2);
}

// A touch counts as activity (resets the idle timer); the event itself passes
// straight through to LVGL. There is no screen-sleep state to wake from.
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);

    if (pressed) {
        idle_note_activity();
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Wire keys + present-default per provider, indexed by PROV_*. Order must match
// the enum in data.h.
struct ProviderKeys {
    const char* s; const char* sr; const char* w; const char* wr;
    const char* st; const char* ok; const char* present; bool present_default;
};
static const ProviderKeys PROVIDER_KEYS[PROVIDER_COUNT] = {
    { "s",  "sr",  "w",  "wr",  "st",  "ok",  "sp", true  },  // Claude: sp defaults true (old payloads => shown)
    { "cs", "csr", "cw", "cwr", "cst", "cok", "cp", false },  // Codex: cp defaults false (absent on old payloads)
};

// Parse a JSON line into UsageData.
static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    for (int i = 0; i < PROVIDER_COUNT; i++) {
        const ProviderKeys& k = PROVIDER_KEYS[i];
        ProviderUsage& p = out->providers[i];
        p.session_pct        = doc[k.s]  | -1.0f;
        p.session_reset_mins = doc[k.sr] | -1;
        p.weekly_pct         = doc[k.w]  | -1.0f;
        p.weekly_reset_mins  = doc[k.wr] | -1;
        strlcpy(p.status, doc[k.st] | "unknown", sizeof(p.status));
        p.ok      = doc[k.ok]      | false;
        p.present = doc[k.present] | k.present_default;
    }

    out->valid = true;
    return true;
}

// ---- Serial command buffer ----
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
#ifndef BOARD_HAS_PSRAM
    // A full RGB565 framebuffer doesn't fit in internal SRAM on PSRAM-free
    // boards (e.g. 480×480×2 = 460 KB). Capture is unsupported there.
    Serial.println("SCREENSHOT_UNSUPPORTED");
    return;
#else
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
#endif
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// Each board provides this. Must bring up the shared I2C bus (Wire.begin
// with the board's SDA/SCL pins) and any board-private hardware that has
// to settle before display/touch (e.g. an IO expander gating the LCD
// reset line). Called exactly once at the start of setup().
extern "C" void board_init(void);

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    board_init();

    display_hal_init();
    display_hal_begin();
    idle_init();   // takes over brightness (DISPLAY_DEFAULT_BRIGHTNESS) and starts the idle timer

    power_hal_init();
    imu_hal_init();
    touch_hal_init();

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);

    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, W * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    net_init();
    input_hal_init();

    ui_init();
    ui_update_wifi_status(net_get_state(), net_get_ssid(), net_get_ip(), net_get_rssi(),
                          net_last_update_ms(), net_daemon_health());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging());
    ui_show_screen(SCREEN_SPLASH);

    Serial.printf("Dashboard ready (%s, %dx%d), waiting for data on WiFi...\n",
        board_caps().name, W, H);
}

static net_state_t     last_net_state     = NET_DISCONNECTED;
static daemon_health_t last_daemon_health = DAEMON_OFFLINE;

static float max_present_session_pct(const UsageData& u) {
    float m = -1.0f;
    for (int i = 0; i < PROVIDER_COUNT; i++) {
        const ProviderUsage& p = u.providers[i];
        if (p.present && p.session_pct >= 0 && p.session_pct > m) m = p.session_pct;
    }
    return m < 0 ? 0.0f : m;   // idle default when no present provider has data
}

// True if any substantive usage/state field differs. Deliberately ignores
// *_reset_mins — those count down every poll, so including them would reset the
// idle timer continuously and defeat auto-power-off.
static bool usage_changed(const UsageData& a, const UsageData& b) {
    for (int i = 0; i < PROVIDER_COUNT; i++) {
        const ProviderUsage& x = a.providers[i];
        const ProviderUsage& y = b.providers[i];
        if (x.session_pct != y.session_pct) return true;
        if (x.weekly_pct  != y.weekly_pct)  return true;
        if (x.ok      != y.ok)      return true;
        if (x.present != y.present) return true;
        if (strcmp(x.status, y.status) != 0) return true;
    }
    return false;
}

// Per-button edge detector. Returns true on each press edge (and notes the
// press as activity so it resets the idle timer); never on release.
struct EdgeButton {
    bool was = false;
    bool action_edge(bool now) {
        bool fire = false;
        if (now != was) {
            if (now) { idle_note_activity(); fire = true; }
            was = now;
        }
        return fire;
    }
};

void loop() {
    lv_timer_handler();
    ui_tick_anim();
    net_tick();
    power_hal_tick();
    imu_hal_tick();
    splash_tick();
    display_hal_tick();

    // ---- Physical buttons ---- (each press resets the idle timer)
    //   PRIMARY   → force an immediate /usage refresh
    //   SECONDARY → no action (WiFi gauge); only present on some boards
    //   PWR       → short press cycles screens (on splash, cycles animations);
    //               a long hold powers the device off in hardware (AXP PWRON).
    // Note: HID keyboard emission (BLE) has been removed — the device is a
    // WiFi-only usage gauge.
    {
        static EdgeButton primary_btn;

        if (primary_btn.action_edge(input_hal_is_held(INPUT_BTN_PRIMARY)))
            net_request_refresh();

        if (board_caps().button_count >= 2) {
            static EdgeButton secondary_btn;
            (void)secondary_btn.action_edge(input_hal_is_held(INPUT_BTN_SECONDARY));
        }

        if (power_hal_pwr_pressed()) {
            idle_note_activity();
            if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
            else                                          ui_cycle_screen();
        }
    }

    // Repaint the WiFi page on a net-state change OR a daemon-health transition.
    // net_daemon_health() is elapsed-time based (it flips to "stale" after the
    // last good GET ages out), so it's polled every loop — but we only touch the
    // labels when the verdict actually changes, so there's no periodic redraw.
    net_state_t     ns = net_get_state();
    daemon_health_t dh = net_daemon_health();
    if (ns != last_net_state || dh != last_daemon_health) {
        last_net_state     = ns;
        last_daemon_health = dh;
        ui_update_wifi_status(ns, net_get_ssid(), net_get_ip(), net_get_rssi(),
                              net_last_update_ms(), dh);
    }

    static int  last_pct      = -2;
    static bool last_charging = false;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    if (pct != last_pct || charging != last_charging) {
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    check_serial_cmd();

    if (net_has_data()) {
        UsageData fresh = {};
        if (parse_json(net_get_data(), &fresh)) {
            if (usage_changed(usage, fresh)) idle_note_activity();  // real change = activity
            usage = fresh;
            int   g_before  = usage_rate_group();
            float max_pct   = max_present_session_pct(usage);
            usage_rate_sample(max_pct);
            int g_after = usage_rate_group();
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, max_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            // A fresh GET advances the "Updated" timestamp even when the daemon
            // verdict is unchanged (still "connected"), so repaint here directly.
            // Resync the change-detection latches so the transition check above
            // doesn't repaint again next tick.
            last_net_state     = net_get_state();
            last_daemon_health = net_daemon_health();
            ui_update_wifi_status(last_net_state, net_get_ssid(), net_get_ip(), net_get_rssi(),
                                  net_last_update_ms(), last_daemon_health);
        }
        // No ack/nack — HTTP pull has no acknowledgement mechanism.
    }

    // Runs last so this iteration's button/touch/data activity is registered
    // before the inactivity timeout is evaluated.
    idle_tick();

    delay(5);
}
