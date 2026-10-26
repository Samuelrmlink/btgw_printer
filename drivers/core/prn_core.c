#include "core/prn_core.h"

#include <string.h>

int prn_hub_attach(prn_hub_t *h, const char *self) {
    h->m = pex_find(PRN_HUB_MODULE);
    h->fn = h->m ? pex_sym(h->m, PRN_HUB_SYM) : 0;
    if (!h->m || !h->fn) {
        h->m = NULL;
        h->fn = 0;
        return PEX_ENOENT;
    }
    int rc = (int)(uint32_t)pex_call(h->m, h->fn, PRN_HUB_REGISTER, (uint32_t)(uintptr_t)self, PRN_ABI, 0);
    if (rc < 0) {
        h->m = NULL;
        h->fn = 0;
    }
    return rc < 0 ? rc : 0;
}

int prn_hub_ok(prn_hub_t *h) {
    if (!h->m) { return 0; }
    if (pex_find(PRN_HUB_MODULE) != h->m) {
        h->m = NULL;
        h->fn = 0;
        return 0;
    }
    return 1;
}

void prn_hub_detach(prn_hub_t *h, const char *self) {
    if (prn_hub_ok(h)) { pex_call(h->m, h->fn, PRN_HUB_UNREGISTER, (uint32_t)(uintptr_t)self, 0, 0); }
    h->m = NULL;
    h->fn = 0;
}

int prn_hub_emit(prn_hub_t *h, const char *kind, const char *json) {
    if (!prn_hub_ok(h)) { return PEX_ENOENT; }
    return (int)(uint32_t)pex_call(h->m, h->fn, PRN_HUB_EMIT, (uint32_t)(uintptr_t)kind,
                                   (uint32_t)(uintptr_t)json, 0);
}

int prn_hub_job_done(prn_hub_t *h, uint32_t job, int outcome, const char *msg) {
    if (!prn_hub_ok(h)) { return PEX_ENOENT; }
    return (int)(uint32_t)pex_call(h->m, h->fn, PRN_HUB_JOB_DONE, job, (uint32_t)outcome,
                                   (uint32_t)(uintptr_t)msg);
}

int prn_hub_kick(prn_hub_t *h) {
    if (!prn_hub_ok(h)) { return PEX_ENOENT; }
    return (int)(uint32_t)pex_call(h->m, h->fn, PRN_HUB_KICK, 0, 0, 0);
}

int prn_fmt_mm100(char *out, size_t cap, int32_t v) {
    char t[16];
    int n = 0;
    uint32_t a = v < 0 ? (uint32_t)-v : (uint32_t)v;
    uint32_t ip = a / 100u, fp = a % 100u;
    if (v < 0 && (ip || fp)) { t[n++] = '-'; }
    char d[12];
    int k = 0;
    do { d[k++] = (char)('0' + ip % 10u); ip /= 10u; } while (ip);
    while (k) { t[n++] = d[--k]; }
    if (fp) {
        t[n++] = '.';
        t[n++] = (char)('0' + fp / 10u);
        if (fp % 10u) { t[n++] = (char)('0' + fp % 10u); }
    }
    t[n] = 0;
    if (cap) {
        size_t c = (size_t)n < cap - 1 ? (size_t)n : cap - 1;
        memcpy(out, t, c);
        out[c] = 0;
    }
    return n;
}

int32_t prn_mm100_to_dots(int32_t mm100, int dpi) {
    /* mm * dpi / 25.4, rounded half away from zero, like the old tspl_mm_to_dots */
    int64_t num = (int64_t)mm100 * dpi;
    int64_t den = 2540;
    return (int32_t)(num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den));
}

int prn_rows_all_black(const uint8_t *rows, size_t n, int black_bit) {
    uint8_t all = black_bit ? 0xFF : 0x00;
    for (size_t i = 0; i < n; i++) { if (rows[i] != all) { return 0; } }
    return 1;
}

void prn_punch_rows(uint8_t *rows, uint32_t row0, uint32_t nrows, uint16_t rb, int black_bit) {
    for (uint32_t i = 0; i < nrows; i++) {
        if ((row0 + i) % 16u) { continue; }
        uint8_t *b = &rows[(size_t)i * rb];
        if (black_bit) { *b &= 0x7F; } else { *b |= 0x80; }
    }
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

int prn_addr_parse(const char *s, uint8_t *a) {
    if (!s) { return 0; }
    for (int i = 0; i < 6; i++) {
        int h = hexv(s[0]), l = h >= 0 ? hexv(s[1]) : -1;
        if (h < 0 || l < 0) { return 0; }
        a[i] = (uint8_t)(h << 4 | l);
        s += 2;
        if (i < 5) { if (*s != ':' && *s != '-') { return 0; } s++; }
    }
    return *s == 0 || *s == '"';
}

void prn_addr_fmt(const uint8_t *a, char *out) {
    static const char H[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        out[i * 3] = H[a[i] >> 4];
        out[i * 3 + 1] = H[a[i] & 15];
        out[i * 3 + 2] = i < 5 ? ':' : 0;
    }
}

uint32_t prn_backoff_ms(uint32_t attempt, uint32_t first, uint32_t max) {
    uint32_t v = first;
    while (attempt-- && v < max) { v *= 2; }
    return v > max ? max : v;
}

int prn_json_raw(const char *json, const char *path, char *out, size_t cap) {
    size_t n = 0;
    const char *v = json ? pex_json_find(json, path, &n) : NULL;
    if (!v) { if (cap) { out[0] = 0; } return -1; }
    if (n >= cap) { n = cap - 1; }
    memcpy(out, v, n);
    out[n] = 0;
    return (int)n;
}
