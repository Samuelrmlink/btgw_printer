/* Nelko PL70e-BT driver against a simulated printer.
 *
 * The peer is a GATT table like the bench unit's (docs/protocols/pl70e-bt.md)
 * with a small TSPL interpreter behind the write characteristic: it answers
 * READSTA, parses SIZE/GAP/DENSITY/CLS/BITMAP/PRINT and keeps the printed
 * page so the test can compare it with the bitmap that went in. The hub is a
 * stub module that records what the driver tells it, and the test calls
 * prn_drv the way printer_hub does. */
#include "t.h"
#include "fakehost.h"
#include "fh_ble.h"
#include "printer_driver.h"
#include "vendors/nelko/pl70e/pl70e_proto.h"
#include "drv.h"
#include <stdio.h>

int pl70e__pex_main(const pex_host_t *h, uint32_t reason, void *arg);
int pl70e__prn_drv(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2);

#define ADDR "DD:0D:30:89:6C:C5"

/* ---- stub hub ---- */
static int s_registered, s_done, s_done_outcome, s_kicks;
static char s_done_msg[64], s_last_state[256], s_last_status[256];
static int hub_fn(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2) {
    (void)a2;
    switch (cmd) {
    case PRN_HUB_REGISTER: s_registered++; return 0;
    case PRN_HUB_UNREGISTER: s_registered--; return 0;
    case PRN_HUB_EMIT:
        if (!strcmp((const char *)a0, "state")) { snprintf(s_last_state, sizeof s_last_state, "%s", (const char *)a1); }
        if (!strcmp((const char *)a0, "status")) { snprintf(s_last_status, sizeof s_last_status, "%s", (const char *)a1); }
        return 0;
    case PRN_HUB_JOB_DONE:
        s_done++;
        s_done_outcome = (int)a1;
        snprintf(s_done_msg, sizeof s_done_msg, "%s", a2 ? (const char *)a2 : "");
        return 0;
    case PRN_HUB_KICK: s_kicks++; return 0;
    }
    return PEX_ENOTSUP;
}
static int hub_main(const pex_host_t *h, uint32_t r, void *a) { (void)h; (void)r; (void)a; return PEX_OK; }

/* ---- simulated printer ---- */
typedef struct {
    int width_bytes, height, x, y, mode;
    char size[300], gap[300];
    int density, copies, prints, cls, set_gap_on;
    uint8_t *page;           /* last BITMAP, as received */
    size_t page_n;
    /* parser */
    uint8_t line[256];
    size_t ln;
    size_t bmp_left;
    uint8_t *bmp;
    size_t bmp_n;
    uint32_t busy_from, busy_until;   /* printing then (fh_now) */
    int hung;                /* a byte came while printing: the bench unit locks up */
    int status;              /* reply to READSTA: 0 paper, 1 no paper, 2 lid, 3 unknown word, -1 silent */
    int queries;
    size_t bytes;
    fh_ble_peer_t *p;
    int notify_chr;
} sim_t;
static sim_t S;
/* A 4x6 label at density 10 takes about this long on the real thing (well
 * under the driver's estimate). */
static uint32_t SIM_PRINT_MS = 3000;

static void sim_reply(const char *s) { fh_ble_peer_notify(S.p, S.notify_chr, s, strlen(s)); }

static void sim_line(const char *l) {
    if (!strncmp(l, "READSTA", 7)) {
        S.queries++;
        if (S.status == 0) { sim_reply("READSTA PAPER,\r\n"); }
        if (S.status == 1) { sim_reply("READSTA NOPAPER,\r\n"); }
        if (S.status == 2) { sim_reply("READSTA LIBOPEN,\r\n"); }
        if (S.status == 3) { sim_reply("READSTA BUSY,\r\n"); }   /* a value the driver doesn't know */
    } else if (!strncmp(l, "SIZE ", 5)) {
        snprintf(S.size, sizeof S.size, "%s", l + 5);
    } else if (!strncmp(l, "GAP ", 4)) {
        snprintf(S.gap, sizeof S.gap, "%s", l + 4);
    } else if (!strncmp(l, "DENSITY ", 8)) {
        S.density = atoi(l + 8);
    } else if (!strcmp(l, "CLS")) {
        S.cls++;
    } else if (!strcmp(l, "SET GAP ON")) {
        S.set_gap_on++;
    } else if (!strncmp(l, "PRINT ", 6)) {
        const char *c = strchr(l, ',');
        S.copies = c ? atoi(c + 1) : atoi(l + 6);
        S.prints++;
        S.busy_from = fh_now() + 300;          /* the rest of its own line may trail in */
        S.busy_until = fh_now() + SIM_PRINT_MS * (uint32_t)(S.copies ? S.copies : 1);
    }
}

static void sim_byte(uint8_t b) {
    if (S.bmp_left) {
        S.bmp[S.bmp_n++] = b;
        if (--S.bmp_left == 0) {
            free(S.page);
            S.page = S.bmp;
            S.page_n = S.bmp_n;
            S.bmp = NULL;
        }
        return;
    }
    if (b == '\n') { return; }
    if (b == '\r') {
        S.line[S.ln] = 0;
        if (S.ln) { sim_line((const char *)S.line); }
        S.ln = 0;
        return;
    }
    if (S.ln < sizeof S.line - 1) { S.line[S.ln++] = b; }
    /* BITMAP x,y,wb,h,mode,  -- binary follows the 5th comma */
    if (b == ',' && S.ln > 7 && !memcmp(S.line, "BITMAP ", 7)) {
        int commas = 0;
        for (size_t i = 0; i < S.ln; i++) { commas += S.line[i] == ','; }
        if (commas == 5) {
            S.line[S.ln] = 0;
            sscanf((const char *)S.line + 7, "%d,%d,%d,%d,%d", &S.x, &S.y, &S.width_bytes, &S.height, &S.mode);
            S.bmp_left = (size_t)S.width_bytes * S.height;
            S.bmp = calloc(1, S.bmp_left);
            S.bmp_n = 0;
            S.ln = 0;
        }
    }
}

static void sim_write(fh_ble_peer_t *p, int chr, const uint8_t *d, size_t n, int wr, void *ctx) {
    (void)p; (void)chr; (void)wr; (void)ctx;
    S.bytes += n;
    if (S.busy_until && (int32_t)(fh_now() - S.busy_from) >= 0 && (int32_t)(fh_now() - S.busy_until) < 0) {
        S.hung = 1;
    }
    if (S.hung) { return; }
    for (size_t i = 0; i < n; i++) { sim_byte(d[i]); }
}

static uint32_t s_op_ms;       /* 0 = the fake host's default */
static fh_ble_peer_t *add_printer(int pipe_18f0, uint16_t mtu) {
    fh_ble_peer_cfg_t c;
    memset(&c, 0, sizeof c);
    c.op_ms = s_op_ms;
    c.addr = ADDR;
    c.name = "PL70e-BT_6CC5-LE";
    c.rssi = -60;
    c.uuid16[0] = 0x18f0;
    c.n_uuid16 = 1;
    c.mtu = mtu;
    int i = 0;
    if (pipe_18f0) {
        c.chrs[i++] = (fh_ble_chr_cfg_t){ "18f0", "2af0", 0x10 };
        c.chrs[i++] = (fh_ble_chr_cfg_t){ "18f0", "2af1", 0x0c };
    }
    c.chrs[i++] = (fh_ble_chr_cfg_t){ "fff0", "fff1", 0x10 };
    c.chrs[i++] = (fh_ble_chr_cfg_t){ "fff0", "fff2", 0x0c };
    c.nchr = i;
    c.on_write = sim_write;
    fh_ble_peer_t *p = fh_ble_peer_add(&c);
    S.p = p;
    S.notify_chr = 0;        /* first entry is the notify of the chosen pipe */
    return p;
}

static pex_mod_t *s_drv;

static void setup(int pipe_18f0, uint16_t mtu) {
    fh_reset();
    free(S.page);
    free(S.bmp);
    memset(&S, 0, sizeof S);
    SIM_PRINT_MS = 3000;
    s_registered = s_done = s_kicks = 0;
    s_done_outcome = -1;
    s_last_state[0] = s_last_status[0] = 0;
    pex_mod_t *hub = fh_module(PRN_HUB_MODULE, hub_main);
    fh_export(hub, PRN_HUB_SYM, (void *)hub_fn);
    fh_load(hub);
    fh_start(hub, "{}");
    add_printer(pipe_18f0, mtu);
    s_drv = fh_module("nelko_pl70e", pl70e__pex_main);
    fh_export(s_drv, PRN_DRV_SYM, (void *)pl70e__prn_drv);
    fh_load(s_drv);
    fh_start(s_drv, "{}");
}

static int drv(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2) {
    pex_mod_t *prev = fh_swap(s_drv);
    int rc = pl70e__prn_drv(cmd, a0, a1, a2);
    fh_swap(prev);
    return rc;
}

static int connected(void *a) { (void)a; return strstr(s_last_state, "\"connected\"") != NULL; }
static int got_done(void *a) { (void)a; return s_done > 0; }

static int answered(void *a) { (void)a; return S.queries > 0 || S.status < 0; }

/* JOB_BEGIN the way printer_hub calls it: again each tick while the driver
 * answers PEX_EAGAIN (it is asking the printer first), for up to 5 s. */
static int begin(const prn_job_t *j) {
    uint32_t t0 = fh_now();
    int rc;
    while ((rc = drv(PRN_DRV_JOB_BEGIN, (uint32_t)j, 0, 0)) == PEX_EAGAIN && fh_now() - t0 < 5000) { fh_run(20); }
    return rc;
}

static void connect_printer(void) {
    /* the address must be in the scan table: the driver scans for it */
    CHECK_EQ(drv(PRN_DRV_CONNECT, (uint32_t)ADDR, (uint32_t)"{}", 0), PEX_EPENDING);
    CHECK(fh_run_until(connected, NULL, 20000));
    CHECK(fh_run_until(answered, NULL, 10000));   /* its first READSTA, after the connect hold */
}

/* A test page: a 1-dot border, stripes and a diagonal. black = 0. */
static uint8_t *make_page(int wb, int h) {
    uint8_t *b = malloc((size_t)wb * h);
    memset(b, 0xff, (size_t)wb * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < wb * 8; x++) {
            int on = x == 0 || y == 0 || x == wb * 8 - 1 || y == h - 1 || (y / 8) % 4 == 0 || x == y;
            if (on) { b[y * wb + x / 8] &= (uint8_t)~(0x80 >> (x % 8)); }
        }
    }
    return b;
}

static void fill_job(prn_job_t *j, int wb, int h) {
    memset(j, 0, sizeof *j);
    j->id = 7;
    snprintf(j->addr, sizeof j->addr, "%s", ADDR);
    snprintf(j->model, sizeof j->model, "pl70e");
    j->width_dots = (uint16_t)(wb * 8);
    j->row_bytes = (uint16_t)wb;
    j->rows_total = (uint32_t)h;
    j->width_mm100 = 10160;
    j->height_mm100 = 15240;
    j->gap_mm100 = 300;
    j->media_type = PRN_MEDIA_GAP;
    j->density = 10;
    j->speed = -1;
    j->copies = 2;
    j->black_bit = 0;
    snprintf(j->options, sizeof j->options, "{}");
}

/* Feed like the hub: offer up to 4 KB at a time, retry on EBUSY each ms. */
static int feed_all(const uint8_t *page, size_t n, int wb) {
    size_t off = 0;
    uint32_t guard = 0;
    while (off < n && guard++ < 600000) {
        size_t k = n - off > 4096 ? 4096 : n - off;
        k = k / wb * wb;
        int rc = drv(PRN_DRV_JOB_FEED, (uint32_t)(page + off), (uint32_t)k, 0);
        if (rc > 0) { off += (size_t)rc; continue; }
        if (rc != PEX_EBUSY) { return rc; }
        fh_step();
    }
    return off == n ? 0 : -1;
}

static void t_proto(void) {
    pl70e_rx_t rx = {0};
    CHECK_EQ(pl70e_rx_feed(&rx, (const uint8_t *)"READSTA PA", 10), -1);
    CHECK_EQ(pl70e_rx_feed(&rx, (const uint8_t *)"PER,\r\n", 6), PL70E_ST_READY);
    CHECK_EQ(pl70e_rx_feed(&rx, (const uint8_t *)"READSTA NOPAPER,\r\n", 18), PL70E_ST_NOPAPER);
    CHECK_EQ(pl70e_rx_feed(&rx, (const uint8_t *)"READSTA LIBOPEN,\r\n", 18), PL70E_ST_LIDOPEN);
    CHECK_EQ(pl70e_rx_feed(&rx, (const uint8_t *)"READSTA OVERHEAT,\r\n", 19), PL70E_ST_OVERHEAT);
    uint8_t b = 0x06;
    CHECK_EQ(pl70e_rx_feed(&rx, &b, 1), PL70E_ST_NOPAPER);
    b = 0x20;
    CHECK_EQ(pl70e_rx_feed(&rx, &b, 1), PL70E_ST_PRINTING);
    b = 0;
    CHECK_EQ(pl70e_rx_feed(&rx, &b, 1), PL70E_ST_READY);
    CHECK(pl70e_name_match("PL70e-BT_6CC5-LE"));
    CHECK(pl70e_name_match("PL70e-BT_6CC5"));
    CHECK(!pl70e_name_match("PM220"));
    CHECK(!pl70e_name_match(NULL));
}

static void t_info(void) {
    setup(1, 185);
    char out[1024];
    int n = drv(PRN_DRV_INFO, (uint32_t)out, sizeof out, 0);
    CHECK(n > 0);
    CHECK_HAS(out, "\"driver\":\"nelko_pl70e\"");
    CHECK_HAS(out, "\"head_dots\":832");
    CHECK_HAS(out, "\"black_bit\":0");
    CHECK_HAS(out, "\"nelko-4x6\"");
    CHECK_HAS(out, "\"nelko-3x1\"");
    CHECK(s_registered == 1);
}

static void t_connect_and_status(void) {
    setup(1, 185);
    connect_printer();
    fh_run(300);
    CHECK(S.queries >= 1);
    char out[512];
    drv(PRN_DRV_STATUS, 0, (uint32_t)out, sizeof out);
    CHECK_HAS(out, "\"printer\":\"ready\"");
    CHECK_HAS(out, "\"paper\":\"ok\"");
    /* no polling while idle: the printer may be feeding or finding the gap */
    int q = S.queries;
    fh_run(30000);
    CHECK_EQ(S.queries, q);
}

/* Any byte while it prints locks the bench unit up (the sim does the same):
 * nothing goes out until the estimated print time has passed, then one
 * READSTA, and the next job only after that. */
static void t_silent_while_printing(void) {
    setup(1, 185);
    connect_printer();
    fh_run(200);
    prn_job_t j;
    fill_job(&j, 76, 203);
    j.height_mm100 = 2540;                     /* 3x1: (25.4 + 3) mm / 7 mm/s = 4.1 s a copy */
    j.copies = 1;
    uint8_t *page = make_page(76, 203);
    CHECK_EQ(begin(&j), 1);
    CHECK_EQ(feed_all(page, 76 * 203, 76), 0);
    CHECK_EQ(drv(PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    uint32_t t0 = 0;
    while (!S.prints && fh_now() < 60000) { fh_run(10); }
    t0 = fh_now();
    size_t b = S.bytes;
    int q = S.queries;
    fh_run(4000);
    CHECK_EQ(S.bytes, b);                      /* silent while it prints */
    CHECK_EQ(drv(PRN_DRV_JOB_BEGIN, (uint32_t)&j, 0, 0), PEX_EBUSY);
    CHECK(fh_run_until(got_done, NULL, 10000));
    CHECK_EQ(s_done_outcome, PRN_JOB_OK);
    CHECK(S.queries == q + 1);
    CHECK(fh_now() - t0 >= 4000);
    CHECK(!S.hung);
    free(page);
}

/* "print_mm_s" shortens the hold for a printer measured to be faster. */
static void t_print_speed_config(void) {
    setup(1, 185);
    SIM_PRINT_MS = 800;                        /* a fast printer */
    CHECK_EQ(drv(PRN_DRV_CONNECT, (uint32_t)ADDR, (uint32_t)"{\"print_mm_s\":50}", 0), PEX_EPENDING);
    CHECK(fh_run_until(connected, NULL, 20000));
    fh_run(200);
    prn_job_t j;
    fill_job(&j, 76, 50);
    j.height_mm100 = 2540;
    j.copies = 1;
    uint8_t *page = make_page(76, 50);
    int rc;
    uint32_t g = fh_now();
    while ((rc = drv(PRN_DRV_JOB_BEGIN, (uint32_t)&j, 0, 0)) != 1 && fh_now() - g < 8000) { fh_run(20); }
    CHECK_EQ(rc, 1);
    CHECK_EQ(feed_all(page, 76 * 50, 76), 0);
    CHECK_EQ(drv(PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    while (!S.prints && fh_now() < 60000) { fh_run(10); }
    uint32_t t0 = fh_now();
    CHECK(fh_run_until(got_done, NULL, 10000));
    CHECK(fh_now() - t0 < 3600);               /* 1 s minimum + 1.5 s margin, not 4.1 s */
    free(page);
}

static void print_page(int wb, int h, uint16_t mtu, int pipe_18f0) {
    setup(pipe_18f0, mtu);
    connect_printer();
    fh_run(200);
    prn_job_t j;
    fill_job(&j, wb, h);
    uint8_t *page = make_page(wb, h);
    CHECK_EQ(begin(&j), 1);
    CHECK_EQ(feed_all(page, (size_t)wb * h, wb), 0);
    CHECK_EQ(drv(PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    CHECK(fh_run_until(got_done, NULL, 120000));
    CHECK_EQ(s_done_outcome, PRN_JOB_OK);
    CHECK_STR(S.size, "101.6 mm,152.4 mm");
    CHECK_STR(S.gap, "3 mm,0 mm");
    CHECK_EQ(S.density, 10);
    CHECK_EQ(S.cls, 1);
    CHECK_EQ(S.width_bytes, wb);
    CHECK_EQ(S.height, h);
    CHECK_EQ(S.mode, 1);
    CHECK_EQ(S.prints, 1);
    CHECK_EQ(S.copies, 2);
    CHECK(!S.hung);                            /* nothing reached it while it printed */
    CHECK(S.page && S.page_n == (size_t)wb * h && !memcmp(S.page, page, S.page_n));
    if (getenv("PL70E_PBM") && S.page) {
        FILE *f = fopen(getenv("PL70E_PBM"), "wb");
        fprintf(f, "P4\n%d %d\n", wb * 8, h);
        for (size_t i = 0; i < S.page_n; i++) { fputc(S.page[i] ^ 0xff, f); }   /* PBM: 1 = black */
        fclose(f);
    }
    free(page);
}

static void t_print_small(void) { print_page(104, 200, 185, 1); }
static void t_print_full_4x6(void) { print_page(104, 1218, 185, 1); }    /* 126 KB */
static void t_print_3x1_mtu23(void) { print_page(76, 203, 23, 1); }     /* default MTU, 3 in wide */
static void t_print_fff0_pipe(void) { print_page(104, 64, 185, 0); }    /* no 18f0 service */

static void t_print_refused_no_paper(void) {
    setup(1, 185);
    S.status = 1;
    connect_printer();
    fh_run(500);
    prn_job_t j;
    fill_job(&j, 104, 10);
    CHECK_EQ(drv(PRN_DRV_JOB_BEGIN, (uint32_t)&j, 0, 0), PEX_EAGAIN);         /* asks first, like the app */
    fh_run(300);
    CHECK_EQ(drv(PRN_DRV_JOB_BEGIN, (uint32_t)&j, 0, 0), PRN_DRV_ENOTREADY);  /* still out of paper */
}

static void t_cancel_pads_bitmap(void) {
    setup(1, 185);
    connect_printer();
    fh_run(200);
    prn_job_t j;
    fill_job(&j, 104, 400);
    uint8_t *page = make_page(104, 400);
    CHECK_EQ(begin(&j), 1);
    CHECK(drv(PRN_DRV_JOB_FEED, (uint32_t)page, 104 * 10, 0) > 0);
    CHECK_EQ(drv(PRN_DRV_CANCEL, 0, 0, 0), PEX_OK);
    CHECK_EQ(s_done_outcome, PRN_JOB_CANCELLED);
    fh_run(30000);
    /* the BITMAP was completed with white rows, nothing was printed, and the
     * printer still answers */
    CHECK(S.page && S.page_n == 104u * 400);
    CHECK_EQ(S.prints, 0);
    CHECK(S.page && S.page[104 * 399] == 0xff);
    /* and the next job prints (its JOB_BEGIN asks READSTA first: the printer answers) */
    s_done = 0;
    fill_job(&j, 104, 50);
    CHECK_EQ(begin(&j), 1);
    CHECK_EQ(feed_all(page, 104 * 50, 104), 0);
    CHECK_EQ(drv(PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    CHECK(fh_run_until(got_done, NULL, 60000));
    CHECK_EQ(S.prints, 1);
    free(page);
}

static void t_link_loss_fails_job_and_reconnects(void) {
    setup(1, 185);
    connect_printer();
    fh_run(200);
    prn_job_t j;
    fill_job(&j, 104, 300);
    uint8_t *page = make_page(104, 300);
    CHECK_EQ(begin(&j), 1);
    drv(PRN_DRV_JOB_FEED, (uint32_t)page, 104 * 20, 0);
    fh_run(50);
    fh_ble_peer_disconnect(S.p);
    fh_run(100);
    CHECK_EQ(s_done, 1);
    CHECK_EQ(s_done_outcome, PRN_JOB_FAILED);
    s_last_state[0] = 0;
    CHECK(fh_run_until(connected, NULL, 30000));
    free(page);
}

static void t_silent_printer_fails_the_job(void) {
    setup(1, 185);
    connect_printer();
    fh_run(200);
    S.status = -1;           /* like the bench unit after its first job */
    prn_job_t j;
    fill_job(&j, 104, 20);
    uint8_t *page = make_page(104, 20);
    CHECK_EQ(begin(&j), 1);
    CHECK_EQ(feed_all(page, 104 * 20, 104), 0);
    CHECK_EQ(drv(PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    CHECK(fh_run_until(got_done, NULL, 90000));   /* 2 x 4x6 hold (46 s), then 30 s of silence */
    CHECK_EQ(s_done_outcome, PRN_JOB_FAILED);
    CHECK_HAS(s_done_msg, "isn't answering");
    free(page);
}

/* A printer that answers READSTA but never says ready again: the job is
 * taken as printed after DONE_TIMEOUT_MS, as before. */
static void t_busy_printer_times_out_ok(void) {
    setup(1, 185);
    connect_printer();
    fh_run(200);
    prn_job_t j;
    fill_job(&j, 104, 20);
    uint8_t *page = make_page(104, 20);
    CHECK_EQ(begin(&j), 1);
    CHECK_EQ(feed_all(page, 104 * 20, 104), 0);
    S.status = 3;            /* answers, but with a state that is neither ready nor an error */
    CHECK_EQ(drv(PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    CHECK(fh_run_until(got_done, NULL, 120000));  /* the hold, then DONE_TIMEOUT_MS */
    CHECK_EQ(s_done_outcome, PRN_JOB_OK);
    CHECK_HAS(s_done_msg, "did not confirm");
    free(page);
}

/* Gone quiet before a job (like the bench unit after its paper was
 * reloaded): the status says so and a job is refused, not sent. */
static void t_quiet_printer_refuses_a_job(void) {
    setup(1, 185);
    connect_printer();
    fh_run(200);
    S.status = -1;
    fh_run(20000);           /* its last answer is stale */
    prn_job_t j;
    fill_job(&j, 104, 10);
    int rc = begin(&j);      /* asks; no answers */
    CHECK(rc == PRN_DRV_ENOTREADY || rc == PEX_EAGAIN);
    CHECK_EQ(begin(&j), PRN_DRV_ENOTREADY);
    char st[512];
    CHECK(drv(PRN_DRV_STATUS, 0, (uint32_t)st, sizeof st) > 0);
    CHECK_HAS(st, "\"unanswered\":");
    CHECK_HAS(st, "\"quiet_ms\":");
    /* it answers again: the refusal asked once more, so the next job goes */
    S.status = 0;
    fh_run(5000);            /* the query still pending times out */
    CHECK_EQ(begin(&j), PRN_DRV_ENOTREADY);
    fh_run(500);
    CHECK(drv(PRN_DRV_STATUS, 0, (uint32_t)st, sizeof st) > 0);
    CHECK(!strstr(st, "unanswered"));
    CHECK_EQ(begin(&j), 1);
    CHECK_EQ(drv(PRN_DRV_CANCEL, 0, 0, 0), PEX_OK);
    CHECK(fh_run_until(got_done, NULL, 10000));
    fh_run(2000);
}

/* The ESC !? byte is a code (the Nelko app's table), with the TSPL bits as
 * a fallback for bytes outside it. */
static void t_status_codes(void) {
    CHECK_EQ(pl70e_status_from_byte(0x00), PL70E_ST_READY);
    CHECK_EQ(pl70e_status_from_byte(0x30), PL70E_ST_READY);      /* low battery: prints */
    CHECK_EQ(pl70e_status_from_byte(0x01), PL70E_ST_LIDOPEN);
    CHECK_EQ(pl70e_status_from_byte(0x02), PL70E_ST_PRINTING);
    CHECK_EQ(pl70e_status_from_byte(0x20), PL70E_ST_PRINTING);
    CHECK_EQ(pl70e_status_from_byte(0x04), PL70E_ST_NOPAPER);
    CHECK_EQ(pl70e_status_from_byte(0x24), PL70E_ST_NOPAPER);
    CHECK_EQ(pl70e_status_from_byte(0x06), PL70E_ST_NOPAPER);    /* orbsa: 0x02 rides along */
    CHECK_EQ(pl70e_status_from_byte(0x10), PL70E_ST_PAUSED);
    CHECK_EQ(pl70e_status_from_byte(0x80), PL70E_ST_OVERHEAT);
    CHECK_EQ(pl70e_status_from_byte(0x03), PL70E_ST_ERROR);
    CHECK_EQ(pl70e_status_from_byte(0x90), PL70E_ST_ERROR);
    pl70e_rx_t rx = { 0 };
    uint8_t b = 0x90;
    CHECK_EQ(pl70e_rx_feed(&rx, &b, 1), PL70E_ST_ERROR);         /* a lone high byte is a status too */
}

/* A write the gateway reports failed never reached the printer. Mid-BITMAP
 * that leaves the printer waiting for image bytes and deaf to READSTA: the
 * job fails, white fills the gap, and the printer answers again. */
static void t_failed_write_resyncs_the_printer(void) {
    setup(1, 185);
    connect_printer();
    fh_run(200);
    prn_job_t j;
    fill_job(&j, 76, 203);
    uint8_t *page = make_page(76, 203);
    CHECK_EQ(begin(&j), 1);
    CHECK(drv(PRN_DRV_JOB_FEED, (uint32_t)page, 76 * 20, 0) > 0);
    fh_ble_peer_fail_writes(S.p, 1);
    int rc = feed_all(page, 76 * 203, 76);
    CHECK(rc == 0 || rc == PEX_ESTATE);       /* the job may already have failed under it */
    CHECK(fh_run_until(got_done, NULL, 10000));
    CHECK_EQ(s_done_outcome, PRN_JOB_FAILED);
    CHECK_HAS(s_done_msg, "didn't reach the printer");
    fh_run(8000);
    CHECK_EQ(S.bmp_left, 0u);                 /* the printer's BITMAP is complete */
    CHECK_EQ(S.prints, 0);                    /* and nothing printed */
    char st[512];
    CHECK(drv(PRN_DRV_STATUS, 0, (uint32_t)st, sizeof st) > 0);
    CHECK_HAS(st, "\"printer\":\"ready\"");   /* READSTA answered again */
    CHECK_HAS(st, "\"writes_failed\":1");
    /* and the next job prints whole */
    s_done = 0;
    CHECK_EQ(begin(&j), 1);
    CHECK_EQ(feed_all(page, 76 * 203, 76), 0);
    CHECK_EQ(drv(PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    CHECK(fh_run_until(got_done, NULL, 90000));
    CHECK_EQ(s_done_outcome, PRN_JOB_OK);
    CHECK(S.page && S.page_n == (size_t)76 * 203 && !memcmp(S.page, page, S.page_n));
    CHECK_EQ(S.prints, 1);
    CHECK(S.set_gap_on >= 1);                 /* the app's SET GAP ON is in every job */
    free(page);
}

/* Like the app: a job only on a fresh "ready", and not straight after an
 * error clears (paper reloaded). PEX_EAGAIN asks the hub to call again. */
static void t_job_waits_for_a_fresh_ready(void) {
    setup(1, 185);
    connect_printer();
    fh_run(200);
    S.status = 1;
    fh_run(9000);                              /* its last "ready" is stale */
    prn_job_t j;
    fill_job(&j, 104, 10);
    CHECK_EQ(drv(PRN_DRV_JOB_BEGIN, (uint32_t)&j, 0, 0), PEX_EAGAIN);   /* asks first */
    fh_run(500);
    CHECK_EQ(drv(PRN_DRV_JOB_BEGIN, (uint32_t)&j, 0, 0), PRN_DRV_ENOTREADY);   /* out of paper */
    S.status = 0;                              /* paper back in */
    fh_run(7000);
    /* call again like the hub, every tick: the "no paper" it knows is old
     * news, so it asks, then lets the printer settle */
    int again = 0, refused = 0, rc;
    uint32_t t0 = fh_now(), ready_at = 0;
    while ((rc = drv(PRN_DRV_JOB_BEGIN, (uint32_t)&j, 0, 0)) != 1 && fh_now() - t0 < 10000) {
        again += rc == PEX_EAGAIN;
        refused += rc == PRN_DRV_ENOTREADY;
        if (!ready_at && strstr(s_last_status, "\"printer\":\"ready\"")) { ready_at = fh_now(); }
        fh_run(20);
    }
    CHECK_EQ(rc, 1);
    CHECK(again > 0);
    CHECK_EQ(refused, 0);                      /* the old "no paper" didn't refuse it */
    CHECK(ready_at && fh_now() - ready_at >= 1400);   /* it settled after turning ready */
    CHECK_EQ(drv(PRN_DRV_CANCEL, 0, 0, 0), PEX_OK);
    CHECK(fh_run_until(got_done, NULL, 10000));
    fh_run(2000);
}

/* A printer left inside a BITMAP (here: the link dropped mid-job) answers
 * nothing; connecting with "recover":"resync" fills the BITMAP with white
 * and it answers again. */
static void t_recover_resync(void) {
    setup(1, 185);
    connect_printer();
    fh_run(200);
    prn_job_t j;
    fill_job(&j, 104, 300);
    uint8_t *page = make_page(104, 300);
    CHECK_EQ(begin(&j), 1);
    CHECK(drv(PRN_DRV_JOB_FEED, (uint32_t)page, 104 * 30, 0) > 0);
    fh_run(1000);
    fh_ble_peer_disconnect(S.p);
    CHECK(fh_run_until(got_done, NULL, 10000));
    CHECK(S.bmp_left > 0);                     /* stuck */
    drv(PRN_DRV_DISCONNECT, 0, 0, 0);
    fh_run(500);
    s_last_state[0] = 0;
    CHECK_EQ(drv(PRN_DRV_CONNECT, (uint32_t)ADDR, (uint32_t)"{\"recover\":\"resync\"}", 0), PEX_EPENDING);
    CHECK(fh_run_until(connected, NULL, 20000));
    fh_run(60000);
    CHECK_EQ(S.bmp_left, 0u);
    CHECK_EQ(S.prints, 0);
    char st[512];
    CHECK(drv(PRN_DRV_STATUS, 0, (uint32_t)st, sizeof st) > 0);
    CHECK_HAS(st, "\"printer\":\"ready\"");
    CHECK(fh_log_has("recover step"));
    free(page);
}

/* The gateway's module event ring drops the oldest event when it overflows,
 * silently (parent B6). A lost write-done must not leave the job "printing"
 * for ever: on the bench a 3x1 job sat at sent == total until the hub gave
 * up after 120 s. */
static void t_lost_write_event(void) {
    setup(1, 185);
    connect_printer();
    fh_run(200);
    prn_job_t j;
    fill_job(&j, 76, 203);
    uint8_t *page = make_page(76, 203);
    CHECK_EQ(begin(&j), 1);
    fh_ble_peer_drop_write_done(S.p, 1);
    CHECK_EQ(feed_all(page, 76 * 203, 76), 0);
    CHECK_EQ(drv(PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    CHECK(fh_run_until(got_done, NULL, 90000));
    CHECK_EQ(s_done_outcome, PRN_JOB_OK);
    CHECK(fh_log_has("write event(s) never came"));
    CHECK(S.page && S.page_n == (size_t)76 * 203 && !memcmp(S.page, page, S.page_n));
    char st[400];
    CHECK(drv(PRN_DRV_STATUS, 0, (uint32_t)st, sizeof st) > 0);
    CHECK_HAS(st, "\"writes_lost\":1");
    free(page);
}

static void t_stop_releases(void) {
    setup(1, 185);
    connect_printer();
    fh_stop(s_drv);
    fh_run(100);
    CHECK_EQ(s_registered, 0);
    CHECK_EQ(fh_ble_listeners(), 0);
    CHECK(!fh_ble_peer_stats(S.p)->connected);
}

/* The same job through the real printer_hub: printer.connect, a job file,
 * printer.print, printer.job events. */
static void t_print_through_real_hub(void) {
    fh_reset();
    free(S.page);
    free(S.bmp);
    memset(&S, 0, sizeof S);
    SIM_PRINT_MS = 3000;
    load_hub();
    add_printer(1, 185);
    s_drv = fh_module("nelko_pl70e", pl70e__pex_main);
    fh_export(s_drv, PRN_DRV_SYM, (void *)pl70e__prn_drv);
    fh_load(s_drv);
    fh_start(s_drv, "{}");
    fh_run(50);
    char R[2048];
    CHECK(fh_op("printer.drivers", "{}", R, sizeof R) > 0);
    CHECK_HAS(R, "nelko_pl70e");
    CHECK(fh_op("printer.connect", "{\"driver\":\"nelko_pl70e\",\"addr\":\"" ADDR "\"}", R, sizeof R) > 0);
    CHECK(wait_ev("printer.state", "\"connected\"", NULL, 20000));
    int wb = 100, h = 600;      /* 800 dots: narrower than the head */
    uint8_t *page = make_page(wb, h);
    fh_fs_put("lfs:/printer/var/job.bin", page, (size_t)wb * h);
    CHECK(fh_op("printer.print", "{\"driver\":\"nelko_pl70e\",\"addr\":\"" ADDR "\",\"model\":\"pl70e\","
                "\"path\":\"printer/var/job.bin\",\"width_dots\":800,\"rows\":600,\"origin\":[16,0],"
                "\"media\":{\"w_mm\":101.6,\"h_mm\":152.4,\"gap_mm\":3,\"type\":\"gap\"},"
                "\"density\":12,\"speed\":-1,\"copies\":3,\"rotate\":0,\"options\":{}}", R, sizeof R) > 0);
    CHECK_HAS(R, "\"job\"");
    CHECK(wait_ev("printer.job", "\"done\"", NULL, 120000));
    CHECK_STR(S.size, "101.6 mm,152.4 mm");
    CHECK_EQ(S.density, 12);
    CHECK_EQ(S.x, 16);
    CHECK_EQ(S.width_bytes, wb);
    CHECK_EQ(S.height, h);
    CHECK_EQ(S.copies, 3);
    CHECK_EQ(S.prints, 1);
    CHECK(S.page && S.page_n == (size_t)wb * h && !memcmp(S.page, page, S.page_n));
    free(page);
}

/* Through the hub on a slow link, as on the bench: the hub feeds rows until
 * the driver's 512-byte ring is full and then calls JOB_END once. The PRINT
 * line has to go out even when it doesn't fit the ring at that moment (the
 * bench job stopped at sent == total with no PRINT, so nothing printed).
 * Several row widths, so the room left at JOB_END varies. */
static void t_job_end_with_full_ring(void) {
    for (int wb = 70; wb <= 80; wb++) {
        fh_reset();
        free(S.page);
        free(S.bmp);
        memset(&S, 0, sizeof S);
        SIM_PRINT_MS = 3000;
    SIM_PRINT_MS = 3000;
        s_op_ms = 60;
        load_hub();
        add_printer(1, 185);
        s_drv = fh_module("nelko_pl70e", pl70e__pex_main);
        fh_export(s_drv, PRN_DRV_SYM, (void *)pl70e__prn_drv);
        fh_load(s_drv);
        fh_start(s_drv, "{}");
        fh_run(50);
        char R[2048];
        CHECK(fh_op("printer.connect", "{\"driver\":\"nelko_pl70e\",\"addr\":\"" ADDR "\"}", R, sizeof R) > 0);
        CHECK(wait_ev("printer.state", "\"connected\"", NULL, 20000));
        int h = 203;
        uint8_t *page = make_page(wb, h);
        fh_fs_put("lfs:/printer/var/job.bin", page, (size_t)wb * h);
        char q[600];
        snprintf(q, sizeof q, "{\"driver\":\"nelko_pl70e\",\"addr\":\"" ADDR "\",\"model\":\"pl70e\","
                 "\"path\":\"printer/var/job.bin\",\"width_dots\":%d,\"rows\":%d,\"origin\":[0,0],"
                 "\"media\":{\"w_mm\":76.2,\"h_mm\":25.4,\"gap_mm\":3,\"type\":\"gap\"},"
                 "\"density\":8,\"speed\":-1,\"copies\":1,\"rotate\":0,\"options\":{}}", wb * 8, h);
        CHECK(fh_op("printer.print", q, R, sizeof R) > 0);
        CHECK(wait_ev("printer.job", "\"done\"", NULL, 120000));
        CHECK(!S.hung);
        CHECK_EQ(S.prints, 1);
        CHECK(S.page && S.page_n == (size_t)wb * h && !memcmp(S.page, page, S.page_n));
        free(page);
    }
    s_op_ms = 0;
}

int main(void) {
    RUN(t_proto);
    RUN(t_info);
    RUN(t_connect_and_status);
    RUN(t_silent_while_printing);
    RUN(t_print_speed_config);
    RUN(t_print_small);
    RUN(t_print_full_4x6);
    RUN(t_print_3x1_mtu23);
    RUN(t_print_fff0_pipe);
    RUN(t_print_refused_no_paper);
    RUN(t_cancel_pads_bitmap);
    RUN(t_link_loss_fails_job_and_reconnects);
    RUN(t_silent_printer_fails_the_job);
    RUN(t_busy_printer_times_out_ok);
    RUN(t_quiet_printer_refuses_a_job);
    RUN(t_status_codes);
    RUN(t_failed_write_resyncs_the_printer);
    RUN(t_job_waits_for_a_fresh_ready);
    RUN(t_recover_resync);
    RUN(t_lost_write_event);
    RUN(t_stop_releases);
    RUN(t_print_through_real_hub);
    RUN(t_job_end_with_full_ring);
    T_DONE();
}
