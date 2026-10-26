/* TSPL output against the old PM220 firmware, byte for byte.
 *
 * Old: pm220_rp2_webapp's job_from_bitmap() and job_test_frame(), compiled
 * from ~/Projects/pm220-pico2w with the calibration file it reads served by
 * the stubs below. New: the reference layout (ref/layout.c, what the web app
 * does), the hub's all-black punch (prn_core) and tspl_head/rows/tail. */
#include "t.h"
#include "core/prn_core.h"
#include "proto/tspl/tspl.h"
#include "ref/layout.h"

#include <stdbool.h>
#include <stdint.h>

/* ---- the old code ---- */




#include "ref/old_fs_stub.h"
#define s_cal old_cal


/* ---- the new path ---- */
typedef struct {
    const char *json;
    ref_cal_t cal;
    int32_t gap_mm100;
    int sx0, sy0, sx1, sy1;
} cal_case_t;

static size_t new_job(const cal_case_t *cc, const uint8_t *src, int sw, int sh, uint8_t *out, size_t cap,
                      ref_layout_t *Lout) {
    static uint8_t crop[48 * 240];
    ref_layout_t L;
    ref_layout(&cc->cal, sw, sh, &L);
    size_t n = ref_crop(&L, src, (sw + 7) / 8, sw, sh, crop, sizeof crop);
    if (!n) { return 0; }
    tspl_job_t j = { "\x1b!o\r\n", 5000, 3000, (uint32_t)cc->gap_mm100, PRN_MEDIA_GAP, 203, 8, -1, 0,
                     L.origin_x, L.origin_y, (uint16_t)((L.print_width_dots + 7) / 8),
                     (uint32_t)L.print_height_dots, 1 };
    int h = tspl_head((char *)out, cap, &j);
    if (h < 0) { return 0; }
    memcpy(out + h, crop, n);
    int t = tspl_tail((char *)out + h + n, cap - (size_t)h - n, &j);
    if (Lout) { *Lout = L; }
    return (size_t)h + n + (size_t)t;
}

/* ---- source bitmaps ---- */
static uint32_t s_seed = 1;
static uint32_t rnd(void) { s_seed = s_seed * 1103515245u + 12345u; return s_seed >> 16; }
enum { P_WHITE, P_BLACK, P_RANDOM, P_CHECK, P_EDGES };
static void pattern(uint8_t *fb, int w, int h, int kind) {
    int wb = (w + 7) / 8;
    memset(fb, 0xFF, (size_t)wb * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int black = 0;
            if (kind == P_BLACK) { black = 1; }
            if (kind == P_RANDOM) { black = (rnd() & 3) == 0; }
            if (kind == P_CHECK) { black = ((x / 8) + (y / 8)) & 1; }
            if (kind == P_EDGES) { black = x < 2 || y < 2 || x >= w - 2 || y >= h - 2; }
            if (black) { fb[y * wb + x / 8] &= (uint8_t)~(1u << (7 - x % 8)); }
        }
    }
}

#define CAL(json, ox, oy, xa, ya, ha, gap, a, b, c, d) \
    { json, { 203, 5000, 3000, ox, oy, xa, ya, ha }, gap, a, b, c, d }
static const cal_case_t CASES[] = {
    CAL(NULL, -125, 300, 0, 0, 0, 200, 32, 32, 351, 207),
    CAL("{\"offset_x_mm\":0,\"offset_y_mm\":0,\"gap_mm\":2}", 0, 0, 0, 0, 0, 200, 32, 32, 351, 207),
    CAL("{\"offset_x_mm\":-1.25,\"offset_y_mm\":3,\"gap_mm\":2.5,\"origin_x_adj\":4,\"origin_y_adj\":-3,"
        "\"print_height_adj\":-8}", -125, 300, 4, -3, -8, 250, 32, 32, 351, 207),
    CAL("{\"offset_x_mm\":-3.5,\"offset_y_mm\":-2.25,\"gap_mm\":3,\"origin_x_adj\":-20,\"origin_y_adj\":-30}",
        -350, -225, -20, -30, 0, 300, 32, 32, 351, 207),
    CAL("{\"offset_x_mm\":6,\"offset_y_mm\":9.75,\"gap_mm\":0,\"print_height_adj\":40,\"origin_x_adj\":80,"
        "\"origin_y_adj\":80}", 600, 975, 80, 80, 40, 0, 32, 32, 351, 207),
    CAL("{\"offset_x_mm\":1.1,\"offset_y_mm\":0.3,\"gap_mm\":1.75,\"safe_x0\":10,\"safe_y0\":12,"
        "\"safe_x1\":300,\"safe_y1\":200}", 110, 30, 0, 0, 0, 175, 10, 12, 300, 200),
};
#define NCASES (int)(sizeof CASES / sizeof CASES[0])

static uint8_t s_src[48 * 240], s_old[16384], s_new[16384];

static void t_jobs_match_old(void) {
    for (int c = 0; c < NCASES; c++) {
        for (int k = P_WHITE; k <= P_EDGES; k++) {
            s_cal = CASES[c].json;
            pattern(s_src, 384, 240, k);
            size_t on = 0;
            CHECK(old_job_from_bitmap(s_src, 48, 240, s_old, sizeof s_old, &on, NULL));
            size_t nn = new_job(&CASES[c], s_src, 384, 240, s_new, sizeof s_new, NULL);
            CHECK_EQ(nn, on);
            if (nn == on && memcmp(s_old, s_new, on) != 0) {
                size_t i = 0;
                while (s_old[i] == s_new[i]) { i++; }
                fprintf(stderr, "case %d pattern %d differs at %zu: old %02x new %02x\n", c, k, i, s_old[i], s_new[i]);
                CHECK(0);
            }
        }
    }
}

static void t_test_frame_matches_old(void) {
    static uint8_t fb[48 * 240];
    for (int c = 0; c < NCASES; c++) {
        s_cal = CASES[c].json;
        size_t on = 0;
        CHECK(old_job_test_frame(s_old, sizeof s_old, &on));
        memset(fb, 0xFF, sizeof fb);
        ref_test_frame(fb, 48, 384, 240, 203, CASES[c].sx0, CASES[c].sy0, CASES[c].sx1, CASES[c].sy1);
        size_t nn = new_job(&CASES[c], fb, 384, 240, s_new, sizeof s_new, NULL);
        CHECK_EQ(nn, on);
        CHECK(nn == on && memcmp(s_old, s_new, on) == 0);
    }
}

/* Smaller source bitmaps take the old path too (it centres them). */
static void t_small_sources(void) {
    static const int sizes[][2] = { { 16, 8 }, { 200, 100 }, { 384, 120 }, { 96, 240 } };
    for (int c = 0; c < NCASES; c++) {
        for (int s = 0; s < 4; s++) {
            int w = sizes[s][0], h = sizes[s][1];
            s_cal = CASES[c].json;
            pattern(s_src, w, h, P_RANDOM);
            size_t on = 0;
            CHECK(old_job_from_bitmap(s_src, w / 8, h, s_old, sizeof s_old, &on, NULL));
            size_t nn = new_job(&CASES[c], s_src, w, h, s_new, sizeof s_new, NULL);
            CHECK_EQ(nn, on);
            CHECK(nn == on && memcmp(s_old, s_new, on) == 0);
        }
    }
}

/* Settings the old firmware never sent: our output for them, pinned. */
static void t_other_settings(void) {
    char b[256];
    tspl_job_t j = { NULL, 10160, 15240, 300, PRN_MEDIA_GAP, 203, 12, 4, 0, 0, 0, 104, 1218, 2 };
    tspl_head(b, sizeof b, &j);
    CHECK_STR(b, "SIZE 101.6 mm,152.4 mm\r\nGAP 3 mm,0 mm\r\nDIRECTION 0,0\r\nDENSITY 12\r\nSPEED 4\r\nCLS\r\n"
                 "BITMAP 0,0,104,1218,1,");
    tspl_tail(b, sizeof b, &j);
    CHECK_STR(b, "\r\nPRINT 2\r\n");
    j = (tspl_job_t){ NULL, 4800, 0, 0, PRN_MEDIA_CONTINUOUS, 203, -1, -1, 1, 8, 0, 48, 400, 1 };
    tspl_head(b, sizeof b, &j);
    CHECK_STR(b, "SIZE 48 mm,50.05 mm\r\nGAP 0 mm,0 mm\r\nDIRECTION 1,0\r\nCLS\r\nBITMAP 8,0,48,400,1,");
    j = (tspl_job_t){ "\x1b!o\r\n", 4000, 3000, 250, PRN_MEDIA_BLACKMARK, 203, 0, -1, 0, 0, 0, 40, 240, 1 };
    tspl_head(b, sizeof b, &j);
    CHECK_STR(b, "\x1b!o\r\nSIZE 40 mm,30 mm\r\nBLINE 2.5 mm,0 mm\r\nDIRECTION 0,0\r\nDENSITY 0\r\nCLS\r\n"
                 "BITMAP 0,0,40,240,1,");
    CHECK_EQ(tspl_head(b, 20, &j), -1);
    char m[16];
    prn_fmt_mm100(m, sizeof m, -125);
    CHECK_STR(m, "-1.25");
    prn_fmt_mm100(m, sizeof m, 10);
    CHECK_STR(m, "0.1");
    prn_fmt_mm100(m, sizeof m, 0);
    CHECK_STR(m, "0");
}

static void t_punch_matches_old_protection(void) {
    static uint8_t rows[48 * 240];
    memset(rows, 0x00, sizeof rows);
    CHECK(prn_rows_all_black(rows, sizeof rows, 0));
    /* the hub punches per chunk: two chunks must equal one pass */
    prn_punch_rows(rows, 0, 100, 48, 0);
    prn_punch_rows(rows + 100 * 48, 100, 140, 48, 0);
    static uint8_t fb[48 * 240], ref[48 * 240];
    memset(fb, 0x00, sizeof fb);
    ref_layout_t L = { 0, 0, 384, 240, 0, 0 };
    ref_crop(&L, fb, 48, 384, 240, ref, sizeof ref);
    CHECK(memcmp(rows, ref, sizeof rows) == 0);
}

/* golden/layout_vectors.json: the layout numbers for the web app's port. */
static void t_write_vectors(void) {
    const char *out = getenv("GOLDEN_OUT");
    if (!out) { return; }
    FILE *f = fopen(out, "w");
    CHECK(f != NULL);
    if (!f) { return; }
    fprintf(f, "{\"about\":\"ref_layout() results (drivers/test/ref/layout.c), checked against the old PM220 "
               "firmware's job_compute_layout. Units: mm100 = hundredths of a mm, dots.\",\n\"cases\":[\n");
    int first = 1;
    static const int pages[][2] = { { 5000, 3000 }, { 10160, 15240 }, { 7620, 2540 }, { 1400, 4000 } };
    static const int srcs[][2] = { { 384, 240 }, { 832, 1218 }, { 600, 200 }, { 96, 320 }, { 16, 8 } };
    for (int p = 0; p < 4; p++) {
        for (int c = 0; c < NCASES; c++) {
            for (int s = 0; s < 5; s++) {
                ref_cal_t cal = CASES[c].cal;
                cal.page_w_mm100 = pages[p][0];
                cal.page_h_mm100 = pages[p][1];
                ref_layout_t L;
                ref_layout(&cal, srcs[s][0], srcs[s][1], &L);
                fprintf(f, "%s{\"dpi\":%d,\"page_w_mm100\":%d,\"page_h_mm100\":%d,\"offset_x_mm100\":%d,"
                           "\"offset_y_mm100\":%d,\"origin_x_adj\":%d,\"origin_y_adj\":%d,\"print_height_adj\":%d,"
                           "\"src_w\":%d,\"src_h\":%d,\"out\":{\"origin_x\":%d,\"origin_y\":%d,"
                           "\"print_width_dots\":%d,\"print_height_dots\":%d,\"shift_x\":%d,\"shift_y\":%d}}",
                        first ? "" : ",\n", cal.dpi, cal.page_w_mm100, cal.page_h_mm100, cal.offset_x_mm100,
                        cal.offset_y_mm100, cal.origin_x_adj, cal.origin_y_adj, cal.print_height_adj, srcs[s][0],
                        srcs[s][1], L.origin_x, L.origin_y, L.print_width_dots, L.print_height_dots, L.shift_x,
                        L.shift_y);
                first = 0;
            }
        }
    }
    fprintf(f, "\n]}\n");
    fclose(f);
}

int main(void) {
    RUN(t_jobs_match_old);
    RUN(t_test_frame_matches_old);
    RUN(t_small_sources);
    RUN(t_other_settings);
    RUN(t_punch_matches_old_protection);
    RUN(t_write_vectors);
    T_DONE();
}
