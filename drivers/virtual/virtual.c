/* A printer that is not there: for trying the web app and the hub on a
 * board (or in tests) with no printer. It "connects" at once, takes rows at
 * a limited rate (so the hub's back-pressure runs), writes the first copy's
 * bitmap to lfs:/printer/var/virtual.bin, and reports a CRC-32 of each copy.
 * It lets the hub feed one pass per copy (no native copies), which is the
 * other path real TSPL drivers do not take.
 *
 * Job options: {"rate": bytes per 5 ms tick (default 1024),
 *               "fail_at": fail after this many rows, "no_file": true}. */
#include "pex.h"
#include "core/prn_core.h"

#include <string.h>

#define SELF "printer_virtual"
#define ADDR "00:00:00:00:5E:01"

static prn_hub_t s_hub;
static int s_task, s_connected, s_connecting;
static uint32_t s_t_conn;
static prn_job_t s_job;
static int s_in_job, s_done_at, s_fd = -1;
static uint32_t s_rate, s_budget, s_rows, s_fail_at, s_crc, s_crcs[4];
static uint32_t s_jobs_done;
static char s_emit[320];

static uint32_t crc32_upd(uint32_t c, const uint8_t *p, size_t n) {
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) { c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    }
    return ~c;
}

static void emit_state(const char *state) {
    pex_snprintf(s_emit, sizeof s_emit, "{\"driver\":\"%s\",\"addr\":\"%s\",\"model\":\"virtual\",\"state\":\"%s\"}",
                 SELF, ADDR, state);
    prn_hub_emit(&s_hub, "state", s_emit);
}

static int info(char *out, size_t cap) {
    return pex_snprintf(out, cap,
        "{\"driver\":\"" SELF "\",\"abi\":1,\"models\":["
        "{\"id\":\"virtual\",\"vendor\":\"btgw_printer\",\"name\":\"Virtual printer\",\"transport\":\"none\","
        "\"dpi\":203,\"head_dots\":832,\"min_len_mm\":5,\"max_len_mm\":400,\"media\":[\"gap\",\"continuous\",\"blackmark\"],"
        "\"density\":{\"min\":0,\"max\":15,\"default\":8},\"speed\":{\"min\":1,\"max\":5,\"default\":3},"
        "\"black_bit\":0,\"copies\":false,\"status\":[\"battery\"],\"presets\":[]}]}");
}

static int status(char *out, size_t cap) {
    return pex_snprintf(out, cap,
        "[{\"driver\":\"" SELF "\",\"addr\":\"" ADDR "\",\"model\":\"virtual\",\"state\":\"%s\",\"battery\":100,"
        "\"jobs\":%u,\"last_crc\":\"%08x\",\"last_rows\":%u}]",
        s_in_job ? "printing" : s_connected ? "connected" : "idle", (unsigned)s_jobs_done, (unsigned)s_crc,
        (unsigned)s_rows);
}

static int feed(const uint8_t *rows, uint32_t n) {
    if (!s_in_job) { return PEX_ESTATE; }
    uint32_t rb = s_job.row_bytes;
    uint32_t take = n < s_budget ? n : s_budget;
    take -= take % rb;
    if (!take) {
        if (s_budget >= rb) { return 0; }
        return PEX_EBUSY;
    }
    if (s_fail_at && s_rows + take / rb > s_fail_at) {
        s_in_job = 0;
        prn_hub_job_done(&s_hub, s_job.id, PRN_JOB_FAILED, "paper jam (fail_at)");
        return PEX_ESTATE;
    }
    s_crc = crc32_upd(s_crc, rows, take);
    if (s_fd >= 0 && s_job.copy == 0) { pex_fs_write(s_fd, rows, take, NULL, NULL); }
    s_budget -= take;
    s_rows += take / rb;
    return (int)take;
}

int prn_drv(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2) {
    switch (cmd) {
    case PRN_DRV_INFO:
        return info((char *)(uintptr_t)a0, a1);
    case PRN_DRV_SCAN:
        if (a0) {
            pex_snprintf(s_emit, sizeof s_emit, "{\"driver\":\"" SELF "\",\"addr\":\"" ADDR "\",\"name\":\"Virtual\","
                                                "\"rssi\":0,\"model\":\"virtual\"}");
            prn_hub_emit(&s_hub, "found", s_emit);
        }
        return PEX_OK;
    case PRN_DRV_DEVICES:
        return pex_snprintf((char *)(uintptr_t)a0, a1,
                            "[{\"driver\":\"" SELF "\",\"addr\":\"" ADDR "\",\"name\":\"Virtual\",\"rssi\":0,"
                            "\"model\":\"virtual\",\"transport\":\"none\"}]");
    case PRN_DRV_CONNECT:
        if (s_connected) { emit_state("connected"); return PEX_OK; }
        s_connecting = 1;
        s_t_conn = pex_ms();
        emit_state("connecting");
        return PEX_EPENDING;
    case PRN_DRV_DISCONNECT:
        s_connected = s_connecting = 0;
        emit_state("idle");
        return PEX_OK;
    case PRN_DRV_STATUS:
        return status((char *)(uintptr_t)a1, a2);
    case PRN_DRV_JOB_BEGIN: {
        const prn_job_t *j = (const prn_job_t *)(uintptr_t)a0;
        if (!s_connected) { return PEX_ESTATE; }
        if (s_in_job) { return PEX_EBUSY; }
        s_job = *j;
        int64_t v;
        s_rate = pex_json_int(j->options, "rate", &v) && v > 0 ? (uint32_t)v : 1024u;
        s_fail_at = pex_json_int(j->options, "fail_at", &v) && v > 0 ? (uint32_t)v : 0;
        int nofile = 0;
        pex_json_bool(j->options, "no_file", &nofile);
        s_budget = s_rate;
        s_rows = 0;
        s_crc = 0;
        s_in_job = 1;
        s_done_at = 0;
        s_fd = nofile ? -1 : pex_fs_open("lfs:/printer/var/virtual.bin", PEX_FS_WRITE, NULL, NULL);
        emit_state("printing");
        return 0;          /* the hub feeds each copy */
    }
    case PRN_DRV_JOB_FEED:
        return feed((const uint8_t *)(uintptr_t)a0, a1);
    case PRN_DRV_JOB_END:
        if (!s_in_job) { return PEX_ESTATE; }
        if (a0 < 4) { s_crcs[a0] = s_crc; }
        if (s_fd >= 0 && a0 == 0) { pex_fs_close(s_fd); s_fd = -1; }
        pex_snprintf(s_emit, sizeof s_emit, "{\"driver\":\"" SELF "\",\"addr\":\"" ADDR "\",\"copy\":%u,"
                                            "\"rows\":%u,\"crc\":\"%08x\"}",
                     (unsigned)a0, (unsigned)s_rows, (unsigned)s_crc);
        prn_hub_emit(&s_hub, "status", s_emit);
        if (a0 + 1u >= s_job.copies) { s_done_at = (int)(pex_ms() + 20u); if (!s_done_at) { s_done_at = 1; } }
        else { s_rows = 0; s_crc = 0; }
        return PEX_OK;
    case PRN_DRV_CANCEL:
        if (s_fd >= 0) { pex_fs_close(s_fd); s_fd = -1; }
        s_in_job = 0;
        s_done_at = 0;
        emit_state("connected");
        return PEX_OK;
    }
    return PEX_EINVAL;
}
PEX_EXPORT(prn_drv);

static int tick(void *arg) {
    (void)arg;
    if (!prn_hub_ok(&s_hub)) { prn_hub_attach(&s_hub, pex_name(pex_self())); }
    if (s_connecting && pex_ms() - s_t_conn >= 10) {
        s_connecting = 0;
        s_connected = 1;
        emit_state("connected");
    }
    if (s_in_job) {
        uint32_t was = s_budget;
        s_budget = s_rate;
        if (!was) { prn_hub_kick(&s_hub); }
        if (s_done_at && (int32_t)(pex_ms() - (uint32_t)s_done_at) >= 0) {
            s_in_job = 0;
            s_done_at = 0;
            s_jobs_done++;
            emit_state("connected");
            prn_hub_job_done(&s_hub, s_job.id, PRN_JOB_OK, NULL);
        }
    }
    return PEX_TASK_AGAIN;
}

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host; (void)arg;
    switch (reason) {
    case PEX_APP_RUN:
        s_connected = s_connecting = s_in_job = 0;
        s_fd = -1;
        prn_hub_attach(&s_hub, pex_name(pex_self()));
        s_task = pex_sched_every(5, tick, NULL);
        return s_task > 0 ? PEX_OK : s_task;
    case PEX_APP_STOP:
        if (s_in_job) { prn_hub_job_done(&s_hub, s_job.id, PRN_JOB_CANCELLED, "driver stopped"); }
        s_in_job = 0;
        if (s_fd >= 0) { pex_fs_close(s_fd); s_fd = -1; }
        prn_hub_detach(&s_hub, pex_name(pex_self()));
        if (s_task > 0) { pex_sched_cancel(s_task); }
        s_task = 0;
        return PEX_OK;
    }
    return PEX_OK;
}
