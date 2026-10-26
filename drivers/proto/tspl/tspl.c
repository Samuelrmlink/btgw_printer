#include "proto/tspl/tspl.h"
#include "core/prn_core.h"

#include <string.h>

typedef struct { char *p; size_t cap, n; int bad; } buf_t;

static void put(buf_t *b, const char *s, size_t n) {
    if (b->n + n >= b->cap) { b->bad = 1; return; }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void puts_(buf_t *b, const char *s) { put(b, s, strlen(s)); }
static void putd(buf_t *b, long v) {
    char t[16];
    int n = 0, neg = v < 0;
    unsigned long a = neg ? (unsigned long)-v : (unsigned long)v;
    char d[12];
    int k = 0;
    do { d[k++] = (char)('0' + a % 10u); a /= 10u; } while (a);
    if (neg) { t[n++] = '-'; }
    while (k) { t[n++] = d[--k]; }
    put(b, t, (size_t)n);
}
static void putmm(buf_t *b, uint32_t mm100) {
    char t[16];
    int n = prn_fmt_mm100(t, sizeof t, (int32_t)mm100);
    put(b, t, (size_t)n);
}

int tspl_head(char *out, size_t cap, const tspl_job_t *j) {
    buf_t b = { out, cap, 0, 0 };
    if (!out || !cap || !j) { return -1; }
    out[0] = 0;
    if (j->prefix) { puts_(&b, j->prefix); }
    uint32_t h = j->height_mm100;
    if (!h) {
        /* continuous media: the label is as long as the bitmap */
        uint32_t dpi = j->dpi ? j->dpi : 203u;
        h = (uint32_t)(((uint64_t)j->rows * 2540u + dpi / 2u) / dpi);
    }
    puts_(&b, "SIZE ");
    putmm(&b, j->width_mm100);
    puts_(&b, " mm,");
    putmm(&b, h);
    puts_(&b, " mm\r\n");
    if (j->media == PRN_MEDIA_BLACKMARK) {
        puts_(&b, "BLINE ");
        putmm(&b, j->gap_mm100);
        puts_(&b, " mm,0 mm\r\n");
    } else {
        puts_(&b, "GAP ");
        putmm(&b, j->media == PRN_MEDIA_CONTINUOUS ? 0 : j->gap_mm100);
        puts_(&b, " mm,0 mm\r\n");
    }
    puts_(&b, "DIRECTION ");
    putd(&b, j->direction);
    puts_(&b, ",0\r\n");
    if (j->density >= 0) { puts_(&b, "DENSITY "); putd(&b, j->density); puts_(&b, "\r\n"); }
    if (j->speed >= 0) { puts_(&b, "SPEED "); putd(&b, j->speed); puts_(&b, "\r\n"); }
    puts_(&b, "CLS\r\n");
    puts_(&b, "BITMAP ");
    putd(&b, j->x);
    puts_(&b, ",");
    putd(&b, j->y);
    puts_(&b, ",");
    putd(&b, j->row_bytes);
    puts_(&b, ",");
    putd(&b, (long)j->rows);
    puts_(&b, ",1,");
    return b.bad ? -1 : (int)b.n;
}

int tspl_tail(char *out, size_t cap, const tspl_job_t *j) {
    buf_t b = { out, cap, 0, 0 };
    if (!out || !cap || !j) { return -1; }
    out[0] = 0;
    puts_(&b, "\r\nPRINT ");
    putd(&b, j->copies > 0 ? j->copies : 1);
    puts_(&b, "\r\n");
    return b.bad ? -1 : (int)b.n;
}
