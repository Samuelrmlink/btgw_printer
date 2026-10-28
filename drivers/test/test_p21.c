/* The P21 model of nelko_ble on the fake host against a simulated P21 over
 * BLE (GATT ff00: write ff02 without response, replies notify on ff01,
 * credits on ff03): scan by name, connect and discover, the status queries
 * and codes, the job byte for byte against nelko-p21-rs through BLE chunks,
 * copies, credits, a failed write and the resync, a silent printer, paper
 * out before and after the job, the power-off notice, link loss, stop, and
 * a job through the real printer_hub. Also the P21 reply vectors and job
 * text. */
#include "t.h"
#include "fakehost.h"
#include "fh_ble.h"
#include "t_hubstub.h"
#include "drv.h"
#include "vendors/nelko/tspl/nelko_proto.h"
#include "proto/tspl/tspl.h"

int nble__pex_main(const pex_host_t *host, uint32_t reason, void *arg);
int nble__prn_drv(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2);

#define ADDR "C0:01:02:03:04:21"
#define PL70E "DD:0D:30:89:6C:C5"
#define CHR_NOTIFY 0
#define CHR_WRITE 1
#define CHR_CREDIT 2

/* ---- the simulated printer ---- */
static struct {
    uint8_t in[65536];          /* every byte the host sent */
    size_t n, parsed;
    size_t bitmap_left;
    int bitmaps, prints, queries_label, queries_status, queries_battery, queries_config;
    int status;                 /* what ESC ! ? answers; -1: nothing at all */
    int status_after_print;     /* > 0: the status becomes this once PRINT came */
    size_t bitmap_at[8];
    uint32_t bitmap_rows[8];
    size_t max_write;
    int writes, with_resp;
    int credit_every;           /* > 0: grant 01 <credit_every> after that many writes */
    fh_ble_peer_t *p;
} PR;

static const uint8_t LABEL_14x40[16] = {0x00, 0x0c, 0x01, 0x12, 0x03, 0x00, 0x03, 0x01,
                                        0x12, 0x12, 0x15, 0x28, 0x0f, 0x0e, 0xed, 0x03};

static void pr_send(const void *d, size_t n) { fh_ble_peer_notify(PR.p, CHR_NOTIFY, d, n); }

static void pr_parse(void) {
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
                blk[0] = (uint8_t)(PR.status < 0 ? 0 : PR.status);
                uint16_t c = nk_crc16_modbus(blk, 14);
                blk[14] = (uint8_t)(c >> 8);
                blk[15] = (uint8_t)c;
                if (PR.status >= 0) { pr_send(blk, 16); }
            } else if (s[1] == '!' && s[2] == '?') {
                PR.queries_status++;
                uint8_t b = (uint8_t)PR.status;
                if (PR.status >= 0) { pr_send(&b, 1); }
            }
            PR.parsed += 3;
            continue;
        }
        const uint8_t *nl = memchr(s, '\n', left);
        if (left >= 7 && memcmp(s, "BITMAP ", 7) == 0) {
            /* BITMAP x,y,wb,h,mode, then binary */
            int commas = 0;
            size_t i = 7;
            for (; i < left && commas < 5; i++) { if (s[i] == ',') { commas++; } }
            if (commas < 5) { return; }
            unsigned x, y, wb, h, mode;
            if (sscanf((const char *)s + 7, "%u,%u,%u,%u,%u,", &x, &y, &wb, &h, &mode) != 5) { return; }
            if (PR.bitmaps < 8) { PR.bitmap_at[PR.bitmaps] = PR.parsed + i; PR.bitmap_rows[PR.bitmaps] = h; }
            PR.bitmaps++;
            PR.bitmap_left = (size_t)wb * h;
            PR.parsed += i;
            continue;
        }
        if (!nl) { return; }
        size_t len = (size_t)(nl - s) + 1;
        if (len >= 10 && memcmp(s, "BATTERY?\r\n", 10) == 0) {
            PR.queries_battery++;
            const uint8_t r[] = {'B', 'A', 'T', 'T', 'E', 'R', 'Y', ' ', 0x76, 0x00, '\r', '\n'};
            if (PR.status >= 0) { pr_send(r, sizeof r); }
        } else if (len >= 9 && memcmp(s, "CONFIG?\r\n", 9) == 0) {
            PR.queries_config++;
            const uint8_t r[] = {'C', 'O', 'N', 'F', 'I', 'G', ' ', 0x00, 0xcb, 0, 0, 3, 4, 2, 4, 2, 1, '\r', '\n'};
            if (PR.status >= 0) { pr_send(r, sizeof r); }
        } else if (len >= 7 && memcmp(s, "PRINT ", 6) == 0) {
            PR.prints++;
            if (PR.status_after_print > 0) { PR.status = PR.status_after_print; }
        }
        PR.parsed += len;
    }
}

static void pr_write(fh_ble_peer_t *p, int chr, const uint8_t *d, size_t n, int wr, void *ctx) {
    (void)p; (void)ctx;
    if (chr != CHR_WRITE) { return; }
    PR.writes++;
    PR.with_resp += wr;
    if (n > PR.max_write) { PR.max_write = n; }
    if (PR.n + n <= sizeof PR.in) { memcpy(PR.in + PR.n, d, n); PR.n += n; }
    pr_parse();
    if (PR.credit_every > 0 && PR.writes % PR.credit_every == 0) {
        const uint8_t g[2] = { 1, (uint8_t)PR.credit_every };
        fh_ble_peer_notify(PR.p, CHR_CREDIT, g, 2);
    }
}

static fh_ble_peer_t *add_peer(const char *addr, const char *name, uint16_t mtu, int pipe) {
    fh_ble_peer_cfg_t c;
    memset(&c, 0, sizeof c);
    c.addr = addr;
    c.name = name;
    c.rssi = -55;
    c.mtu = mtu;
    if (pipe) {
        c.uuid16[0] = 0xff00;
        c.n_uuid16 = 1;
        c.chrs[CHR_NOTIFY] = (fh_ble_chr_cfg_t){ "ff00", "ff01", 0x10 };
        c.chrs[CHR_WRITE] = (fh_ble_chr_cfg_t){ "ff00", "ff02", 0x04 };
        c.chrs[CHR_CREDIT] = (fh_ble_chr_cfg_t){ "ff00", "ff03", 0x10 };
        c.chrs[3] = (fh_ble_chr_cfg_t){ "ff00", "ff04", 0x04 };
        c.nchr = 4;
        c.on_write = pr_write;
    }
    return fh_ble_peer_add(&c);
}

static pex_mod_t *s_drv;

static pex_mod_t *load_drv(void) {
    pex_mod_t *m = fh_module("nelko_ble", nble__pex_main);
    fh_export(m, PRN_DRV_SYM, (void *)nble__prn_drv);
    CHECK_EQ(fh_load(m), PEX_OK);
    CHECK_EQ(fh_start(m, "{}"), PEX_OK);
    return m;
}

static void setup_mtu(uint16_t mtu) {
    fh_reset();
    memset(&PR, 0, sizeof PR);
    fh_verbose(getenv("FH_V") != NULL);
    hs_reset();
    PR.p = add_peer(ADDR, "P21", mtu, 1);
    s_drv = load_drv();
    fh_run(50);
}
static void setup(void) { setup_mtu(23); }   /* the P21's own server MTU [M] */

static int drv(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2) { return hs_drv(s_drv, cmd, a0, a1, a2); }

static void connect_ok(void) {
    CHECK_EQ(drv(PRN_DRV_CONNECT, (uint32_t)(uintptr_t)ADDR, (uint32_t)(uintptr_t)"{\"model\":\"p21\"}", 0),
             PEX_EPENDING);
    CHECK(fh_run_until(hs_state_is, "connected", 20000));
    fh_run(500);
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

/* JOB_BEGIN as the hub calls it: again while it says PEX_EAGAIN, for 5 s */
static int begin(prn_job_t *j) {
    int rc;
    uint32_t t0 = fh_now();
    while ((rc = drv(PRN_DRV_JOB_BEGIN, (uint32_t)(uintptr_t)j, 0, 0)) == PEX_EAGAIN && fh_now() - t0 < 5000) {
        fh_run(5);
    }
    return rc;
}

/* one whole job: begin, every copy's rows, end */
static void print_job(prn_job_t *j, const uint8_t *bm) {
    CHECK_EQ(begin(j), 0);
    for (int c = 0; c < j->copies; c++) {
        CHECK_EQ(hs_feed_all(s_drv, bm, (size_t)j->rows_total * 12, 1200), 0);
        CHECK_EQ(drv(PRN_DRV_JOB_END, (uint32_t)c, 0, 0), PEX_OK);
    }
}

static uint8_t s_golden[8192];
static size_t load(const char *path, uint8_t *b, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) { return 0; }
    size_t n = fread(b, 1, cap, f);
    fclose(f);
    return n;
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

/* ---- tests ---- */

/* the job text alone against nelko-p21-rs's bytes, at two densities */
static void golden_text(int density) {
    char path[512];
    snprintf(path, sizeof path, GOLDEN_DIR "/golden_p21_14x40_d%d.bin", density);
    size_t gn = load(path, s_golden, sizeof s_golden);
    uint8_t bm[12 * 284];
    CHECK_EQ(load(GOLDEN_DIR "/golden_p21_14x40.bitmap", bm, sizeof bm), sizeof bm);
    tspl_job_t t = { "\x1b!o\r\n", 1400, 4000, 500, PRN_MEDIA_GAP, 203, density, -1, 1, 0, 0, 12, 284, 1, 1, 1, 0 };
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
    tspl_job_t t = { NULL, 1200, 3000, 300, PRN_MEDIA_GAP, 203, 15, -1, 1, 0, 0, 12, 240, 1, 1, 1, 0 };
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

/* Scan by name: the P21 is ours, the PL70e-BT (either name it advertises)
 * and a phone called "Pixel P21" are not. */
static void t_register_scan(void) {
    setup();
    add_peer(PL70E, "PL70e-BT_6CC5-LE", 185, 0);
    add_peer("DD:0D:30:89:6C:C6", "PL70e-BT_6CC6", 185, 0);
    add_peer("11:22:33:44:55:66", "Pixel P21", 185, 0);
    CHECK_STR(HS.registered, "nelko_ble");
    char caps[2048];
    CHECK(drv(PRN_DRV_INFO, (uint32_t)(uintptr_t)caps, sizeof caps, 0) > 0);
    CHECK_HAS(caps, "{\"driver\":\"nelko_ble\",\"abi\":1,\"models\":[{\"id\":\"p21\"");
    CHECK_HAS(caps, "\"transport\":\"ble\"");
    CHECK_HAS(caps, "\"head_dots\":96");
    CHECK_HAS(caps, "\"black_bit\":0");
    CHECK_HAS(caps, "\"placement\":false");
    CHECK_HAS(caps, "\"copies\":false");
    CHECK_EQ(drv(PRN_DRV_SCAN, 3000, 0, 0), PEX_OK);
    fh_run(3600);
    CHECK_EQ(hs_count("found", ADDR), 1);
    CHECK_EQ(hs_count("found", NULL), 1);
    CHECK_HAS(hs_last("found"), "\"model\":\"p21\"");
    CHECK_HAS(hs_last("found"), "\"transport\":\"ble\"");
    char dev[512];
    CHECK(drv(PRN_DRV_DEVICES, (uint32_t)(uintptr_t)dev, sizeof dev, 0) > 0);
    CHECK_STR(dev, "[{\"driver\":\"nelko_ble\",\"addr\":\"" ADDR "\",\"name\":\"P21\",\"rssi\":-55,\"model\":\"p21\","
                   "\"transport\":\"ble\"}]");
    CHECK(!strstr(dev, "PL70e"));
    /* connecting with no model in the config takes it from the name */
    CHECK_EQ(drv(PRN_DRV_CONNECT, (uint32_t)(uintptr_t)ADDR, (uint32_t)(uintptr_t)"{}", 0), PEX_EPENDING);
    CHECK(fh_run_until(hs_state_is, "connected", 20000));
    CHECK_HAS(hs_last("state"), "\"model\":\"p21\"");
    CHECK_HAS(hs_last("state"), "\"driver\":\"nelko_ble\"");
}

static void t_connect_status(void) {
    setup();
    connect_ok();
    const fh_ble_stats_t *st = fh_ble_peer_stats(PR.p);
    CHECK(st->subscribed[CHR_NOTIFY] == 1 && st->subscribed[CHR_CREDIT] == 1);
    CHECK_EQ(PR.queries_config, 1);
    CHECK_EQ(PR.queries_battery, 1);
    CHECK_EQ(PR.queries_label, 1);
    CHECK_EQ(PR.with_resp, 0);              /* ff02 is write without response */
    char s[640];
    CHECK(drv(PRN_DRV_STATUS, 0, (uint32_t)(uintptr_t)s, sizeof s) > 0);
    CHECK(s[0] == '[');
    CHECK_HAS(s, "\"driver\":\"nelko_ble\"");
    CHECK_HAS(s, "\"model\":\"p21\"");
    CHECK_HAS(s, "\"battery\":76");
    CHECK_HAS(s, "\"printer\":\"ready\"");
    CHECK_HAS(s, "\"w_mm\":14");
    CHECK_HAS(s, "\"h_mm\":40");
    CHECK_HAS(s, "\"type\":\"gap\"");
    CHECK_HAS(s, "\"fw\":\"4.2.4\"");
    CHECK_HAS(s, "\"paper\":true");
    CHECK_HAS(s, "\"lid\":false");
    CHECK_HAS(s, "\"mtu\":23");
    CHECK_EQ(drv(PRN_DRV_STATUS, (uint32_t)(uintptr_t)"11:11:11:11:11:11", (uint32_t)(uintptr_t)s, sizeof s), 2);
    /* idle: ESC !? every 10 s, BATTERY? every 30 s; a code change is a status event */
    int q = PR.queries_status;
    PR.status = 0x30;                        /* low battery: still prints */
    fh_run(31000);
    CHECK(PR.queries_status >= q + 3);
    CHECK_EQ(PR.queries_battery, 2);
    CHECK_HAS(hs_last("status"), "\"printer\":\"low_battery\"");
    prn_job_t j = job(1, 10, 8, 1);
    pattern(10);
    print_job(&j, s_bm);
    CHECK(fh_run_until(hs_done, NULL, 10000));
    CHECK_EQ(HS.done_outcome, PRN_JOB_OK);
}

/* The golden job through 20-byte writes (MTU 23) and 244-byte ones */
static void job_golden(uint16_t mtu, size_t max_write) {
    setup_mtu(mtu);
    connect_ok();
    uint8_t bm[12 * 284];
    size_t bn = load(GOLDEN_DIR "/golden_p21_14x40.bitmap", bm, sizeof bm);
    size_t gn = load(GOLDEN_DIR "/golden_p21_14x40_d8.bin", s_golden, sizeof s_golden);
    CHECK_EQ(bn, sizeof bm);
    CHECK(gn > bn);
    size_t start = PR.n;
    int q = PR.queries_status;
    prn_job_t j = job(7, 284, 8, 1);
    CHECK_EQ(begin(&j), 0);
    CHECK_EQ(PR.queries_status, q + 1);     /* a fresh ESC !? before the job */
    CHECK_EQ(hs_feed_all(s_drv, bm, bn, 700), 0);
    CHECK_EQ(drv(PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    CHECK(fh_run_until(hs_done, NULL, 10000));
    CHECK_EQ(HS.done_id, 7);
    CHECK_EQ(HS.done_outcome, PRN_JOB_OK);
    CHECK_EQ(PR.bitmaps, 1);
    CHECK_EQ(PR.prints, 1);
    CHECK_EQ(PR.max_write, max_write);
    /* first the readiness check, ESC ! ? with no CRLF */
    CHECK(memcmp(PR.in + start, "\x1b!?\x1b!o", 6) == 0);
    start += 3;
    /* from the job's first byte to PRINT's CRLF, then the ESC ! ? after it */
    CHECK(PR.n - start >= gn);
    CHECK(memcmp(PR.in + start, s_golden, gn) == 0);
    CHECK_EQ(PR.in[start + gn], 0x1b);
    CHECK_EQ(PR.queries_status, q + 2);
}
static void t_job_golden_mtu23(void) { job_golden(23, 20); }
static void t_job_golden_mtu247(void) { job_golden(247, 244); }

/* Copies: the hub feeds the rows once per copy; each is a whole job with
 * PRINT 1, and the next head goes only once the copy before is sent. */
static void t_copies(void) {
    setup();
    connect_ok();
    pattern(320);
    prn_job_t j = job(9, 320, 12, 2);
    print_job(&j, s_bm);
    CHECK(fh_run_until(hs_done, NULL, 20000));
    CHECK_EQ(HS.done_outcome, PRN_JOB_OK);
    CHECK_EQ(PR.bitmaps, 2);
    CHECK_EQ(PR.prints, 2);
    for (int k = 0; k < 2; k++) {
        CHECK_EQ(PR.bitmap_rows[k], 320);
        CHECK(memcmp(PR.in + PR.bitmap_at[k], s_bm, 12 * 320) == 0);
    }
    CHECK(memmem(PR.in, PR.n, "DENSITY 12\r\n", 12) != NULL);
}

/* ff03 flow control: the packet size it announces caps the writes, and the
 * credits it grants are kept to; with no credit for 500 ms it sends anyway. */
static void t_credits(void) {
    setup_mtu(247);
    connect_ok();
    const uint8_t size[3] = { 2, 0x7f, 0x00 };
    fh_ble_peer_notify(PR.p, CHR_CREDIT, size, 3);
    const uint8_t grant[2] = { 1, 2 };
    fh_ble_peer_notify(PR.p, CHR_CREDIT, grant, 2);
    PR.credit_every = 2;
    fh_run(50);
    pattern(284);
    prn_job_t j = job(3, 284, 8, 1);
    PR.max_write = 0;
    print_job(&j, s_bm);
    CHECK(fh_run_until(hs_done, NULL, 10000));
    CHECK_EQ(HS.done_outcome, PRN_JOB_OK);
    CHECK_EQ(PR.max_write, 127);
    char s[640];
    CHECK(drv(PRN_DRV_STATUS, 0, (uint32_t)(uintptr_t)s, sizeof s) > 0);
    CHECK(!strstr(s, "credit_waits"));
    /* it stops granting: slower, but the job still goes */
    PR.credit_every = 0;
    HS.ndone = 0;
    j = job(4, 284, 8, 1);
    print_job(&j, s_bm);
    CHECK(fh_run_until(hs_done, NULL, 60000));
    CHECK_EQ(HS.done_outcome, PRN_JOB_OK);
    CHECK_EQ(PR.prints, 2);
    CHECK(drv(PRN_DRV_STATUS, 0, (uint32_t)(uintptr_t)s, sizeof s) > 0);
    CHECK_HAS(s, "\"credit_waits\":");
}

static void t_wrong_width(void) {
    setup();
    connect_ok();
    prn_job_t j = job(1, 100, 8, 1);
    j.width_dots = 384;
    j.row_bytes = 48;
    CHECK_EQ(drv(PRN_DRV_JOB_BEGIN, (uint32_t)(uintptr_t)&j, 0, 0), PEX_EINVAL);
}

/* Paper out: the answer to its own ESC !? refuses the job; an old answer
 * only makes it ask. Busy waits; an error after PRINT fails the job. */
static void t_printer_error(void) {
    setup();
    connect_ok();
    pattern(100);
    PR.status = 0x04;
    fh_run(11000);                           /* the idle poll has seen it */
    CHECK_HAS(hs_last("status"), "\"paper\":false");
    prn_job_t j = job(3, 100, 8, 1);
    fh_run(2100);                            /* that answer is old now */
    CHECK_EQ(drv(PRN_DRV_JOB_BEGIN, (uint32_t)(uintptr_t)&j, 0, 0), PEX_EAGAIN);
    fh_run(300);
    CHECK_EQ(drv(PRN_DRV_JOB_BEGIN, (uint32_t)(uintptr_t)&j, 0, 0), PRN_DRV_ENOTREADY);
    CHECK_EQ(PR.bitmaps, 0);
    /* paper back in */
    PR.status = 0;
    CHECK_EQ(begin(&j), 0);
    CHECK_EQ(drv(PRN_DRV_CANCEL, 0, 0, 0), PEX_OK);
    fh_run(500);
    /* busy before the job: it waits, then prints */
    PR.status = 0x20;
    HS.ndone = 0;
    j = job(5, 100, 8, 1);
    for (int i = 0; i < 100; i++) {
        int rc = drv(PRN_DRV_JOB_BEGIN, (uint32_t)(uintptr_t)&j, 0, 0);
        CHECK_EQ(rc, PEX_EAGAIN);
        fh_run(10);
    }
    PR.status = 0;
    CHECK_EQ(begin(&j), 0);
    CHECK_EQ(hs_feed_all(s_drv, s_bm, 1200, 1200), 0);
    CHECK_EQ(drv(PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    CHECK(fh_run_until(hs_done, NULL, 10000));
    CHECK_EQ(HS.done_outcome, PRN_JOB_OK);
    /* the paper runs out during the job: the status after it fails the job */
    PR.status_after_print = 0x04;
    HS.ndone = 0;
    j = job(6, 100, 8, 1);
    print_job(&j, s_bm);
    CHECK(fh_run_until(hs_done, NULL, 10000));
    CHECK_EQ(HS.done_id, 6);
    CHECK_EQ(HS.done_outcome, PRN_JOB_FAILED);
    CHECK_STR(HS.done_msg, "out of paper");
}

/* A write the gateway reports failed never reached the printer. Mid-BITMAP
 * that leaves the printer waiting for image bytes and deaf to ESC !?: the
 * job fails, white fills the gap, and the printer answers again. */
static void t_failed_write_resyncs(void) {
    setup();
    connect_ok();
    pattern(284);
    prn_job_t j = job(8, 284, 8, 1);
    CHECK_EQ(begin(&j), 0);
    CHECK(drv(PRN_DRV_JOB_FEED, (uint32_t)(uintptr_t)s_bm, 12 * 20, 0) > 0);
    fh_run(100);
    fh_ble_peer_fail_writes(PR.p, 1);
    int rc = hs_feed_all(s_drv, s_bm + 12 * 20, 12 * 264, 1200);
    CHECK(rc == 0 || rc == PEX_ESTATE);       /* the job may already have failed under it */
    if (rc == 0) { drv(PRN_DRV_JOB_END, 0, 0, 0); }
    CHECK(fh_run_until(hs_done, NULL, 10000));
    CHECK_EQ(HS.done_outcome, PRN_JOB_FAILED);
    CHECK_HAS(HS.done_msg, "didn't reach the printer");
    fh_run(12000);
    CHECK_EQ(PR.bitmap_left, 0u);             /* the printer's BITMAP is complete */
    CHECK_EQ(PR.prints, 0);                   /* and nothing printed */
    char s[640];
    CHECK(drv(PRN_DRV_STATUS, 0, (uint32_t)(uintptr_t)s, sizeof s) > 0);
    CHECK_HAS(s, "\"printer\":\"ready\"");
    CHECK_HAS(s, "\"writes_failed\":1");
    /* and the next job prints whole */
    HS.ndone = 0;
    int b0 = PR.bitmaps;
    j = job(9, 284, 8, 1);
    print_job(&j, s_bm);
    CHECK(fh_run_until(hs_done, NULL, 10000));
    CHECK_EQ(HS.done_outcome, PRN_JOB_OK);
    CHECK_EQ(PR.prints, 1);
    CHECK(PR.bitmaps == b0 + 1 && memcmp(PR.in + PR.bitmap_at[b0], s_bm, 12 * 284) == 0);
}

/* sent, and then not one answer: failed, as it may not have printed */
static void t_silent_printer(void) {
    setup();
    connect_ok();
    pattern(50);
    prn_job_t j = job(2, 50, 8, 1);
    CHECK_EQ(begin(&j), 0);
    PR.status = -1;
    CHECK_EQ(hs_feed_all(s_drv, s_bm, 600, 600), 0);
    CHECK_EQ(drv(PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    CHECK(fh_run_until(hs_done, NULL, 20000));
    CHECK_EQ(HS.done_outcome, PRN_JOB_FAILED);
    CHECK_HAS(HS.done_msg, "isn't answering");
    char s[640];
    CHECK(drv(PRN_DRV_STATUS, 0, (uint32_t)(uintptr_t)s, sizeof s) > 0);
    CHECK_HAS(s, "\"unanswered\":");
    /* still quiet: a job only gets PEX_EAGAIN (the hub gives up after 5 s) */
    j = job(3, 50, 8, 1);
    CHECK_EQ(begin(&j), PEX_EAGAIN);
}

static void t_cancel_pads_bitmap(void) {
    setup();
    connect_ok();
    pattern(400);
    prn_job_t j = job(4, 400, 8, 1);
    CHECK_EQ(begin(&j), 0);
    CHECK(drv(PRN_DRV_JOB_FEED, (uint32_t)(uintptr_t)s_bm, 12 * 10, 0) > 0);
    CHECK_EQ(drv(PRN_DRV_CANCEL, 0, 0, 0), PEX_OK);
    CHECK_EQ(HS.done_outcome, PRN_JOB_CANCELLED);
    fh_run(8000);
    CHECK_EQ(PR.bitmaps, 1);
    CHECK_EQ(PR.bitmap_left, 0u);
    CHECK_EQ(PR.prints, 0);
    HS.ndone = 0;
    j = job(5, 40, 8, 1);
    print_job(&j, s_bm);
    CHECK(fh_run_until(hs_done, NULL, 10000));
    CHECK_EQ(HS.done_outcome, PRN_JOB_OK);
    CHECK_EQ(PR.prints, 1);
}

/* a lost write-done event (parent B6) must not hold the job for ever */
static void t_lost_write_event(void) {
    setup();
    connect_ok();
    pattern(100);
    prn_job_t j = job(6, 100, 8, 1);
    CHECK_EQ(begin(&j), 0);
    fh_ble_peer_drop_write_done(PR.p, 1);
    CHECK_EQ(hs_feed_all(s_drv, s_bm, 1200, 1200), 0);
    CHECK_EQ(drv(PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    CHECK(fh_run_until(hs_done, NULL, 20000));
    CHECK_EQ(HS.done_outcome, PRN_JOB_OK);
    CHECK(fh_log_has("write event(s) never came"));
    char s[640];
    CHECK(drv(PRN_DRV_STATUS, 0, (uint32_t)(uintptr_t)s, sizeof s) > 0);
    CHECK_HAS(s, "\"writes_lost\":1");
}

/* link loss fails the job and reconnects; the power-off notice drops the link */
static void t_link_loss_and_power_off(void) {
    setup();
    connect_ok();
    pattern(400);
    prn_job_t j = job(4, 400, 8, 1);
    CHECK_EQ(begin(&j), 0);
    drv(PRN_DRV_JOB_FEED, (uint32_t)(uintptr_t)s_bm, 12 * 30, 0);
    fh_run(20);
    fh_ble_peer_disconnect(PR.p);
    fh_run(50);
    CHECK_EQ(HS.ndone, 1);
    CHECK_EQ(HS.done_outcome, PRN_JOB_FAILED);
    CHECK_STR(HS.done_msg, "link lost");
    CHECK(hs_state_is("waiting"));
    CHECK(fh_run_until(hs_state_is, "connected", 30000));
    fh_run(500);
    fh_ble_peer_notify(PR.p, CHR_NOTIFY, "AT+DISCOVERABLE=0\r", 18);
    fh_run(20);
    CHECK_HAS(hs_last("state"), "\"why\":\"printer switched off\"");
    /* a user disconnect: no reconnect */
    CHECK_EQ(drv(PRN_DRV_DISCONNECT, 0, 0, 0), PEX_OK);
    fh_run(50);
    CHECK(hs_state_is("idle"));
    int c = fh_ble_peer_stats(PR.p)->connects;
    fh_run(70000);
    CHECK_EQ(fh_ble_peer_stats(PR.p)->connects, c);
}

static void t_stop_releases(void) {
    setup();
    connect_ok();
    CHECK_EQ(fh_stop(s_drv), PEX_OK);
    fh_run(100);
    CHECK_EQ(HS.unregisters, 1);
    CHECK_EQ(fh_ble_listeners(), 0);
    CHECK(!fh_ble_peer_stats(PR.p)->connected);
}

/* Through the real printer_hub: printer.connect, a job file, printer.print
 * with two copies, printer.job events; and the hub's printer.status. */
static void t_through_real_hub(void) {
    fh_reset();
    memset(&PR, 0, sizeof PR);
    load_hub();
    PR.p = add_peer(ADDR, "P21", 23, 1);
    s_drv = load_drv();
    fh_run(50);
    char R[3000];
    CHECK(fh_op("printer.drivers", "{}", R, sizeof R) > 0);
    CHECK_HAS(R, "\"driver\":\"nelko_ble\"");
    CHECK(fh_op("printer.scan", "{\"ms\":2000,\"driver\":\"nelko_ble\"}", R, sizeof R) > 0);
    CHECK(wait_ev("printer.found", ADDR, "\"model\":\"p21\"", 3000));
    CHECK(fh_op("printer.connect", "{\"driver\":\"nelko_ble\",\"addr\":\"" ADDR "\",\"config\":{\"model\":\"p21\"}}",
                R, sizeof R) > 0);
    CHECK(wait_ev("printer.state", "\"connected\"", NULL, 20000));
    pattern(284);
    fh_fs_put("lfs:/printer/var/job.bin", s_bm, 12 * 284);
    CHECK(fh_op("printer.print", "{\"driver\":\"nelko_ble\",\"addr\":\"" ADDR "\",\"model\":\"p21\","
                "\"path\":\"printer/var/job.bin\",\"width_dots\":96,\"rows\":284,\"origin\":[0,0],"
                "\"media\":{\"w_mm\":14,\"h_mm\":40,\"gap_mm\":5,\"type\":\"gap\"},"
                "\"density\":20,\"speed\":-1,\"copies\":2,\"rotate\":0,\"options\":{}}", R, sizeof R) > 0);
    CHECK_HAS(R, "\"job\"");
    CHECK(wait_ev("printer.job", "\"done\"", NULL, 60000));
    CHECK_EQ(PR.prints, 2);
    CHECK_EQ(PR.bitmaps, 2);
    CHECK(memcmp(PR.in + PR.bitmap_at[1], s_bm, 12 * 284) == 0);
    CHECK(memmem(PR.in, PR.n, "DENSITY 15\r\n", 12) != NULL);    /* clamped to the model's range */
    CHECK(memmem(PR.in, PR.n, "SIZE 14.0 mm,40.0 mm\r\nGAP 5.0 mm,0 mm\r\n", 38) != NULL);
    CHECK(fh_op("printer.status", "{}", R, sizeof R) > 0);
    CHECK_HAS(R, "\"driver\":\"nelko_ble\"");
    CHECK_HAS(R, "\"printer\":\"ready\"");
}

int main(void) {
    RUN(t_proto);
    RUN(t_register_scan);
    RUN(t_connect_status);
    RUN(t_job_golden_mtu23);
    RUN(t_job_golden_mtu247);
    RUN(t_copies);
    RUN(t_credits);
    RUN(t_wrong_width);
    RUN(t_printer_error);
    RUN(t_failed_write_resyncs);
    RUN(t_silent_printer);
    RUN(t_cancel_pads_bitmap);
    RUN(t_lost_write_event);
    RUN(t_link_loss_and_power_off);
    RUN(t_stop_releases);
    RUN(t_through_real_hub);
    T_DONE();
}
