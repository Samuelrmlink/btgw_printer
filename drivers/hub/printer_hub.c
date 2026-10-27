/* printer_hub: the printer.* ops and events, the driver registry, and the
 * job streamer. Drivers register through prn_hub (drivers/include/
 * printer_driver.h). A print job is a bitmap file the web app wrote; the hub
 * checks it, reads it in chunks of whole rows (LittleFS at once, the card
 * through callbacks) and feeds the rows to the driver, which encodes them
 * and owns the link. docs/architecture.md has the op and event shapes. */
#include "pex.h"
#include "core/prn_core.h"

#include <string.h>

#define DRV_MAX 8
#define MODEL_MAX 4
#define CHUNK 2048
#define MAX_WIDTH 4096
#define DONE_TIMEOUT_MS 120000u
#define PROGRESS_MS 250u

typedef struct {
    char id[24];
    int8_t black_bit, dmin, dmax, ddef, smin, smax, sdef, has_speed, copies;
    int8_t classic;          /* transport "classic": never connected without the user */
} model_t;

typedef struct {
    char name[32];
    pex_mod_t *m;
    uint32_t fn;
    model_t models[MODEL_MAX];
    int nmodels;
} drv_t;

enum { J_IDLE = 0, J_SCAN, J_FEED, J_WAIT };

static drv_t s_drv[DRV_MAX];
static char s_scratch[2048];
static char s_emit[512];
static int s_task;
static uint32_t s_next_job = 1;

static struct {
    int st;
    uint32_t id;
    drv_t *d;
    prn_job_t job;
    char path[132];
    int sd, fd, pending, native;
    uint8_t buf[CHUNK];
    uint32_t buf_len, buf_off;
    uint32_t rows_read;      /* rows read in this pass (or the scan) */
    uint32_t rows_fed;       /* rows taken by the driver in this pass */
    uint32_t sent, total;    /* bytes, over every pass */
    uint32_t t_start, t_progress, t_wait;
    int err;
} J;

/* ---- drivers ---- */
static drv_t *drv_by_name(const char *n) {
    for (int i = 0; i < DRV_MAX; i++) {
        if (s_drv[i].m && strcmp(s_drv[i].name, n) == 0) { return &s_drv[i]; }
    }
    return NULL;
}

static void job_end(int outcome, const char *msg);

static void drv_drop(drv_t *d) {
    if (J.st != J_IDLE && J.d == d) { job_end(PRN_JOB_FAILED, "driver unloaded"); }
    pex_log(PEX_LOG_INFO, "driver %s gone", d->name);
    memset(d, 0, sizeof *d);
}

static int drv_live(drv_t *d) {
    if (!d || !d->m) { return 0; }
    if (pex_find(d->name) != d->m || pex_sym(d->m, PRN_DRV_SYM) != d->fn) {
        drv_drop(d);
        return 0;
    }
    return 1;
}

static int dcall(drv_t *d, uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2) {
    if (!drv_live(d)) { return PEX_ENOENT; }
    return (int)(uint32_t)pex_call(d->m, d->fn, cmd, a0, a1, a2);
}
#define P(x) ((uint32_t)(uintptr_t)(x))

static int jint(const char *json, const char *path, int def) {
    int64_t v;
    return pex_json_int(json, path, &v) ? (int)v : def;
}

static void cache_models(drv_t *d) {
    d->nmodels = 0;
    int n = dcall(d, PRN_DRV_INFO, P(s_scratch), sizeof s_scratch, 0);
    if (n <= 0) { return; }
    int cnt = pex_json_len(s_scratch, "models");
    char p[48];
    for (int i = 0; i < cnt && d->nmodels < MODEL_MAX; i++) {
        model_t *m = &d->models[d->nmodels];
        memset(m, 0, sizeof *m);
        pex_snprintf(p, sizeof p, "models.%d.id", i);
        if (!pex_json_str(s_scratch, p, m->id, sizeof m->id)) { continue; }
        char tr[16];
        pex_snprintf(p, sizeof p, "models.%d.transport", i);
        m->classic = (int8_t)(pex_json_str(s_scratch, p, tr, sizeof tr) && strcmp(tr, "classic") == 0);
        pex_snprintf(p, sizeof p, "models.%d.black_bit", i);
        m->black_bit = (int8_t)jint(s_scratch, p, 0);
        pex_snprintf(p, sizeof p, "models.%d.density.min", i);
        m->dmin = (int8_t)jint(s_scratch, p, 0);
        pex_snprintf(p, sizeof p, "models.%d.density.max", i);
        m->dmax = (int8_t)jint(s_scratch, p, 15);
        pex_snprintf(p, sizeof p, "models.%d.density.default", i);
        m->ddef = (int8_t)jint(s_scratch, p, m->dmax / 2);
        pex_snprintf(p, sizeof p, "models.%d.speed.default", i);
        int64_t v;
        m->has_speed = (int8_t)pex_json_int(s_scratch, p, &v);
        m->sdef = m->has_speed ? (int8_t)v : -1;
        pex_snprintf(p, sizeof p, "models.%d.speed.min", i);
        m->smin = (int8_t)jint(s_scratch, p, m->sdef);
        pex_snprintf(p, sizeof p, "models.%d.speed.max", i);
        m->smax = (int8_t)jint(s_scratch, p, m->sdef);
        d->nmodels++;
    }
}

static model_t *model_of(drv_t *d, const char *id) {
    for (int i = 0; i < d->nmodels; i++) {
        if (!id || !id[0] || strcmp(d->models[i].id, id) == 0) { return &d->models[i]; }
    }
    return NULL;
}

/* Printers saved with auto_reconnect come back when their driver registers,
 * except Bluetooth Classic ones: paging a Classic printer that is switched off
 * stalls the gateway's Wi-Fi for about half a minute (parent-requirements B4),
 * so those wait for the user to press Connect in the web app. */
static int autoconnect(void *arg) {
    drv_t *d = arg;
    if (!drv_live(d)) { return PEX_TASK_DONE; }
    enum { CFG_MAX = 4096 };
    int fd = pex_fs_open("lfs:/printer/printers.json", PEX_FS_READ, NULL, NULL);
    if (fd < 0) { return PEX_TASK_DONE; }
    char *s_cfg = pex_malloc(CFG_MAX);
    if (!s_cfg) { pex_fs_close(fd); return PEX_TASK_DONE; }
    int n = 0, r;
    while (n < CFG_MAX - 1 && (r = pex_fs_read(fd, s_cfg + n, CFG_MAX - 1 - (size_t)n, NULL, NULL)) > 0) {
        n += r;
    }
    pex_fs_close(fd);
    s_cfg[n] = 0;
    int cnt = pex_json_len(s_cfg, "printers");
    char p[48], drv[32], addr[24];
    for (int i = 0; i < cnt; i++) {
        int on = 1;
        pex_snprintf(p, sizeof p, "printers.%d.driver", i);
        if (!pex_json_str(s_cfg, p, drv, sizeof drv) || strcmp(drv, d->name)) { continue; }
        pex_snprintf(p, sizeof p, "printers.%d.auto_reconnect", i);
        pex_json_bool(s_cfg, p, &on);
        pex_snprintf(p, sizeof p, "printers.%d.addr", i);
        if (!on || !pex_json_str(s_cfg, p, addr, sizeof addr)) { continue; }
        char mid[24] = "";
        pex_snprintf(p, sizeof p, "printers.%d.model", i);
        pex_json_str(s_cfg, p, mid, sizeof mid);
        const model_t *mm = model_of(d, mid);
        if (!mm || mm->classic) {
            pex_log(PEX_LOG_INFO, "auto-connect %s %s skipped: Classic, waits for the user", d->name, addr);
            continue;
        }
        pex_snprintf(p, sizeof p, "printers.%d", i);
        size_t len = 0;
        const char *obj = pex_json_find(s_cfg, p, &len);
        char save = obj ? obj[len] : 0;
        if (obj) { ((char *)obj)[len] = 0; }
        pex_log(PEX_LOG_INFO, "auto-connect %s %s", d->name, addr);
        dcall(d, PRN_DRV_CONNECT, P(addr), P(obj ? obj : "{}"), 0);
        if (obj) { ((char *)obj)[len] = save; }
    }
    pex_free(s_cfg);
    return PEX_TASK_DONE;
}

/* ---- the job ---- */
static void emit_job(const char *state, const char *msg) {
    pex_jw_t w;
    pex_jw_init(&w, s_emit, sizeof s_emit);
    pex_jw_obj(&w, NULL);
    pex_jw_int(&w, "job", J.id);
    pex_jw_str(&w, "state", state);
    if (J.d) { pex_jw_str(&w, "driver", J.d->name); }
    pex_jw_str(&w, "addr", J.job.addr);
    pex_jw_int(&w, "copy", J.job.copy);
    pex_jw_int(&w, "copies", J.job.copies);
    pex_jw_int(&w, "sent", J.sent);
    pex_jw_int(&w, "total", J.total);
    if (msg) { pex_jw_str(&w, "message", msg); }
    pex_jw_end(&w);
    if (pex_jw_finish(&w) > 0) { pex_bus_emit("printer.job", s_emit); }
}

static void job_close_file(void) {
    if (J.fd >= 0) { pex_fs_close(J.fd); }
    J.fd = -1;
}

static void job_end(int outcome, const char *msg) {
    if (J.st == J_IDLE) { return; }
    static const char *const names[] = { "done", "failed", "cancelled" };
    job_close_file();
    J.st = J_IDLE;
    emit_job(names[outcome < 0 || outcome > 2 ? 1 : outcome], msg);
    J.d = NULL;
}

static uint32_t chunk_bytes(void) {
    uint32_t per = CHUNK / J.job.row_bytes;
    return per * J.job.row_bytes;
}

static void on_read(void *arg, int rc);
static void step_job(void);

/* Start reading the next chunk. 1 if data is (or will be) there, 0 at the end of the pass. */
static int read_next(void) {
    uint32_t left = J.job.rows_total - J.rows_read;
    if (!left) { return 0; }
    uint32_t want = chunk_bytes();
    if (want > left * J.job.row_bytes) { want = left * J.job.row_bytes; }
    if (J.sd) {
        int rc = pex_fs_read(J.fd, J.buf, want, on_read, NULL);
        if (rc == PEX_EPENDING) { J.pending = 1; return 1; }
        on_read(NULL, rc);
        return 1;
    }
    uint32_t got = 0;
    while (got < want) {
        int rc = pex_fs_read(J.fd, J.buf + got, want - got, NULL, NULL);
        if (rc <= 0) { on_read(NULL, rc < 0 ? rc : (int)got); return 1; }
        got += (uint32_t)rc;
    }
    on_read(NULL, (int)got);
    return 1;
}

static void on_read(void *arg, int rc) {
    (void)arg;
    J.pending = 0;
    if (J.st == J_IDLE) { return; }
    if (rc <= 0 || rc % J.job.row_bytes) {
        J.err = 1;
        job_end(PRN_JOB_FAILED, rc < 0 ? "read failed" : "short file");
        return;
    }
    uint32_t rows = (uint32_t)rc / J.job.row_bytes;
    if (J.st == J_SCAN) {
        if (J.job.all_black && !prn_rows_all_black(J.buf, (size_t)rc, J.job.black_bit)) { J.job.all_black = 0; }
        J.rows_read += rows;
        return;
    }
    if (J.job.all_black) { prn_punch_rows(J.buf, J.rows_read, rows, J.job.row_bytes, J.job.black_bit); }
    J.rows_read += rows;
    J.buf_len = (uint32_t)rc;
    J.buf_off = 0;
}

static void on_open(void *arg, int rc) {
    (void)arg;
    J.pending = 0;
    if (J.st == J_IDLE) { if (rc >= 0) { pex_fs_close(rc); } return; }
    if (rc < 0) { job_end(PRN_JOB_FAILED, rc == PEX_ENOENT ? "no such file" : "open failed"); return; }
    J.fd = rc;
    int32_t size = pex_fs_size(rc);
    if (size >= 0 && (uint32_t)size != (uint32_t)J.job.row_bytes * J.job.rows_total) {
        job_end(PRN_JOB_FAILED, "file size != rows * row bytes");
    }
}

static int kick_task(void *arg) {
    (void)arg;
    if (J.st == J_FEED) { step_job(); }
    return PEX_TASK_DONE;
}

static void on_seek(void *arg, int rc) {
    (void)arg;
    J.pending = 0;
    if (rc < 0 && J.st != J_IDLE) { job_end(PRN_JOB_FAILED, "seek failed"); }
}

static void rewind_pass(void) {
    J.rows_read = 0;
    J.rows_fed = 0;
    J.buf_len = J.buf_off = 0;
    int rc = pex_fs_seek(J.fd, 0, J.sd ? on_seek : NULL, NULL);
    if (rc == PEX_EPENDING) { J.pending = 1; }
    else if (rc < 0) { job_end(PRN_JOB_FAILED, "seek failed"); }
}

static void begin_feed(void) {
    int rc = dcall(J.d, PRN_DRV_JOB_BEGIN, P(&J.job), 0, 0);
    if (rc < 0) {
        job_end(PRN_JOB_FAILED, rc == PEX_EBUSY ? "printer busy" : rc == PEX_ESTATE ? "not connected" : "refused");
        return;
    }
    J.native = rc == 1;
    J.total = J.job.rows_total * J.job.row_bytes * (J.native ? 1u : J.job.copies);
    J.st = J_FEED;
    rewind_pass();
    emit_job("sending", NULL);
}

static void step_job(void) {
    uint32_t now = pex_ms();
    if (J.st == J_IDLE || J.pending) { return; }
    if (J.st == J_SCAN) {
        if (J.rows_read < J.job.rows_total && J.job.all_black) { read_next(); return; }
        begin_feed();
        return;
    }
    if (J.st == J_WAIT) {
        if (now - J.t_wait > DONE_TIMEOUT_MS) { job_end(PRN_JOB_FAILED, "printer did not finish"); }
        return;
    }
    /* J_FEED: as much as the driver takes, for a few ms of this pass */
    for (int guard = 0; guard < 64 && J.st == J_FEED && !J.pending; guard++) {
        if (J.buf_off >= J.buf_len) {
            if (read_next()) { continue; }
            dcall(J.d, PRN_DRV_JOB_END, J.job.copy, 0, 0);
            if (J.st != J_FEED) { return; }
            if (J.native || J.job.copy + 1u >= J.job.copies) {
                J.st = J_WAIT;
                J.t_wait = now;
                emit_job("printing", NULL);
                return;
            }
            J.job.copy++;
            rewind_pass();
            continue;
        }
        int rc = dcall(J.d, PRN_DRV_JOB_FEED, P(J.buf + J.buf_off), J.buf_len - J.buf_off, 0);
        if (rc == PEX_EBUSY || rc == 0) { break; }
        if (rc < 0) { job_end(PRN_JOB_FAILED, rc == PEX_ESTATE ? "link lost" : "driver error"); return; }
        if ((uint32_t)rc % J.job.row_bytes) { job_end(PRN_JOB_FAILED, "driver took a partial row"); return; }
        J.buf_off += (uint32_t)rc;
        J.rows_fed += (uint32_t)rc / J.job.row_bytes;
        J.sent += (uint32_t)rc;
        if (pex_ms() - now > 20) { break; }
    }
    if (J.st == J_FEED && now - J.t_progress >= PROGRESS_MS) {
        J.t_progress = now;
        emit_job("sending", NULL);
    }
}

static int tick(void *arg) {
    (void)arg;
    for (int i = 0; i < DRV_MAX; i++) {
        if (s_drv[i].m) { drv_live(&s_drv[i]); }
    }
    if (J.st != J_IDLE && !drv_live(J.d)) { job_end(PRN_JOB_FAILED, "driver unloaded"); }
    step_job();
    return PEX_TASK_AGAIN;
}

/* ---- the dispatch the drivers call ---- */
int prn_hub(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2) {
    switch (cmd) {
    case PRN_HUB_REGISTER: {
        const char *name = (const char *)(uintptr_t)a0;
        if (!name || a1 != PRN_ABI) { return PEX_EINVAL; }
        pex_mod_t *m = pex_find(name);
        uint32_t fn = m ? pex_sym(m, PRN_DRV_SYM) : 0;
        if (!fn) { return PEX_ENOENT; }
        drv_t *d = drv_by_name(name);
        for (int i = 0; !d && i < DRV_MAX; i++) {
            if (!s_drv[i].m) { d = &s_drv[i]; }
        }
        if (!d) { return PEX_ENOMEM; }
        memset(d, 0, sizeof *d);
        pex_snprintf(d->name, sizeof d->name, "%s", name);
        d->m = m;
        d->fn = fn;
        cache_models(d);
        pex_log(PEX_LOG_INFO, "driver %s: %d model(s)", name, d->nmodels);
        pex_sched_post(autoconnect, d);
        return PEX_OK;
    }
    case PRN_HUB_UNREGISTER: {
        drv_t *d = drv_by_name((const char *)(uintptr_t)a0);
        if (d) { drv_drop(d); }
        return PEX_OK;
    }
    case PRN_HUB_EMIT: {
        const char *kind = (const char *)(uintptr_t)a0, *json = (const char *)(uintptr_t)a1;
        char topic[40];
        if (!kind || !json) { return PEX_EINVAL; }
        pex_snprintf(topic, sizeof topic, "printer.%s", kind);
        return pex_bus_emit(topic, json);
    }
    case PRN_HUB_JOB_DONE:
        if (J.st == J_IDLE || a0 != J.id) { return PEX_ENOENT; }
        job_end((int)a1, (const char *)(uintptr_t)a2);
        return PEX_OK;
    case PRN_HUB_KICK:
        /* not from inside the driver's own callback: on the next loop pass */
        if (J.st == J_FEED) { pex_sched_post(kick_task, NULL); }
        return PEX_OK;
    }
    return PEX_EINVAL;
}
PEX_EXPORT(prn_hub);

/* ---- ops ---- */
static int err(char *reply, size_t cap, int code, const char *msg) {
    pex_snprintf(reply, cap, "%s", msg);
    return code;
}

typedef struct { char *p; size_t cap, n; int bad; } out_t;
static void out_s(out_t *o, const char *s) {
    size_t n = strlen(s);
    if (o->n + n >= o->cap) { o->bad = 1; return; }
    memcpy(o->p + o->n, s, n + 1);
    o->n += n;
}

static int want_driver(const char *params, char *name, size_t cap) {
    return pex_json_str(params, "driver", name, cap);
}

static int op_drivers(void *arg, const char *params, char *reply, size_t cap) {
    (void)arg; (void)params;
    out_t o = { reply, cap, 0, 0 };
    int first = 1;
    out_s(&o, "{\"drivers\":[");
    for (int i = 0; i < DRV_MAX && !o.bad; i++) {
        if (!drv_live(&s_drv[i])) { continue; }
        int n = dcall(&s_drv[i], PRN_DRV_INFO, P(s_scratch), sizeof s_scratch, 0);
        if (n <= 0) { continue; }
        if (!first) { out_s(&o, ","); }
        out_s(&o, s_scratch);
        first = 0;
    }
    out_s(&o, "]}");
    return o.bad ? err(reply, cap, PEX_ENOMEM, "reply too large") : (int)o.n;
}

static int op_scan(void *arg, const char *params, char *reply, size_t cap) {
    (void)arg;
    char only[32];
    int has = want_driver(params, only, sizeof only);
    int ms = jint(params, "ms", 8000);
    out_t o = { reply, cap, 0, 0 };
    int first = 1;
    out_s(&o, "{\"started\":[");
    for (int i = 0; i < DRV_MAX; i++) {
        drv_t *d = &s_drv[i];
        if (!drv_live(d) || (has && strcmp(only, d->name))) { continue; }
        if (dcall(d, PRN_DRV_SCAN, (uint32_t)(ms < 0 ? 0 : ms), 0, 0) < 0) { continue; }
        if (!first) { out_s(&o, ","); }
        out_s(&o, "\"");
        out_s(&o, d->name);
        out_s(&o, "\"");
        first = 0;
    }
    out_s(&o, "]}");
    return (int)o.n;
}

/* Concatenate the drivers' JSON arrays (from cmd) into "key":[...]. */
static int gather(const char *params, uint32_t cmd, const char *key, out_t *o) {
    char only[32], addr[24];
    int has = want_driver(params, only, sizeof only);
    int has_addr = pex_json_str(params, "addr", addr, sizeof addr);
    int first = 1;
    out_s(o, "\"");
    out_s(o, key);
    out_s(o, "\":[");
    for (int i = 0; i < DRV_MAX && !o->bad; i++) {
        drv_t *d = &s_drv[i];
        if (!drv_live(d) || (has && strcmp(only, d->name))) { continue; }
        int n = cmd == PRN_DRV_DEVICES ? dcall(d, cmd, P(s_scratch), sizeof s_scratch, 0)
                                       : dcall(d, cmd, has_addr ? P(addr) : 0, P(s_scratch), sizeof s_scratch);
        if (n < 2 || s_scratch[0] != '[') { continue; }
        s_scratch[n - 1] = 0;          /* drop the ']' */
        if (!s_scratch[1]) { continue; }
        if (!first) { out_s(o, ","); }
        out_s(o, s_scratch + 1);
        first = 0;
    }
    out_s(o, "]");
    return o->bad ? PEX_ENOMEM : 0;
}

static int op_devices(void *arg, const char *params, char *reply, size_t cap) {
    (void)arg;
    out_t o = { reply, cap, 0, 0 };
    out_s(&o, "{");
    gather(params, PRN_DRV_DEVICES, "devices", &o);
    out_s(&o, "}");
    return o.bad ? err(reply, cap, PEX_ENOMEM, "reply too large") : (int)o.n;
}

static int op_status(void *arg, const char *params, char *reply, size_t cap) {
    (void)arg;
    out_t o = { reply, cap, 0, 0 };
    out_s(&o, "{");
    gather(params, PRN_DRV_STATUS, "printers", &o);
    out_s(&o, ",\"job\":");
    if (J.st == J_IDLE) {
        out_s(&o, "null");
    } else {
        char b[200];
        static const char *const st[] = { "idle", "checking", "sending", "printing" };
        pex_snprintf(b, sizeof b, "{\"job\":%u,\"state\":\"%s\",\"driver\":\"%s\",\"copy\":%u,\"copies\":%u,"
                                  "\"sent\":%u,\"total\":%u}",
                     (unsigned)J.id, st[J.st], J.d ? J.d->name : "", (unsigned)J.job.copy,
                     (unsigned)J.job.copies, (unsigned)J.sent, (unsigned)J.total);
        out_s(&o, b);
    }
    out_s(&o, "}");
    return o.bad ? err(reply, cap, PEX_ENOMEM, "reply too large") : (int)o.n;
}

static drv_t *need_driver(const char *params, char *reply, size_t cap, int *rc) {
    char name[32];
    if (!want_driver(params, name, sizeof name)) { *rc = err(reply, cap, PEX_EINVAL, "driver missing"); return NULL; }
    drv_t *d = drv_by_name(name);
    if (!drv_live(d)) { *rc = err(reply, cap, PEX_ENOENT, "no such driver"); return NULL; }
    return d;
}

static int op_connect(void *arg, const char *params, char *reply, size_t cap) {
    (void)arg;
    int rc = 0;
    char addr[24];
    drv_t *d = need_driver(params, reply, cap, &rc);
    if (!d) { return rc; }
    if (!pex_json_str(params, "addr", addr, sizeof addr)) { return err(reply, cap, PEX_EINVAL, "addr missing"); }
    size_t len = 0;
    const char *cfg = pex_json_find(params, "config", &len);
    static char c[640];
    if (!cfg || len >= sizeof c) { cfg = "{}"; len = 2; }
    memcpy(c, cfg, len);
    c[len] = 0;
    rc = dcall(d, PRN_DRV_CONNECT, P(addr), P(c), 0);
    if (rc < 0 && rc != PEX_EPENDING) { return err(reply, cap, rc, "connect refused"); }
    return pex_snprintf(reply, cap, "{\"pending\":true}");
}

static int op_disconnect(void *arg, const char *params, char *reply, size_t cap) {
    (void)arg;
    char only[32], addr[24];
    int has = want_driver(params, only, sizeof only);
    int has_addr = pex_json_str(params, "addr", addr, sizeof addr);
    for (int i = 0; i < DRV_MAX; i++) {
        drv_t *d = &s_drv[i];
        if (!drv_live(d) || (has && strcmp(only, d->name))) { continue; }
        dcall(d, PRN_DRV_DISCONNECT, has_addr ? P(addr) : 0, 0, 0);
    }
    return pex_snprintf(reply, cap, "{\"ok\":true}");
}

static int mm100(const char *json, const char *path, uint32_t *out) {
    double v;
    if (!pex_json_num(json, path, &v) || v < 0 || v > 1000) { return 0; }
    *out = (uint32_t)(v * 100.0 + 0.5);
    return 1;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int op_print(void *arg, const char *params, char *reply, size_t cap) {
    (void)arg;
    int rc = 0;
    if (J.st != J_IDLE) { return err(reply, cap, PEX_EBUSY, "busy"); }
    drv_t *d = need_driver(params, reply, cap, &rc);
    if (!d) { return rc; }
    prn_job_t *j = &J.job;
    memset(&J, 0, sizeof J);
    J.fd = -1;
    if (!pex_json_str(params, "addr", j->addr, sizeof j->addr)) { j->addr[0] = 0; }
    pex_json_str(params, "model", j->model, sizeof j->model);
    model_t *m = model_of(d, j->model);
    if (!m) { return err(reply, cap, PEX_EINVAL, "unknown model"); }
    pex_snprintf(j->model, sizeof j->model, "%s", m->id);
    char path[128];
    if (!pex_json_str(params, "path", path, sizeof path)) { return err(reply, cap, PEX_EINVAL, "path missing"); }
    if (strncmp(path, "lfs:", 4) == 0 || strncmp(path, "sd:", 3) == 0) {
        pex_snprintf(J.path, sizeof J.path, "%s", path);
    } else if (strncmp(path, "sd/", 3) == 0) {
        pex_snprintf(J.path, sizeof J.path, "sd:/%s", path + 3);
    } else {
        pex_snprintf(J.path, sizeof J.path, "lfs:/%s", path);
    }
    J.sd = strncmp(J.path, "sd:", 3) == 0;
    int w = jint(params, "width_dots", 0), rows = jint(params, "rows", 0);
    if (w < 1 || w > MAX_WIDTH || rows < 1 || rows > 65535) { return err(reply, cap, PEX_EINVAL, "bad size"); }
    j->width_dots = (uint16_t)w;
    j->row_bytes = (uint16_t)((w + 7) / 8);
    j->rows_total = (uint32_t)rows;
    j->origin_x = (int16_t)jint(params, "origin.0", 0);
    j->origin_y = (int16_t)jint(params, "origin.1", 0);
    if (!mm100(params, "media.w_mm", &j->width_mm100)) { return err(reply, cap, PEX_EINVAL, "media.w_mm missing"); }
    j->height_mm100 = 0;
    mm100(params, "media.h_mm", &j->height_mm100);
    mm100(params, "media.gap_mm", &j->gap_mm100);
    char type[16] = "gap";
    pex_json_str(params, "media.type", type, sizeof type);
    j->media_type = strcmp(type, "continuous") == 0 ? PRN_MEDIA_CONTINUOUS
                  : strcmp(type, "blackmark") == 0  ? PRN_MEDIA_BLACKMARK : PRN_MEDIA_GAP;
    if (!j->height_mm100) { j->media_type = PRN_MEDIA_CONTINUOUS; }
    int dens = jint(params, "density", -1);
    j->density = (int8_t)(dens < 0 ? m->ddef : clampi(dens, m->dmin, m->dmax));
    int spd = jint(params, "speed", -1);
    j->speed = (int8_t)(!m->has_speed ? -1 : spd < 0 ? m->sdef : clampi(spd, m->smin, m->smax));
    j->copies = (uint8_t)clampi(jint(params, "copies", 1), 1, 99);
    j->rotate = (uint8_t)(jint(params, "rotate", 0) & 3);
    j->black_bit = (uint8_t)m->black_bit;
    j->all_black = 1;
    size_t olen = 0;
    const char *opt = pex_json_find(params, "options", &olen);
    if (opt && olen < sizeof j->options) { memcpy(j->options, opt, olen); j->options[olen] = 0; }
    else { pex_snprintf(j->options, sizeof j->options, "{}"); }

    if (!J.sd) {
        pex_fs_entry_t e;
        if (pex_fs_stat(J.path, &e, NULL, NULL) != PEX_OK) { return err(reply, cap, PEX_ENOENT, "no such file"); }
        if (e.size != (uint32_t)j->row_bytes * j->rows_total) { return err(reply, cap, PEX_EINVAL, "file size != rows * row bytes"); }
        J.fd = pex_fs_open(J.path, PEX_FS_READ, NULL, NULL);
        if (J.fd < 0) { return err(reply, cap, J.fd, "open failed"); }
    } else {
        int r = pex_fs_open(J.path, PEX_FS_READ, on_open, NULL);
        if (r != PEX_EPENDING && r < 0) { return err(reply, cap, r, "open failed"); }
        J.fd = r >= 0 ? r : -1;
    }
    J.id = s_next_job++;
    j->id = J.id;
    J.d = d;
    J.st = J_SCAN;
    J.t_start = J.t_progress = pex_ms();
    if (J.sd && J.fd < 0) { J.pending = 1; }
    emit_job("queued", NULL);
    return pex_snprintf(reply, cap, "{\"job\":%u}", (unsigned)J.id);
}

static int op_cancel(void *arg, const char *params, char *reply, size_t cap) {
    (void)arg;
    int id = jint(params, "job", 0);
    if (J.st == J_IDLE || (id && (uint32_t)id != J.id)) { return err(reply, cap, PEX_ENOENT, "no such job"); }
    dcall(J.d, PRN_DRV_CANCEL, 0, 0, 0);
    job_end(PRN_JOB_CANCELLED, NULL);
    return pex_snprintf(reply, cap, "{\"ok\":true}");
}

static const struct { const char *name; pex_op_fn fn; } OPS[] = {
    { "printer.drivers", op_drivers },     { "printer.scan", op_scan },
    { "printer.devices", op_devices },     { "printer.connect", op_connect },
    { "printer.disconnect", op_disconnect }, { "printer.status", op_status },
    { "printer.print", op_print },         { "printer.cancel", op_cancel },
};

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host; (void)arg;
    switch (reason) {
    case PEX_INIT:
        for (unsigned i = 0; i < sizeof OPS / sizeof OPS[0]; i++) {
            int rc = pex_bus_op(OPS[i].name, OPS[i].fn, NULL);
            if (rc < 0) { pex_log(PEX_LOG_ERR, "op %s: %d", OPS[i].name, rc); return rc; }
        }
        return PEX_OK;
    case PEX_APP_RUN:
        memset(&J, 0, sizeof J);
        J.fd = -1;
        s_task = pex_sched_every(5, tick, NULL);
        return s_task > 0 ? PEX_OK : s_task;
    case PEX_APP_STOP:
        if (J.st != J_IDLE) {
            dcall(J.d, PRN_DRV_CANCEL, 0, 0, 0);
            job_end(PRN_JOB_CANCELLED, "hub stopped");
        }
        if (s_task > 0) { pex_sched_cancel(s_task); }
        s_task = 0;
        return PEX_OK;
    case PEX_DEINIT:
        memset(s_drv, 0, sizeof s_drv);
        return PEX_OK;
    }
    return PEX_OK;
}
