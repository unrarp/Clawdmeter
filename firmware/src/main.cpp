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

// Touch policy is driven by IDLE_WAKE_ON_TOUCH:
//   true  → a press edge while asleep wakes the device and the first touch is
//           swallowed (mirrors the button wake-consumption); a press while
//           awake counts as activity.
//   false → touch never counts as activity and is fully swallowed while the
//           panel is dark, so pets/sleeves can't wake it overnight and LVGL
//           can't quietly toggle splash<->usage on a black panel.
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);
    const bool raw_pressed = pressed;

    if (IDLE_WAKE_ON_TOUCH) {
        static bool touch_was = false;
        static bool touch_wake_swallowed = false;
        if (raw_pressed && !touch_was) {
            // Press edge — consume as wake if asleep.
            if (idle_consume_wake_press()) {
                touch_wake_swallowed = true;
                pressed = false;
            }
        } else if (!raw_pressed && touch_was) {
            // Release edge.
            if (touch_wake_swallowed) {
                touch_wake_swallowed = false;
                pressed = false;
            }
        } else if (raw_pressed && touch_wake_swallowed) {
            // Held finger through wake — keep hiding until release.
            pressed = false;
        }
        touch_was = raw_pressed;
    } else if (idle_is_asleep()) {
        pressed = false;
    }

    if (pressed) {
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
    ui_update_wifi_status(net_get_state(), net_get_ssid(), net_get_ip(), net_get_rssi(), net_last_update_ms());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging());
    ui_show_screen(SCREEN_SPLASH);

    Serial.printf("Dashboard ready (%s, %dx%d), waiting for data on WiFi...\n",
        board_caps().name, W, H);
}

static net_state_t last_net_state = NET_DISCONNECTED;

static float max_present_session_pct(const UsageData& u) {
    float m = -1.0f;
    for (int i = 0; i < PROVIDER_COUNT; i++) {
        const ProviderUsage& p = u.providers[i];
        if (p.present && p.session_pct >= 0 && p.session_pct > m) m = p.session_pct;
    }
    return m < 0 ? 0.0f : m;   // idle default when no present provider has data
}

// Per-button edge detector. The first press from sleep is consumed as a
// wake-only event (idle_consume_wake_press) and reports no action; subsequent
// awake press edges report an action for the caller to handle.
struct EdgeButton {
    bool was = false;
    // Pass the current held state once per loop. Returns true exactly on a
    // press edge that should fire the button's action — never on the first
    // press from sleep (consumed as wake-only) or on release.
    bool action_edge(bool now) {
        bool fire = false;
        if (now != was) {
            if (now && !idle_consume_wake_press()) fire = true;
            was = now;
        }
        return fire;
    }
};

void loop() {
    idle_tick();
    lv_timer_handler();
    ui_tick_anim();
    net_tick();
    power_hal_tick();
    imu_hal_tick();
    splash_tick();
    // Rotation transition (blank + ramp) would fight the idle fade — skip
    // ticks while the panel is dark. A rotation that happens during sleep
    // is detected by the next tick after wake and ramped in then.
    if (!idle_is_asleep()) display_hal_tick();

    // ---- Physical buttons ----
    //   PRIMARY   → wake-only (first press); local activity on subsequent presses
    //   SECONDARY → wake-only (first press); local activity on subsequent presses
    //               (only if the board has a secondary button)
    //   PWR       → cycle screens; on splash, cycle animations
    // First press from sleep is consumed as a wake-only event by
    // idle_consume_wake_press(); the normal action fires from the second
    // press. Activity bookkeeping happens inside idle_consume_wake_press
    // so no separate idle_note_activity() call is needed here.
    // Note: HID keyboard emission (BLE) has been removed — the device is a
    // WiFi-only usage gauge.
    {
        static EdgeButton primary_btn;

        if (primary_btn.action_edge(input_hal_is_held(INPUT_BTN_PRIMARY)))
            net_request_refresh();  // awake press → force an immediate /usage fetch

        if (board_caps().button_count >= 2) {
            static EdgeButton secondary_btn;
            // No awake action (WiFi gauge); the call still runs the wake-consume
            // bookkeeping so the first press from sleep only wakes the panel.
            (void)secondary_btn.action_edge(input_hal_is_held(INPUT_BTN_SECONDARY));
        }

        if (power_hal_pwr_pressed()) {
            if (!idle_consume_wake_press()) {
                if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
                else                                          ui_cycle_screen();
            }
        }
    }

    net_state_t ns = net_get_state();
    if (ns != last_net_state) {
        last_net_state = ns;
        ui_update_wifi_status(ns, net_get_ssid(), net_get_ip(), net_get_rssi(), net_last_update_ms());
    }

    // The "Updated: Ns ago" / daemon-staleness lines are computed at call time,
    // so they only advance when this runs. State-change and new-data events are
    // ~45 s apart — without a periodic tick the age sticks at "0s ago". Refresh
    // once a second while the WiFi diagnostics screen is the one on display.
    {
        static uint32_t last_wifi_tick = 0;
        uint32_t now = millis();
        if (ui_get_current_screen() == SCREEN_WIFI && now - last_wifi_tick >= 1000) {
            last_wifi_tick = now;
            ui_update_wifi_status(ns, net_get_ssid(), net_get_ip(), net_get_rssi(), net_last_update_ms());
        }
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
        if (parse_json(net_get_data(), &usage)) {
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
            // Refresh diagnostics display with the latest network state.
            ui_update_wifi_status(net_get_state(), net_get_ssid(), net_get_ip(), net_get_rssi(), net_last_update_ms());
        }
        // No ack/nack — HTTP pull has no acknowledgement mechanism.
    }

    delay(5);
}
