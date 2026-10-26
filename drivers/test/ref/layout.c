#include "ref/layout.h"

#include <string.h>

int ref_mm100_to_dots(int32_t mm100, int dpi) {
    /* the old (int)(mm * dpi / 25.4f + 0.5f) in float, so rounding matches */
    float mm = (float)mm100 / 100.0f;
    return (int)(mm * (float)dpi / 25.4f + (mm >= 0 ? 0.5f : -0.5f));
}

void ref_layout(const ref_cal_t *c, int src_w, int src_h, ref_layout_t *L) {
    int page_w = ref_mm100_to_dots(c->page_w_mm100, c->dpi);
    int page_h = ref_mm100_to_dots(c->page_h_mm100, c->dpi);
    int ox = (page_w - src_w) / 2, oy = (page_h - src_h) / 2;
    if (ox < 0) { ox = 0; }
    if (oy < 0) { oy = 0; }
    int dx = ref_mm100_to_dots(c->offset_x_mm100, c->dpi);
    int dy = ref_mm100_to_dots(c->offset_y_mm100, c->dpi);
    int sx = 0, sy = 0;
    if (ox + dx < 0) { sx = ox + dx; ox = 0; } else { ox += dx; }
    if (oy + dy < 0) { sy = oy + dy; oy = 0; } else { oy += dy; }
    ox += c->origin_x_adj;
    oy += c->origin_y_adj;
    if (ox < 0) { sx += ox; ox = 0; }
    if (oy < 0) { sy += oy; oy = 0; }
    if (ox > page_w - 1) { ox = page_w - 1; }
    if (oy > page_h - 1) { oy = page_h - 1; }
    int aw = page_w - ox, ah = page_h - oy;
    if (aw < 1) { aw = 1; }
    if (ah < 1) { ah = 1; }
    L->origin_x = ox;
    L->origin_y = oy;
    L->print_width_dots = src_w < aw ? src_w : aw;
    L->print_height_dots = (src_h < ah ? src_h : ah) + c->print_height_adj;
    if (L->print_height_dots < 1) { L->print_height_dots = 1; }
    if (L->print_height_dots > ah) { L->print_height_dots = ah; }
    L->shift_x = sx;
    L->shift_y = sy;
}

static int get(const uint8_t *fb, int wb, int w, int h, int x, int y) {
    if (x < 0 || y < 0 || x >= w || y >= h) { return 0; }
    return (fb[y * wb + x / 8] & (1u << (7 - x % 8))) == 0;
}
static void set(uint8_t *fb, int wb, int w, int h, int x, int y, int black) {
    if (x < 0 || y < 0 || x >= w || y >= h) { return; }
    uint8_t *b = &fb[y * wb + x / 8];
    if (black) { *b &= (uint8_t)~(1u << (7 - x % 8)); } else { *b |= (uint8_t)(1u << (7 - x % 8)); }
}

size_t ref_crop(const ref_layout_t *L, const uint8_t *src, int swb, int sw, int sh, uint8_t *out, size_t cap) {
    int w = L->print_width_dots, h = L->print_height_dots, wb = (w + 7) / 8;
    size_t n = (size_t)wb * (size_t)h;
    if (n > cap) { return 0; }
    memset(out, 0xFF, n);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (get(src, swb, sw, sh, x - L->shift_x, y - L->shift_y)) { set(out, wb, w, h, x, y, 1); }
        }
    }
    int all = 1;
    for (size_t i = 0; i < n; i++) { if (out[i]) { all = 0; break; } }
    if (all) { for (int y = 0; y < h; y += 16) { set(out, wb, w, h, 0, y, 0); } }
    return n;
}

void ref_test_frame(uint8_t *fb, int wb, int w, int h, int dpi, int sx0, int sy0, int sx1, int sy1) {
    const int th = 3;
    int ix = ref_mm100_to_dots(200, dpi), iy0 = ref_mm100_to_dots(200, dpi), iy1 = ref_mm100_to_dots(500, dpi);
    if (ix < th + 1) { ix = th + 1; }
    if (iy0 < th + 1) { iy0 = th + 1; }
    if (iy1 < th + 1) { iy1 = th + 1; }
    int x0 = ix, y0 = iy0, x1 = w - 1 - ix, y1 = h - 1 - iy1;
    if (x1 <= x0 || y1 <= y0) { return; }
    for (int t = 0; t < th; t++) {
        for (int x = x0; x <= x1; x++) { set(fb, wb, w, h, x, y0 + t, 1); set(fb, wb, w, h, x, y1 - t, 1); }
        for (int y = y0; y <= y1; y++) { set(fb, wb, w, h, x0 + t, y, 1); set(fb, wb, w, h, x1 - t, y, 1); }
    }
    int cx = w / 2, cy = h / 2;
    int arm = ((x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0)) / 4;
    for (int i = -arm; i <= arm; i++) {
        set(fb, wb, w, h, cx + i, cy, 1);
        set(fb, wb, w, h, cx + i, cy + 1, 1);
        set(fb, wb, w, h, cx, cy + i, 1);
        set(fb, wb, w, h, cx + 1, cy + i, 1);
    }
    for (int t = 0; t < 2; t++) {
        int a0 = sx0 + t, b0 = sy0 + t, a1 = sx1 - t, b1 = sy1 - t;
        if (a1 <= a0 || b1 <= b0) { break; }
        for (int x = a0; x <= a1; x++) {
            if ((x & 7) < 4) { set(fb, wb, w, h, x, b0, 1); set(fb, wb, w, h, x, b1, 1); }
        }
        for (int y = b0; y <= b1; y++) {
            if ((y & 7) < 4) { set(fb, wb, w, h, a0, y, 1); set(fb, wb, w, h, a1, y, 1); }
        }
    }
}
