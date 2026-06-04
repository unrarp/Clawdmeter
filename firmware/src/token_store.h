#pragma once
#include <Arduino.h>
#include "data.h"   // for PROV_CLAUDE / PROV_CODEX / PROVIDER_COUNT

// Persistent per-provider credential cache backed by NVS (Preferences).
// One opaque record per provider: an auth token, plus an account id (only
// Codex uses the account id; Claude passes account=nullptr / "").
// First persistent-storage use in the firmware. Keep all NVS access here —
// net.cpp must not touch Preferences directly.

void token_store_init(void);   // open the NVS namespace; call once at startup

// Load provider `prov`'s cached token (and account id) into the caller's
// buffers. Returns true iff a non-empty token was stored. Output buffers are
// always NUL-terminated (set to "" when absent). `account`/`account_sz` may be
// 0/nullptr for providers without an account id.
bool token_store_load(int prov, char* token, size_t token_sz,
                      char* account, size_t account_sz);

// Persist provider `prov`'s token (+ account id; pass nullptr/"" if none).
void token_store_save(int prov, const char* token, const char* account);

// Erase provider `prov`'s stored record (token + account id).
void token_store_clear(int prov);
