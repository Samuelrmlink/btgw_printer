/* Nelko TSPL printers over Bluetooth Classic SPP: the driver behind
 * printer_hub for every Nelko model of that firmware family (one TSPL subset,
 * the same ESC !? / ESC !o status bytes, BATTERY? and CONFIG?). The models
 * differ in head width, pairing and presets; MODELS[] below has a row each.
 * docs/protocols/nelko-tspl.md (PM220) and nelko-p21.md (P21).
 *
 * Ported from pm220_rp2_webapp's firmware (src/bt/bt_core.c for the link,
 * src/printer/tspl.c for the job) onto the parent's PEX classic group:
 *   - find: inquiry, keeping devices whose name matches a model's name rule.
 *     A device with no name yet (Class of Device major class 6, imaging) is
 *     held until the inquiry ends, and reported with "model":null only if it
 *     still has no name: the user picks the model. A name that comes later
 *     re-reports it (with its model, or "gone":true when it is not ours);
 *   - link: RFCOMM to the SPP UUID 0x1101 (channel 1 when SDP has no record),
 *     legacy PIN answered from the model's list (a PIN that worked before
 *     first) through PEX_CL_PIN, so the firmware's one drop-key retry tries
 *     the next PIN, not the same one again; SSP just-works;
 *   - no reconnect of its own: a connect op is one attempt (the PIN walk and
 *     a busy radio aside). Paging a printer that is switched off stalls the
 *     gateway's Wi-Fi for about half a minute (parent-requirements B4), so a
 *     lost link or an unreachable printer waits for the user's next connect;
 *   - status: CONFIG?, BATTERY? (every 25 s when idle), ESC !? before a job,
 *     ESC !o's 16-byte status (it leads every job, as the old firmware did);
 *   - jobs: TSPL from proto/tspl, the rows streamed as the TX buffer frees.
 * Nothing in this file rasterises: the hub feeds the web app's rows. */
#include "pex.h"
#include "core/prn_core.h"
#include "proto/tspl/tspl.h"
#include "vendors/nelko/tspl/nelko_proto.h"

#include <string.h>

PEX_WEAK_IMPORT(pex_bt_classic_scan);
PEX_WEAK_IMPORT(pex_bt_classic_scan_stop);
PEX_WEAK_IMPORT(pex_bt_classic_scanning);
PEX_WEAK_IMPORT(pex_bt_classic_devices);
PEX_WEAK_IMPORT(pex_bt_classic_open);
PEX_WEAK_IMPORT(pex_bt_classic_pin);
PEX_WEAK_IMPORT(pex_bt_classic_passkey);
PEX_WEAK_IMPORT(pex_bt_classic_confirm);
PEX_WEAK_IMPORT(pex_bt_classic_send);
PEX_WEAK_IMPORT(pex_bt_classic_tx_room);
PEX_WEAK_IMPORT(pex_bt_classic_listen);
PEX_WEAK_IMPORT(pex_bt_classic_close);
PEX_WEAK_IMPORT(pex_bt_classic_state);
PEX_WEAK_IMPORT(pex_bt_classic_forget);

#define DRIVER "nelko_tspl"
#define SPP_UUID 0x1101
#define FALLBACK_CH 1
#define DEADLINE_MS 20000u
#define REPLY_MS 1800u
#define IDLE_MS 200u
#define KEEPALIVE_MS 25000u
#define PREFLIGHT_MS 1500u
#define BUSY_WAIT_MS 10000u
#define AFTER_MS 15000u
#define NAME_GRACE_MS 6000u
/* after the scan: how long a device reported without a name is watched for
 * its name (the radio asks for names one at a time, a few seconds each) */
#define NAME_WATCH_MS 20000u
#define BACKOFF_FIRST_MS 8000u
#define BACKOFF_MAX_MS 60000u
#define DEVS 16
#define PINS 4
#define COD_MAJOR(c) (((c) >> 8) & 0x1Fu)

/* ---- the models ---- */
enum {
    F_ANYWHERE = 1,     /* the name rule matches anywhere in the name, else at its start */
    F_PLACEMENT = 2,    /* BITMAP x,y: the bitmap may be narrower than the head */
    F_COPIES = 4,       /* PRINT n; else the hub feeds the rows once per copy */
    F_CODES = 8,        /* the status byte is a code (nk_status_name), not bits */
    F_AFTER = 16,       /* ESC !? after the job: an error code fails it */
    F_ESC_CRLF = 32,    /* ESC !? is followed by CRLF */
    F_P21_TEXT = 64,    /* the job text nelko-p21-rs sends: "14.0 mm", DIRECTION 1,1, BITMAP 0,0 */
    F_ASK_MEDIA = 128,  /* ESC !o after connecting, for the roll's size */
};

typedef struct {
    const char *id;
    const char *match;          /* name rule, any case */
    uint16_t head_dots;
    int8_t dmin, dmax;
    uint8_t flags;
    const char *pins[2];        /* legacy PINs to try, after a remembered one */
    const char *caps;           /* the model's object in the caps JSON */
} model_t;

static const model_t MODELS[] = {
    { "pm220", "PM220", 384, 0, 15, F_ANYWHERE | F_PLACEMENT | F_COPIES | F_ESC_CRLF, { "0000", "1234" },
      "{\"id\":\"pm220\",\"vendor\":\"Nelko\",\"name\":\"PM220\",\"transport\":\"classic\","
      "\"dpi\":203,\"head_dots\":384,\"min_len_mm\":10,\"max_len_mm\":200,\"media\":[\"gap\",\"continuous\",\"blackmark\"],"
      "\"density\":{\"min\":0,\"max\":15,\"default\":8},\"speed\":null,\"black_bit\":0,\"copies\":true,\"placement\":true,"
      "\"status\":[\"battery\",\"paper\",\"lid\",\"media\"],"
      "\"presets\":[\"nelko-pm220-50x30\",\"nelko-pm220-40x30\",\"nelko-pm220-40x20\",\"nelko-pm220-30x20\",\"nelko-pm220-50x20\",\"nelko-pm220-50x80\"]}" },
    { "p21", "P21", 96, 1, 15, F_CODES | F_AFTER | F_P21_TEXT | F_ASK_MEDIA, { "0000", "1234" },
      "{\"id\":\"p21\",\"vendor\":\"Nelko\",\"name\":\"P21\",\"transport\":\"classic\","
      "\"dpi\":203,\"head_dots\":96,\"min_len_mm\":15,\"max_len_mm\":100,\"media\":[\"gap\",\"continuous\"],"
      "\"density\":{\"min\":1,\"max\":15,\"default\":8},\"speed\":null,\"black_bit\":0,\"copies\":false,\"placement\":false,"
      "\"orientation\":\"along_feed\",\"status\":[\"battery\",\"paper\",\"lid\",\"media\"],"
      "\"presets\":[\"nelko-p21-14x40\",\"nelko-p21-12x40\",\"nelko-p21-14x50\",\"nelko-p21-14x75\",\"nelko-p21-15x30\",\"nelko-p21-14x30\"]}" },
};
#define NMODELS (int)(sizeof MODELS / sizeof MODELS[0])

enum { L_IDLE = 0, L_OPENING, L_OPEN, L_BACKOFF };
enum { J_NONE = 0, J_PREFLIGHT, J_SEND, J_TAIL, J_DRAIN, J_AFTER };
enum { S_OFF = 0, S_INQUIRY, S_GRACE };

static prn_hub_t s_hub;
static int s_task;
static char s_self[32] = DRIVER;

/* the link */
static const model_t *s_model = &MODELS[0];
static int s_link, s_handle, s_want, s_backoff_n;
static uint8_t s_addr[6];
static char s_addr_s[18];
static uint8_t s_channel;
/* Classic security level for the session (0..4). 0 by default: the parent
 * firmware (b397a84) checks the level before BTstack has read the encryption
 * key size, so every session asking for level >= 2 fails after pairing
 * (docs/parent-requirements.md B1). printers.json "security_level" overrides. */
static uint8_t s_level;
static uint32_t s_t_retry, s_room_max;
static char s_pins[PINS][16];
static int s_npins, s_pin_i;
/* The PIN the next attempt starts with. The firmware fails the whole session
 * when the printer rejects a PIN (seen in mcusim against b397a84: HCI
 * authentication failure 0x05, then FAILED with why rfcomm, not auth), so a
 * PIN list can only be walked across attempts: an attempt that answered a
 * PIN and failed before OPEN moves this on. */
static int s_pin_next;
static char s_pin_used[16];
static char s_why[48];

/* what the printer said */
static nk_rx_t s_rx;
static uint32_t s_t_rx;
static int s_expect;
static uint32_t s_t_asked, s_t_keepalive;
static int s_q[6], s_nq;   /* queries waiting to be sent */
static nk_battery_t s_bat;
static nk_config_t s_cfg;
static nk_status_t s_st;
static int s_have_bat, s_have_cfg, s_have_st, s_ready = -1;

/* scanning */
static int s_scanning;      /* S_* */
static uint32_t s_t_scan;
static uint8_t s_seen[DEVS][6];
static uint8_t s_seen_unnamed[DEVS];   /* reported with "model":null */
static int s_nseen;
static uint32_t s_t_watch;             /* end of the late-name watch, 0 = off */

/* the job */
static int s_job;
static prn_job_t s_jb;
static tspl_job_t s_tj;
static char s_pend[256];
static uint16_t s_pend_n, s_pend_off;
static uint32_t s_t_job;
static int s_preflight;

static char s_out[400];

/* ---- helpers ---- */
static int classic_ok(void) { return pex_avail(pex_bt_classic_open) && pex_avail(pex_bt_classic_send); }

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

/* the model whose name rule matches, or NULL */
static const model_t *model_by_name(const char *n) {
    for (int i = 0; i < NMODELS; i++) {
        const model_t *m = &MODELS[i];
        for (const char *p = n; *p; p++) {
            size_t k = 0;
            while (m->match[k] && upper((unsigned char)p[k]) == m->match[k]) { k++; }
            if (!m->match[k]) { return m; }
            if (!(m->flags & F_ANYWHERE)) { break; }
        }
    }
    return NULL;
}

static const model_t *model_by_id(const char *id) {
    for (int i = 0; id && i < NMODELS; i++) {
        if (strcmp(MODELS[i].id, id) == 0) { return &MODELS[i]; }
    }
    return NULL;
}

/* A device we would take: 1 with *m set when a model's name rule matches; 2
 * for a device with no name yet that looks like a printer (the model is the
 * user's pick); 0 otherwise. A device whose name matches no rule is never
 * ours, whatever its Class of Device says (a PL70e-BT is imaging too). */
static int classify(const pex_cl_dev_t *d, const model_t **m) {
    *m = d->name[0] ? model_by_name(d->name) : NULL;
    if (*m) { return 1; }
    return !d->name[0] && COD_MAJOR(d->cod) == 6u ? 2 : 0;
}

static const char *link_name(void) {
    if (s_job) { return "printing"; }
    switch (s_link) {
    case L_OPENING: return s_pin_i ? "pairing" : "connecting";
    case L_OPEN: return "connected";
    case L_BACKOFF: return "waiting";
    }
    return s_scanning ? "scanning" : "idle";
}

static void head_fields(pex_jw_t *w) {
    pex_jw_str(w, "driver", s_self);
    pex_jw_str(w, "addr", s_addr_s);
    pex_jw_str(w, "model", s_model->id);
    pex_jw_str(w, "state", link_name());
    if (s_why[0]) { pex_jw_str(w, "why", s_why); }
}

static void emit_state(void) {
    pex_jw_t w;
    pex_jw_init(&w, s_out, sizeof s_out);
    pex_jw_obj(&w, NULL);
    head_fields(&w);
    if (s_link == L_OPEN) {
        pex_jw_int(&w, "channel", s_channel);
        if (s_pin_used[0]) { pex_jw_str(&w, "pin", s_pin_used); }
    }
    if (s_link == L_BACKOFF) { pex_jw_int(&w, "retry_ms", (int64_t)(s_t_retry - pex_ms())); }
    pex_jw_end(&w);
    if (pex_jw_finish(&w) > 0) { prn_hub_emit(&s_hub, "state", s_out); }
}

static void emit_log(const char *msg) {
    pex_jw_t w;
    pex_log(PEX_LOG_INFO, "%s: %s", s_self, msg);
    pex_jw_init(&w, s_out, sizeof s_out);
    pex_jw_obj(&w, NULL);
    pex_jw_str(&w, "driver", s_self);
    pex_jw_str(&w, "addr", s_addr_s);
    pex_jw_str(&w, "msg", msg);
    pex_jw_end(&w);
    if (pex_jw_finish(&w) > 0) { prn_hub_emit(&s_hub, "log", s_out); }
}

/* What a status byte means for printing: NULL when it may print (*busy set
 * when it should wait first), else the reason it can't. */
static const char *verdict(int b, int *busy) {
    *busy = 0;
    if (s_model->flags & F_CODES) {
        if (b == 0x20 || b == 0x02) { *busy = 1; return NULL; }
        return nk_status_error(b) ? nk_status_name(b) : NULL;
    }
    if (b & NK_READY_LID) { return "lid open"; }
    if (b & NK_READY_PAPER) { return "out of paper"; }
    *busy = (b & NK_READY_BUSY) != 0;
    return NULL;
}

static void status_fields(pex_jw_t *w) {
    head_fields(w);
    if (s_have_bat) {
        pex_jw_int(w, "battery", s_bat.percent);
        pex_jw_bool(w, "charging", s_bat.charging);
    }
    if (s_ready >= 0) {
        int b = s_ready;
        if (s_model->flags & F_CODES) {
            pex_jw_str(w, "printer", nk_status_name(b));
            pex_jw_bool(w, "lid", b == 0x01);
            pex_jw_bool(w, "paper", b != 0x04 && b != 0x24);
            pex_jw_bool(w, "busy", b == 0x20);
        } else {
            pex_jw_bool(w, "lid", (b & NK_READY_LID) != 0);
            pex_jw_bool(w, "paper", (b & NK_READY_PAPER) == 0);
            pex_jw_bool(w, "busy", (b & NK_READY_BUSY) != 0);
        }
    }
    if (s_have_st) {
        pex_jw_obj(w, "media");
        pex_jw_str(w, "type", nk_paper_name(s_st.paper));
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
    char pin[16];
    s_npins = 0;
    if (cfg && pex_json_str(cfg, "pin", pin, sizeof pin) && pin[0]) {
        pex_snprintf(s_pins[s_npins++], sizeof s_pins[0], "%s", pin);
    }
    for (int i = 0; i < 2 && s_npins < PINS; i++) {
        const char *p = s_model->pins[i];
        int dup = !p;
        for (int k = 0; p && k < s_npins; k++) { dup |= strcmp(s_pins[k], p) == 0; }
        if (!dup) { pex_snprintf(s_pins[s_npins++], sizeof s_pins[0], "%s", p); }
    }
}

static int open_link(void) {
    pex_cl_open_t o;
    memset(&o, 0, sizeof o);
    memcpy(o.addr, s_addr, 6);
    o.uuid = SPP_UUID;
    o.channel = s_channel;           /* 0: SDP */
    o.fallback = FALLBACK_CH;
    o.level = s_level;
    o.io = PEX_CL_IO_NONE;
    o.bond = PEX_CL_BOND_OMIT;
    o.drop_key = 1;
    o.deadline_ms = DEADLINE_MS;
    o.npin = 0;                      /* every PIN request comes to us: see on_cl */
    o.fn = on_cl;
    o.arg = NULL;
    s_pin_i = s_pin_next < s_npins ? s_pin_next : 0;
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
        pex_log(PEX_LOG_INFO, "%s: %s, retry in %u ms", s_model->id, why, (unsigned)d);
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
        nk_rx_push(&s_rx, b, (size_t)n);
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
            pex_log(PEX_LOG_INFO, "%s: PIN request, answering try %d", s_model->id, s_pin_i + 1);
            pex_bt_classic_pin(ev->handle, s_pins[s_pin_i++]);
            emit_state();
        } else {
            pex_bt_classic_pin(ev->handle, NULL);    /* out of PINs: cancel */
        }
        break;
    case PEX_CL_PASSKEY:
        pex_bt_classic_passkey(ev->handle, 1, 0);    /* no person to type one */
        break;
    case PEX_CL_CONFIRM:
        pex_bt_classic_confirm(ev->handle, 1);       /* SSP just-works */
        break;
    case PEX_CL_OPEN: {
        pex_cl_info_t in;
        s_link = L_OPEN;
        s_backoff_n = 0;
        if (s_pin_i > 0) { s_pin_next = s_pin_i - 1; }   /* this PIN works: first next time */
        if (pex_bt_classic_state(ev->handle, &in) == PEX_OK) { s_channel = in.channel; }
        s_room_max = 0;
        room();
        s_rx.n = 0;
        s_nq = 0;
        s_expect = NK_NONE;
        ask(NK_CONFIG);
        ask(NK_BATTERY);
        ask(s_model->flags & F_ASK_MEDIA ? NK_STATUS : NK_READY);
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
        int pinned = s_link == L_OPENING && s_pin_used[0];
        int again = 0;
        if (pinned) {
            /* a PIN was answered and the attempt failed: the same attempt
             * goes on with the next PIN, once through the list */
            s_pin_next = s_pin_i;
            if (s_pin_next < s_npins) {
                again = 1;
            } else {
                s_pin_next = 0;
                pex_log(PEX_LOG_WARN, "%s: pairing failed with every PIN", s_model->id);
            }
        }
        if (!again) {
            s_want = 0;        /* the user connects again; nothing pages on its own */
            if (ev->why == 1 || ev->why == 7) { w = "not reachable (is it on?)"; }
        }
        schedule_retry(w);
        if (again) {
            s_backoff_n = 0;
            s_t_retry = pex_ms() + 500u;
        }
        break;
    }
    }
}

/* ---- queries and replies ---- */
static void send_query(int kind) {
    static const char *const q[] = { "", "BATTERY?\r\n", "CONFIG?\r\n", "\x1b!?\r\n", "\x1b!o\r\n" };
    const char *s = q[kind];
    size_t n = strlen(s);
    if (kind == NK_READY && !(s_model->flags & F_ESC_CRLF)) { n = 3; }
    if (send_bytes(s, n) > 0) {
        s_expect = kind;
        s_t_asked = pex_ms();
    }
}

static void job_ok(const char *msg) {
    uint32_t id = s_jb.id;
    s_job = 0;
    s_t_keepalive = pex_ms();
    prn_hub_job_done(&s_hub, id, PRN_JOB_OK, msg);
    emit_state();
}

static void take_replies(void) {
    nk_reply_t r;
    int idle = s_rx.n && pex_ms() - s_t_rx >= IDLE_MS;
    while (s_rx.n && nk_rx_take(&s_rx, s_expect, idle, &r)) {
        if (r.kind == NK_NONE) { continue; }
        if (r.kind == s_expect) { s_expect = NK_NONE; }
        switch (r.kind) {
        case NK_BATTERY: s_bat = r.u.battery; s_have_bat = 1; break;
        case NK_CONFIG: s_cfg = r.u.config; s_have_cfg = 1; break;
        case NK_READY: s_ready = r.u.ready; break;
        case NK_STATUS: s_st = r.u.status; s_have_st = 1; s_ready = s_st.readiness; break;
        }
        emit_status();
        if (s_job == J_AFTER && r.kind == NK_READY) {
            int busy;
            const char *bad = verdict(s_ready, &busy);
            if (bad) {
                job_fail(bad);
                emit_state();
            } else {
                job_ok(NULL);
            }
        }
    }
    if (s_expect != NK_NONE && pex_ms() - s_t_asked > REPLY_MS) { s_expect = NK_NONE; }
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

/* append to the pending bytes: tspl_head or tspl_tail */
static int pend_add(int (*fn)(char *, size_t, const tspl_job_t *)) {
    int n = fn(s_pend + s_pend_n, sizeof s_pend - s_pend_n, &s_tj);
    if (n < 0) { return 0; }
    s_pend_n = (uint16_t)(s_pend_n + n);
    return 1;
}

static int job_begin(const prn_job_t *j) {
    if (s_link != L_OPEN) { return PEX_ESTATE; }
    if (s_job) { return PEX_EBUSY; }
    /* the job's model is the user's pick: it wins over the connect's guess */
    const model_t *jm = model_by_id(j->model);
    if (jm) { s_model = jm; }
    const model_t *m = s_model;
    if (!j->rows_total || j->row_bytes != (j->width_dots + 7) / 8 ||
        ((m->flags & F_PLACEMENT) ? j->width_dots > m->head_dots : j->width_dots != m->head_dots)) {
        return PEX_EINVAL;
    }
    s_jb = *j;
    memset(&s_tj, 0, sizeof s_tj);
    s_tj.prefix = "\x1b!o\r\n";
    s_tj.width_mm100 = j->width_mm100;
    s_tj.height_mm100 = j->height_mm100;
    s_tj.gap_mm100 = j->gap_mm100;
    s_tj.media = j->media_type;
    s_tj.dpi = 203;
    s_tj.density = j->density < 0 ? -1 : j->density < m->dmin ? m->dmin : j->density > m->dmax ? m->dmax : j->density;
    s_tj.speed = -1;                 /* no SPEED: the old PM220 firmware never sent it */
    s_tj.row_bytes = j->row_bytes;
    s_tj.rows = j->rows_total;
    s_tj.copies = (m->flags & F_COPIES) ? j->copies : 1;
    if (m->flags & F_P21_TEXT) {
        /* byte for byte what nelko-p21-rs sends (the golden test) */
        s_tj.mm_tenths = 1;
        s_tj.direction = 1;
        s_tj.mirror = 1;
        if (j->media_type == PRN_MEDIA_GAP && !j->gap_mm100) { s_tj.gap_mm100 = 500; }
    } else {
        s_tj.direction = j->rotate == 2 ? 1 : 0;
        s_tj.x = j->origin_x;
        s_tj.y = j->origin_y;
    }
    s_pend_n = s_pend_off = 0;
    if (!pend_add(tspl_head)) { return PEX_EINVAL; }
    s_job = J_PREFLIGHT;
    s_preflight = 1;
    int v = 1;
    if (pex_json_bool(j->options, "preflight", &v) && !v) { s_preflight = 0; }
    s_t_job = pex_ms();
    if (s_preflight) {
        s_nq = 0;
        s_expect = NK_NONE;
        s_ready = -1;
        send_query(NK_READY);
    } else {
        s_job = J_SEND;
    }
    emit_state();
    return (m->flags & F_COPIES) ? 1 : 0;    /* 1: copies come from PRINT n */
}

static void job_preflight(void) {
    uint32_t t = pex_ms() - s_t_job;
    if (s_expect == NK_READY && t < PREFLIGHT_MS) { return; }
    if (s_ready >= 0) {
        int busy;
        const char *bad = verdict(s_ready, &busy);
        if (bad) { job_fail(bad); emit_state(); return; }
        if (busy && t < BUSY_WAIT_MS) {
            if (s_expect == NK_NONE && t % 500u < 6) { send_query(NK_READY); }
            return;
        }
    }
    s_job = J_SEND;
    s_expect = NK_STATUS;            /* the job's ESC !o answers with the 16-byte status */
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

static void job_end(uint32_t copy) {
    if (s_job != J_SEND) { return; }
    pend_add(tspl_tail);
    if (!(s_model->flags & F_COPIES) && copy + 1u < s_jb.copies) {
        pend_add(tspl_head);         /* the hub feeds the next copy's rows */
        return;
    }
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
            if (s_model->flags & F_AFTER) {
                /* ask how the printer took it: an error code fails the job */
                s_job = J_AFTER;
                s_t_job = pex_ms();
                s_rx.n = 0;
                send_query(NK_READY);
            } else {
                job_ok(NULL);
            }
        }
        break;
    case J_AFTER:
        if (s_expect == NK_NONE && pex_ms() - s_t_job > REPLY_MS) { send_query(NK_READY); }
        if (pex_ms() - s_t_job > AFTER_MS) { job_ok("sent; no reply from the printer"); }
        break;
    }
}

/* ---- scanning ---- */
static void dev_obj(pex_jw_t *w, const pex_cl_dev_t *d, const model_t *m) {
    char a[18];
    prn_addr_fmt(d->addr, a);
    pex_jw_obj(w, NULL);
    pex_jw_str(w, "driver", s_self);
    pex_jw_str(w, "addr", a);
    pex_jw_str(w, "name", d->name);
    pex_jw_int(w, "rssi", d->rssi);
    if (m) { pex_jw_str(w, "model", m->id); } else { pex_jw_null(w, "model"); }
    pex_jw_str(w, "transport", "classic");
    pex_jw_int(w, "cod", d->cod);
    pex_jw_end(w);
}

/* Devices with no name are listed only once no inquiry runs: their name
 * may still come. */
static int devices_json(char *out, size_t cap) {
    pex_cl_dev_t d[DEVS];
    pex_jw_t w;
    int n = classic_ok() ? pex_bt_classic_devices(d, DEVS, 0, 0, NULL) : 0;
    int settled = !s_scanning && !pex_bt_classic_scanning();
    pex_jw_init(&w, out, cap);
    pex_jw_arr(&w, NULL);
    for (int i = 0; i < n; i++) {
        const model_t *m;
        int k = classify(&d[i], &m);
        if (k == 1 || (k == 2 && settled)) { dev_obj(&w, &d[i], m); }
    }
    pex_jw_end(&w);
    return pex_jw_finish(&w);
}

static void emit_found(const pex_cl_dev_t *d, const model_t *m, int gone) {
    pex_jw_t w;
    pex_jw_init(&w, s_out, sizeof s_out);
    if (gone) {
        char a[18];
        prn_addr_fmt(d->addr, a);
        pex_jw_obj(&w, NULL);
        pex_jw_str(&w, "driver", s_self);
        pex_jw_str(&w, "addr", a);
        pex_jw_str(&w, "name", d->name);
        pex_jw_bool(&w, "gone", 1);
        pex_jw_end(&w);
    } else {
        dev_obj(&w, d, m);
    }
    if (pex_jw_finish(&w) > 0) { prn_hub_emit(&s_hub, "found", s_out); }
}

static int seen_at(const uint8_t *addr) {
    for (int k = 0; k < s_nseen; k++) {
        if (memcmp(s_seen[k], addr, 6) == 0) { return k; }
    }
    return -1;
}

/* k = classify(): 1 = one of ours by name, 2 = no name yet */
static void report(const pex_cl_dev_t *d, const model_t *m, int k) {
    int i = seen_at(d->addr);
    if (i >= 0) {
        /* reported without a name: its name has come since */
        if (s_seen_unnamed[i] && d->name[0]) {
            s_seen_unnamed[i] = 0;
            emit_found(d, m, k != 1);
        }
        return;
    }
    if (k == 0 || s_nseen >= DEVS) { return; }
    memcpy(s_seen[s_nseen], d->addr, 6);
    s_seen_unnamed[s_nseen++] = k == 2;
    emit_found(d, m, 0);
}

static void scan_step(void) {
    if (!s_scanning && !s_t_watch) { return; }
    if (pex_ms() - s_t_scan < 250) { return; }
    pex_cl_dev_t d[DEVS];
    int n = pex_bt_classic_devices(d, DEVS, 0, 0, NULL);
    if (!s_scanning) {
        /* watching for late names only */
        s_t_scan = pex_ms();
        for (int i = 0; i < n; i++) {
            const model_t *m;
            int k = classify(&d[i], &m);
            report(&d[i], m, k);
        }
        if ((int32_t)(pex_ms() - s_t_watch) >= 0) { s_t_watch = 0; }
        return;
    }
    if (s_scanning == S_INQUIRY && !pex_bt_classic_scanning()) {
        /* names still being asked for may come a moment after the inquiry */
        s_scanning = S_GRACE;
        s_t_scan = pex_ms();
    }
    int done = s_scanning == S_GRACE && pex_ms() - s_t_scan >= NAME_GRACE_MS;
    if (s_scanning == S_INQUIRY) { s_t_scan = pex_ms(); }
    for (int i = 0; i < n; i++) {
        const model_t *m;
        int k = classify(&d[i], &m);
        if (k == 1 || (k == 2 && done) || (k == 0 && seen_at(d[i].addr) >= 0)) { report(&d[i], m, k); }
    }
    if (done) {
        s_scanning = S_OFF;
        s_t_watch = (pex_ms() + NAME_WATCH_MS) | 1u;
        emit_state();
    }
}

/* ---- the dispatch the hub calls ---- */
static int info(char *out, size_t cap) {
    int n = pex_snprintf(out, cap, "{\"driver\":\"%s\",\"abi\":1,\"available\":%s,\"models\":[",
                         s_self, classic_ok() ? "true" : "false");
    for (int i = 0; i < NMODELS && n >= 0; i++) {
        size_t k = strlen(MODELS[i].caps);
        if ((size_t)n + k + 4 > cap) { return PEX_ENOMEM; }
        if (i) { out[n++] = ','; }
        memcpy(out + n, MODELS[i].caps, k);
        n += (int)k;
    }
    if (n < 0 || (size_t)n + 3 > cap) { return PEX_ENOMEM; }
    memcpy(out + n, "]}", 3);
    return n + 2;
}

/* the model for a connect: the config's "model", else the name the inquiry
 * saw, else the first model (and a printer.log line saying so) */
static void pick_model(const uint8_t *a, const char *cfg) {
    char id[24];
    const model_t *m = cfg && pex_json_str(cfg, "model", id, sizeof id) ? model_by_id(id) : NULL;
    if (!m) {
        pex_cl_dev_t d[DEVS];
        int n = pex_bt_classic_devices(d, DEVS, 0, 0, NULL);
        for (int i = 0; i < n && !m; i++) {
            if (memcmp(d[i].addr, a, 6) == 0 && d[i].name[0]) { m = model_by_name(d[i].name); }
        }
    }
    s_model = m ? m : &MODELS[0];
    if (!m) {
        char msg[80];
        pex_snprintf(msg, sizeof msg, "model unknown (no \"model\" in the config, no name): using %s", s_model->id);
        emit_log(msg);
    }
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
    pick_model(a, cfg);
    build_pins(cfg);
    s_pin_next = 0;
    int64_t ch = 0;
    s_channel = cfg && pex_json_int(cfg, "channel", &ch) && ch > 0 && ch < 31 ? (uint8_t)ch : 0;
    int64_t lv = 0;
    s_level = cfg && pex_json_int(cfg, "security_level", &lv) && lv >= 0 && lv <= 4 ? (uint8_t)lv : 0;
    s_have_bat = s_have_cfg = s_have_st = 0;
    s_ready = -1;
    s_want = 1;
    s_backoff_n = 0;
    if (s_scanning) { pex_bt_classic_scan_stop(); s_scanning = S_OFF; }
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
        if (!a0) { pex_bt_classic_scan_stop(); s_scanning = S_OFF; return PEX_OK; }
        if (s_link == L_OPENING) { return PEX_EBUSY; }
        if (!s_scanning) {
            int rc = pex_bt_classic_scan(a0);
            if (rc < 0 && rc != PEX_EBUSY) { return rc; }
            s_scanning = S_INQUIRY;
            s_nseen = 0;
            s_t_watch = 0;
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
    case PRN_DRV_JOB_END: job_end(a0); return PEX_OK;
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
                ask(NK_BATTERY);
            }
            if (s_expect == NK_NONE && s_nq) {
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
        s_model = &MODELS[0];
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
        s_scanning = S_OFF;
        prn_hub_detach(&s_hub, s_self);
        if (s_task > 0) { pex_sched_cancel(s_task); }
        s_task = 0;
        return PEX_OK;
    }
    return PEX_OK;
}
