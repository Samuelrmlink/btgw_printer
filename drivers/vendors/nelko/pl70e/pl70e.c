/* Nelko PL70e-BT driver (BLE). The printer is a TSPL label printer behind a
 * Feasycom module; each GATT "transparent UART" pipe is a byte stream to the
 * TSPL parser (docs/protocols/pl70e-bt.md). The web app renders the bitmap;
 * this module wraps it in TSPL and streams it in MTU-sized writes.
 *
 * One printer at a time. A 20 ms tick drives:
 *   IDLE -> CONNECT (scan first if the address is not in the scan table)
 *        -> DISCOVER (find a write/notify pipe, subscribe) -> READY
 *   READY: READSTA every few seconds while idle; jobs stream from a TX ring
 *   link loss: BACKOFF, then CONNECT again while auto_reconnect is set.
 * BLE callbacks only record what happened; the tick acts on it. */
#include "pex.h"
#include "printer_driver.h"
#include "core/prn_core.h"
#include "proto/tspl/tspl.h"
#include "pl70e_proto.h"
#include <string.h>

#define DRV_NAME "nelko_pl70e"
#define TICK_MS 20
#define CONNECT_MS 15000
#define DISCOVER_MS 8000
#define POLL_MS 5000
#define QUERY_TIMEOUT_MS 1500
#define DONE_TIMEOUT_MS 60000     /* after the last byte: wait this long for READY */
#define SCAN_FOR_CONNECT_MS 5000
#define TX_CAP 2048
#define INFLIGHT_MAX 3            /* the gateway queues 4 GATT ops per link */

enum { ST_IDLE = 0, ST_SCAN, ST_CONNECT, ST_DISCOVER, ST_READY, ST_BACKOFF };
enum { JOB_NONE = 0, JOB_SEND, JOB_WAIT };

static prn_hub_t s_hub;
static int s_task, s_listen = -1;
static uint8_t s_st;
static uint32_t s_t0, s_backoff_n, s_scan_until;
static uint8_t s_addr[6];
static char s_addr_str[18];
static int s_have_addr, s_auto, s_wnr;
static int s_wchr = -1, s_nchr = -1, s_pipe;
static int s_mtu;

/* BLE event flags, set by on_bt, consumed by tick */
static volatile uint8_t s_ev_up, s_ev_ready, s_ev_fail, s_ev_down;
static int s_inflight;
static int s_write_err;

/* printer status */
static pl70e_rx_t s_rx;
static int s_status = PL70E_ST_UNKNOWN;
static uint32_t s_status_at, s_query_at;
static uint8_t s_query_pending;
static int s_status_sent = -1;     /* last status emitted */

/* job */
static uint8_t s_job_st;
static prn_job_t s_job;
static uint8_t s_tx[TX_CAP];
static uint32_t s_tx_head, s_tx_tail;     /* ring, head = write, tail = read */
static uint8_t s_tail_queued;
static uint32_t s_rows_in, s_sent_total, s_job_bytes;
static uint32_t s_wait_t0;
static uint32_t s_pad_left;       /* white bitmap bytes still owed after a cancel */
static uint8_t s_pad_crlf;

static char s_buf[512];

static uint32_t tx_used(void) { return s_tx_head - s_tx_tail; }
static uint32_t tx_room(void) { return TX_CAP - tx_used(); }

static void tx_put(const void *p, uint32_t n) {
    const uint8_t *b = p;
    for (uint32_t i = 0; i < n; i++) { s_tx[(s_tx_head + i) % TX_CAP] = b[i]; }
    s_tx_head += n;
}

static void emit_state(const char *state, const char *why) {
    pex_jw_t w;
    pex_jw_init(&w, s_buf, sizeof s_buf);
    pex_jw_obj(&w, NULL);
    pex_jw_str(&w, "addr", s_addr_str);
    pex_jw_str(&w, "model", "pl70e");
    pex_jw_str(&w, "state", state);
    if (why) { pex_jw_str(&w, "why", why); }
    pex_jw_end(&w);
    if (pex_jw_finish(&w) >= 0) { prn_hub_emit(&s_hub, "state", s_buf); }
}

static void emit_status(void) {
    pex_jw_t w;
    pex_jw_init(&w, s_buf, sizeof s_buf);
    pex_jw_obj(&w, NULL);
    pex_jw_str(&w, "addr", s_addr_str);
    pex_jw_str(&w, "model", "pl70e");
    pex_jw_str(&w, "printer", pl70e_status_name(s_status));
    pex_jw_str(&w, "paper", s_status == PL70E_ST_NOPAPER ? "out" : s_status == PL70E_ST_UNKNOWN ? "unknown" : "ok");
    pex_jw_str(&w, "lid", s_status == PL70E_ST_LIDOPEN ? "open" : s_status == PL70E_ST_UNKNOWN ? "unknown" : "closed");
    pex_jw_end(&w);
    if (pex_jw_finish(&w) >= 0) { prn_hub_emit(&s_hub, "status", s_buf); }
}

static void finish_job(int outcome, const char *msg) {
    uint32_t id = s_job.id;
    s_job_st = JOB_NONE;
    if (!s_pad_left && !s_pad_crlf) { s_tx_head = s_tx_tail = 0; }
    s_tail_queued = 0;
    prn_hub_job_done(&s_hub, id, outcome, msg);
}

static void drop_link(const char *why) {
    if (s_job_st != JOB_NONE) { finish_job(PRN_JOB_FAILED, why); }
    if (s_have_addr) { pex_bt_disconnect(s_addr); }
    s_wchr = s_nchr = -1;
    s_inflight = 0;
    s_pad_left = 0;
    s_pad_crlf = 0;
    s_tx_head = s_tx_tail = 0;
    s_query_pending = 0;
    s_status = PL70E_ST_UNKNOWN;
    s_status_sent = -1;
    s_ev_up = s_ev_ready = s_ev_fail = s_ev_down = 0;
    if (s_auto && s_have_addr) {
        s_st = ST_BACKOFF;
        s_t0 = pex_ms();
        emit_state("error", why);
    } else {
        s_st = ST_IDLE;
        emit_state("idle", why);
    }
}

static void on_bt(void *arg, const pex_bt_event_t *ev) {
    (void)arg;
    if (!s_have_addr || memcmp(ev->addr, s_addr, 6) != 0) { return; }
    switch (ev->kind) {
    case PEX_BT_LINK_UP: s_ev_up = 1; break;
    case PEX_BT_READY: s_ev_ready = 1; break;
    case PEX_BT_FAILED: s_ev_fail = 1; break;
    case PEX_BT_LINK_DOWN: s_ev_down = 1; break;
    case PEX_BT_WRITE:
        if (s_inflight > 0) { s_inflight--; }
        if (ev->len >= 2 && ev->data && (ev->data[0] | ev->data[1])) { s_write_err = 1; }
        break;
    case PEX_BT_NOTIFY:
    case PEX_BT_INDICATE: {
        int st = pl70e_rx_feed(&s_rx, ev->data, ev->len);
        if (st >= 0) {
            s_status = st;
            s_status_at = pex_ms();
            s_query_pending = 0;
        }
        break;
    }
    default: break;
    }
}

/* Send one chunk if the link has room. Returns 1 if something was queued. */
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

static int pump_one(void) {
    pad_fill();
    if (s_wchr < 0 || s_inflight >= INFLIGHT_MAX) { return 0; }
    uint32_t n = tx_used();
    if (!n) { return 0; }
    uint32_t max = (uint32_t)(s_mtu > 23 ? s_mtu - 3 : 20);
    if (max > 244) { max = 244; }
    if (n > max) { n = max; }
    uint8_t chunk[244];
    for (uint32_t i = 0; i < n; i++) { chunk[i] = s_tx[(s_tx_tail + i) % TX_CAP]; }
    int rc = pex_bt_write(s_addr, s_wchr, chunk, n, s_wnr ? 0 : 1, 1);
    if (rc == PEX_EBUSY) { return 0; }
    if (rc < 0) {
        s_write_err = 1;
        return 0;
    }
    s_inflight++;
    s_tx_tail += n;
    s_sent_total += n;
    return 1;
}

static void send_query(void) {
    if (s_job_st == JOB_SEND || s_pad_left || s_pad_crlf || tx_room() < sizeof PL70E_QUERY) { return; }
    tx_put(PL70E_QUERY, sizeof PL70E_QUERY - 1);
    s_query_pending = 1;
    s_query_at = pex_ms();
}

static int find_pipe(void) {
    for (int i = 0; i < 4; i++) {
        int w = pex_bt_find(s_addr, pl70e_pipes[i].svc, pl70e_pipes[i].write, 0);
        int n = pex_bt_find(s_addr, pl70e_pipes[i].svc, pl70e_pipes[i].notify, 0);
        if (w >= 0 && n >= 0) {
            s_wchr = w;
            s_nchr = n;
            s_pipe = i;
            return 1;
        }
    }
    return 0;
}

static void start_connect(void) {
    s_ev_up = s_ev_ready = s_ev_fail = s_ev_down = 0;
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
    emit_state("connecting", NULL);
}

static void tick_job(uint32_t now) {
    if (s_job_st == JOB_SEND && s_tail_queued && tx_used() == 0 && s_inflight == 0) {
        s_job_st = JOB_WAIT;
        s_wait_t0 = now;
        s_status = PL70E_ST_UNKNOWN;
        send_query();
        emit_state("printing", NULL);
    }
    if (s_job_st == JOB_WAIT) {
        if (s_status == PL70E_ST_READY && s_status_at >= s_wait_t0) {
            finish_job(PRN_JOB_OK, NULL);
            emit_state("connected", NULL);
        } else if (s_status == PL70E_ST_NOPAPER || s_status == PL70E_ST_LIDOPEN ||
                   s_status == PL70E_ST_OVERHEAT) {
            finish_job(PRN_JOB_FAILED, pl70e_status_name(s_status));
            emit_status();
        } else if (now - s_wait_t0 > DONE_TIMEOUT_MS) {
            /* Sent in full; the printer just never confirmed. */
            finish_job(PRN_JOB_OK, "sent; printer did not confirm");
        } else if (!s_query_pending || now - s_query_at > QUERY_TIMEOUT_MS) {
            send_query();
        }
    }
}

static int tick(void *arg) {
    (void)arg;
    uint32_t now = pex_ms();
    if (!prn_hub_ok(&s_hub)) { prn_hub_attach(&s_hub, DRV_NAME); }
    switch (s_st) {
    case ST_IDLE:
        break;
    case ST_SCAN:
        {
            pex_bt_dev_t d[8];
            int n = pex_bt_devices(d, 8, PL70E_NAME_PREFIX, NULL);
            for (int i = 0; i < n; i++) {
                if (memcmp(d[i].addr, s_addr, 6) == 0) {
                    pex_bt_scan_stop();
                    start_connect();
                    return PEX_TASK_AGAIN;
                }
            }
        }
        if ((int32_t)(now - s_scan_until) > 0) { drop_link("not found"); }
        break;
    case ST_CONNECT:
        if (s_ev_fail) { drop_link("connect failed"); break; }
        if (s_ev_ready) {
            s_st = ST_DISCOVER;
            s_t0 = now;
        } else if (now - s_t0 > CONNECT_MS) {
            drop_link("connect timeout");
        }
        break;
    case ST_DISCOVER:
        if (s_ev_down) { drop_link("link lost"); break; }
        if (find_pipe()) {
            pex_bt_subscribe(s_addr, s_nchr, 1);
            s_mtu = pex_bt_mtu(s_addr);
            s_rx.n = 0;
            s_backoff_n = 0;
            s_st = ST_READY;
            s_status_at = 0;
            send_query();
            pex_log(PEX_LOG_INFO, "pl70e: %s ready (pipe %s, mtu %d)", s_addr_str,
                    pl70e_pipes[s_pipe].svc, s_mtu);
            emit_state("connected", NULL);
        } else if (now - s_t0 > DISCOVER_MS) {
            drop_link("no TSPL pipe");
        }
        break;
    case ST_READY: {
        if (s_ev_down) { drop_link("link lost"); break; }
        if (s_write_err) { s_write_err = 0; pex_log(PEX_LOG_WARN, "pl70e: write failed"); }
        while (pump_one()) {}
        tick_job(now);
        if (s_job_st == JOB_NONE && !s_query_pending && now - s_query_at > POLL_MS) { send_query(); }
        if (s_query_pending && now - s_query_at > QUERY_TIMEOUT_MS * 3) { s_query_pending = 0; }
        if (s_status != s_status_sent && s_status != PL70E_ST_UNKNOWN) {
            s_status_sent = s_status;
            emit_status();
        }
        if (s_job_st == JOB_SEND && tx_room() >= s_job.row_bytes) { prn_hub_kick(&s_hub); }
        break;
    }
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
    pex_jw_t w;
    pex_jw_init(&w, out, cap);
    pex_jw_obj(&w, NULL);
    pex_jw_str(&w, "driver", DRV_NAME);
    pex_jw_int(&w, "abi", PRN_ABI);
    pex_jw_arr(&w, "models");
    pex_jw_obj(&w, NULL);
    pex_jw_str(&w, "id", "pl70e");
    pex_jw_str(&w, "vendor", "Nelko");
    pex_jw_str(&w, "name", "PL70e-BT");
    pex_jw_str(&w, "transport", "ble");
    pex_jw_int(&w, "dpi", PL70E_DPI);
    pex_jw_int(&w, "head_dots", PL70E_HEAD_DOTS);
    pex_jw_int(&w, "min_len_mm", 10);
    pex_jw_int(&w, "max_len_mm", 300);
    pex_jw_raw(&w, "media", "[\"gap\",\"continuous\"]");
    pex_jw_raw(&w, "density", "{\"min\":0,\"max\":15,\"default\":8}");
    pex_jw_null(&w, "speed");
    pex_jw_int(&w, "black_bit", 0);
    pex_jw_bool(&w, "copies", 1);
    pex_jw_raw(&w, "status", "[\"paper\",\"lid\",\"overheat\"]");
    pex_jw_raw(&w, "presets", "[\"4x6in\",\"3x1in\",\"4x4in\",\"4x3in\",\"2x1in\"]");
    pex_jw_end(&w);
    pex_jw_end(&w);
    pex_jw_end(&w);
    return pex_jw_finish(&w);
}

static int drv_devices(char *out, size_t cap) {
    pex_bt_dev_t d[8];
    int n = pex_bt_devices(d, 8, PL70E_NAME_PREFIX, NULL);
    pex_jw_t w;
    pex_jw_init(&w, out, cap);
    pex_jw_arr(&w, NULL);
    for (int i = 0; i < n; i++) {
        if (!pl70e_name_match(d[i].name)) { continue; }
        char a[18];
        pex_bt_addr_str(d[i].addr, a);
        pex_jw_obj(&w, NULL);
        pex_jw_str(&w, "driver", DRV_NAME);
        pex_jw_str(&w, "addr", a);
        pex_jw_str(&w, "name", d[i].name);
        pex_jw_int(&w, "rssi", d[i].rssi);
        pex_jw_str(&w, "model", "pl70e");
        pex_jw_str(&w, "transport", "ble");
        pex_jw_end(&w);
    }
    pex_jw_end(&w);
    return pex_jw_finish(&w);
}

static int drv_connect(const char *addr, const char *cfg) {
    uint8_t a[6];
    if (!addr || pex_bt_addr(addr, a) < 0) { return PEX_EINVAL; }
    if (s_st != ST_IDLE && s_st != ST_BACKOFF && memcmp(a, s_addr, 6) != 0) { return PEX_EBUSY; }
    if (s_st == ST_READY && memcmp(a, s_addr, 6) == 0) { return PEX_OK; }
    memcpy(s_addr, a, 6);
    pex_bt_addr_str(s_addr, s_addr_str);
    s_have_addr = 1;
    int v = 1;
    s_auto = (cfg && pex_json_bool(cfg, "auto_reconnect", &v)) ? v : 1;
    v = 0;
    s_wnr = (cfg && pex_json_bool(cfg, "options.write_no_response", &v)) ? v : 0;
    s_backoff_n = 0;
    start_connect();
    return PEX_EPENDING;
}

static int drv_disconnect(const char *addr) {
    uint8_t a[6];
    if (addr && (pex_bt_addr(addr, a) < 0 || memcmp(a, s_addr, 6) != 0)) { return PEX_ENOENT; }
    s_auto = 0;
    if (s_st != ST_IDLE) { drop_link("disconnected"); }
    return PEX_OK;
}

static const char *state_name(void) {
    switch (s_st) {
    case ST_SCAN: return "scanning";
    case ST_CONNECT: case ST_DISCOVER: return "connecting";
    case ST_READY: return s_job_st != JOB_NONE ? "printing" : "connected";
    case ST_BACKOFF: return "error";
    default: return "idle";
    }
}

static int drv_status(const char *addr, char *out, size_t cap) {
    pex_jw_t w;
    pex_jw_init(&w, out, cap);
    pex_jw_arr(&w, NULL);
    if (s_have_addr && (!addr || strcmp(addr, s_addr_str) == 0)) {
        pex_jw_obj(&w, NULL);
        pex_jw_str(&w, "driver", DRV_NAME);
        pex_jw_str(&w, "addr", s_addr_str);
        pex_jw_str(&w, "model", "pl70e");
        pex_jw_str(&w, "state", state_name());
        pex_jw_str(&w, "printer", pl70e_status_name(s_status));
        if (s_status != PL70E_ST_UNKNOWN) {
            pex_jw_str(&w, "paper", s_status == PL70E_ST_NOPAPER ? "out" : "ok");
            pex_jw_str(&w, "lid", s_status == PL70E_ST_LIDOPEN ? "open" : "closed");
        }
        if (s_job_st != JOB_NONE) {
            pex_jw_int(&w, "sent", s_sent_total);
            pex_jw_int(&w, "total", s_job_bytes);
        }
        pex_jw_end(&w);
    }
    pex_jw_end(&w);
    return pex_jw_finish(&w);
}

static void make_tspl(tspl_job_t *t, const prn_job_t *j) {
    memset(t, 0, sizeof *t);
    t->prefix = NULL;
    t->width_mm100 = j->width_mm100;
    t->height_mm100 = j->height_mm100;
    t->gap_mm100 = j->media_type == PRN_MEDIA_CONTINUOUS ? 0 : j->gap_mm100;
    t->media = j->media_type;
    t->dpi = PL70E_DPI;
    t->density = j->density < 0 ? 8 : j->density;
    t->speed = j->speed;              /* -1: no SPEED line (the app sends none) */
    t->direction = 0;
    t->x = j->origin_x;
    t->y = j->origin_y;
    t->row_bytes = j->row_bytes;
    t->rows = j->rows_total;
    t->copies = j->copies ? j->copies : 1;
}

static int drv_job_begin(const prn_job_t *j) {
    if (s_st != ST_READY) { return PEX_ESTATE; }
    if (s_job_st != JOB_NONE || s_pad_left || s_pad_crlf) { return PEX_EBUSY; }
    if (j->row_bytes == 0 || j->row_bytes * 8u > PL70E_HEAD_DOTS || j->rows_total == 0) { return PEX_EINVAL; }
    if (j->black_bit != 0) { return PEX_EINVAL; }
    if (s_status == PL70E_ST_NOPAPER || s_status == PL70E_ST_LIDOPEN) { return PEX_ESTATE; }
    s_job = *j;
    tspl_job_t t;
    make_tspl(&t, j);
    char head[256];
    int n = tspl_head(head, sizeof head, &t);
    if (n < 0) { return PEX_EINVAL; }
    /* a status query still in the ring goes first; that's fine for TSPL */
    if ((uint32_t)n > tx_room()) { return PEX_EBUSY; }
    tx_put(head, (uint32_t)n);
    s_rows_in = 0;
    s_tail_queued = 0;
    s_sent_total = 0;
    s_job_bytes = (uint32_t)n + j->row_bytes * j->rows_total;
    s_job_st = JOB_SEND;
    s_query_pending = 0;
    emit_state("printing", NULL);
    return 1;                         /* PRINT n prints the copies */
}

static int drv_job_feed(const uint8_t *rows, uint32_t len) {
    if (s_job_st != JOB_SEND || s_tail_queued) { return PEX_ESTATE; }
    uint32_t rb = s_job.row_bytes;
    uint32_t left = (s_job.rows_total - s_rows_in) * rb;
    if (len > left) { len = left; }
    uint32_t take = tx_room() / rb * rb;
    if (take > len) { take = len; }
    if (!take) { return PEX_EBUSY; }
    tx_put(rows, take);
    s_rows_in += take / rb;
    return (int)take;
}

static int drv_job_end(void) {
    if (s_job_st != JOB_SEND) { return PEX_ESTATE; }
    if (s_rows_in != s_job.rows_total) { return PEX_ESTATE; }
    tspl_job_t t;
    make_tspl(&t, &s_job);
    char tail[48];
    int n = tspl_tail(tail, sizeof tail, &t);
    if (n < 0 || (uint32_t)n > tx_room()) { return PEX_EBUSY; }
    tx_put(tail, (uint32_t)n);
    s_job_bytes += (uint32_t)n;
    s_tail_queued = 1;
    return PEX_OK;
}

static int drv_cancel(void) {
    if (s_job_st == JOB_NONE) { return PEX_OK; }
    /* Bytes already sent can't be recalled. If the BITMAP were cut short the
     * printer would read the next job as bitmap data, so the rows not yet
     * fed go out as white and the PRINT line is left off: the label buffer
     * is filled but never printed, and the next job's CLS clears it. */
    if (s_job_st == JOB_SEND && !s_tail_queued) {
        s_pad_left = (s_job.rows_total - s_rows_in) * s_job.row_bytes;
        s_pad_crlf = 1;
    }
    finish_job(PRN_JOB_CANCELLED, NULL);
    return PEX_OK;
}

int prn_drv(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2) {
    switch (cmd) {
    case PRN_DRV_INFO: return drv_info((char *)a0, a1);
    case PRN_DRV_SCAN:
        if (a0) { return pex_bt_scan(a0, 1); }
        pex_bt_scan_stop();
        return PEX_OK;
    case PRN_DRV_DEVICES: return drv_devices((char *)a0, a1);
    case PRN_DRV_CONNECT: return drv_connect((const char *)a0, (const char *)a1);
    case PRN_DRV_DISCONNECT: return drv_disconnect((const char *)a0);
    case PRN_DRV_STATUS: return drv_status((const char *)a0, (char *)a1, a2);
    case PRN_DRV_JOB_BEGIN: return drv_job_begin((const prn_job_t *)a0);
    case PRN_DRV_JOB_FEED: return drv_job_feed((const uint8_t *)a0, a1);
    case PRN_DRV_JOB_END: return drv_job_end();
    case PRN_DRV_CANCEL: return drv_cancel();
    default: return PEX_ENOTSUP;
    }
}
PEX_EXPORT(prn_drv);

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host;
    switch (reason) {
    case PEX_INIT:
        return PEX_OK;
    case PEX_APP_RUN: {
        const char *cfg = (const char *)arg;
        s_st = ST_IDLE;
        s_job_st = JOB_NONE;
        s_have_addr = 0;
        s_listen = pex_bt_listen(NULL, on_bt, NULL);
        s_task = pex_sched_every(TICK_MS, tick, NULL);
        prn_hub_attach(&s_hub, DRV_NAME);
        /* {"addr":"..","auto_reconnect":true} in the start config: connect now */
        char a[20];
        if (cfg && pex_json_str(cfg, "addr", a, sizeof a)) { drv_connect(a, cfg); }
        return PEX_OK;
    }
    case PEX_APP_STOP:
        s_auto = 0;
        if (s_st != ST_IDLE) { drop_link("stopped"); }
        if (s_listen >= 0) { pex_bt_unlisten(s_listen); s_listen = -1; }
        if (s_task > 0) { pex_sched_cancel(s_task); s_task = 0; }
        prn_hub_detach(&s_hub, DRV_NAME);
        return PEX_OK;
    case PEX_DEINIT:
        return PEX_OK;
    default:
        return PEX_OK;
    }
}
