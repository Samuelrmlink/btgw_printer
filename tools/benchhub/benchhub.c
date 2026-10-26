/* A minimal stand-in for printer_hub, for bench tests on a gateway whose PEX
 * pool can't hold the real hub next to a driver. Same module name and
 * prn_hub export, so a driver can't tell. One driver, one job:
 *   printer.connect {"driver","addr"}
 *   printer.print   {"path","width_dots","rows","media":{"w_mm","h_mm","gap_mm"},
 *                    "density","copies","origin":[x,y]}
 * Not shipped: drivers/hub/printer_hub.c is the real thing. */
#include "pex.h"
#include "printer_driver.h"
#include <string.h>

static pex_mod_t *s_drv;
static uint32_t s_fn;
static char s_drv_name[24];
static int s_fd = -1, s_task;
static prn_job_t s_job;
static uint8_t s_buf[416];   /* 4 rows of a 4-inch head */
static uint32_t s_have, s_off, s_rows_fed;
static uint8_t s_ended;
static char s_emit[200];

static int call(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2) {
    if (!s_drv || pex_find(s_drv_name) != s_drv) { return PEX_ENOENT; }
    return (int)(uint32_t)pex_call(s_drv, s_fn, cmd, a0, a1, a2);
}

static void emit_job(const char *state, const char *msg) {
    pex_snprintf(s_emit, sizeof s_emit, "{\"job\":%u,\"state\":\"%s\",\"sent\":%u,\"total\":%u,\"message\":\"%s\"}",
                 (unsigned)s_job.id, state, (unsigned)s_rows_fed, (unsigned)s_job.rows_total, msg ? msg : "");
    pex_bus_emit("printer.job", s_emit);
}

static void close_job(void) {
    if (s_fd >= 0) { pex_fs_close(s_fd); s_fd = -1; }
    if (s_task > 0) { pex_sched_cancel(s_task); s_task = 0; }
}

static int pump(void *arg) {
    (void)arg;
    if (s_fd < 0) { return PEX_TASK_DONE; }
    for (int guard = 0; guard < 8; guard++) {
        if (s_off == s_have) {
            if (s_rows_fed >= s_job.rows_total) {
                if (!s_ended && call(PRN_DRV_JOB_END, 0, 0, 0) == PEX_OK) {
                    s_ended = 1;
                    emit_job("printing", NULL);
                    if (s_fd >= 0) { pex_fs_close(s_fd); s_fd = -1; }
                    s_task = 0;
                    return PEX_TASK_DONE;
                }
                return PEX_TASK_AGAIN;
            }
            uint32_t want = (sizeof s_buf / s_job.row_bytes) * s_job.row_bytes;
            int n = pex_fs_read(s_fd, s_buf, want, NULL, NULL);
            if (n <= 0) { emit_job("failed", "short file"); call(PRN_DRV_CANCEL, 0, 0, 0); close_job(); return PEX_TASK_DONE; }
            s_have = (uint32_t)n / s_job.row_bytes * s_job.row_bytes;
            s_off = 0;
        }
        int rc = call(PRN_DRV_JOB_FEED, (uint32_t)(s_buf + s_off), s_have - s_off, 0);
        if (rc == PEX_EBUSY) { return PEX_TASK_AGAIN; }
        if (rc < 0) { emit_job("failed", "driver refused rows"); close_job(); return PEX_TASK_DONE; }
        s_off += (uint32_t)rc;
        s_rows_fed += (uint32_t)rc / s_job.row_bytes;
    }
    return PEX_TASK_AGAIN;
}

int prn_hub(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2) {
    switch (cmd) {
    case PRN_HUB_REGISTER: {
        pex_mod_t *m = pex_find((const char *)a0);
        if (!m) { return PEX_ENOENT; }
        uint32_t fn = pex_sym(m, PRN_DRV_SYM);
        if (!fn) { return PEX_ENOENT; }
        s_drv = m;
        s_fn = fn;
        pex_snprintf(s_drv_name, sizeof s_drv_name, "%s", (const char *)a0);
        pex_log(PEX_LOG_INFO, "benchhub: %s registered", s_drv_name);
        return PEX_OK;
    }
    case PRN_HUB_UNREGISTER:
        if (s_drv && !strcmp((const char *)a0, s_drv_name)) { s_drv = NULL; }
        return PEX_OK;
    case PRN_HUB_EMIT: {
        char topic[32];
        pex_snprintf(topic, sizeof topic, "printer.%s", (const char *)a0);
        pex_bus_emit(topic, (const char *)a1);
        return PEX_OK;
    }
    case PRN_HUB_JOB_DONE:
        emit_job(a1 == PRN_JOB_OK ? "done" : a1 == PRN_JOB_CANCELLED ? "cancelled" : "failed",
                 a2 ? (const char *)a2 : NULL);
        close_job();
        return PEX_OK;
    case PRN_HUB_KICK:
        return PEX_OK;
    }
    return PEX_ENOTSUP;
}
PEX_EXPORT(prn_hub);

static int op_connect(void *arg, const char *p, char *r, size_t cap) {
    (void)arg;
    char addr[20];
    if (!pex_json_str(p, "addr", addr, sizeof addr)) { return pex_snprintf(r, cap, "{\"error\":\"addr\"}"); }
    int rc = call(PRN_DRV_CONNECT, (uint32_t)addr, (uint32_t)"{}", 0);
    return pex_snprintf(r, cap, "{\"rc\":%d}", rc);
}

static int op_print(void *arg, const char *p, char *r, size_t cap) {
    (void)arg;
    if (s_fd >= 0) { return pex_snprintf(r, cap, "{\"error\":\"busy\"}"); }
    char path[64], lpath[72];
    int64_t v;
    double d;
    if (!pex_json_str(p, "path", path, sizeof path)) { return pex_snprintf(r, cap, "{\"error\":\"path\"}"); }
    memset(&s_job, 0, sizeof s_job);
    s_job.id++;
    static uint32_t ids;
    s_job.id = ++ids;
    pex_json_int(p, "width_dots", &v); s_job.width_dots = (uint16_t)v;
    s_job.row_bytes = (uint16_t)((s_job.width_dots + 7) / 8);
    pex_json_int(p, "rows", &v); s_job.rows_total = (uint32_t)v;
    if (pex_json_num(p, "media.w_mm", &d)) { s_job.width_mm100 = (uint32_t)(d * 100 + 0.5); }
    if (pex_json_num(p, "media.h_mm", &d)) { s_job.height_mm100 = (uint32_t)(d * 100 + 0.5); }
    if (pex_json_num(p, "media.gap_mm", &d)) { s_job.gap_mm100 = (uint32_t)(d * 100 + 0.5); }
    s_job.media_type = PRN_MEDIA_GAP;
    s_job.density = pex_json_int(p, "density", &v) ? (int8_t)v : -1;
    s_job.speed = -1;
    s_job.copies = pex_json_int(p, "copies", &v) ? (uint8_t)v : 1;
    if (pex_json_int(p, "origin[0]", &v)) { s_job.origin_x = (int16_t)v; }
    if (pex_json_int(p, "origin[1]", &v)) { s_job.origin_y = (int16_t)v; }
    s_job.black_bit = 0;
    s_job.options[0] = 0x7b; s_job.options[1] = 0x7d; s_job.options[2] = 0;
    if (!s_job.row_bytes || !s_job.rows_total) { return pex_snprintf(r, cap, "{\"error\":\"size\"}"); }
    pex_snprintf(lpath, sizeof lpath, "lfs:/%s", path);
    s_fd = pex_fs_open(lpath, PEX_FS_READ, NULL, NULL);
    if (s_fd < 0) { int e = s_fd; s_fd = -1; return pex_snprintf(r, cap, "{\"error\":\"open %d\"}", e); }
    int rc = call(PRN_DRV_JOB_BEGIN, (uint32_t)&s_job, 0, 0);
    if (rc < 0) { pex_fs_close(s_fd); s_fd = -1; return pex_snprintf(r, cap, "{\"error\":\"begin %d\"}", rc); }
    s_have = s_off = s_rows_fed = 0;
    s_ended = 0;
    s_task = pex_sched_every(10, pump, NULL);
    emit_job("sending", NULL);
    return pex_snprintf(r, cap, "{\"job\":%u}", (unsigned)s_job.id);
}

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host; (void)arg;
    if (reason == PEX_INIT) {
        pex_bus_op("printer.connect", op_connect, NULL);
        pex_bus_op("printer.print", op_print, NULL);
    }
    if (reason == PEX_APP_STOP || reason == PEX_DEINIT) { close_job(); }
    return PEX_OK;
}
