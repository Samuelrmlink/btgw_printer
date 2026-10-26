/* The Phomemo D30 driver on the fake host against a simulated D30: the
 * preamble once per link, one GS v 0 block per label with the rows intact,
 * copies, a stall, and a link drop. */
#include "t.h"
#include "fakehost.h"
#include "fh_classic.h"
#include "t_hubstub.h"
#include "vendors/phomemo/d30/d30_proto.h"

int d30__pex_main(const pex_host_t *host, uint32_t reason, void *arg);
int d30__prn_drv(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2);

#define ADDR "DC:0D:30:00:00:30"

static struct {
    uint8_t in[32768];
    size_t n;
} PR;

static void pr_rx(fh_cl_peer_t *p, const uint8_t *d, size_t n, void *ctx) {
    (void)p; (void)ctx;
    if (PR.n + n <= sizeof PR.in) { memcpy(PR.in + PR.n, d, n); PR.n += n; }
}

static fh_cl_peer_t *s_peer;
static pex_mod_t *s_drv;

static void setup(void) {
    fh_reset();
    fh_verbose(getenv("FH_V") != NULL);
    memset(&PR, 0, sizeof PR);
    hs_reset();
    fh_cl_peer_cfg_t c = {.addr = ADDR, .name = "D30", .cod = 0x000680, .rssi = -55,
                          .sdp_channel = 1, .on_rx = pr_rx};
    s_peer = fh_cl_peer_add(&c);
    s_drv = fh_module("phomemo_d30", d30__pex_main);
    fh_export(s_drv, PRN_DRV_SYM, (void *)d30__prn_drv);
    fh_load(s_drv);
    fh_start(s_drv, "{}");
    fh_run(50);
    CHECK_STR(HS.registered, "phomemo_d30");
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_CONNECT, (uint32_t)(uintptr_t)ADDR, (uint32_t)(uintptr_t)"{}", 0), PEX_EPENDING);
    CHECK(fh_run_until(hs_state_is, "connected", 3000));
    fh_run(50);
}

static uint8_t s_bm[12 * 320];
static prn_job_t job(uint32_t id, uint32_t rows, int copies) {
    for (uint32_t i = 0; i < rows * 12; i++) { s_bm[i] = (uint8_t)(i * 31 + 7); }
    prn_job_t j;
    memset(&j, 0, sizeof j);
    j.id = id;
    snprintf(j.addr, sizeof j.addr, "%s", ADDR);
    snprintf(j.model, sizeof j.model, "d30");
    j.width_dots = 96;
    j.row_bytes = 12;
    j.rows_total = rows;
    j.width_mm10 = 120;
    j.height_mm10 = 400;
    j.gap_mm10 = 40;
    j.density = -1;
    j.speed = -1;
    j.copies = (uint8_t)copies;
    return j;
}

static void t_preamble_and_job(void) {
    setup();
    const uint8_t *pre;
    size_t pn = d30_preamble(&pre);
    CHECK_EQ(PR.n, pn);
    CHECK(memcmp(PR.in, pre, pn) == 0);
    char caps[1024];
    CHECK(hs_drv(s_drv, PRN_DRV_INFO, (uint32_t)(uintptr_t)caps, sizeof caps, 0) > 0);
    CHECK_HAS(caps, "\"black_bit\":1");

    prn_job_t j = job(5, 320, 1);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_BEGIN, (uint32_t)(uintptr_t)&j, 0, 0), PEX_OK);
    CHECK_EQ(hs_feed_all(s_drv, s_bm, 12 * 320, 500), 0);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    CHECK(fh_run_until(hs_done, NULL, 3000));
    CHECK_EQ(HS.done_outcome, PRN_JOB_OK);
    uint8_t head[14];
    d30_job_head(320, head, sizeof head);
    CHECK_EQ(PR.n, pn + 14 + 12 * 320);
    CHECK(memcmp(PR.in + pn, head, 14) == 0);
    CHECK(memcmp(PR.in + pn + 14, s_bm, 12 * 320) == 0);
}

static void t_copies_stall(void) {
    setup();
    size_t start = PR.n;
    prn_job_t j = job(6, 240, 2);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_BEGIN, (uint32_t)(uintptr_t)&j, 0, 0), PEX_OK);
    fh_cl_peer_stall(s_peer, 1);
    int r = 0, busy = 0;
    for (int i = 0; i < 20; i++) {
        r = hs_drv(s_drv, PRN_DRV_JOB_FEED, (uint32_t)(uintptr_t)s_bm, 12 * 240, 0);
        if (r == PEX_EBUSY) { busy++; }
        if (r > 0) { break; }
    }
    CHECK(r > 0 && r % 12 == 0);
    size_t off = r > 0 ? (size_t)r : 0;
    CHECK(busy == 0 || off > 0);
    fh_cl_peer_stall(s_peer, 0);
    /* feed the rest from where the driver took up to */
    CHECK_EQ(hs_feed_all(s_drv, s_bm + off, 12 * 240 - off, 500), 0);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_END, 0, 0, 0), PEX_OK);
    CHECK_EQ(hs_feed_all(s_drv, s_bm, 12 * 240, 500), 0);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_END, 1, 0, 0), PEX_OK);
    CHECK(fh_run_until(hs_done, NULL, 3000));
    CHECK_EQ(HS.done_outcome, PRN_JOB_OK);
    /* no preamble again; two heads, each followed by the whole bitmap */
    size_t per = 14 + 12 * 240;
    CHECK_EQ(PR.n - start, 2 * per);
    uint8_t head[14];
    d30_job_head(240, head, sizeof head);
    for (int k = 0; k < 2; k++) {
        CHECK(memcmp(PR.in + start + k * per, head, 14) == 0);
        CHECK(memcmp(PR.in + start + k * per + 14, s_bm, 12 * 240) == 0);
    }
}

static void t_drop(void) {
    setup();
    prn_job_t j = job(8, 320, 1);
    CHECK_EQ(hs_drv(s_drv, PRN_DRV_JOB_BEGIN, (uint32_t)(uintptr_t)&j, 0, 0), PEX_OK);
    fh_cl_peer_stall(s_peer, 1);
    hs_drv(s_drv, PRN_DRV_JOB_FEED, (uint32_t)(uintptr_t)s_bm, 12 * 100, 0);
    fh_cl_peer_close(s_peer);
    fh_run(30);
    CHECK_EQ(HS.ndone, 1);
    CHECK_EQ(HS.done_outcome, PRN_JOB_FAILED);
    /* after the reconnect the preamble goes out again */
    fh_cl_peer_stall(s_peer, 0);
    size_t before = PR.n;
    fh_run(9000);
    CHECK(hs_state_is("connected"));
    const uint8_t *pre;
    size_t pn = d30_preamble(&pre);
    CHECK(PR.n - before >= pn);
    CHECK(memcmp(PR.in + PR.n - pn, pre, pn) == 0);
}

int main(void) {
    RUN(t_preamble_and_job);
    RUN(t_copies_stall);
    RUN(t_drop);
    T_DONE();
}
