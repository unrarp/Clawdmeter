#include <Preferences.h>
#include "token_store.h"

static Preferences prefs;
static bool s_init = false;

void token_store_init(void) {
    if (s_init) return;
    prefs.begin("clawdtok", false);
    s_init = true;
}

bool token_store_load(int prov, char* token, size_t token_sz,
                      char* account, size_t account_sz) {
    if (prov < 0 || prov >= PROVIDER_COUNT || !token || token_sz == 0) {
        if (token && token_sz > 0) token[0] = '\0';
        if (account && account_sz > 0) account[0] = '\0';
        return false;
    }
    char key[4];
    snprintf(key, sizeof(key), "t%d", prov);
    // getString(key, buf, len) writes straight into the caller's buffer — no
    // transient heap String for the ~2 KB Codex JWT. Returns 0 (and may leave
    // the buffer untouched) when the key is absent, so NUL-terminate on miss.
    size_t n = prefs.getString(key, token, token_sz);
    if (n == 0) token[0] = '\0';

    if (account && account_sz > 0) {
        snprintf(key, sizeof(key), "a%d", prov);
        if (prefs.getString(key, account, account_sz) == 0) account[0] = '\0';
    }

    return n > 0;
}

void token_store_save(int prov, const char* token, const char* account) {
    if (prov < 0 || prov >= PROVIDER_COUNT) return;
    char key[4];
    snprintf(key, sizeof(key), "t%d", prov);
    prefs.putString(key, token);

    snprintf(key, sizeof(key), "a%d", prov);
    if (account && account[0] != '\0') {
        prefs.putString(key, account);
    } else {
        prefs.remove(key);
    }
}

void token_store_clear(int prov) {
    if (prov < 0 || prov >= PROVIDER_COUNT) return;
    char key[4];
    snprintf(key, sizeof(key), "t%d", prov);
    prefs.remove(key);
    snprintf(key, sizeof(key), "a%d", prov);
    prefs.remove(key);
}
