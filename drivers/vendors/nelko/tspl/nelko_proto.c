/* docs/protocols/nelko-tspl.md and nelko-p21.md; status codes from
 * merlinschumacher/nelko-p21-print (facts only, see docs/references.md). */
#include "vendors/nelko/tspl/nelko_proto.h"

#include <string.h>

uint16_t nk_crc16_modbus(const uint8_t *p, size_t n) {
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) { c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1); }
    }
    return c;
}

/* "<word>" then one space; the payload without a trailing CRLF. Only one
 * space: the payload is binary, and 20 % battery is BCD 0x20. */
static int payload(const uint8_t *p, size_t n, const char *word, const uint8_t **pl, size_t *pn) {
    size_t w = strlen(word);
    if (n < w || memcmp(p, word, w) != 0) { return 0; }
    p += w;
    n -= w;
    if (n && *p == ' ') { p++; n--; }
    if (n >= 2 && p[n - 2] == '\r' && p[n - 1] == '\n') { n -= 2; }
    *pl = p;
    *pn = n;
    return 1;
}

int nk_parse_battery(const uint8_t *p, size_t n, nk_battery_t *o) {
    const uint8_t *q;
    size_t m;
    if (!payload(p, n, "BATTERY", &q, &m) || m < 1) { return 0; }
    int pct = ((q[0] >> 4) & 15) * 10 + (q[0] & 15);
    if (pct > 100) { pct = q[0]; }
    o->percent = pct;
    o->charging = m > 1 && q[1] != 0;
    return 1;
}

static void ver(char *out, const uint8_t *v) {
    int n = 0;
    for (int i = 0; i < 3; i++) {
        unsigned x = v[i];
        char d[4];
        int k = 0;
        do { d[k++] = (char)('0' + x % 10); x /= 10; } while (x);
        while (k) { out[n++] = d[--k]; }
        if (i < 2) { out[n++] = '.'; }
    }
    out[n] = 0;
}

int nk_parse_config(const uint8_t *p, size_t n, nk_config_t *o) {
    const uint8_t *q;
    size_t m;
    if (!payload(p, n, "CONFIG", &q, &m)) { return 0; }
    memset(o, 0, sizeof *o);
    o->settings = -1;
    if (m >= 2) {
        o->dpi = q[0] << 8 | q[1];
        if (o->dpi == 0 && m > 2) { o->dpi = q[1]; }
    }
    if (m >= 8) { ver(o->hw, q + 2); ver(o->fw, q + 5); }
    if (m >= 9) { o->settings = q[8]; }
    return 1;
}

int nk_parse_status(const uint8_t *p, size_t n, nk_status_t *o) {
    if (n >= 2 && p[n - 2] == '\r' && p[n - 1] == '\n') { n -= 2; }
    if (n < 14) { return 0; }
    o->readiness = p[0];
    o->paper = p[7];
    o->length_mm = p[11];
    o->width_mm = p[13];
    o->crc_ok = n >= 16 && nk_crc16_modbus(p, n - 2) == (uint16_t)(p[n - 2] << 8 | p[n - 1]);
    return 1;
}

const char *nk_paper_name(int paper) {
    return paper == 0 ? "continuous" : paper == 1 ? "gap" : paper == 2 ? "blackmark" : "unknown";
}

void nk_rx_push(nk_rx_t *rx, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (rx->n >= sizeof rx->b) {
            memmove(rx->b, rx->b + 1, sizeof rx->b - 1);
            rx->n--;
        }
        rx->b[rx->n++] = p[i];
    }
}

static void drop(nk_rx_t *rx, size_t k) {
    if (k > rx->n) { k = rx->n; }
    memmove(rx->b, rx->b + k, rx->n - k);
    rx->n = (uint16_t)(rx->n - k);
}

static size_t crlf_from(const nk_rx_t *rx, size_t from) {
    for (size_t i = from; i + 1 < rx->n; i++) {
        if (rx->b[i] == '\r' && rx->b[i + 1] == '\n') { return i + 2; }
    }
    return 0;
}

static int starts(const nk_rx_t *rx, const char *w) {
    size_t n = strlen(w);
    size_t k = rx->n < n ? rx->n : n;
    return memcmp(rx->b, w, k) == 0;
}

size_t nk_rx_take(nk_rx_t *rx, int expect, int idle, nk_reply_t *out) {
    out->kind = NK_NONE;
    while (rx->n && (rx->b[0] == '\r' || rx->b[0] == '\n') && expect != NK_READY && expect != NK_STATUS) { drop(rx, 1); }
    if (!rx->n) { return 0; }
    if (starts(rx, "BATTERY")) {
        if (rx->n < 7) { return 0; }
        size_t end = crlf_from(rx, 9);
        if (!end && !idle) { return 0; }
        if (!end) { end = rx->n; }
        if (nk_parse_battery(rx->b, end, &out->u.battery)) { out->kind = NK_BATTERY; }
        drop(rx, end);
        return end;
    }
    if (starts(rx, "CONFIG")) {
        if (rx->n < 6) { return 0; }
        size_t end = crlf_from(rx, 7 + 8);
        if (!end && !idle) { return 0; }
        if (!end) { end = rx->n; }
        if (nk_parse_config(rx->b, end, &out->u.config)) { out->kind = NK_CONFIG; }
        drop(rx, end);
        return end;
    }
    if (expect == NK_READY) {
        out->kind = NK_READY;
        out->u.ready = rx->b[0];
        size_t k = 1;
        if (rx->n >= 3 && rx->b[1] == '\r' && rx->b[2] == '\n') { k = 3; }
        drop(rx, k);
        return k;
    }
    if (rx->n >= 16 || (idle && rx->n >= 14)) {
        size_t k = rx->n >= 16 ? 16 : rx->n;
        nk_status_t st;
        nk_parse_status(rx->b, k, &st);
        if (expect == NK_STATUS || st.crc_ok) {
            out->kind = NK_STATUS;
            out->u.status = st;
            if (rx->n >= k + 2 && rx->b[k] == '\r' && rx->b[k + 1] == '\n') { k += 2; }
            drop(rx, k);
            return k;
        }
    }
    if (idle) {
        size_t k = rx->n;
        drop(rx, k);
        return k;
    }
    return 0;
}

const char *nk_status_name(int s) {
    switch (s) {
    case 0x00: return "ready";
    case 0x01: return "lid_open";
    case 0x02: return "feeding";
    case 0x03: return "paper_error";
    case 0x04: return "no_paper";
    case 0x10: return "paused";
    case 0x20: return "busy";
    case 0x24: return "out_of_paper";
    case 0x30: return "low_battery";
    case 0x80: return "overheated";
    case 0x90: return "no_power";
    }
    return "unknown";
}

int nk_status_error(int s) { return s != 0x00 && s != 0x02 && s != 0x20; }
