#include "splash.h"
#include "splash_animations.h"
#include "theme.h"
#include "usage_rate.h"
#include "hal/board_caps.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

// Square canvas sized to the smaller display dimension. Each animation declares
// its own width×height grid (clawd-tank SVG set: 128×128); render_frame() picks
// a per-anim integer cell size to fit and centers the grid in the canvas,
// leaving a black border rather than cropping.
static int  canvas_dim = 480;       // recomputed in splash_init()

// Background fallback when palette is missing
#define COL_EMPTY    0x0000  // true black (matches THEME_BG)

#include "fonts.h"

static lv_obj_t *splash_container = NULL;
static lv_obj_t *canvas = NULL;
static lv_obj_t *label_status = NULL;     // shown only when no animations loaded
static uint16_t *canvas_buf = NULL;        // 480x480 RGB565 (PSRAM)

static uint16_t cur_anim = 0;
static uint16_t cur_frame = 0;
static uint32_t frame_started_ms = 0;
static uint32_t last_pick_ms = 0;
static bool active = false;

// While splash is showing, auto-cycle to the next animation in the current
// rate-driven group every this many ms.
#define SPLASH_ROTATE_INTERVAL_MS 20000

// Usage-rate animation groups: 4 groups × up to 4 animations each.
// Filled at init by mapping each animation's `group` tier name to an index.
#define GROUP_COUNT 4
#define GROUP_MAX   4
static int8_t  group_lists[GROUP_COUNT][GROUP_MAX];
static uint8_t group_size[GROUP_COUNT] = {0};
static uint8_t group_rotation[GROUP_COUNT] = {0};

// Rate-bucket tier names indexed by usage-rate group. This is the single
// firmware authority for tier name <-> index, so the order MUST match
// usage_rate_group() (usage_rate.cpp: 0=idle, 1=normal, 2=active, 3=heavy).
static const char* const RATE_TIERS[GROUP_COUNT] = { "idle", "normal", "active", "heavy" };

static int tier_index(const char* group) {
    if (!group) return -1;
    for (int g = 0; g < GROUP_COUNT; g++)
        if (strcmp(group, RATE_TIERS[g]) == 0) return g;
    return -1;
}

static void resolve_group_lists(void) {
    for (int g = 0; g < GROUP_COUNT; g++) {
        group_size[g] = 0;
        for (int s = 0; s < GROUP_MAX; s++) {
            group_lists[g][s] = -1;
        }
    }
    for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
        int g = tier_index(splash_anims[i].group);
        if (g < 0) {
            Serial.printf("splash: '%s' has unknown group '%s' — skipped\n",
                          splash_anims[i].name, splash_anims[i].group);
            continue;
        }
        if (group_size[g] >= GROUP_MAX) {
            Serial.printf("splash: group '%s' full (max %d) — '%s' skipped\n",
                          RATE_TIERS[g], GROUP_MAX, splash_anims[i].name);
            continue;
        }
        group_lists[g][group_size[g]++] = (int8_t)i;
    }
}

static uint16_t *row_buf = NULL;   // scratch row, sized to canvas_dim

static void render_frame(const splash_anim_def_t *a, uint16_t frame_idx) {
    if (!row_buf || !canvas_buf || !a) return;
    const int gw = a->width, gh = a->height;
    if (gw <= 0 || gh <= 0) return;
    const uint8_t *cells = a->frames[frame_idx];
    const uint16_t *palette = a->palette;

    // Largest integer cell that fits the grid in the square canvas, centered.
    int cell = canvas_dim / (gw > gh ? gw : gh);
    if (cell < 1) cell = 1;
    // Defensive: an animation larger than the canvas (cell forced to 1) would
    // overflow row_buf/canvas_buf and produce negative offsets — crop the drawn
    // grid to what fits. Current 128x128 set on a >=192 canvas is unaffected.
    int cols = gw, rows = gh;
    if (cols * cell > canvas_dim) cols = canvas_dim / cell;
    if (rows * cell > canvas_dim) rows = canvas_dim / cell;
    const int draw_w = cols * cell, draw_h = rows * cell;
    const int ox = (canvas_dim - draw_w) / 2, oy = (canvas_dim - draw_h) / 2;

    // Black border around the (possibly non-filling) art.
    memset(canvas_buf, 0, (size_t)canvas_dim * canvas_dim * 2);

    for (int gy = 0; gy < rows; gy++) {
        for (int gx = 0; gx < cols; gx++) {
            uint8_t code = cells[gy * gw + gx];
            uint16_t color = (palette && code < SPLASH_PALETTE_SIZE) ? palette[code] : COL_EMPTY;
            uint16_t *p = &row_buf[gx * cell];
            for (int i = 0; i < cell; i++) p[i] = color;
        }
        for (int dy = 0; dy < cell; dy++) {
            uint16_t *dst = &canvas_buf[(size_t)(oy + gy * cell + dy) * canvas_dim + ox];
            memcpy(dst, row_buf, (size_t)draw_w * 2);
        }
    }
    if (canvas) lv_obj_invalidate(canvas);
}

static void show_placeholder() {
    // Solid dark background + centered status label.
    if (canvas_buf) {
        for (int i = 0; i < canvas_dim * canvas_dim; i++) canvas_buf[i] = COL_EMPTY;
    }
    if (canvas) lv_obj_invalidate(canvas);
    if (label_status) lv_obj_clear_flag(label_status, LV_OBJ_FLAG_HIDDEN);
}

void splash_init(lv_obj_t *parent) {
    const BoardCaps& c = board_caps();
    int min_dim = (c.width < c.height) ? c.width : c.height;
    canvas_dim = min_dim;            // square canvas fits the smaller dimension

#ifdef BOARD_HAS_PSRAM
    const uint32_t canvas_caps = MALLOC_CAP_SPIRAM;
#else
    // Without PSRAM the full 480×480 RGB565 canvas (460 KB) won't fit. Cap
    // the canvas so the buffer stays under ~80 KB, leaving the rest of
    // internal SRAM free for LVGL and the audio/PMU stacks. The canvas is
    // centered, so the cost is extra black border around the pixel art — not
    // cropping (and per-anim cell sizing still fits the grid).
    const uint32_t canvas_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const int MAX_DIM_NO_PSRAM = 192;  // 192*192*2 = 72 KB
    if (canvas_dim > MAX_DIM_NO_PSRAM) canvas_dim = MAX_DIM_NO_PSRAM;
#endif

    canvas_buf = (uint16_t*)heap_caps_malloc((size_t)canvas_dim * canvas_dim * 2, canvas_caps);
    row_buf    = (uint16_t*)heap_caps_malloc((size_t)canvas_dim * 2,              canvas_caps);
    if (!canvas_buf || !row_buf) {
        Serial.println("splash: failed to alloc canvas buffer");
        return;
    }

    splash_container = lv_obj_create(parent);
    lv_obj_set_size(splash_container, c.width, c.height);
    lv_obj_set_pos(splash_container, 0, 0);
    lv_obj_set_style_bg_color(splash_container, THEME_BG, 0);
    lv_obj_set_style_bg_opa(splash_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash_container, 0, 0);
    lv_obj_set_style_pad_all(splash_container, 0, 0);
    lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_SCROLLABLE);

    canvas = lv_canvas_create(splash_container);
    lv_canvas_set_buffer(canvas, canvas_buf, canvas_dim, canvas_dim, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(canvas);

    // Placeholder label (visible only when no animations are loaded)
    label_status = lv_label_create(splash_container);
    lv_label_set_text(label_status,
        "no animations loaded\n\n"
        "regenerate with\n"
        "tools/svg_pipeline/build.sh");
    lv_obj_set_style_text_font(label_status, &font_styrene_28, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xb0aea5), 0);
    lv_obj_set_style_text_align(label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label_status);

    resolve_group_lists();

    if (SPLASH_ANIM_COUNT == 0) {
        show_placeholder();
    } else {
        lv_obj_add_flag(label_status, LV_OBJ_FLAG_HIDDEN);
        render_frame(&splash_anims[0], 0);
        frame_started_ms = millis();
    }

    lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
}

void splash_tick(void) {
    if (!active || SPLASH_ANIM_COUNT == 0) return;

    // Auto-rotate to the next animation in the current group.
    if (millis() - last_pick_ms >= SPLASH_ROTATE_INTERVAL_MS) {
        splash_pick_for_current_rate();
    }

    const splash_anim_def_t *a = &splash_anims[cur_anim];
    if (a->frame_count == 0) return;

    uint16_t hold = a->holds[cur_frame];
    if (millis() - frame_started_ms >= hold) {
        cur_frame = (cur_frame + 1) % a->frame_count;
        frame_started_ms = millis();
        render_frame(a, cur_frame);
    }
}

void splash_next(void) {
    if (SPLASH_ANIM_COUNT == 0) return;
    cur_anim = (cur_anim + 1) % SPLASH_ANIM_COUNT;
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    render_frame(a, 0);
    Serial.printf("splash: -> %s\n", a->name);
}

void splash_pick_for_current_rate(void) {
    if (SPLASH_ANIM_COUNT == 0) return;
    int g = usage_rate_group();
    if (g < 0 || g >= GROUP_COUNT) g = 0;
    if (group_size[g] == 0) return;

    uint8_t slot = group_rotation[g] % group_size[g];
    group_rotation[g]++;
    int8_t idx = group_lists[g][slot];
    if (idx < 0) return;

    cur_anim = (uint16_t)idx;
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    render_frame(&splash_anims[cur_anim], 0);
}

bool splash_is_active(void) { return active; }

void splash_show(void) {
    splash_pick_for_current_rate();
    if (splash_container) lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = true;
}

void splash_hide(void) {
    if (splash_container) lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = false;
}

lv_obj_t* splash_get_root(void) {
    return splash_container;
}
