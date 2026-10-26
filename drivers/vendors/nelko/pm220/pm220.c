/* Nelko PM220 over Bluetooth Classic SPP: the driver behind printer_hub.
 *
 * Ported from pm220_rp2_webapp's firmware (src/bt/bt_core.c for the link,
 * src/printer/tspl.c for the job) onto the parent's PEX classic group:
 *   - find: inquiry, keeping devices named PM220, or nameless ones of Class
 *     of Device major class 6 (imaging);
 *   - link: RFCOMM to the SPP UUID 0x1101 (channel 1 when SDP has no record),
 *     legacy PIN answered from our own list ("0000" then "1234", a PIN that
 *     worked before first) through PEX_CL_PIN, so the firmware's one
 *     drop-key retry tries the next PIN, not the same one again;
 *   - reconnect: 8 s, doubling to 60 s, while the user wants the printer;
 *   - status: CONFIG?, BATTERY? (every 25 s when idle), ESC !? before a job,
 *     ESC !o's 16-byte status (it leads every job, as the old firmware did);
 *   - jobs: TSPL from proto/tspl, the rows streamed as the TX buffer frees.
 * Nothing in this file rasterises: the hub feeds the web app's rows. */
#include "pex.h"
#include "core/prn_core.h"
#include "proto/tspl/tspl.h"
#include "vendors/nelko/pm220/pm220_proto.h"

#include <string.h>

PEX_WEAK_IMPORT(pex_bt_classic_scan);
PEX_WEAK_IMPORT(pex_bt_classic_scan_stop);
PEX_WEAK_IMPORT(pex_bt_classic_scanning);
PEX_WEAK_IMPORT(pex_bt_classic_devices);
PEX_WEAK_IMPORT(pex_bt_classic_open);
PEX_WEAK_IMPORT(pex_bt_classic_pin);
PEX_WEAK_IMPORT(pex_bt_classic_send);
PEX_WEAK_IMPORT(pex_bt_classic_tx_room);
PEX_WEAK_IMPORT(pex_bt_classic_listen);
PEX_WEAK_IMPORT(pex_bt_classic_close);
PEX_WEAK_IMPORT(pex_bt_classic_state);
PEX_WEAK_IMPORT(pex_bt_classic_forget);

#define DRIVER "nelko_pm220"
#define SPP_UUID 0x1101
#define FALLBACK_CH 1
#define DEADLINE_MS 20000u
#define REPLY_MS 1800u
#define IDLE_MS 200u
#define KEEPALIVE_MS 25000u
#define PREFLIGHT_MS 1500u
#define BUSY_WAIT_MS 10000u
#define BACKOFF_FIRST_MS 8000u
#define BACKOFF_MAX_MS 60000u
#define DEVS 16
#define PINS 4
#define COD_MAJOR(c) (((c) >> 8) & 0x1Fu)

enum { L_IDLE = 0, L_OPENING, L_OPEN, L_BACKOFF };
enum { J_NONE = 0, J_PREFLIGHT, J_SEND, J_TAIL, J_DRAIN };

static prn_hub_t s_hub;
static int s_task;
static char s_self[32] = DRIVER;

/* the link */
static int s_link, s_handle, s_want, s_auth_fails, s_backoff_n;
static uint8_t s_addr[6];
static char s_addr_s[18];
static uint8_t s_channel;
static uint32_t s_t_retry, s_mtu, s_room_max;
static char s_pins[PINS][16];
static int s_npins, s_pin_i;
static char s_pin_used[16];
static char s_why[48];

/* what the printer said */
static pm_rx_t s_rx;
static uint32_t s_t_rx;
static int s_expect;
static uint32_t s_t_asked, s_t_keepalive;
static int s_q[6], s_nq;   /* queries waiting to be sent */
static pm_battery_t s_bat;
static pm_config_t s_cfg;
static pm_status_t s_st;
static int s_have_bat, s_have_cfg, s_have_st, s_ready = -1;

/* scanning */
static int s_scanning;
static uint32_t s_t_scan;
static uint8_t s_seen[DEVS][6];
static int s_nseen;

/* the job */
static int s_job;
static prn_job_t s_jb;
static tspl_job_t s_tj;
static char s_pend[200];
static uint16_t s_pend_n, s_pend_off;
static uint32_t s_t_job;
static int s_preflight;

static char s_out[400];

/* ---- helpers ---- */
static int classic_ok(void) { return pex_avail(pex_bt_classic_open) && pex_avail(pex_bt_classic_send); }

static const char *link_name(void) {
    if (s_job) { return "printing"; }
    switch (s_link) {
    case L_OPENING: return s_pin_i ? "pairing" : "connecting";
    case L_OPEN: return "connected";
    case L_BACKOFF: return "waiting";
    }
    return s_scanning ? "scanning" : "idle";
}

static void emit_state(void) {
    pex_jw_t w;
    pex_jw_init(&w, s_out, sizeof s_out);
    pex_jw_obj(&w, NULL);
    pex_jw_str(&w, "driver", s_self);
    pex_jw_str(&w, "addr", s_addr_s);
    pex_jw_str(&w, "model", "pm220");
    pex_jw_str(&w, "state", link_name());
    if (s_why[0]) { pex_jw_str(&w, "why", s_why); }
    if (s_link == L_OPEN) {
        pex_jw_int(&w, "channel", s_channel);
        if (s_pin_used[0]) { pex_jw_str(&w, "pin", s_pin_used); }
    }
    if (s_link == L_BACKOFF) { pex_jw_int(&w, "retry_ms", (int64_t)(s_t_retry - pex_ms())); }
    pex_jw_end(&w);
    if (pex_jw_finish(&w) > 0) { prn_hub_emit(&s_hub, "state", s_out); }
}

static void status_fields(pex_jw_t *w) {
    pex_jw_str(w, "driver", s_self);
    pex_jw_str(w, "addr", s_addr_s);
    pex_jw_str(w, "model", "pm220");
    pex_jw_str(w, "state", link_name());
    if (s_why[0]) { pex_jw_str(w, "why", s_why); }
    if (s_have_bat) {
        pex_jw_int(w, "battery", s_bat.percent);
        pex_jw_bool(w, "charging", s_bat.charging);
    }
    if (s_ready >= 0) {
        pex_jw_bool(w, "lid", (s_ready & PM_READY_LID) != 0);
        pex_jw_bool(w, "paper", (s_ready & PM_READY_PAPER) == 0);
        pex_jw_bool(w, "busy", (s_ready & PM_READY_BUSY) != 0);
    }
    if (s_have_st) {
        pex_jw_obj(w, "media");
        pex_jw_str(w, "type", pm_paper_name(s_st.paper));
        if (s_st.width_mm || s_st.length_mm) {
            pex_jw_int(w, "w_mm", s_st.width_mm);
            pex_jw_int(w, "h_mm", s_st.length_mm);
        }
        pex_jw_bool(w, "crc_ok", s_st.crc_ok);
        pex_jw_end(w);
    }
    if (s_have_cfg) {
        pex_jw_int(w, "dpi", s_cfg.dpi);
        if (s_cfg.fw[0]) { pex_jw_str(w, "fw", s_cfg.fw); }
        if (s_cfg.hw[0]) { pex_jw_str(w, "hw", s_cfg.hw); }
    }
}

static void emit_status(void) {
    pex_jw_t w;
    pex_jw_init(&w, s_out, sizeof s_out);
    pex_jw_obj(&w, NULL);
    status_fields(&w);
    pex_jw_end(&w);
    if (pex_jw_finish(&w) > 0) { prn_hub_emit(&s_hub, "status", s_out); }
}

static void set_why(const char *why) { pex_snprintf(s_why, sizeof s_why, "%s", why ? why : ""); }

static void ask(int kind) {
    if (s_nq < (int)(sizeof s_q / sizeof s_q[0])) { s_q[s_nq++] = kind; }
}

static int send_bytes(const void *d, size_t n) {
    if (s_link != L_OPEN) { return PEX_ESTATE; }
    return pex_bt_classic_send(s_handle, d, n);
}

static int room(void) {
    int r = s_link == L_OPEN ? pex_bt_classic_tx_room(s_handle) : 0;
    if (r > 0 && (uint32_t)r > s_room_max) { s_room_max = (uint32_t)r; }
    return r;
}

/* ---- the link ---- */
static void on_cl(void *arg, const pex_cl_event_t *ev);

static void build_pins(const char *cfg) {
    static const char *const def[] = { "0000", "1234" };
    char pin[16];
    s_npins = 0;
    if (cfg && pex_json_str(cfg, "pin", pin, sizeof pin) && pin[0]) {
        pex_snprintf(s_pins[s_npins++], sizeof s_pins[0], "%s", pin);
    }
    for (int i = 0; i < 2 && s_npins < PINS; i++) {
        int dup = 0;
        for (int k = 0; k < s_npins; k++) { dup |= strcmp(s_pins[k], def[i]) == 0; }
        if (!dup) { pex_snprintf(s_pins[s_npins++], sizeof s_pins[0], "%s", def[i]); }
    }
}

static int open_link(void) {
    pex_cl_open_t o;
    memset(&o, 0, sizeof o);
    memcpy(o.addr, s_addr, 6);
    o.uuid = SPP_UUID;
    o.channel = s_channel;           /* 0: SDP */
    o.fallback = FALLBACK_CH;
    o.level = PEX_CL_LEVEL_OMIT;
    o.io = PEX_CL_IO_NONE;
    o.bond = PEX_CL_BOND_OMIT;
    o.drop_key = 1;
    o.deadline_ms = DEADLINE_MS;
    o.npin = 0;                      /* every PIN request comes to us: see on_cl */
    o.fn = on_cl;
    o.arg = NULL;
    s_pin_i = 0;
    s_pin_used[0] = 0;
    int h = pex_bt_classic_open(&o);
    if (h < 0) { return h; }
    s_handle = h;
    s_link = L_OPENING;
    set_why(NULL);
    emit_state();
    return PEX_OK;
}

static void schedule_retry(const char *why) {
    s_handle = 0;
    s_link = L_IDLE;
    set_why(why);
    if (s_want) {
        uint32_t d = prn_backoff_ms((uint32_t)s_backoff_n++, BACKOFF_FIRST_MS, BACKOFF_MAX_MS);
        s_t_retry = pex_ms() + d;
        s_link = L_BACKOFF;
        pex_log(PEX_LOG_INFO, "pm220: %s, retry in %u ms", why, (unsigned)d);
    }
    emit_state();
}

static void job_fail(const char *msg) {
    if (!s_job) { return; }
    s_job = 0;
    s_pend_n = s_pend_off = 0;
    prn_hub_job_done(&s_hub, s_jb.id, PRN_JOB_FAILED, msg);
}

static void on_rx(int h) {
    uint8_t b[64];
    int n;
    while ((n = pex_bt_classic_listen(h, b, sizeof b)) > 0) {
        pm_rx_push(&s_rx, b, (size_t)n);
        s_t_rx = pex_ms();
    }
}

static void on_cl(void *arg, const pex_cl_event_t *ev) {
    (void)arg;
    if (ev->handle != s_handle) { return; }
    switch (ev->kind) {
    case PEX_CL_PIN:
        if (s_pin_i < s_npins) {
            pex_snprintf(s_pin_used, sizeof s_pin_used, "%s", s_pins[s_pin_i]);
            pex_log(PEX_LOG_INFO, "pm220: PIN request, answering try %d", s_pin_i + 1);
            pex_bt_classic_pin(ev->handle, s_pins[s_pin_i++]);
            emit_state();
        } else {
            pex_bt_classic_pin(ev->handle, NULL);    /* out of PINs: cancel */
        }
        break;
    case PEX_CL_OPEN: {
        pex_cl_info_t in;
        s_link = L_OPEN;
        s_backoff_n = 0;
        s_auth_fails = 0;
        if (pex_bt_classic_state(ev->handle, &in) == PEX_OK) {
            s_channel = in.channel;
            s_mtu = in.mtu;
        }
        s_room_max = 0;
        room();
        s_rx.n = 0;
        s_nq = 0;
        s_expect = PM_NONE;
        ask(PM_CONFIG);
        ask(PM_BATTERY);
        ask(PM_READY);
        s_t_keepalive = pex_ms();
        set_why(NULL);
        emit_state();
        break;
    }
    case PEX_CL_RX:
        on_rx(ev->handle);
        break;
    case PEX_CL_TX_ROOM:
        if (s_job) { prn_hub_kick(&s_hub); }
        break;
    case PEX_CL_FAILED:
    case PEX_CL_CLOSED: {
        static const char *const why[] = { "", "timeout", "cancelled", "pairing failed", "security",
                                           "no SPP service", "RFCOMM", "not reachable", "closed",
                                           "link lost", "bonded" };
        const char *w = ev->why >= 0 && ev->why <= 10 ? why[ev->why] : "failed";
        if (s_job) { job_fail(ev->why == 9 ? "link lost" : w); }
        if (ev->kind == PEX_CL_CLOSED && !s_want) {
            s_handle = 0;
            s_link = L_IDLE;
            set_why(NULL);
            emit_state();
            break;
        }
        if (ev->why == 3 && ++s_auth_fails >= 3) {
            s_want = 0;               /* three rounds of every PIN: stop asking */
            pex_log(PEX_LOG_WARN, "pm220: pairing failed with every PIN");
        }
        schedule_retry(w);
        break;
    }
    }
}

/* ---- queries and replies ---- */
static void send_query(int kind) {
    static const char *const q[] = { "", "BATTERY?\r\n", "CONFIG?\r\n", "\x1b!?\r\n", "\x1b!o\r\n" };
    const char *s = q[kind];
    if (send_bytes(s, strlen(s)) > 0) {
        s_expect = kind;
        s_t_asked = pex_ms();
    }
}

static void take_replies(void) {
    pm_reply_t r;
    int idle = s_rx.n && pex_ms() - s_t_rx >= IDLE_MS;
    while (s_rx.n && pm_rx_take(&s_rx, s_expect, idle, &r)) {
        if (r.kind == PM_NONE) { continue; }
        if (r.kind == s_expect) { s_expect = PM_NONE; }
        switch (r.kind) {
        case PM_BATTERY: s_bat = r.u.battery; s_have_bat = 1; break;
        case PM_CONFIG: s_cfg = r.u.config; s_have_cfg = 1; break;
        case PM_READY: s_ready = r.u.ready; break;
        case PM_STATUS: s_st = r.u.status; s_have_st = 1; s_ready = s_st.readiness; break;
        }
        emit_status();
    }
    if (s_expect != PM_NONE && pex_ms() - s_t_asked > REPLY_MS) { s_expect = PM_NONE; }
}

/* ---- the job ---- */
static int flush_pend(void) {
    while (s_pend_off < s_pend_n) {
        int r = room();
        if (r <= 0) { return 0; }
        uint16_t k = (uint16_t)(s_pend_n - s_pend_off);
        if ((int)k > r) { k = (uint16_t)r; }
        int rc = send_bytes(s_pend + s_pend_off, k);
        if (rc <= 0) { return 0; }
        s_pend_off = (uint16_t)(s_pend_off + rc);
    }
    s_pend_n = s_pend_off = 0;
    return 1;
}

static int job_begin(const prn_job_t *j) {
    if (s_link != L_OPEN) { return PEX_ESTATE; }
    if (s_job) { return PEX_EBUSY; }
    s_jb = *j;
    memset(&s_tj, 0, sizeof s_tj);
    s_tj.prefix = "\x1b!o\r\n";
    s_tj.width_mm100 = j->width_mm100;
    s_tj.height_mm100 = j->height_mm100;
    s_tj.gap_mm100 = j->gap_mm100;
    s_tj.media = j->media_type;
    s_tj.dpi = 203;
    s_tj.density = j->density;
    s_tj.speed = -1;                 /* the PM220 is not sent SPEED (the old firmware never did) */
    s_tj.direction = j->rotate == 2 ? 1 : 0;
    s_tj.x = j->origin_x;
    s_tj.y = j->origin_y;
    s_tj.row_bytes = j->row_bytes;
    s_tj.rows = j->rows_total;
    s_tj.copies = j->copies;
    int n = tspl_head(s_pend, sizeof s_pend, &s_tj);
    if (n < 0) { return PEX_EINVAL; }
    s_pend_n = (uint16_t)n;
    s_pend_off = 0;
    s_job = J_PREFLIGHT;
    s_preflight = 1;
    int v = 1;
    if (pex_json_bool(j->options, "preflight", &v) && !v) { s_preflight = 0; }
    s_t_job = pex_ms();
    if (s_preflight) {
        s_nq = 0;
        s_expect = PM_NONE;
        s_ready = -1;
        send_query(PM_READY);
    } else {
        s_job = J_SEND;
    }
    emit_state();
    return 1;                        /* copies come from PRINT n */
}

static void job_preflight(void) {
    uint32_t t = pex_ms() - s_t_job;
    if (s_expect == PM_READY && t < PREFLIGHT_MS) { return; }
    if (s_ready >= 0 && (s_ready & PM_READY_LID)) { job_fail("lid open"); emit_state(); return; }
    if (s_ready >= 0 && (s_ready & PM_READY_PAPER)) { job_fail("out of paper"); emit_state(); return; }
    if (s_ready >= 0 && (s_ready & PM_READY_BUSY) && t < BUSY_WAIT_MS) {
        if (s_expect == PM_NONE && t % 500u < 6) { send_query(PM_READY); }
        return;
    }
    s_job = J_SEND;
    s_expect = PM_STATUS;            /* the job's ESC !o answers with the 16-byte status */
    s_t_asked = pex_ms();
    prn_hub_kick(&s_hub);
}

static int job_feed(const uint8_t *rows, uint32_t n) {
    if (!s_job) { return PEX_ESTATE; }
    if (s_link != L_OPEN) { return PEX_ESTATE; }
    if (s_job == J_PREFLIGHT) { return PEX_EBUSY; }
    if (s_job != J_SEND) { return PEX_ESTATE; }
    if (!flush_pend()) { return PEX_EBUSY; }
    int r = room();
    uint32_t rb = s_jb.row_bytes;
    uint32_t k = (uint32_t)(r > 0 ? r : 0) / rb * rb;
    if (k > n) { k = n - n % rb; }
    if (!k) { return PEX_EBUSY; }
    int rc = send_bytes(rows, k);
    if (rc == PEX_EBUSY) { return PEX_EBUSY; }
    if (rc < 0) { return PEX_ESTATE; }
    return rc;
}

static void job_end(void) {
    if (s_job != J_SEND) { return; }
    int n = tspl_tail(s_pend, sizeof s_pend, &s_tj);
    s_pend_n = (uint16_t)(n > 0 ? n : 0);
    s_pend_off = 0;
    s_job = J_TAIL;
}

static void job_step(void) {
    switch (s_job) {
    case J_PREFLIGHT: job_preflight(); break;
    case J_SEND: if (s_pend_n) { flush_pend(); } break;
    case J_TAIL:
        if (flush_pend()) { s_job = J_DRAIN; }
        break;
    case J_DRAIN:
        /* everything handed to the radio: done once the TX buffer is empty */
        if (room() >= (int)s_room_max) {
            uint32_t id = s_jb.id;
            s_job = 0;
            s_t_keepalive = pex_ms();
            prn_hub_job_done(&s_hub, id, PRN_JOB_OK, NULL);
            emit_state();
        }
        break;
    }
}

/* ---- scanning ---- */
static int wanted(const pex_cl_dev_t *d) {
    const char *n = d->name;
    for (const char *p = n; *p; p++) {
        if ((p[0] == 'P' || p[0] == 'p') && (p[1] == 'M' || p[1] == 'm') && strncmp(p + 2, "220", 3) == 0) { return 1; }
    }
    return !n[0] && COD_MAJOR(d->cod) == 6u;
}

static int devices_json(char *out, size_t cap) {
    pex_cl_dev_t d[DEVS];
    pex_jw_t w;
    char a[18];
    int n = classic_ok() ? pex_bt_classic_devices(d, DEVS, 0, 0, NULL) : 0;
    pex_jw_init(&w, out, cap);
    pex_jw_arr(&w, NULL);
    for (int i = 0; i < n; i++) {
        if (!wanted(&d[i])) { continue; }
        prn_addr_fmt(d[i].addr, a);
        pex_jw_obj(&w, NULL);
        pex_jw_str(&w, "driver", s_self);
        pex_jw_str(&w, "addr", a);
        pex_jw_str(&w, "name", d[i].name);
        pex_jw_int(&w, "rssi", d[i].rssi);
        pex_jw_str(&w, "model", "pm220");
        pex_jw_str(&w, "transport", "classic");
        pex_jw_int(&w, "cod", d[i].cod);
        pex_jw_end(&w);
    }
    pex_jw_end(&w);
    return pex_jw_finish(&w);
}

static void scan_step(void) {
    if (!s_scanning || pex_ms() - s_t_scan < 250) { return; }
    s_t_scan = pex_ms();
    pex_cl_dev_t d[DEVS];
    int n = pex_bt_classic_devices(d, DEVS, 0, 0, NULL);
    for (int i = 0; i < n; i++) {
        if (!wanted(&d[i])) { continue; }
        int seen = 0;
        for (int k = 0; k < s_nseen; k++) { seen |= memcmp(s_seen[k], d[i].addr, 6) == 0; }
        if (seen || s_nseen >= DEVS) { continue; }
        memcpy(s_seen[s_nseen++], d[i].addr, 6);
        char a[18];
        prn_addr_fmt(d[i].addr, a);
        pex_jw_t w;
        pex_jw_init(&w, s_out, sizeof s_out);
        pex_jw_obj(&w, NULL);
        pex_jw_str(&w, "driver", s_self);
        pex_jw_str(&w, "addr", a);
        pex_jw_str(&w, "name", d[i].name);
        pex_jw_int(&w, "rssi", d[i].rssi);
        pex_jw_str(&w, "model", "pm220");
        pex_jw_end(&w);
        if (pex_jw_finish(&w) > 0) { prn_hub_emit(&s_hub, "found", s_out); }
    }
    if (!pex_bt_classic_scanning()) {
        s_scanning = 0;
        emit_state();
    }
}

/* ---- the dispatch the hub calls ---- */
static int info(char *out, size_t cap) {
    return pex_snprintf(out, cap,
        "{\"driver\":\"%s\",\"abi\":1,\"available\":%s,\"models\":["
        "{\"id\":\"pm220\",\"vendor\":\"Nelko\",\"name\":\"PM220\",\"transport\":\"classic\","
        "\"dpi\":203,\"head_dots\":384,\"min_len_mm\":10,\"max_len_mm\":200,\"media\":[\"gap\",\"continuous\",\"blackmark\"],"
        "\"density\":{\"min\":0,\"max\":15,\"default\":8},\"speed\":null,\"black_bit\":0,\"copies\":true,"
        "\"status\":[\"battery\",\"paper\",\"lid\",\"media\"],"
        "\"presets\":[\"50x30mm\",\"40x30mm\",\"40x20mm\",\"30x20mm\",\"50x20mm\",\"50x80mm\"]}]}",
        s_self, classic_ok() ? "true" : "false");
}

static int connect_(const char *addr, const char *cfg) {
    if (!classic_ok()) { return PEX_ENOTSUP; }
    uint8_t a[6];
    if (!prn_addr_parse(addr, a)) { return PEX_EINVAL; }
    if (s_link != L_IDLE && s_link != L_BACKOFF && memcmp(a, s_addr, 6) == 0) { emit_state(); return PEX_OK; }
    if (s_link == L_OPENING || s_link == L_OPEN) {
        if (s_job) { return PEX_EBUSY; }
        s_want = 0;
        pex_bt_classic_close(s_handle);
        s_handle = 0;
    }
    memcpy(s_addr, a, 6);
    prn_addr_fmt(a, s_addr_s);
    build_pins(cfg);
    int64_t ch = 0;
    s_channel = cfg && pex_json_int(cfg, "channel", &ch) && ch > 0 && ch < 31 ? (uint8_t)ch : 0;
    s_have_bat = s_have_cfg = s_have_st = 0;
    s_ready = -1;
    s_want = 1;
    s_backoff_n = 0;
    s_auth_fails = 0;
    if (s_scanning) { pex_bt_classic_scan_stop(); s_scanning = 0; }
    int rc = open_link();
    if (rc == PEX_EBUSY) {           /* the radio is paging or scanning: try shortly */
        s_link = L_BACKOFF;
        s_t_retry = pex_ms() + 500u;
        return PEX_EPENDING;
    }
    return rc == PEX_OK ? PEX_EPENDING : rc;
}

static void disconnect_(void) {
    s_want = 0;
    if (s_job) { job_fail("disconnected"); }
    if (s_handle) { pex_bt_classic_close(s_handle); }
    s_handle = 0;
    s_link = L_IDLE;
    set_why(NULL);
    emit_state();
}

static int status_json(char *out, size_t cap) {
    pex_jw_t w;
    pex_jw_init(&w, out, cap);
    pex_jw_arr(&w, NULL);
    if (s_addr_s[0]) {
        pex_jw_obj(&w, NULL);
        status_fields(&w);
        pex_jw_end(&w);
    }
    pex_jw_end(&w);
    return pex_jw_finish(&w);
}

int prn_drv(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2) {
    switch (cmd) {
    case PRN_DRV_INFO: return info((char *)(uintptr_t)a0, a1);
    case PRN_DRV_SCAN:
        if (!classic_ok()) { return PEX_ENOTSUP; }
        if (!a0) { pex_bt_classic_scan_stop(); s_scanning = 0; return PEX_OK; }
        if (s_link == L_OPENING) { return PEX_EBUSY; }
        if (!s_scanning) {
            int rc = pex_bt_classic_scan(a0);
            if (rc < 0 && rc != PEX_EBUSY) { return rc; }
            s_scanning = 1;
            s_nseen = 0;
            s_t_scan = pex_ms();
            emit_state();
        }
        return PEX_OK;
    case PRN_DRV_DEVICES: return devices_json((char *)(uintptr_t)a0, a1);
    case PRN_DRV_CONNECT: return connect_((const char *)(uintptr_t)a0, (const char *)(uintptr_t)a1);
    case PRN_DRV_DISCONNECT: {
        const char *a = (const char *)(uintptr_t)a0;
        uint8_t b[6];
        if (a && (!prn_addr_parse(a, b) || memcmp(b, s_addr, 6))) { return PEX_ENOENT; }
        disconnect_();
        return PEX_OK;
    }
    case PRN_DRV_STATUS: return status_json((char *)(uintptr_t)a1, a2);
    case PRN_DRV_JOB_BEGIN: return job_begin((const prn_job_t *)(uintptr_t)a0);
    case PRN_DRV_JOB_FEED: return job_feed((const uint8_t *)(uintptr_t)a0, a1);
    case PRN_DRV_JOB_END: job_end(); return PEX_OK;
    case PRN_DRV_CANCEL:
        s_job = 0;
        s_pend_n = s_pend_off = 0;
        emit_state();
        return PEX_OK;
    }
    return PEX_EINVAL;
}
PEX_EXPORT(prn_drv);

static int tick(void *arg) {
    (void)arg;
    if (!prn_hub_ok(&s_hub)) { prn_hub_attach(&s_hub, s_self); }
    if (!classic_ok()) { return PEX_TASK_AGAIN; }
    scan_step();
    if (s_link == L_BACKOFF && s_want && (int32_t)(pex_ms() - s_t_retry) >= 0) {
        int rc = open_link();
        if (rc == PEX_EBUSY) { s_t_retry = pex_ms() + 500u; }
        else if (rc < 0) { schedule_retry("open refused"); }
    }
    if (s_link == L_OPEN) {
        take_replies();
        if (s_job) {
            job_step();
        } else {
            if (pex_ms() - s_t_keepalive >= KEEPALIVE_MS) {
                s_t_keepalive = pex_ms();
                ask(PM_BATTERY);
            }
            if (s_expect == PM_NONE && s_nq) {
                int k = s_q[0];
                memmove(s_q, s_q + 1, sizeof s_q[0] * (size_t)--s_nq);
                send_query(k);
            }
        }
    }
    return PEX_TASK_AGAIN;
}

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host; (void)arg;
    switch (reason) {
    case PEX_APP_RUN:
        pex_snprintf(s_self, sizeof s_self, "%s", pex_name(pex_self()));
        s_link = L_IDLE;
        s_job = 0;
        s_want = 0;
        s_handle = 0;
        s_addr_s[0] = 0;
        prn_hub_attach(&s_hub, s_self);
        s_task = pex_sched_every(5, tick, NULL);
        return s_task > 0 ? PEX_OK : s_task;
    case PEX_APP_STOP:
        if (s_job) { job_fail("driver stopped"); }
        s_want = 0;
        if (s_handle && classic_ok()) { pex_bt_classic_close(s_handle); }
        s_handle = 0;
        s_link = L_IDLE;
        if (s_scanning && classic_ok()) { pex_bt_classic_scan_stop(); }
        s_scanning = 0;
        prn_hub_detach(&s_hub, s_self);
        if (s_task > 0) { pex_sched_cancel(s_task); }
        s_task = 0;
        return PEX_OK;
    }
    return PEX_OK;
}
