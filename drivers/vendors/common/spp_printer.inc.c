/* Shared body of an SPP printer driver; see spp_printer.h. Included once by
 * the driver's .c after it defines
 *     static const spp_drv_t DRV = {...};
 * All state is static to the including module. */
#include "spp_printer.h"
#include <string.h>

#define SPP_PEND_CAP 512
#define SPP_TICK_MS 20
#define SPP_DEADLINE_MS 20000
#define SPP_WAIT_MS 15000
#define SPP_BACKOFF_MIN 8000
#define SPP_BACKOFF_MAX 60000

enum { L_IDLE = 0, L_OPENING, L_OPEN, L_BACKOFF };
enum { J_NONE = 0, J_HEAD, J_ROWS, J_BETWEEN, J_DRAIN, J_WAIT };

static struct {
    int task;
    uint8_t registered, running;
    uint32_t reg_try;
    uint8_t scan_on;
    uint32_t scan_look;
    uint8_t reported[16][6];
    uint8_t nreported;
    uint8_t addr[6];
    char addr_str[18];
    uint8_t have_addr, link, pin_i, user_close, auto_reconnect;
    int h;
    uint16_t mtu;
    int tx_full;
    uint32_t t0, backoff_ms, last_ka;
    char why[24];
    uint8_t pend[SPP_PEND_CAP];
    uint16_t pend_len;
    prn_job_t job;
    uint8_t jst, tail_due;
    uint32_t jt0;
    uint8_t last_rx[48];
    uint8_t last_rx_len;
    pex_cl_dev_t devs[16];
    char emit[640];
} S;

static const char *const WHY[] = {"none", "timeout", "cancel", "auth", "security", "sdp",
                                  "rfcomm", "page", "closed", "remote", "dedicated"};

static const char *why_name(int w) {
    return (w >= 0 && w < (int)(sizeof WHY / sizeof WHY[0])) ? WHY[w] : "error";
}

static int hub_call(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2) {
    pex_mod_t *m = pex_find(PRN_HUB_MODULE);
    if (!m) { return PEX_ENOENT; }
    uint32_t fn = pex_sym(m, PRN_HUB_SYM);
    if (!fn) { return PEX_ENOENT; }
    return (int)(int32_t)(uint32_t)pex_call(m, fn, cmd, a0, a1, a2);
}

static void hub_emit(const char *kind, const char *json) {
    hub_call(PRN_HUB_EMIT, (uint32_t)(uintptr_t)kind, (uint32_t)(uintptr_t)json, 0);
}

static const char *link_name(void) {
    if (S.link == L_OPEN) { return S.jst != J_NONE ? "printing" : "connected"; }
    if (S.link == L_OPENING) { return "connecting"; }
    if (S.scan_on) { return "scanning"; }
    if (S.link == L_BACKOFF) { return "connecting"; }
    return S.why[0] ? "error" : "idle";
}

static void emit_state(void) {
    pex_jw_t w;
    pex_jw_init(&w, S.emit, sizeof S.emit);
    pex_jw_obj(&w, NULL);
    if (S.have_addr) { pex_jw_str(&w, "addr", S.addr_str); }
    pex_jw_str(&w, "model", DRV.model);
    pex_jw_str(&w, "state", link_name());
    if (S.why[0]) { pex_jw_str(&w, "why", S.why); }
    pex_jw_end(&w);
    if (pex_jw_finish(&w) >= 0) { hub_emit("state", S.emit); }
}

void spp_log_rx(const uint8_t *d, size_t n) {
    if (n > sizeof S.last_rx) { d += n - sizeof S.last_rx; n = sizeof S.last_rx; }
    memcpy(S.last_rx, d, n);
    S.last_rx_len = (uint8_t)n;
}

static int write_status(char *out, size_t cap) {
    pex_jw_t w;
    pex_jw_init(&w, out, cap);
    pex_jw_obj(&w, NULL);
    if (S.have_addr) { pex_jw_str(&w, "addr", S.addr_str); }
    pex_jw_str(&w, "model", DRV.model);
    pex_jw_str(&w, "state", link_name());
    if (S.why[0]) { pex_jw_str(&w, "error", S.why); }
    if (S.link == L_OPEN) { pex_jw_int(&w, "mtu", S.mtu); }
    if (DRV.status_fields) { DRV.status_fields(&w); }
    if (S.last_rx_len) { pex_jw_hex(&w, "last_rx", S.last_rx, S.last_rx_len); }
    pex_jw_end(&w);
    return pex_jw_finish(&w);
}

void spp_emit_status(void) {
    if (write_status(S.emit, sizeof S.emit) >= 0) { hub_emit("status", S.emit); }
}

/* Push the pending bytes into the session's TX buffer. */
static void flush(void) {
    while (S.pend_len && S.h > 0) {
        int n = pex_bt_classic_send(S.h, S.pend, S.pend_len);
        if (n <= 0) { return; }
        memmove(S.pend, S.pend + n, S.pend_len - (size_t)n);
        S.pend_len = (uint16_t)(S.pend_len - n);
    }
}

/* Room for one spp_send right now: the session's free TX (only while
 * nothing is pending, to keep the order) plus what pend can still take. */
static size_t room(void) {
    size_t r = SPP_PEND_CAP - S.pend_len;
    if (S.pend_len == 0 && S.h > 0) {
        int t = pex_bt_classic_tx_room(S.h);
        if (t > 0) { r += (size_t)t; }
    }
    return r;
}

int spp_send(const void *data, size_t len) {
    if (S.link != L_OPEN || S.h <= 0) { return PEX_ESTATE; }
    flush();
    if (len > room()) { return PEX_EBUSY; }
    const uint8_t *p = (const uint8_t *)data;
    if (S.pend_len == 0) {
        int n = pex_bt_classic_send(S.h, p, len);
        if (n > 0) { p += n; len -= (size_t)n; }
    }
    memcpy(S.pend + S.pend_len, p, len);
    S.pend_len = (uint16_t)(S.pend_len + len);
    return PEX_OK;
}

int spp_job_active(void) { return S.jst != J_NONE; }

void spp_job_finished(int outcome, const char *msg) {
    if (S.jst == J_NONE) { return; }
    uint32_t id = S.job.id;
    S.jst = J_NONE;
    S.tail_due = 0;
    hub_call(PRN_HUB_JOB_DONE, id, (uint32_t)outcome, (uint32_t)(uintptr_t)msg);
    emit_state();
}

static void cl_event(void *arg, const pex_cl_event_t *ev);

static void open_link(void) {
    pex_cl_open_t o;
    memset(&o, 0, sizeof o);
    memcpy(o.addr, S.addr, 6);
    o.uuid = 0x1101;
    o.channel = DRV.channel;
    o.fallback = DRV.fallback;
    o.level = PEX_CL_LEVEL_OMIT;
    o.io = PEX_CL_IO_NONE;
    o.bond = PEX_CL_BOND_OMIT;
    o.drop_key = 1;
    o.deadline_ms = SPP_DEADLINE_MS;
    for (uint8_t i = S.pin_i; i < DRV.npin && o.npin < PEX_CL_PIN_MAX; i++) {
        size_t n = strlen(DRV.pins[i]);
        if (n >= PEX_CL_PIN_LEN) { n = PEX_CL_PIN_LEN - 1; }
        memcpy(o.pin[o.npin], DRV.pins[i], n);
        o.pin[o.npin][n] = 0;
        o.npin++;
    }
    o.fn = cl_event;
    o.arg = NULL;
    int h = pex_bt_classic_open(&o);
    S.t0 = pex_ms();
    if (h > 0) {
        S.h = h;
        S.link = L_OPENING;
        S.why[0] = 0;
    } else if (h == PEX_EBUSY) {
        /* the radio is paging or in an inquiry: try again in a second */
        S.link = L_BACKOFF;
        S.backoff_ms = 1000;
    } else {
        S.link = L_IDLE;
        pex_snprintf(S.why, sizeof S.why, "open %d", h);
    }
    emit_state();
}

static void link_down(int why, int kind) {
    S.h = 0;
    S.pend_len = 0;
    S.tail_due = 0;
    pex_snprintf(S.why, sizeof S.why, "%s", why_name(why));
    if (S.jst != J_NONE) { spp_job_finished(PRN_JOB_FAILED, "link lost"); }
    if (kind == PEX_CL_FAILED && why == 3 && S.pin_i + 1 < DRV.npin) {
        /* The PIN list is answered in order within one attempt only; a
         * printer that rejects the first PIN fails the session, so start
         * again from the next one (the host also dropped the stale key). */
        S.pin_i++;
        pex_log(PEX_LOG_INFO, "%s: auth failed, trying PIN %u", DRV.driver, (unsigned)S.pin_i + 1);
        open_link();
        return;
    }
    if (!S.user_close && S.auto_reconnect && S.have_addr) {
        S.link = L_BACKOFF;
        S.t0 = pex_ms();
        if (S.backoff_ms < SPP_BACKOFF_MIN) { S.backoff_ms = SPP_BACKOFF_MIN; }
    } else {
        S.link = L_IDLE;
    }
    emit_state();
}

static void cl_event(void *arg, const pex_cl_event_t *ev) {
    (void)arg;
    if (ev->handle != S.h) { return; }
    switch (ev->kind) {
    case PEX_CL_OPEN: {
        S.link = L_OPEN;
        S.mtu = ev->len;
        pex_cl_info_t inf;
        if (pex_bt_classic_state(S.h, &inf) >= 0) { S.mtu = inf.mtu; }
        S.tx_full = pex_bt_classic_tx_room(S.h);
        S.pin_i = 0;
        S.backoff_ms = SPP_BACKOFF_MIN;
        S.why[0] = 0;
        S.last_ka = pex_ms();
        emit_state();
        if (DRV.on_open) { DRV.on_open(); }
        break;
    }
    case PEX_CL_RX: {
        uint8_t buf[252];
        int n;
        while ((n = pex_bt_classic_listen(S.h, buf, sizeof buf)) > 0) {
            spp_log_rx(buf, (size_t)n);
            if (DRV.on_rx) { DRV.on_rx(buf, (size_t)n); }
        }
        break;
    }
    case PEX_CL_TX_ROOM:
        flush();
        hub_call(PRN_HUB_KICK, 0, 0, 0);
        break;
    case PEX_CL_PIN:
        /* every fixed PIN was tried: no person to ask, give up this attempt */
        pex_bt_classic_pin(S.h, NULL);
        break;
    case PEX_CL_PASSKEY:
        pex_bt_classic_passkey(S.h, 1, 0);
        break;
    case PEX_CL_CONFIRM:
        pex_bt_classic_confirm(S.h, 1);
        break;
    case PEX_CL_FAILED:
    case PEX_CL_CLOSED:
        link_down(ev->why, ev->kind);
        break;
    default:
        break;
    }
}

static int seen(const uint8_t *a) {
    for (uint8_t i = 0; i < S.nreported; i++) {
        if (memcmp(S.reported[i], a, 6) == 0) { return 1; }
    }
    if (S.nreported < 16) { memcpy(S.reported[S.nreported++], a, 6); }
    return 0;
}

static void report_found(void) {
    int n = pex_bt_classic_devices(S.devs, 16, DRV.cod_mask, DRV.cod_value, DRV.name_prefix);
    for (int i = 0; i < n; i++) {
        if (seen(S.devs[i].addr)) { continue; }
        char a[18];
        pex_bt_addr_str(S.devs[i].addr, a);
        pex_jw_t w;
        pex_jw_init(&w, S.emit, sizeof S.emit);
        pex_jw_obj(&w, NULL);
        pex_jw_str(&w, "addr", a);
        pex_jw_str(&w, "name", S.devs[i].name);
        pex_jw_int(&w, "rssi", S.devs[i].rssi);
        pex_jw_str(&w, "model", DRV.model);
        pex_jw_str(&w, "transport", "classic");
        pex_jw_end(&w);
        if (pex_jw_finish(&w) >= 0) { hub_emit("found", S.emit); }
    }
}

static void job_tick(void) {
    flush();
    if (S.tail_due) {
        uint8_t t[64];
        int tl = DRV.job_tail ? DRV.job_tail(&S.job, t, sizeof t) : 0;
        if (tl <= 0 || spp_send(t, (size_t)tl) == PEX_OK) {
            S.tail_due = 0;
            if (S.jst == J_DRAIN) { S.jt0 = pex_ms(); }
        }
        return;
    }
    if (S.jst == J_DRAIN && S.pend_len == 0) {
        int r = pex_bt_classic_tx_room(S.h);
        if (r >= S.tx_full) {
            if (!DRV.job_sent || DRV.job_sent(&S.job)) {
                spp_job_finished(PRN_JOB_OK, NULL);
            } else {
                S.jst = J_WAIT;
                S.jt0 = pex_ms();
            }
        }
    } else if (S.jst == J_WAIT && pex_ms() - S.jt0 > SPP_WAIT_MS) {
        spp_job_finished(PRN_JOB_OK, "sent; no reply from the printer");
    }
}

static int tick(void *arg) {
    (void)arg;
    if (!S.running) { return PEX_TASK_DONE; }
    uint32_t now = pex_ms();
    if (!S.registered && now - S.reg_try >= 500) {
        S.reg_try = now;
        if (hub_call(PRN_HUB_REGISTER, (uint32_t)(uintptr_t)DRV.driver, PRN_ABI, 0) >= 0) {
            S.registered = 1;
        }
    }
    if (S.scan_on && now - S.scan_look >= 500) {
        S.scan_look = now;
        report_found();
        if (!pex_bt_classic_scanning()) {
            S.scan_on = 0;
            emit_state();
        }
    }
    if (S.link == L_BACKOFF && now - S.t0 >= S.backoff_ms) {
        uint32_t next = S.backoff_ms * 2;
        open_link();
        if (S.link == L_OPENING) {
            S.backoff_ms = next > SPP_BACKOFF_MAX ? SPP_BACKOFF_MAX : next;
        }
    }
    if (S.link == L_OPEN) {
        if (S.jst != J_NONE) {
            job_tick();
        } else {
            flush();
            if (DRV.keepalive_ms && DRV.on_keepalive && now - S.last_ka >= DRV.keepalive_ms) {
                S.last_ka = now;
                DRV.on_keepalive();
            }
        }
    }
    return PEX_TASK_AGAIN;
}

static int put_head(void) {
    uint8_t hd[256];
    int hl = DRV.job_head(&S.job, hd, sizeof hd);
    if (hl < 0) { return PEX_EINVAL; }
    if (hl > 0 && spp_send(hd, (size_t)hl) != PEX_OK) { return PEX_EBUSY; }
    S.jst = J_ROWS;
    return PEX_OK;
}

static int feed(const uint8_t *rows, uint32_t len) {
    if (S.jst == J_NONE) { return PEX_ESTATE; }
    if (S.link != L_OPEN) { return PEX_ESTATE; }
    if (S.tail_due) { job_tick(); if (S.tail_due) { return PEX_EBUSY; } }
    if (S.jst == J_HEAD || S.jst == J_BETWEEN) {
        int r = put_head();
        if (r != PEX_OK) { return r; }
    }
    uint32_t rb = S.job.row_bytes;
    size_t r = room();
    uint32_t take = len;
    if (take > r) { take = (uint32_t)r; }
    take -= take % rb;
    if (take == 0) { return PEX_EBUSY; }
    if (spp_send(rows, take) != PEX_OK) { return PEX_EBUSY; }
    return (int)take;
}

static int job_end(uint32_t copy) {
    if (S.jst == J_NONE) { return PEX_ESTATE; }
    S.tail_due = 1;
    if (copy + 1 < S.job.copies) {
        S.jst = J_BETWEEN;
        S.job.copy = (uint8_t)(copy + 1);
    } else {
        S.jst = J_DRAIN;
        S.jt0 = pex_ms();
    }
    job_tick();
    return PEX_OK;
}

static int job_begin(const prn_job_t *j) {
    if (S.link != L_OPEN) { return PEX_ESTATE; }
    if (j->width_dots != DRV.head_dots || j->row_bytes != (DRV.head_dots + 7) / 8 || j->rows_total == 0) {
        return PEX_EINVAL;
    }
    if (S.jst != J_NONE && j->id == S.job.id) {
        /* the hub begins each copy again: keep going with the new copy */
        S.job = *j;
        S.jst = J_HEAD;
        return PEX_OK;
    }
    if (S.jst != J_NONE) { return PEX_EBUSY; }
    S.job = *j;
    if (S.job.copies == 0) { S.job.copies = 1; }
    S.jst = J_HEAD;
    S.jt0 = pex_ms();
    emit_state();
    return PEX_OK;
}

static int devices_json(char *out, size_t cap) {
    int n = pex_bt_classic_devices(S.devs, 16, DRV.cod_mask, DRV.cod_value, DRV.name_prefix);
    pex_jw_t w;
    pex_jw_init(&w, out, cap);
    pex_jw_arr(&w, NULL);
    for (int i = 0; i < n; i++) {
        char a[18];
        pex_bt_addr_str(S.devs[i].addr, a);
        pex_jw_obj(&w, NULL);
        pex_jw_str(&w, "addr", a);
        pex_jw_str(&w, "name", S.devs[i].name);
        pex_jw_int(&w, "rssi", S.devs[i].rssi);
        pex_jw_str(&w, "model", DRV.model);
        pex_jw_str(&w, "transport", "classic");
        pex_jw_end(&w);
    }
    pex_jw_end(&w);
    return pex_jw_finish(&w);
}

static int do_connect(const char *addr, const char *cfg) {
    uint8_t a[6];
    if (!addr || pex_bt_addr(addr, a) < 0) { return PEX_EINVAL; }
    int ar = 1;
    if (cfg && pex_json_bool(cfg, "auto_reconnect", &ar) != 1) { ar = 1; }
    if (S.h > 0) {
        if (memcmp(a, S.addr, 6) == 0 && S.link != L_IDLE) {
            emit_state();
            return PEX_EPENDING;
        }
        S.user_close = 1;
        pex_bt_classic_close(S.h);
        S.h = 0;
    }
    memcpy(S.addr, a, 6);
    pex_bt_addr_str(a, S.addr_str);
    S.have_addr = 1;
    S.auto_reconnect = (uint8_t)(ar != 0);
    S.user_close = 0;
    S.pin_i = 0;
    S.backoff_ms = SPP_BACKOFF_MIN;
    S.why[0] = 0;
    open_link();
    return PEX_EPENDING;
}

static int do_disconnect(void) {
    S.user_close = 1;
    if (S.jst != J_NONE) { spp_job_finished(PRN_JOB_CANCELLED, "disconnected"); }
    if (S.h > 0) { pex_bt_classic_close(S.h); }
    S.h = 0;
    S.pend_len = 0;
    S.link = L_IDLE;
    S.why[0] = 0;
    emit_state();
    return PEX_OK;
}

int prn_drv(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2) {
    switch (cmd) {
    case PRN_DRV_INFO: {
        size_t n = strlen(DRV.caps_json);
        if (n + 1 > a1) { return PEX_ENOMEM; }
        memcpy((char *)(uintptr_t)a0, DRV.caps_json, n + 1);
        return (int)n;
    }
    case PRN_DRV_SCAN:
        if (a0 == 0) {
            pex_bt_classic_scan_stop();
            S.scan_on = 0;
            emit_state();
            return PEX_OK;
        } else {
            int r = pex_bt_classic_scan(a0);
            if (r < 0 && r != PEX_EBUSY) { return r; }
            S.scan_on = 1;
            S.scan_look = pex_ms();
            S.nreported = 0;
            emit_state();
            return PEX_OK;
        }
    case PRN_DRV_DEVICES:
        return devices_json((char *)(uintptr_t)a0, a1);
    case PRN_DRV_CONNECT:
        return do_connect((const char *)(uintptr_t)a0, (const char *)(uintptr_t)a1);
    case PRN_DRV_DISCONNECT:
        return do_disconnect();
    case PRN_DRV_STATUS:
        return write_status((char *)(uintptr_t)a1, a2);
    case PRN_DRV_JOB_BEGIN:
        return job_begin((const prn_job_t *)(uintptr_t)a0);
    case PRN_DRV_JOB_FEED:
        return feed((const uint8_t *)(uintptr_t)a0, a1);
    case PRN_DRV_JOB_END:
        return job_end(a0);
    case PRN_DRV_CANCEL:
        S.pend_len = 0;
        spp_job_finished(PRN_JOB_CANCELLED, NULL);
        return PEX_OK;
    default:
        (void)a2;
        return PEX_ENOTSUP;
    }
}
PEX_EXPORT(prn_drv);

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host;
    (void)arg;
    switch (reason) {
    case PEX_INIT:
        return PEX_OK;
    case PEX_APP_RUN:
        memset(&S, 0, sizeof S);
        S.running = 1;
        S.reg_try = pex_ms() - 1000;
        S.task = pex_sched_every(SPP_TICK_MS, tick, NULL);
        return S.task > 0 ? PEX_OK : S.task;
    case PEX_APP_STOP:
        if (S.registered) {
            hub_call(PRN_HUB_UNREGISTER, (uint32_t)(uintptr_t)DRV.driver, 0, 0);
            S.registered = 0;
        }
        S.user_close = 1;
        if (S.h > 0) { pex_bt_classic_close(S.h); }
        S.h = 0;
        if (S.scan_on) { pex_bt_classic_scan_stop(); }
        S.running = 0;
        if (S.task > 0) { pex_sched_cancel(S.task); }
        S.task = 0;
        return PEX_OK;
    case PEX_DEINIT:
        return PEX_OK;
    default:
        return PEX_OK;
    }
}
