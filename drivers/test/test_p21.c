/* The P21 model of nelko_tspl on the fake host against a simulated P21:
 * inquiry by name, SSP and PIN pairing, the status queries, jobs byte-for-byte
 * against nelko-p21-rs, copies, stalls, link loss and reconnect, a printer
 * error before and after the job, and stop. Also the P21 reply vectors and
 * job text (once test_p21_proto.c). */
#include "t.h"
#include "fakehost.h"
#include "fh_classic.h"
#include "t_hubstub.h"
#include "vendors/nelko/tspl/nelko_proto.h"
#include "proto/tspl/tspl.h"

int nelko__pex_main(const pex_host_t *host, uint32_t reason, void *arg);
int nelko__prn_drv(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2);

#define ADDR "C0:01:02:03:04:21"

/* ---- the simulated printer ---- */
static struct {
    uint8_t in[65536];          /* every byte the host sent */
    size_t n, parsed;
    size_t bitmap_left, bitmap_start;
    int bitmaps, prints, queries_label, queries_status, queries_battery, queries_config;
    uint32_t last_rows;
    uint8_t status;             /* what ESC ! ? answers */
    int status_after_print;     /* > 0: the status becomes this once PRINT came */
    size_t bitmap_at[8];
    uint32_t bitmap_rows[8];
} PR;

static const uint8_t LABEL_14x40[16] = {0x00, 0x0c, 0x01, 0x12, 0x03, 0x00, 0x03, 0x01,
                                        0x12, 0x12, 0x15, 0x28, 0x0f, 0x0e, 0xed, 0x03};

static void pr_parse(fh_cl_peer_t *p) {
    while (PR.parsed < PR.n) {
        if (PR.bitmap_left) {
            size_t k = PR.n - PR.parsed < PR.bitmap_left ? PR.n - PR.parsed : PR.bitmap_left;
            PR.parsed += k;
            PR.bitmap_left -= k;
            continue;
        }
        const uint8_t *s = PR.in + PR.parsed;
        size_t left = PR.n - PR.parsed;
        if (s[0] == 0x1b) {
            if (left < 3) { return; }
            if (s[1] == '!' && s[2] == 'o') {
                PR.queries_label++;
                uint8_t blk[16];
                memcpy(blk, LABEL_14x40, 16);
                blk[0] = PR.status;
                uint16_t c = nk_crc16_modbus(blk, 14);
                blk[14] = (uint8_t)(c >> 8);
                blk[15] = (uint8_t)c;
                fh_cl_peer_send(p, blk, 16);
            } else if (s[1] == '!' && s[2] == '?') {
                PR.queries_status++;
                fh_cl_peer_send(p, &PR.status, 1);
            }
            PR.parsed += 3;
            continue;
        }
        /* a text line */
        const uint8_t *nl = memchr(s, '\n', left);
        const char *bm = "BITMAP ";
        if (left >= 7 && memcmp(s, bm, 7) == 0) {
            /* BITMAP x,y,wb,h,mode, then binary */
            int commas = 0;
            size_t i = 7;
            for (; i < left && commas < 5; i++) { if (s[i] == ',') { commas++; } }
            if (commas < 5) { return; }
            unsigned x, y, wb, h, mode;
            if (sscanf((const char *)s + 7, "%u,%u,%u,%u,%u,", &x, &y, &wb, &h, &mode) != 5) { return; }
            if (PR.bitmaps < 8) { PR.bitmap_at[PR.bitmaps] = PR.parsed + i; PR.bitmap_rows[PR.bitmaps] = h; }
            PR.bitmaps++;
            PR.last_rows = h;
            PR.bitmap_left = (size_t)wb * h;
            PR.parsed += i;
            continue;
        }
        if (!nl) { return; }
        size_t len = (size_t)(nl - s) + 1;
        if (len >= 10 && memcmp(s, "BATTERY?\r\n", 10) == 0) {
            PR.queries_battery++;
            const uint8_t r[] = {'B', 'A', 'T', 'T', 'E', 'R', 'Y', ' ', 0x76, 0x00, '\r', '\n'};
            fh_cl_peer_send(p, r, sizeof r);
        } else if (len >= 9 && memcmp(s, "CONFIG?\r\n", 9) == 0) {
            PR.queries_config++;
            const uint8_t r[] = {'C', 'O', 'N', 'F', 'I', 'G', ' ', 0x00, 0xcb, 0, 0, 3, 4, 2, 4, 2, 1, '\r', '\n'};
            fh_cl_peer_send(p, r, sizeof r);
        } else if (len >= 7 && memcmp(s, "PRINT ", 6) == 0) {
            PR.prints++;
            if (PR.status_after_print > 0) { PR.status = (uint8_t)PR.status_after_print; }
        }
        PR.parsed += len;
    }
}

static void pr_rx(fh_cl_peer_t *p, const uint8_t *d, size_t n, void *ctx) {
    (void)ctx;
    if (PR.n + n <= sizeof PR.in) { memcpy(PR.in + PR.n, d, n); PR.n += n; }
    pr_parse(p);
}

static fh_cl_peer_t *s_peer;
static pex_mod_t *s_drv;

static void setup(const char *pin) {
    fh_reset();
    memset(&PR, 0, sizeof PR);
    fh_verbose(getenv("FH_V") != NULL);
    hs_reset();
    fh_cl_peer_cfg_t c = {.addr = ADDR, .name = "P21", .cod = 0x000680, .rssi = -60,
                          .sdp_channel = 1, .pin = pin, .on_rx = pr_rx};
    s_peer = fh_cl_peer_add(&c);
    s_drv = fh_module("nelko_tspl", nelko__pex_main);
    fh_export(s_drv, PRN_DRV_SYM, (void *)nelko__prn_drv);
    CHECK_EQ(fh_load(s_drv), PEX_OK);
    CHECK_EQ(fh_start(s_drv, "{}"), PEX_OK);
    fh_run(50);
}

static void connect_ok(void) {
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_CONNECT, (uint32_t)(uintptr_t)ADDR, (uint32_t)(uintptr_t)"{\"model\":\"p21\"}", 0),
             PEX_EPENDING);
    CHECK(fh_run_until(hs_state_is, "connected", 3000));
    fh_run(200);
}

static uint8_t s_bm[12 * 600];
static void pattern(uint32_t rows) {
    for (uint32_t y = 0; y < rows; y++) {
        for (uint32_t b = 0; b < 12; b++) { s_bm[y * 12 + b] = (uint8_t)(0xff ^ (y * 7 + b * 13)); }
    }
}

static prn_job_t job(uint32_t id, uint32_t rows, int density, int copies) {
    prn_job_t j;
    memset(&j, 0, sizeof j);
    j.id = id;
    snprintf(j.addr, sizeof j.addr, "%s", ADDR);
    snprintf(j.model, sizeof j.model, "p21");
    j.width_dots = 96;
    j.row_bytes = 12;
    j.rows_total = rows;
    j.width_mm100 = 1400;
    j.height_mm100 = 4000;
    j.gap_mm100 = 500;
    j.media_type = PRN_MEDIA_GAP;
    j.density = (int8_t)density;
    j.speed = -1;
    j.copies = (uint8_t)copies;
    snprintf(j.options, sizeof j.options, "{}");
    return j;
}

static void t_register_scan(void) {
    setup(NULL);
    CHECK_STR(HS.registered, "nelko_tspl");
    char caps[2048];
    CHECK(hs_drv(s_drv, PRN_DRV_INFO, (uint32_t)(uintptr_t)caps, sizeof caps, 0) > 0);
    CHECK_HAS(caps, "\"head_dots\":96");
    CHECK_HAS(caps, "\"black_bit\":0");
    CHECK_HAS(caps, "\"placement\":false");
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_SCAN, 3000, 0, 0), PEX_OK);
    fh_run(3600);
    CHECK_EQ(hs_count("found", ADDR), 1);
    char dev[512];
    CHECK(hs_drv(s_drv, PRN_DRV_DEVICES, (uint32_t)(uintptr_t)dev, sizeof dev, 0) > 0);
    CHECK_HAS(dev, ADDR);
    CHECK_HAS(dev, "\"name\":\"P21\"");
    CHECK(dev[0] == '[');
    CHECK_HAS(dev, "\"driver\":\"nelko_tspl\"");
    CHECK_HAS(dev, "\"model\":\"p21\"");
    CHECK_HAS(hs_last("found"), "\"model\":\"p21\"");
    /* connecting with no model in the config takes it from the name */
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_CONNECT, (uint32_t)(uintptr_t)ADDR, (uint32_t)(uintptr_t)"{}", 0), PEX_EPENDING);
    CHECK(fh_run_until(hs_state_is, "connected", 3000));
    CHECK_HAS(hs_last("state"), "\"model\":\"p21\"");
    CHECK_EQ(hs_count("log", NULL), 0);
}

static void t_connect_status(void) {
    setup(NULL);
    connect_ok();
    CHECK_EQ(PR.queries_config, 1);
    CHECK_EQ(PR.queries_battery, 1);
    CHECK_EQ(PR.queries_label, 1);
    char st[640];
    CHECK(hs_drv(s_drv, PRN_DRV_STATUS, 0, (uint32_t)(uintptr_t)st, sizeof st) > 0);
    CHECK(st[0] == '[');
    CHECK_HAS(st, "\"driver\":\"nelko_tspl\"");
    CHECK_HAS(st, "\"model\":\"p21\"");
    CHECK_HAS(st, "\"battery\":76");
    CHECK_HAS(st, "\"printer\":\"ready\"");
    CHECK_HAS(st, "\"w_mm\":14");
    CHECK_HAS(st, "\"h_mm\":40");
    CHECK_HAS(st, "\"type\":\"gap\"");
    CHECK_HAS(st, "\"fw\":\"4.2.4\"");
    CHECK_HAS(st, "\"paper\":true");
    CHECK_HAS(st, "\"lid\":false");
    /* keepalive asks battery and the label again */
    fh_run(25100);
    fh_run(300);
    CHECK_EQ(PR.queries_battery, 2);
}

static void t_pin_legacy(void) {
    /* a printer that wants 1234: 0000 fails twice (the host's own retry), then the driver
     * opens again from the next PIN */
    setup("1234");
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_CONNECT, (uint32_t)(uintptr_t)ADDR, 0, 0), PEX_EPENDING);
    CHECK(fh_run_until(hs_state_is, "connected", 5000));
    /* no config, no scan: the first model, and a printer.log line says so */
    CHECK_HAS(hs_last("log"), "using pm220");
    CHECK_HAS(hs_last("log"), "\"driver\":\"nelko_tspl\"");
    const fh_cl_stats_t *s = fh_cl_peer_stats(s_peer);
    CHECK_STR(s->pins_tried[0], "0000");
    CHECK_STR(s->pins_tried[s->pin_requests - 1], "1234");
    CHECK(fh_cl_host_has_key(ADDR));
}

static uint8_t s_golden[8192];
static size_t load(const char *path, uint8_t *b, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) { return 0; }
    size_t n = fread(b, 1, cap, f);
    fclose(f);
    return n;
}

static void t_job_golden(void) {
    /* the golden job: nelko-p21-rs printing the same 96x284 bitmap at density 8 */
    setup(NULL);
    connect_ok();
    uint8_t bm[12 * 284];
    size_t bn = load(GOLDEN_DIR "/golden_p21_14x40.bitmap", bm, sizeof bm);
    size_t gn = load(GOLDEN_DIR "/golden_p21_14x40_d8.bin", s_golden, sizeof s_golden);
    CHECK_EQ(bn, sizeof bm);
    CHECK(gn > bn);
    size_t start = PR.n;
    prn_job_t j = job(7, 284, 8, 1);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_BEGIN, (uint32_t)(uintptr_t)&j, 0, 0), PEX_OK);
    CHECK_EQ(hs_feed_all(s_drv, bm, bn, 700), 0);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    CHECK(fh_run_until(hs_done, NULL, 5000));
    CHECK_EQ(HS.done_id, 7);
    CHECK_EQ(HS.done_outcome, PRN_JOB_OK);
    CHECK_EQ(PR.bitmaps, 1);
    CHECK_EQ(PR.prints, 1);
    /* first the readiness check, ESC ! ? with no CRLF */
    CHECK(memcmp(PR.in + start, "\x1b!?\x1b!o", 6) == 0);
    start += 3;
    /* what the printer got from the job's first byte to PRINT's CRLF; the
     * status query after it is separate */
    CHECK(PR.n - start >= gn);
    CHECK(memcmp(PR.in + start, s_golden, gn) == 0);
    CHECK_EQ(PR.in[start + gn], 0x1b);   /* then ESC ! ? */
    CHECK_EQ(PR.queries_status, 2);
}

static void t_copies_and_stall(void) {
    setup(NULL);
    connect_ok();
    pattern(320);
    prn_job_t j = job(9, 320, 12, 2);
    snprintf(j.options, sizeof j.options, "{\"preflight\":false}");
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_BEGIN, (uint32_t)(uintptr_t)&j, 0, 0), PEX_OK);
    fh_cl_peer_stall(s_peer, 1);
    /* the driver takes what fits, then says busy; nothing is lost */
    int busy = 0;
    size_t off = 0;
    for (int i = 0; i < 50 && off < 12 * 320; i++) {
        int r = hs_drv(s_drv, PRN_DRV_JOB_FEED, (uint32_t)(uintptr_t)(s_bm + off), 1200, 0);
        if (r == PEX_EBUSY) { busy++; fh_run(1); continue; }
        CHECK(r > 0 && r % 12 == 0);
        if (r > 0) { off += (size_t)r; }
    }
    CHECK(busy > 0);
    fh_cl_peer_stall(s_peer, 0);
    CHECK_EQ(hs_feed_all(s_drv, s_bm + off, 12 * 320 - off, 1200), 0);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    /* the hub feeds copy 2 without a new BEGIN */
    CHECK_EQ(hs_feed_all(s_drv, s_bm, 12 * 320, 1200), 0);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_END, 1, 0, 0), PEX_OK);
    CHECK(fh_run_until(hs_done, NULL, 5000));
    CHECK_EQ(HS.done_outcome, PRN_JOB_OK);
    CHECK_EQ(PR.bitmaps, 2);
    CHECK_EQ(PR.prints, 2);
    for (int k = 0; k < 2; k++) {
        CHECK_EQ(PR.bitmap_rows[k], 320);
        CHECK(memcmp(PR.in + PR.bitmap_at[k], s_bm, 12 * 320) == 0);
    }
    /* the head carried the job's settings */
    PR.in[PR.n < sizeof PR.in ? PR.n : sizeof PR.in - 1] = 0;
    CHECK(memmem(PR.in, PR.n, "DENSITY 12\r\n", 12) != NULL);
}

static void t_wrong_width(void) {
    setup(NULL);
    connect_ok();
    prn_job_t j = job(1, 100, 8, 1);
    j.width_dots = 384;
    j.row_bytes = 48;
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_BEGIN, (uint32_t)(uintptr_t)&j, 0, 0), PEX_EINVAL);
}

static void t_printer_error(void) {
    setup(NULL);
    connect_ok();
    pattern(100);
    /* no paper before the job: the readiness check refuses it */
    PR.status = 0x04;
    prn_job_t j = job(3, 100, 8, 1);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_BEGIN, (uint32_t)(uintptr_t)&j, 0, 0), PEX_OK);
    CHECK(fh_run_until(hs_done, NULL, 5000));
    CHECK_EQ(HS.done_outcome, PRN_JOB_FAILED);
    CHECK_STR(HS.done_msg, "out of paper");
    CHECK_EQ(PR.bitmaps, 0);
    /* the paper runs out during the job: the status after it fails the job */
    PR.status = 0;
    PR.status_after_print = 0x04;
    HS.ndone = 0;
    j = job(4, 100, 8, 1);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_BEGIN, (uint32_t)(uintptr_t)&j, 0, 0), PEX_OK);
    CHECK_EQ(hs_feed_all(s_drv, s_bm, 1200, 1200), 0);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    CHECK(fh_run_until(hs_done, NULL, 5000));
    CHECK_EQ(HS.done_id, 4);
    CHECK_EQ(HS.done_outcome, PRN_JOB_FAILED);
    CHECK_STR(HS.done_msg, "out of paper");
    CHECK_EQ(PR.bitmaps, 1);
    /* busy is fine before a job: it waits, then prints */
    PR.status_after_print = 0;
    PR.status = 0x20;
    HS.ndone = 0;
    j = job(5, 100, 8, 1);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_BEGIN, (uint32_t)(uintptr_t)&j, 0, 0), PEX_OK);
    fh_run(800);
    PR.status = 0;
    CHECK_EQ(hs_feed_all(s_drv, s_bm, 1200, 1200), 0);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    CHECK(fh_run_until(hs_done, NULL, 5000));
    CHECK_EQ(HS.done_outcome, PRN_JOB_OK);
    CHECK_EQ(PR.bitmaps, 2);
}

static void t_link_loss(void) {
    setup(NULL);
    connect_ok();
    pattern(400);
    prn_job_t j = job(4, 400, 8, 1);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_BEGIN, (uint32_t)(uintptr_t)&j, 0, 0), PEX_OK);
    fh_cl_peer_stall(s_peer, 1);
    hs_drv(s_drv, PRN_DRV_JOB_FEED, (uint32_t)(uintptr_t)s_bm, 12 * 100, 0);
    fh_cl_peer_close(s_peer);
    fh_cl_peer_stall(s_peer, 0);
    fh_run(50);
    CHECK_EQ(HS.ndone, 1);
    CHECK_EQ(HS.done_outcome, PRN_JOB_FAILED);
    CHECK_STR(HS.done_msg, "link lost");
    int pages = fh_cl_peer_stats(s_peer)->pages;
    /* no reconnect of its own: paging a printer that is off stalls the
     * gateway's Wi-Fi (parent-requirements B4); the user connects again */
    fh_run(60000);
    CHECK_EQ(fh_cl_peer_stats(s_peer)->pages, pages);
    CHECK(hs_state_is("idle"));
    connect_ok();
}

static void t_disconnect_stop(void) {
    setup(NULL);
    connect_ok();
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_DISCONNECT, 0, 0, 0), PEX_OK);
    fh_run(100);
    CHECK(hs_state_is("idle"));
    CHECK_EQ(fh_cl_sessions(), 0);
    int pages = fh_cl_peer_stats(s_peer)->pages;
    fh_run(10000);
    CHECK_EQ(fh_cl_peer_stats(s_peer)->pages, pages);   /* no reconnect after a user disconnect */
    connect_ok();
    CHECK_EQ(fh_stop(s_drv), PEX_OK);
    fh_run(100);
    CHECK_EQ(HS.unregisters, 1);
    CHECK_EQ(fh_cl_sessions(), 0);
    fh_unload(s_drv);
}

static void hexb(const char *s, uint8_t *out, size_t *n) {
    size_t k = 0;
    for (; s[0] && s[1]; s += 2) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[k++] = (uint8_t)v;
    }
    *n = k;
}

/* the job text alone against nelko-p21-rs's bytes, at two densities */
static void golden_text(int density) {
    char path[512];
    snprintf(path, sizeof path, GOLDEN_DIR "/golden_p21_14x40_d%d.bin", density);
    size_t gn = load(path, s_golden, sizeof s_golden);
    uint8_t bm[12 * 284];
    CHECK_EQ(load(GOLDEN_DIR "/golden_p21_14x40.bitmap", bm, sizeof bm), sizeof bm);
    tspl_job_t t = { "\x1b!o\r\n", 1400, 4000, 500, PRN_MEDIA_GAP, 203, density, -1, 1, 0, 0, 12, 284, 1, 1, 1 };
    static uint8_t out[8192];
    int h = tspl_head((char *)out, 256, &t);
    CHECK(h > 0);
    memcpy(out + h, bm, sizeof bm);
    int k = tspl_tail((char *)out + h + sizeof bm, 64, &t);
    CHECK(k > 0);
    CHECK_EQ((size_t)h + sizeof bm + (size_t)k, gn);
    CHECK(memcmp(out, s_golden, gn) == 0);
}

static void t_proto(void) {
    golden_text(8);
    golden_text(15);
    /* other sizes format as tenths */
    char b[256];
    tspl_job_t t = { NULL, 1200, 3000, 300, PRN_MEDIA_GAP, 203, 15, -1, 1, 0, 0, 12, 240, 1, 1, 1 };
    CHECK(tspl_head(b, sizeof b, &t) > 0);
    CHECK(strstr(b, "SIZE 12.0 mm,30.0 mm\r\n") == b);
    CHECK(strstr(b, "GAP 3.0 mm,0 mm\r\n") != NULL);
    CHECK(strstr(b, "DIRECTION 1,1\r\nDENSITY 15\r\n") != NULL);
    CHECK(strstr(b, "BITMAP 0,0,12,240,1,") != NULL);
    t.width_mm100 = 1234;            /* rounded to the tenth */
    tspl_head(b, sizeof b, &t);
    CHECK(strstr(b, "SIZE 12.3 mm,") == b);
    CHECK_EQ(tspl_head(b, 10, &t), -1);

    /* CRC vectors: nelko-p21-rs and the AGPL notes (facts) */
    uint8_t d[64];
    size_t n;
    hexb("200c011203000301121215280f0e", d, &n);
    CHECK_EQ(nk_crc16_modbus(d, n), 0x0d22);
    hexb("000c011203000301121215280f0eed03", d, &n);
    nk_status_t l;
    CHECK(nk_parse_status(d, n, &l));
    CHECK(l.readiness == 0 && l.width_mm == 14 && l.length_mm == 40 && l.paper == 1 && l.crc_ok);
    d[15] ^= 1;
    CHECK(nk_parse_status(d, n, &l));
    CHECK(!l.crc_ok);

    hexb("434f4e4649472000cb00000304020402010d0a", d, &n);
    nk_config_t c;
    CHECK(nk_parse_config(d, n, &c));
    CHECK_EQ(c.dpi, 203);
    CHECK_STR(c.fw, "4.2.4");
    CHECK_EQ(c.settings, 2);

    nk_battery_t bt;
    const uint8_t bat[] = {'B', 'A', 'T', 'T', 'E', 'R', 'Y', ' ', 0x87, 0x00, '\r', '\n'};
    CHECK(nk_parse_battery(bat, sizeof bat, &bt));
    CHECK(bt.percent == 87 && !bt.charging);

    CHECK_STR(nk_status_name(0x24), "out_of_paper");
    CHECK(nk_status_error(0x01) && !nk_status_error(0x00) && !nk_status_error(0x20));
    CHECK(!nk_status_error(0x30) && nk_status_error(0x10) && nk_status_error(0x80) && nk_status_error(0x24));
}

int main(void) {
    RUN(t_proto);
    RUN(t_register_scan);
    RUN(t_connect_status);
    RUN(t_pin_legacy);
    RUN(t_job_golden);
    RUN(t_copies_and_stall);
    RUN(t_wrong_width);
    RUN(t_printer_error);
    RUN(t_link_loss);
    RUN(t_disconnect_stop);
    T_DONE();
}
