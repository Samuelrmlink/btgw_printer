/* Nelko TSPL printers over BLE: the P21 now, and its firmware family (P22,
 * P31S) as more rows in MODELS[] later. The command stream is the one the
 * PM220 speaks over SPP (docs/protocols/nelko-p21.md): CONFIG?, BATTERY?,
 * ESC !? (one status code), ESC !o (the 16-byte label block) and the TSPL
 * job. It goes over GATT service ff00: write ff02 (without response), the
 * replies notify on ff01, flow-control credits notify on ff03.
 *
 * One printer at a time. A 5 ms tick drives:
 *   IDLE -> CONNECT (scan first if the address is not in the scan table)
 *        -> DISCOVER (ff02 + ff01, subscribe ff01 and ff03) -> READY
 *   READY: CONFIG?, BATTERY?, ESC !o once, then ESC !? every POLL_MS and
 *          BATTERY? every KEEPALIVE_MS while idle, one query at a time;
 *          jobs stream from a TX ring
 *   link loss: BACKOFF, then CONNECT again while auto_reconnect is set.
 * Writes are min(MTU - 3, 244, the packet size ff03 announced) bytes, at
 * least GAP_MS apart, 3 in flight, and within the credits ff03 granted (a
 * printer that grants none is paced by the gap alone).
 * BLE callbacks only record what happened; the tick acts on it. The BLE
 * plumbing follows nelko_pl70e (vendors/nelko/pl70e/pl70e.c). */
#include "pex.h"
#include "printer_driver.h"
#include "core/prn_core.h"
#include "proto/tspl/tspl.h"
#include "vendors/nelko/tspl/nelko_proto.h"
#include <stdarg.h>
#include <string.h>

#define DRV_NAME "nelko_ble"
#define TICK_MS 5
#define CONNECT_MS 15000u
#define DISCOVER_MS 8000u
#define SCAN_FOR_CONNECT_MS 5000u
#define REPLY_MS 1800u            /* a query with no answer for this long is given up */
#define IDLE_MS 200u              /* no byte for this long closes a reply that has no terminator */
#define POLL_MS 10000u            /* ESC !? while idle */
#define KEEPALIVE_MS 30000u       /* BATTERY? while idle (the app: every 30 s) */
/* Like the Nelko app, a job goes only on a fresh "ready" (00 or 30, low
 * battery) to an ESC !? asked right before it; older, and JOB_BEGIN asks
 * and answers PEX_EAGAIN (the hub calls again for 5 s). */
#define FRESH_MS 2000u
#define BEGIN_ASK_MS 4000u        /* JOB_BEGIN asked this recently: its answer decides */
#define WAIT_ASK_MS 700u          /* after the job: ESC !? this often until it says ready */
#define SILENT_MS 15000u          /* after the job: no answer at all -> failed */
#define DONE_MS 30000u            /* after the job: answers, never ready -> taken as printed */
/* The gateway reports every write within 5 s, but its 8-deep module event
 * ring drops the oldest event when it overflows (parent-requirements B6). */
#define WRITE_LOST_MS 6000u
#define CREDIT_WAIT_MS 500u       /* no credit for this long: send anyway */
#define GAP_MS 10u                /* default gap between writes; config "gap_ms" */
#define TX_CAP 512
#define INFLIGHT_MAX 3            /* the gateway queues 4 GATT ops per link */

/* ---- the models ---- */
typedef struct {
    const char *id;
    const char *prefix;          /* the advertised name starts with this */
    uint16_t head_dots;
    int8_t dmin, dmax;
    const char *caps;
} model_t;

/* PEX_PTR_TABLE: tables of pointers live in RW under model B (pex.h) */
static PEX_PTR_TABLE model_t MODELS[] = {
    { "p21", "P21", 96, 1, 15,
      "{\"id\":\"p21\",\"vendor\":\"Nelko\",\"name\":\"P21\",\"transport\":\"ble\","
      "\"dpi\":203,\"head_dots\":96,\"min_len_mm\":15,\"max_len_mm\":100,\"media\":[\"gap\",\"continuous\"],"
      "\"density\":{\"min\":1,\"max\":15,\"default\":8},\"speed\":null,\"black_bit\":0,\"copies\":false,\"placement\":false,"
      "\"orientation\":\"along_feed\",\"status\":[\"battery\",\"paper\",\"lid\",\"media\"],"
      "\"presets\":[\"nelko-p21-14x40\",\"nelko-p21-12x40\",\"nelko-p21-14x50\",\"nelko-p21-14x75\",\"nelko-p21-15x30\",\"nelko-p21-14x30\"]}" },
};
#define NMODELS (int)(sizeof MODELS / sizeof MODELS[0])

enum { ST_IDLE = 0, ST_SCAN, ST_CONNECT, ST_DISCOVER, ST_READY, ST_BACKOFF };
enum { J_NONE = 0, J_SEND, J_WAIT };

static prn_hub_t s_hub;
static int s_task, s_listen = -1;
static const model_t *s_model = &MODELS[0];

/* the link */
static uint8_t s_st, s_sub;
static uint32_t s_t0, s_backoff_n, s_scan_until;
static uint8_t s_addr[6];
static char s_addr_str[18];
static char s_why[40];
static int s_have_addr, s_auto, s_wr;
static int s_wchr = -1, s_nchr = -1, s_cchr = -1;
static int s_mtu;
static uint32_t s_gap_ms = GAP_MS;
static volatile uint8_t s_ev_ready, s_ev_fail, s_ev_down, s_ev_off;

/* writes in flight and flow control */
static int s_inflight;
static uint32_t s_write_ev_at, s_write_at;
static uint16_t s_wlen[INFLIGHT_MAX + 1];   /* sizes of the writes in flight, oldest first */
static uint8_t s_wlen_n;
static uint32_t s_lost_bytes;      /* bytes of failed writes not yet dealt with */
static uint32_t s_writes_failed, s_writes_lost, s_credit_waits;
static int s_credits = -1;         /* packets ff03 allows (-1: it never said) */
static uint32_t s_credit_at;
static uint16_t s_pkt;             /* packet size ff03 announced, 0: none */

/* what the printer said */
static nk_rx_t s_rx;
static uint32_t s_t_rx, s_t_asked, s_t_poll, s_t_keepalive;
static int s_expect;
static uint8_t s_q[4], s_nq;       /* queries waiting to be sent */
static nk_battery_t s_bat;
static nk_config_t s_cfg;
static nk_status_t s_lbl;
static uint8_t s_have_bat, s_have_cfg, s_have_lbl;
static int s_ready = -1;           /* the last status code */
static uint32_t s_ready_at, s_begin_ask;
static uint32_t s_unanswered;

/* scanning */
static uint8_t s_scanning, s_nseen;
static uint8_t s_seen[8][6];
static uint32_t s_t_scan;

/* the job */
static uint8_t s_job_st;
static prn_job_t s_job;
static tspl_job_t s_tj;
static uint8_t s_tx[TX_CAP];
static uint32_t s_tx_head, s_tx_tail;     /* ring, head = write, tail = read */
static char s_pend[192];                  /* job text waiting for room in the ring */
static uint16_t s_pend_n;
static uint8_t s_head_due;                /* the next copy's head goes once the ring is sent */
static uint8_t s_last_pass, s_heard;
static uint32_t s_rows_in, s_sent_total, s_wait_t0;
static uint32_t s_pad_left;               /* white bitmap bytes still owed */
static uint8_t s_pad_crlf;

static char s_out[400];

static uint32_t tx_used(void) { return s_tx_head - s_tx_tail; }
static uint32_t tx_room(void) { return TX_CAP - tx_used(); }

static void tx_put(const void *p, uint32_t n) {
    const uint8_t *b = p;
    for (uint32_t i = 0; i < n; i++) { s_tx[(s_tx_head + i) % TX_CAP] = b[i]; }
    s_tx_head += n;
}

static const model_t *model_by_name(const char *n) {
    for (int i = 0; n && i < NMODELS; i++) {
        if (strncmp(n, MODELS[i].prefix, strlen(MODELS[i].prefix)) == 0) { return &MODELS[i]; }
    }
    return NULL;
}

static const model_t *model_by_id(const char *id) {
    for (int i = 0; i < NMODELS; i++) {
        if (strcmp(MODELS[i].id, id) == 0) { return &MODELS[i]; }
    }
    return NULL;
}

static const char *state_name(void) {
    switch (s_st) {
    case ST_SCAN: case ST_CONNECT: case ST_DISCOVER: return "connecting";
    case ST_READY: return s_job_st != J_NONE ? "printing" : "connected";
    case ST_BACKOFF: return "waiting";
    }
    return s_scanning ? "scanning" : "idle";
}

/* JSON by format strings into one buffer: in a module, one literal per
 * object costs far less than a pex_jw call per field. The values are our
 * own words and numbers, plus a name that matched a model's prefix. */
static char *s_w;
static size_t s_wcap, s_wn;

static void w_start(char *out, size_t cap) { s_w = out; s_wcap = cap; s_wn = 0; out[0] = 0; }

static void w_add(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (s_wn < s_wcap) {
        int k = pex_vsnprintf(s_w + s_wn, s_wcap - s_wn, fmt, ap);
        s_wn = k < 0 ? s_wcap : s_wn + (size_t)k;
    }
    va_end(ap);
}

/* the length, or PEX_ENOMEM if it didn't fit */
static int w_end(void) { return s_wn < s_wcap ? (int)s_wn : PEX_ENOMEM; }

static void head_fields(void) {
    w_add("{\"driver\":\"" DRV_NAME "\",\"addr\":\"%s\",\"model\":\"%s\",\"state\":\"%s\"",
          s_addr_str, s_model->id, state_name());
    if (s_why[0]) { w_add(",\"why\":\"%s\"", s_why); }
}

static void emit_state(const char *why) {
    pex_snprintf(s_why, sizeof s_why, "%s", why ? why : "");
    w_start(s_out, sizeof s_out);
    head_fields();
    if (s_st == ST_BACKOFF) { w_add(",\"retry_ms\":%u", (unsigned)prn_backoff_ms(s_backoff_n, 8000, 60000)); }
    w_add("}");
    if (w_end() > 0) { prn_hub_emit(&s_hub, "state", s_out); }
}

/* What a status code means for printing: NULL when it may print (*busy set
 * when it should wait first), else the reason it can't. */
static const char *verdict(int b, int *busy) {
    *busy = b == 0x20 || b == 0x02;
    if (*busy || !nk_status_error(b)) { return NULL; }
    switch (b) {
    case 0x01: return "lid open";
    case 0x04: case 0x24: return "out of paper";
    case 0x10: return "paused";
    case 0x80: return "overheated";
    }
    return nk_status_name(b);
}

static const char *tf(int v) { return v ? "true" : "false"; }

/* The status object, as nelko_tspl's, plus the link's counters. */
static void status_obj(void) {
    head_fields();
    if (s_have_bat) { w_add(",\"battery\":%d,\"charging\":%s", s_bat.percent, tf(s_bat.charging)); }
    if (s_ready >= 0) {
        w_add(",\"printer\":\"%s\",\"lid\":%s,\"paper\":%s,\"busy\":%s", nk_status_name(s_ready), tf(s_ready == 0x01),
              tf(s_ready != 0x04 && s_ready != 0x24), tf(s_ready == 0x20));
    }
    if (s_have_lbl) {
        w_add(",\"media\":{\"type\":\"%s\"", nk_paper_name(s_lbl.paper));
        if (s_lbl.width_mm || s_lbl.length_mm) { w_add(",\"w_mm\":%d,\"h_mm\":%d", s_lbl.width_mm, s_lbl.length_mm); }
        w_add(",\"crc_ok\":%s}", tf(s_lbl.crc_ok));
    }
    if (s_have_cfg) { w_add(",\"dpi\":%d,\"fw\":\"%s\",\"hw\":\"%s\"", s_cfg.dpi, s_cfg.fw, s_cfg.hw); }
    if (s_st == ST_READY) {
        w_add(",\"mtu\":%d", s_mtu);
        if (s_credits >= 0) { w_add(",\"credits\":%d", s_credits); }
        if (s_unanswered) {
            /* how long the printer has said nothing (since connect if never) */
            w_add(",\"unanswered\":%u,\"quiet_ms\":%u", (unsigned)s_unanswered,
                  (unsigned)(pex_ms() - (s_ready_at ? s_ready_at : s_t0)));
        }
    }
    if (s_job_st != J_NONE) { w_add(",\"sent\":%u,\"inflight\":%d", (unsigned)s_sent_total, s_inflight); }
    /* per connection, and only once they count */
    if (s_writes_failed) { w_add(",\"writes_failed\":%u", (unsigned)s_writes_failed); }
    if (s_writes_lost) { w_add(",\"writes_lost\":%u", (unsigned)s_writes_lost); }
    if (s_credit_waits) { w_add(",\"credit_waits\":%u", (unsigned)s_credit_waits); }
    w_add("}");
}

static void emit_status(void) {
    w_start(s_out, sizeof s_out);
    status_obj();
    if (w_end() > 0) { prn_hub_emit(&s_hub, "status", s_out); }
}

static void finish_job(int outcome, const char *msg) {
    uint32_t id = s_job.id;
    s_job_st = J_NONE;
    s_pend_n = 0;
    s_head_due = 0;
    s_t_poll = s_t_keepalive = pex_ms();
    prn_hub_job_done(&s_hub, id, outcome, msg);
    emit_state(NULL);
}

static void drop_link(const char *why) {
    if (s_job_st != J_NONE) { finish_job(PRN_JOB_FAILED, why); }
    if (s_have_addr) { pex_bt_disconnect(s_addr); }
    s_wchr = s_nchr = s_cchr = -1;
    s_inflight = 0;
    s_wlen_n = 0;
    s_lost_bytes = s_pad_left = 0;
    s_pad_crlf = 0;
    s_tx_head = s_tx_tail = 0;
    s_expect = NK_NONE;
    s_nq = 0;
    s_ready = -1;
    s_ev_ready = s_ev_fail = s_ev_down = s_ev_off = 0;
    s_st = s_auto && s_have_addr ? ST_BACKOFF : ST_IDLE;
    s_t0 = pex_ms();
    emit_state(why);
}

static void on_bt(void *arg, const pex_bt_event_t *ev) {
    (void)arg;
    if (!s_have_addr || memcmp(ev->addr, s_addr, 6) != 0) { return; }
    switch (ev->kind) {
    case PEX_BT_READY: s_ev_ready = 1; break;
    case PEX_BT_FAILED: s_ev_fail = 1; break;
    case PEX_BT_LINK_DOWN: s_ev_down = 1; break;
    case PEX_BT_WRITE: {
        /* write done: status = att_error | reason << 8, and no data */
        if (s_inflight > 0) { s_inflight--; }
        s_write_ev_at = pex_ms();
        uint16_t len = 0;
        if (s_wlen_n) {
            len = s_wlen[0];
            memmove(s_wlen, s_wlen + 1, (size_t)(--s_wlen_n) * sizeof s_wlen[0]);
        }
        if (ev->status != 0) {
            /* the gateway says these bytes didn't reach the printer */
            s_writes_failed++;
            s_lost_bytes += len;
        }
        break;
    }
    case PEX_BT_NOTIFY:
    case PEX_BT_INDICATE:
        if (ev->chr == s_cchr && s_cchr >= 0) {
            /* ff03: 01 NN = NN more packets may come, 02 LL LL = the packet size */
            if (ev->len >= 2 && ev->data[0] == 1) {
                s_credits = ev->data[1];
                s_credit_at = pex_ms();
            } else if (ev->len >= 3 && ev->data[0] == 2) {
                s_pkt = (uint16_t)(ev->data[1] | ev->data[2] << 8);
            }
        } else if (ev->len >= 3 && memcmp(ev->data, "AT+", 3) == 0) {
            /* "AT+DISCOVERABLE=0\r": the P21 is switching itself off */
            s_ev_off = 1;
        } else {
            nk_rx_push(&s_rx, ev->data, ev->len);
            s_t_rx = pex_ms();
        }
        break;
    default: break;
    }
}

/* ---- sending ---- */

/* White bitmap bytes and a CRLF owed after a cancel or lost bytes. */
static void pad_fill(void) {
    while (s_pad_left && tx_room()) {
        s_tx[s_tx_head % TX_CAP] = 0xff;
        s_tx_head++;
        s_pad_left--;
    }
    if (!s_pad_left && s_pad_crlf && tx_room() >= 2) {
        tx_put("\r\n", 2);
        s_pad_crlf = 0;
    }
}

static void pend_fill(void) {
    if (s_pend_n && s_pend_n <= tx_room()) {
        tx_put(s_pend, s_pend_n);
        s_pend_n = 0;
    }
}

/* One write if the link, the gap and the credits allow. 1 if one went. */
static int pump_one(uint32_t now) {
    if (s_wchr < 0 || s_inflight >= INFLIGHT_MAX || !tx_used()) { return 0; }
    if (now - s_write_at < s_gap_ms) { return 0; }
    if (s_credits == 0) {
        if (now - s_credit_at < CREDIT_WAIT_MS) { return 0; }
        s_credit_waits++;           /* none came: go on, paced by the gap */
        s_credit_at = now;
    }
    uint32_t n = tx_used();
    uint32_t max = (uint32_t)(s_mtu > 23 ? s_mtu - 3 : 20);
    if (max > 244) { max = 244; }
    if (s_pkt >= 20 && max > s_pkt) { max = s_pkt; }
    if (n > max) { n = max; }
    uint8_t chunk[244];
    for (uint32_t i = 0; i < n; i++) { chunk[i] = s_tx[(s_tx_tail + i) % TX_CAP]; }
    int rc = pex_bt_write(s_addr, s_wchr, chunk, n, s_wr, 1);
    if (rc == PEX_EBUSY) { return 0; }
    if (rc < 0) {
        s_writes_failed++;
        s_lost_bytes += n;
    } else {
        if (!s_inflight) { s_write_ev_at = now; }
        s_inflight++;
        if (s_wlen_n < INFLIGHT_MAX + 1) { s_wlen[s_wlen_n++] = (uint16_t)n; }
    }
    if (s_credits > 0) { s_credits--; }
    s_write_at = now;
    s_tx_tail += n;
    s_sent_total += n;
    return 1;
}

static void send_query(int kind) {
    static PEX_PTR_TABLE const char *q[] = { "", "BATTERY?\r\n", "CONFIG?\r\n", "\x1b!?", "\x1b!o\r\n" };
    const char *s = q[kind];
    uint32_t n = (uint32_t)strlen(s);
    if (s_pad_left || s_pad_crlf || tx_room() < n) { return; }
    tx_put(s, n);
    s_expect = kind;
    s_t_asked = pex_ms();
}

static void ask(int kind) {
    if (s_nq < sizeof s_q) { s_q[s_nq++] = (uint8_t)kind; }
}

/* A failed write lost its bytes. Inside a BITMAP the printer would still
 * wait for them and read everything after as image data, deaf to ESC !?.
 * So the job fails, and the lost bytes plus the rows not yet queued go out
 * as white, then a CRLF: the parser is back in step, no PRINT runs, and the
 * next job's CLS clears the buffer. Too much white is harmless (one junk
 * line, ended by the CRLF). Only one copy's bitmap is ever in the ring. */
static void lost_bytes(void) {
    uint32_t n = s_lost_bytes;
    s_lost_bytes = 0;
    pex_log(PEX_LOG_WARN, DRV_NAME ": %u bytes didn't reach the printer", (unsigned)n);
    if (s_job_st != J_SEND) { return; }    /* a status query: nothing to resync */
    s_pad_left += n + (s_job.rows_total - s_rows_in) * s_job.row_bytes;
    s_pad_crlf = 1;
    char m[48];
    pex_snprintf(m, sizeof m, "%u bytes didn't reach the printer", (unsigned)n);
    finish_job(PRN_JOB_FAILED, m);
}

/* ---- replies ---- */
static void take_replies(uint32_t now) {
    nk_reply_t r;
    int idle = s_rx.n && now - s_t_rx >= IDLE_MS;
    if (idle && s_rx.n == 1 && s_expect == NK_NONE && strcmp(nk_status_name(s_rx.b[0]), "unknown")) {
        s_expect = NK_READY;              /* a status code nobody asked for (low battery, ...) */
    }
    while (s_rx.n && nk_rx_take(&s_rx, s_expect, idle, &r)) {
        if (r.kind == NK_NONE) { continue; }
        if (r.kind == s_expect) { s_expect = NK_NONE; }
        switch (r.kind) {
        case NK_BATTERY: s_bat = r.u.battery; s_have_bat = 1; break;
        case NK_CONFIG: s_cfg = r.u.config; s_have_cfg = 1; break;
        case NK_STATUS: s_lbl = r.u.status; s_have_lbl = 1; r.u.ready = s_lbl.readiness; /* fall through */
        case NK_READY:
            s_ready = r.u.ready;
            s_ready_at = now;
            s_unanswered = 0;
            break;
        }
        emit_status();
        if (s_job_st == J_WAIT && r.kind == NK_READY) {
            int busy;
            const char *bad = verdict(s_ready, &busy);
            s_heard = 1;
            if (bad) {
                finish_job(PRN_JOB_FAILED, bad);
            } else if (!busy) {
                finish_job(PRN_JOB_OK, NULL);
            }
        }
    }
    if (s_expect != NK_NONE && now - s_t_asked > REPLY_MS) {
        s_expect = NK_NONE;
        s_unanswered++;
    }
}

/* ---- the job ---- */
static int pend_add(int (*fn)(char *, size_t, const tspl_job_t *)) {
    int n = fn(s_pend + s_pend_n, sizeof s_pend - s_pend_n, &s_tj);
    if (n < 0) { return 0; }
    s_pend_n = (uint16_t)(s_pend_n + n);
    return 1;
}

static void tick_job(uint32_t now) {
    if (s_inflight > 0 && now - s_write_ev_at > WRITE_LOST_MS) {
        s_writes_lost += (uint32_t)s_inflight;
        pex_log(PEX_LOG_WARN, DRV_NAME ": %d write event(s) never came; carrying on", s_inflight);
        s_inflight = 0;
        s_wlen_n = 0;
        s_write_ev_at = now;
    }
    if (s_job_st == J_SEND && !s_pend_n && !tx_used() && !s_inflight && (s_head_due || s_last_pass)) {
        if (s_head_due) {
            /* the copy before went out whole: the next copy's head */
            s_head_due = 0;
            s_rows_in = 0;
            pend_add(tspl_head);
        } else {
            s_job_st = J_WAIT;
            s_wait_t0 = now;
            s_heard = 0;
        }
    }
    if (s_job_st == J_SEND && !s_pend_n && !s_head_due && s_rows_in < s_job.rows_total &&
        tx_room() >= s_job.row_bytes) {
        prn_hub_kick(&s_hub);
    }
    if (s_job_st == J_WAIT) {
        if (!s_heard && now - s_wait_t0 > SILENT_MS) {
            /* not one ESC !? answer since the last byte went out */
            finish_job(PRN_JOB_FAILED, "the printer isn't answering (job sent; it may not have printed)");
        } else if (now - s_wait_t0 > DONE_MS) {
            finish_job(PRN_JOB_OK, "sent; printer did not confirm");
        } else if (s_expect == NK_NONE && now - s_t_asked > WAIT_ASK_MS) {
            send_query(NK_READY);
        }
    }
}

static void tick_ready(uint32_t now) {
    if (s_ev_down) { drop_link("link lost"); return; }
    if (s_ev_off) { drop_link("printer switched off"); return; }
    if (s_lost_bytes) { lost_bytes(); }
    pad_fill();
    pend_fill();
    while (pump_one(now)) {}
    take_replies(now);
    tick_job(now);
    if (s_job_st == J_NONE && s_expect == NK_NONE) {
        if (now - s_t_keepalive >= KEEPALIVE_MS) { s_t_keepalive = now; ask(NK_BATTERY); }
        if (now - s_t_poll >= POLL_MS) { s_t_poll = now; ask(NK_READY); }
        if (s_nq) {
            send_query(s_q[0]);
            /* off the queue once it went into the ring (not while white is owed) */
            if (s_expect != NK_NONE) { memmove(s_q, s_q + 1, --s_nq); }
        }
    }
}

/* ---- the link ---- */
static void start_connect(void) {
    s_ev_ready = s_ev_fail = s_ev_down = s_ev_off = 0;
    s_wchr = s_nchr = s_cchr = -1;
    int rc = pex_bt_connect(s_addr);
    if (rc == PEX_ENOENT) {
        /* not in the scan table: the gateway needs the address type */
        pex_bt_scan(SCAN_FOR_CONNECT_MS, 1);
        s_scan_until = pex_ms() + SCAN_FOR_CONNECT_MS;
        s_st = ST_SCAN;
        return;
    }
    if (rc < 0 && rc != PEX_EPENDING) {
        drop_link("connect refused");
        return;
    }
    int ready = 0;
    if (pex_bt_state(s_addr, &ready) == PEX_BT_CONNECTED && ready) { s_ev_ready = 1; }
    s_st = ST_CONNECT;
    s_t0 = pex_ms();
    emit_state(NULL);
}

static void discover(uint32_t now) {
    if (s_ev_down) { drop_link("link lost"); return; }
    if (s_wchr < 0) {
        s_wchr = pex_bt_find(s_addr, "ff00", "ff02", 0);
        s_nchr = pex_bt_find(s_addr, "ff00", "ff01", 0);
        s_cchr = pex_bt_find(s_addr, "ff00", "ff03", 0);
        if (s_wchr < 0 || s_nchr < 0) {
            s_wchr = s_nchr = s_cchr = -1;
            if (now - s_t0 > DISCOVER_MS) { drop_link("no ff00 service"); }
            return;
        }
        int props = pex_bt_props(s_addr, s_wchr);
        if (props >= 0 && !(props & 0x04)) { s_wr = 1; }    /* no write without response */
        s_sub = 0;
    }
    /* the op queue may be full: subscribe one at a time, again on EBUSY */
    if (s_sub == 0 && pex_bt_subscribe(s_addr, s_nchr, 1) >= 0) { s_sub = 1; }
    if (s_sub == 1 && (s_cchr < 0 || pex_bt_subscribe(s_addr, s_cchr, 1) >= 0)) { s_sub = 2; }
    if (s_sub < 2) { return; }
    s_mtu = pex_bt_mtu(s_addr);
    s_rx.n = 0;
    s_backoff_n = 0;
    s_credits = -1;
    s_pkt = 0;
    s_writes_failed = s_writes_lost = s_credit_waits = s_unanswered = 0;
    s_have_bat = s_have_cfg = s_have_lbl = 0;
    s_ready_at = s_begin_ask = 0;
    s_t_poll = s_t_keepalive = now;
    s_nq = 0;
    ask(NK_CONFIG);
    ask(NK_BATTERY);
    ask(NK_STATUS);
    s_st = ST_READY;
    pex_log(PEX_LOG_INFO, DRV_NAME ": %s ready (mtu %d, %s)", s_addr_str, s_mtu,
            s_wr ? "write" : "write without response");
    emit_state(NULL);
}

/* ---- scanning ---- */
/* one device object, after a comma when the writer holds one already */
static void dev_obj(const pex_bt_dev_t *d, const model_t *m) {
    char a[18];
    pex_bt_addr_str(d->addr, a);
    w_add("%s{\"driver\":\"" DRV_NAME "\",\"addr\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"model\":\"%s\","
          "\"transport\":\"ble\"}", s_wn > 1 ? "," : "", a, d->name, d->rssi, m->id);
}

/* fn for every device of ours in the scan table */
static void each_dev(void (*fn)(const pex_bt_dev_t *, const model_t *, void *), void *arg) {
    pex_bt_dev_t d[8];
    for (int i = 0; i < NMODELS; i++) {
        int n = pex_bt_devices(d, 8, MODELS[i].prefix, NULL);
        for (int k = 0; k < n; k++) {
            if (model_by_name(d[k].name) == &MODELS[i]) { fn(&d[k], &MODELS[i], arg); }
        }
    }
}

static void found_one(const pex_bt_dev_t *d, const model_t *m, void *arg) {
    (void)arg;
    for (int i = 0; i < s_nseen; i++) {
        if (memcmp(s_seen[i], d->addr, 6) == 0) { return; }
    }
    if (s_nseen < 8) { memcpy(s_seen[s_nseen++], d->addr, 6); }
    w_start(s_out, sizeof s_out);
    dev_obj(d, m);
    if (w_end() > 0) { prn_hub_emit(&s_hub, "found", s_out); }
}

/* s_model from the advertised name of s_addr */
static void named_one(const pex_bt_dev_t *d, const model_t *m, void *arg) {
    (void)arg;
    if (memcmp(d->addr, s_addr, 6) == 0) { s_model = m; }
}

static void list_one(const pex_bt_dev_t *d, const model_t *m, void *arg) { (void)arg; dev_obj(d, m); }

static int tick(void *arg) {
    (void)arg;
    uint32_t now = pex_ms();
    if (!prn_hub_ok(&s_hub)) { prn_hub_attach(&s_hub, DRV_NAME); }
    if (s_scanning && now - s_t_scan >= 250) {
        s_t_scan = now;
        each_dev(found_one, NULL);
        if (!pex_bt_scanning()) { s_scanning = 0; if (s_st == ST_IDLE) { emit_state(NULL); } }
    }
    switch (s_st) {
    case ST_SCAN: {
        pex_bt_dev_t d[8];
        int n = pex_bt_devices(d, 8, s_model->prefix, NULL);
        for (int i = 0; i < n; i++) {
            if (memcmp(d[i].addr, s_addr, 6) == 0) {
                pex_bt_scan_stop();
                start_connect();
                return PEX_TASK_AGAIN;
            }
        }
        if ((int32_t)(now - s_scan_until) > 0) { drop_link("not found"); }
        break;
    }
    case ST_CONNECT:
        if (s_ev_fail) { drop_link("connect failed"); }
        else if (s_ev_ready) { s_st = ST_DISCOVER; s_t0 = now; }
        else if (now - s_t0 > CONNECT_MS) { drop_link("connect timeout"); }
        break;
    case ST_DISCOVER: discover(now); break;
    case ST_READY: tick_ready(now); break;
    case ST_BACKOFF:
        if (now - s_t0 > prn_backoff_ms(s_backoff_n, 8000, 60000)) {
            s_backoff_n++;
            start_connect();
        }
        break;
    }
    return PEX_TASK_AGAIN;
}

/* ---- prn_drv commands ---- */
static int drv_info(char *out, size_t cap) {
    int n = pex_snprintf(out, cap, "{\"driver\":\"" DRV_NAME "\",\"abi\":1,\"models\":[");
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

static int drv_devices(char *out, size_t cap) {
    if (cap < 3) { return PEX_ENOMEM; }
    w_start(out, cap);
    w_add("[");
    each_dev(list_one, NULL);
    w_add("]");
    return w_end();
}

static int drv_connect(const char *addr, const char *cfg) {
    uint8_t a[6];
    if (!addr || pex_bt_addr(addr, a) < 0) { return PEX_EINVAL; }
    if (s_st != ST_IDLE && memcmp(a, s_addr, 6) == 0 && s_st != ST_BACKOFF) { emit_state(NULL); return PEX_OK; }
    if (s_st != ST_IDLE) {
        if (s_job_st != J_NONE) { return PEX_EBUSY; }
        s_auto = 0;
        drop_link("another printer");
    }
    memcpy(s_addr, a, 6);
    pex_bt_addr_str(s_addr, s_addr_str);
    s_have_addr = 1;
    /* the model: the config's, else the advertised name's, else the first */
    char id[24];
    const model_t *m = cfg && pex_json_str(cfg, "model", id, sizeof id) ? model_by_id(id) : NULL;
    if (!m) {
        s_model = NULL;
        each_dev(named_one, NULL);
        m = s_model;
    }
    s_model = m ? m : &MODELS[0];
    int v = 1;
    s_auto = (cfg && pex_json_bool(cfg, "auto_reconnect", &v)) ? v : 1;
    v = 0;
    s_wr = (cfg && pex_json_bool(cfg, "write_response", &v)) ? v : 0;
    int64_t g = GAP_MS;
    s_gap_ms = cfg && pex_json_int(cfg, "gap_ms", &g) && g >= 0 && g <= 200 ? (uint32_t)g : GAP_MS;
    s_backoff_n = 0;
    if (s_scanning) { pex_bt_scan_stop(); s_scanning = 0; }
    start_connect();
    return PEX_EPENDING;
}

static int drv_disconnect(const char *addr) {
    uint8_t a[6];
    if (addr && (pex_bt_addr(addr, a) < 0 || memcmp(a, s_addr, 6) != 0)) { return PEX_ENOENT; }
    s_auto = 0;
    if (s_st != ST_IDLE) { drop_link(NULL); }
    return PEX_OK;
}

static int drv_status(const char *addr, char *out, size_t cap) {
    if (cap < 3) { return PEX_ENOMEM; }
    w_start(out, cap);
    w_add("[");
    if (s_have_addr && (!addr || strcmp(addr, s_addr_str) == 0)) { status_obj(); }
    w_add("]");
    return w_end();
}

/* The job text is nelko-p21-rs's, byte for byte (test_p21 t_job_golden):
 * ESC !o first, mm with one decimal, DIRECTION 1,1, BITMAP 0,0, GAP 5.0 mm
 * when the media gives no gap, PRINT 1. */
static void make_tspl(const prn_job_t *j) {
    const model_t *m = s_model;
    memset(&s_tj, 0, sizeof s_tj);
    s_tj.prefix = "\x1b!o\r\n";
    s_tj.width_mm100 = j->width_mm100;
    s_tj.height_mm100 = j->height_mm100;
    s_tj.gap_mm100 = j->media_type == PRN_MEDIA_GAP && !j->gap_mm100 ? 500 : j->gap_mm100;
    s_tj.media = j->media_type;
    s_tj.dpi = 203;
    s_tj.density = j->density < 0 ? -1 : j->density < m->dmin ? m->dmin : j->density > m->dmax ? m->dmax : j->density;
    s_tj.speed = -1;
    s_tj.direction = 1;
    s_tj.mirror = 1;
    s_tj.row_bytes = j->row_bytes;
    s_tj.rows = j->rows_total;
    s_tj.copies = 1;                  /* the hub feeds each copy */
    s_tj.mm_tenths = 1;
}

static int drv_job_begin(const prn_job_t *j) {
    if (s_st != ST_READY) { return PEX_ESTATE; }
    if (s_job_st != J_NONE || s_pad_left || s_pad_crlf) { return PEX_EBUSY; }
    const model_t *jm = model_by_id(j->model);
    if (jm) { s_model = jm; }         /* the job's model is the user's pick */
    if (!j->rows_total || j->width_dots != s_model->head_dots || j->row_bytes != (j->width_dots + 7) / 8 ||
        j->black_bit != 0) {
        return PEX_EINVAL;
    }
    uint32_t now = pex_ms();
    int fresh = s_ready >= 0 && s_ready_at && now - s_ready_at <= FRESH_MS;
    int own = fresh && s_begin_ask && (int32_t)(s_ready_at - s_begin_ask) >= 0 && now - s_begin_ask < BEGIN_ASK_MS;
    int busy = 0;
    const char *bad = fresh ? verdict(s_ready, &busy) : NULL;
    if (bad && own) {
        s_begin_ask = 0;
        return PRN_DRV_ENOTREADY;     /* the answer to its own ESC !?: out of paper, lid open ... */
    }
    if (!own || bad || busy) {
        /* ask (once the query before it is answered) and let the hub call again */
        if (s_expect == NK_NONE && !(own && busy && now - s_t_asked < WAIT_ASK_MS / 2)) {
            send_query(NK_READY);
            if (s_expect == NK_READY) { s_begin_ask = s_t_asked; }
        }
        return PEX_EAGAIN;
    }
    s_begin_ask = 0;
    s_job = *j;
    if (!s_job.copies) { s_job.copies = 1; }
    make_tspl(j);
    s_pend_n = 0;
    if (!pend_add(tspl_head)) { return PEX_EINVAL; }
    s_rows_in = 0;
    s_sent_total = 0;
    s_head_due = s_last_pass = 0;
    s_job_st = J_SEND;
    s_expect = NK_STATUS;             /* the job's ESC !o answers with the 16-byte block */
    s_t_asked = now;
    s_nq = 0;
    emit_state(NULL);
    return 0;                         /* the hub feeds the rows once per copy */
}

static int drv_job_feed(const uint8_t *rows, uint32_t len) {
    if (s_job_st != J_SEND || s_st != ST_READY) { return PEX_ESTATE; }
    pend_fill();
    if (s_pend_n || s_head_due) { return PEX_EBUSY; }
    uint32_t rb = s_job.row_bytes;
    uint32_t left = (s_job.rows_total - s_rows_in) * rb;
    if (len > left) { len = left; }
    uint32_t take = tx_room() / rb * rb;
    if (take > len) { take = len - len % rb; }
    if (!take) { return PEX_EBUSY; }
    tx_put(rows, take);
    s_rows_in += take / rb;
    return (int)take;
}

/* The contract has no "busy" for JOB_END and the hub calls it once per
 * copy, so the PRINT line waits in s_pend until the ring has room. */
static int drv_job_end(uint32_t copy) {
    if (s_job_st != J_SEND || s_rows_in != s_job.rows_total) { return PEX_ESTATE; }
    if (!pend_add(tspl_tail)) { return PEX_EINVAL; }
    if (copy + 1u < s_job.copies) {
        s_head_due = 1;
    } else {
        s_last_pass = 1;
    }
    return PEX_OK;
}

static int drv_cancel(void) {
    if (s_job_st == J_NONE) { return PEX_OK; }
    /* Bytes already sent can't be recalled. A BITMAP cut short would read
     * the next job as image data, so the rows not yet fed go out as white
     * and the PRINT line (still in s_pend) is left off. */
    if (s_job_st == J_SEND && s_rows_in < s_job.rows_total && !s_head_due) {
        s_pad_left = (s_job.rows_total - s_rows_in) * s_job.row_bytes;
        s_pad_crlf = 1;
    }
    finish_job(PRN_JOB_CANCELLED, NULL);
    return PEX_OK;
}

int prn_drv(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2) {
    switch (cmd) {
    case PRN_DRV_INFO: return drv_info((char *)(uintptr_t)a0, a1);
    case PRN_DRV_SCAN:
        if (!a0) { pex_bt_scan_stop(); s_scanning = 0; return PEX_OK; }
        {
            int rc = pex_bt_scan(a0, 1);
            if (rc < 0 && rc != PEX_EBUSY) { return rc; }
        }
        s_scanning = 1;
        s_nseen = 0;
        s_t_scan = pex_ms();
        if (s_st == ST_IDLE) { emit_state(NULL); }
        return PEX_OK;
    case PRN_DRV_DEVICES: return drv_devices((char *)(uintptr_t)a0, a1);
    case PRN_DRV_CONNECT: return drv_connect((const char *)(uintptr_t)a0, (const char *)(uintptr_t)a1);
    case PRN_DRV_DISCONNECT: return drv_disconnect((const char *)(uintptr_t)a0);
    case PRN_DRV_STATUS: return drv_status((const char *)(uintptr_t)a0, (char *)(uintptr_t)a1, a2);
    case PRN_DRV_JOB_BEGIN: return drv_job_begin((const prn_job_t *)(uintptr_t)a0);
    case PRN_DRV_JOB_FEED: return drv_job_feed((const uint8_t *)(uintptr_t)a0, a1);
    case PRN_DRV_JOB_END: return drv_job_end(a0);
    case PRN_DRV_CANCEL: return drv_cancel();
    }
    return PEX_ENOTSUP;
}
PEX_EXPORT(prn_drv);

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host;
    switch (reason) {
    case PEX_APP_RUN: {
        const char *cfg = (const char *)arg;
        s_st = ST_IDLE;
        s_job_st = J_NONE;
        s_have_addr = 0;
        s_addr_str[0] = 0;
        s_model = &MODELS[0];
        s_listen = pex_bt_listen(NULL, on_bt, NULL);
        s_task = pex_sched_every(TICK_MS, tick, NULL);
        prn_hub_attach(&s_hub, DRV_NAME);
        /* {"addr":"..","auto_reconnect":true} in the start config: connect now */
        char a[20];
        if (cfg && pex_json_str(cfg, "addr", a, sizeof a)) { drv_connect(a, cfg); }
        return s_task > 0 ? PEX_OK : s_task;
    }
    case PEX_APP_STOP:
        s_auto = 0;
        if (s_st != ST_IDLE) { drop_link("stopped"); }
        if (s_scanning) { pex_bt_scan_stop(); s_scanning = 0; }
        if (s_listen >= 0) { pex_bt_unlisten(s_listen); s_listen = -1; }
        if (s_task > 0) { pex_sched_cancel(s_task); s_task = 0; }
        prn_hub_detach(&s_hub, DRV_NAME);
        return PEX_OK;
    }
    return PEX_OK;
}
