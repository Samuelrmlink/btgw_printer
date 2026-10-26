/* The Nelko P21 driver on the fake host against a simulated P21: inquiry by
 * name, SSP and PIN pairing, the status queries, jobs byte-for-byte against
 * nelko-p21-rs, copies, stalls, link loss and reconnect, a printer error
 * after the job, and stop. */
#include "t.h"
#include "fakehost.h"
#include "fh_classic.h"
#include "t_hubstub.h"
#include "vendors/nelko/p21/p21_proto.h"

int p21__pex_main(const pex_host_t *host, uint32_t reason, void *arg);
int p21__prn_drv(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2);

#define ADDR "C0:01:02:03:04:21"

/* ---- the simulated printer ---- */
static struct {
    uint8_t in[65536];          /* every byte the host sent */
    size_t n, parsed;
    size_t bitmap_left, bitmap_start;
    int bitmaps, prints, queries_label, queries_status, queries_battery, queries_config;
    uint32_t last_rows;
    uint8_t status;             /* what ESC ! ? answers */
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
                uint16_t c = p21_crc16(blk, 14);
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
    s_drv = fh_module("nelko_p21", p21__pex_main);
    fh_export(s_drv, PRN_DRV_SYM, (void *)p21__prn_drv);
    CHECK_EQ(fh_load(s_drv), PEX_OK);
    CHECK_EQ(fh_start(s_drv, "{}"), PEX_OK);
    fh_run(50);
}

static void connect_ok(void) {
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_CONNECT, (uint32_t)(uintptr_t)ADDR, (uint32_t)(uintptr_t)"{}", 0), PEX_EPENDING);
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
    CHECK_STR(HS.registered, "nelko_p21");
    char caps[1024];
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
    CHECK_HAS(dev, "\"driver\":\"nelko_p21\"");
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
    CHECK_HAS(st, "\"driver\":\"nelko_p21\"");
    CHECK_HAS(st, "\"battery\":76");
    CHECK_HAS(st, "\"printer\":\"ready\"");
    CHECK_HAS(st, "\"w_mm\":14");
    CHECK_HAS(st, "\"h_mm\":40");
    CHECK_HAS(st, "\"type\":\"gap\"");
    CHECK_HAS(st, "\"firmware\":\"4.2.4\"");
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
    /* what the printer got from the job's first byte to PRINT's CRLF; the
     * status query after it is separate */
    CHECK(PR.n - start >= gn);
    CHECK(memcmp(PR.in + start, s_golden, gn) == 0);
    CHECK_EQ(PR.in[start + gn], 0x1b);   /* then ESC ! ? */
    CHECK_EQ(PR.queries_status, 1);
}

static void t_copies_and_stall(void) {
    setup(NULL);
    connect_ok();
    pattern(320);
    prn_job_t j = job(9, 320, 12, 2);
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
    PR.status = 0x04;   /* no paper */
    prn_job_t j = job(3, 100, 8, 1);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_BEGIN, (uint32_t)(uintptr_t)&j, 0, 0), PEX_OK);
    CHECK_EQ(hs_feed_all(s_drv, s_bm, 1200, 1200), 0);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    CHECK(fh_run_until(hs_done, NULL, 5000));
    CHECK_EQ(HS.done_outcome, PRN_JOB_FAILED);
    CHECK_STR(HS.done_msg, "no_paper");
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
    /* reconnects on its own after the backoff */
    fh_run(9000);
    CHECK(fh_cl_peer_stats(s_peer)->pages > pages);
    CHECK(hs_state_is("connected"));
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

int main(void) {
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
