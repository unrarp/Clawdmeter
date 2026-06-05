#include "usage_rate.h"

#include <Arduino.h>

#include "data.h"  // PROVIDER_COUNT

// Thresholds in %/min. A 5-hour (300 min) session ÷ 100% = 0.33 %/min to fill
// exactly at the same pace as the session itself resets — the user wants the
// "heavy" tier to start right there (filling in 4–5 hours).
//   < 0.10  →  Idle    (17h+ to fill, basically dormant)
//   < 0.20  →  Normal  (8–17h to fill, slow steady use)
//   < 0.33  →  Active  (5–8h, heavy but not yet pace-matching)
//   >=0.33  →  Heavy   (≤5h, matching or beating the session reset)
#define RATE_THRESH_NORMAL 0.10f
#define RATE_THRESH_ACTIVE 0.20f
#define RATE_THRESH_HEAVY  0.33f

// Minimum span between oldest and newest sample before we trust the computed
// rate. The whole point of the ring buffer is to smooth out single-sample
// jitter — at ~60s fetch interval, a 1% bump between two consecutive samples
// looks like 1 %/min (Heavy) but really just means you grew 1% in the last
// minute. We require ~4 min of accumulated history so the rate reflects a
// real trend, not one noisy delta. Side-effect: ~4 min warm-up after boot
// during which we report Idle.
#define MIN_WINDOW_MS 240000UL

// Window length (samples × ~60s fetch interval ≈ 9 min). session_pct arrives
// quantized to whole percent (the upstream 5h-utilization header is 2-decimal),
// so the smallest non-zero rate the tracker can ever see is 1% ÷ window. The
// window is deliberately ~9 min, not shorter: real usage is bursty (heavy while
// a request streams, flat while the user reads), and a short window keeps
// catching the flat patches and reporting Idle. At ~9 min, a single whole-percent
// crossing reads Normal (≈0.11 %/min), two Active, three Heavy, and a brief flat
// stretch between bursts no longer collapses the rate to zero. Going much longer
// would push a single crossing below the Normal threshold (1%÷15min ≈ 0.067).
#define RING_SIZE 10

struct Sample {
    uint32_t ms;
    float pct;
};

// Per-provider ring buffers — each provider's rate is computed independently so a
// flat-but-high provider can't mask a climbing one, and each provider detects its
// own session reset (Claude and Codex reset on different clocks).
static Sample ring[PROVIDER_COUNT][RING_SIZE];
static uint8_t count[PROVIDER_COUNT] = {0};
static uint8_t head[PROVIDER_COUNT] = {0};  // index of next write slot

static inline uint8_t oldest_idx(int p) {
    return (head[p] + RING_SIZE - count[p]) % RING_SIZE;
}

static inline uint8_t latest_idx(int p) {
    return (head[p] + RING_SIZE - 1) % RING_SIZE;
}

static void usage_rate_reset(int p) {
    count[p] = 0;
    head[p] = 0;
}

void usage_rate_sample(int provider, float session_pct) {
    if (provider < 0 || provider >= PROVIDER_COUNT) return;
    uint32_t now = millis();

    if (count[provider] > 0) {
        // Session reset: pct dropped substantially. Restart tracking.
        if (session_pct + 5.0f < ring[provider][latest_idx(provider)].pct) {
            usage_rate_reset(provider);
        }
    }

    ring[provider][head[provider]] = {now, session_pct};
    head[provider] = (head[provider] + 1) % RING_SIZE;
    if (count[provider] < RING_SIZE) count[provider]++;
}

static int group_for(int p) {
    if (count[p] < 2) return 0;

    uint8_t o = oldest_idx(p);
    uint8_t l = latest_idx(p);
    // If no new sample has arrived for >3× the window, treat as idle so a
    // provider that goes offline doesn't freeze the ring at its last active tier.
    if (millis() - ring[p][l].ms > MIN_WINDOW_MS * 3) return 0;
    uint32_t dt = ring[p][l].ms - ring[p][o].ms;
    if (dt < MIN_WINDOW_MS) return 0;

    float dp = ring[p][l].pct - ring[p][o].pct;
    if (dp < 0.0f) dp = 0.0f;
    float rate = dp * 60000.0f / (float)dt;

    if (rate < RATE_THRESH_NORMAL) return 0;
    if (rate < RATE_THRESH_ACTIVE) return 1;
    if (rate < RATE_THRESH_HEAVY) return 2;
    return 3;
}

int usage_rate_group(void) {
    int max_g = 0;
    for (int p = 0; p < PROVIDER_COUNT; p++) {
        int g = group_for(p);
        if (g > max_g) max_g = g;
    }
    return max_g;
}
