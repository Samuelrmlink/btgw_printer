/* Nelko P21 job bytes and reply parsers. Job layout and parsers adapted from
 * julienvincent/nelko-p21-rs (MIT, see THIRD_PARTY.md); status values from
 * docs/protocols/nelko-p21.md. */
#include "p21_proto.h"
#include <string.h>

/* No snprintf: the module build has no libc printf, and the job head is
 * fixed text with a few numbers. */
typedef struct { char *p; size_t n, cap; int bad; } sb_t;

static void put(sb_t *b, const char *s) {
    while (*s) {
        if (b->n + 1 >= b->cap) { b->bad = 1; return; }
        b->p[b->n++] = *s++;
    }
    b->p[b->n] = 0;
}

static void put_u(sb_t *b, uint32_t v) {
    char t[12];
    int i = 0;
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 11);
    char s[12];
    for (int k = 0; k < i; k++) { s[k] = t[i - 1 - k]; }
    s[i] = 0;
    put(b, s);
}

/* tenths of a mm as "14.0" */
static void put_mm10(sb_t *b, uint32_t v) {
    put_u(b, v / 10);
    char s[3] = {'.', (char)('0' + v % 10), 0};
    put(b, s);
}

int p21_job_head(const p21_job_t *j, char *out, size_t cap) {
    sb_t b = {out, 0, cap, cap == 0};
    if (cap) { out[0] = 0; }
    int d = j->density;
    if (d < 1) { d = 1; }
    if (d > 15) { d = 15; }
    put(&b, "SIZE ");
    put_mm10(&b, j->width_mm10);
    put(&b, " mm,");
    put_mm10(&b, j->height_mm10);
    put(&b, " mm\r\nGAP ");
    put_mm10(&b, j->gap_mm10);
    put(&b, " mm,0 mm\r\nDIRECTION 1,1\r\nDENSITY ");
    put_u(&b, (uint32_t)d);
    put(&b, "\r\nCLS\r\nBITMAP 0,0,");
    put_u(&b, P21_ROW_BYTES);
    put(&b, ",");
    put_u(&b, j->rows);
    put(&b, ",1,");
    return b.bad ? -1 : (int)b.n;
}

int p21_job_tail(char *out, size_t cap) {
    sb_t b = {out, 0, cap, cap == 0};
    put(&b, "\r\nPRINT 1\r\n");
    return b.bad ? -1 : (int)b.n;
}

uint16_t p21_crc16(const uint8_t *d, size_t n) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xa001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

int p21_parse_label(const uint8_t *d, size_t n, p21_label_t *o) {
    if (n != 16) { return 0; }
    uint16_t c = p21_crc16(d, 14);
    if (d[14] != (uint8_t)(c >> 8) || d[15] != (uint8_t)c) { return 0; }
    memset(o, 0, sizeof *o);
    o->status = d[0];
    o->color = d[4];
    o->paper_type = d[7];
    o->gap_mm = d[8];
    o->length_mm = d[11];
    o->usable_mm = d[12];
    o->width_mm = d[13];
    memcpy(o->code, d + 2, 12);
    for (int i = 0; i < 12; i++) {
        if (o->code[i]) { o->tag_present = 1; }
    }
    return 1;
}

static const uint8_t *prefixed(const uint8_t *d, size_t n, const char *pre, size_t want, size_t want2) {
    size_t pl = strlen(pre);
    if (n < pl + 2 || memcmp(d, pre, pl) != 0) { return 0; }
    if (d[n - 2] != '\r' || d[n - 1] != '\n') { return 0; }
    size_t m = n - pl - 2;
    if (m != want && m != want2) { return 0; }
    return d + pl;
}

int p21_parse_battery(const uint8_t *d, size_t n, p21_battery_t *o) {
    const uint8_t *p = prefixed(d, n, "BATTERY ", 2, 2);
    if (!p) { return 0; }
    o->percent = (uint8_t)(((p[0] >> 4) & 0x0f) * 10 + (p[0] & 0x0f));
    o->charging = p[1] != 0;
    return 1;
}

int p21_parse_config(const uint8_t *d, size_t n, p21_config_t *o) {
    const uint8_t *p = prefixed(d, n, "CONFIG ", 10, 20);
    if (!p) { return 0; }
    o->dpi = (uint16_t)((p[0] << 8) | p[1]);
    memcpy(o->hw, p + 2, 3);
    memcpy(o->fw, p + 5, 3);
    o->timer = p[8];
    o->beep = p[9];
    return 1;
}

const char *p21_status_name(uint8_t s) {
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
    default: return "unknown";
    }
}

int p21_status_error(uint8_t s) {
    return s != 0x00 && s != 0x02 && s != 0x20;
}

const char *p21_paper_name(uint8_t t) {
    switch (t) {
    case 0: return "continuous";
    case 1: return "gap";
    case 2: return "blackmark";
    default: return "unknown";
    }
}

const char *p21_color_name(uint8_t c) {
    switch (c) {
    case 2: return "transparent";
    case 3: return "white";
    case 4: return "pink";
    case 5: return "blue";
    case 6: return "yellow";
    default: return "unknown";
    }
}
