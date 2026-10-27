/* The bt group (LE central) on the fake host, against simulated peripherals.
 * Behaviour follows btgw/src/pex/pex_gw_bt.c and src/bt/le/central.c; the
 * numbers are theirs: 8 listeners, an 8-deep event queue that drops the
 * oldest, 252 bytes of data per event, 4 GATT ops in flight per link. */
#include "fh_internal.h"
#include "fh_ble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PEERS 8
#define LISTENERS 8
#define QDEPTH 8
#define QDATA 252
#define OPQ 4

typedef struct { int kind, chr, with_resp, want_done; uint32_t due; uint16_t len; uint8_t d[QDATA]; } op_t;

struct fh_ble_peer {
    fh_ble_peer_cfg_t c;
    uint8_t addr[6];
    int seen;                /* in the scan table */
    int state;               /* PEX_BT_DISCONNECTED / CONNECTING / CONNECTED */
    int ready;
    uint32_t up_at, ready_at;
    op_t q[OPQ];
    int nq;
    int stall;
    int drop_done;           /* write-done events to lose (the gateway's event ring drops the oldest when full) */
    int fail_writes;         /* writes to fail: the data never arrives, the done event carries an error */
    uint8_t val[FH_BLE_CHR_MAX][QDATA];
    uint16_t vlen[FH_BLE_CHR_MAX];
    fh_ble_stats_t st;
};

typedef struct { int id; pex_mod_t *m; pex_bt_fn fn; void *arg; uint8_t addr[6]; int any; } lis_t;
typedef struct { pex_bt_event_t ev; uint8_t d[QDATA]; } qe_t;

static fh_ble_peer_t s_p[PEERS];
static int s_np;
static lis_t s_l[LISTENERS];
static int s_lid = 1;
static qe_t s_q[QDEPTH];
static uint32_t s_qh, s_qt, s_dropped;
static int s_scanning;
static uint32_t s_scan_end, s_scan_seen_at;

static int parse(const char *s, uint8_t *a) {
    unsigned v[6];
    if (!s || sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) { return 0; }
    for (int i = 0; i < 6; i++) { a[i] = (uint8_t)v[i]; }
    return 1;
}
static fh_ble_peer_t *peer(const uint8_t *a) {
    if (!a) { return NULL; }
    for (int i = 0; i < s_np; i++) { if (memcmp(s_p[i].addr, a, 6) == 0) { return &s_p[i]; } }
    return NULL;
}
static int nlis(void) {
    int n = 0;
    for (int i = 0; i < LISTENERS; i++) { n += s_l[i].fn != NULL; }
    return n;
}
static void push(int kind, const uint8_t *addr, int chr, const void *d, size_t n, int status) {
    if (!nlis()) { return; }
    if (s_qh - s_qt >= QDEPTH) { s_qt++; s_dropped++; }
    qe_t *e = &s_q[s_qh++ % QDEPTH];
    memset(&e->ev, 0, sizeof e->ev);
    memcpy(e->ev.addr, addr, 6);
    e->ev.kind = (uint8_t)kind;
    e->ev.chr = (int16_t)chr;
    e->ev.status = (int16_t)status;
    e->ev.len = (uint16_t)(n > QDATA ? QDATA : n);
    if (d && e->ev.len) { memcpy(e->d, d, e->ev.len); }
}

/* ---- peers, for tests ---- */
fh_ble_peer_t *fh_ble_peer_add(const fh_ble_peer_cfg_t *cfg) {
    if (s_np >= PEERS) { return NULL; }
    fh_ble_peer_t *p = &s_p[s_np++];
    memset(p, 0, sizeof *p);
    p->c = *cfg;
    parse(cfg->addr, p->addr);
    if (!p->c.mtu) { p->c.mtu = 247; }
    if (!p->c.connect_ms) { p->c.connect_ms = 40; }
    if (!p->c.op_ms) { p->c.op_ms = 8; }
    return p;
}
void fh_ble_peer_notify(fh_ble_peer_t *p, int chr, const void *d, size_t n) {
    if (!p || !p->ready || chr < 0 || chr >= p->c.nchr) { return; }
    int sub = p->st.subscribed[chr];
    if (!sub) { return; }
    push(sub == 2 ? PEX_BT_INDICATE : PEX_BT_NOTIFY, p->addr, chr, d, n, 0);
}
void fh_ble_peer_set_value(fh_ble_peer_t *p, int chr, const void *d, size_t n) {
    if (!p || chr < 0 || chr >= FH_BLE_CHR_MAX) { return; }
    if (n > QDATA) { n = QDATA; }
    memcpy(p->val[chr], d, n);
    p->vlen[chr] = (uint16_t)n;
}
static void link_down(fh_ble_peer_t *p) {
    int was = p->state != PEX_BT_DISCONNECTED;
    p->state = PEX_BT_DISCONNECTED;
    p->ready = 0;
    p->nq = 0;
    p->st.connected = 0;
    memset(p->st.subscribed, 0, sizeof p->st.subscribed);
    if (was) { push(PEX_BT_LINK_DOWN, p->addr, -1, NULL, 0, 0x08); }
}
void fh_ble_peer_disconnect(fh_ble_peer_t *p) { if (p) { link_down(p); } }
void fh_ble_peer_stall(fh_ble_peer_t *p, int on) { if (p) { p->stall = on; } }
void fh_ble_peer_drop_write_done(fh_ble_peer_t *p, int n) { if (p) { p->drop_done = n; } }
void fh_ble_peer_fail_writes(fh_ble_peer_t *p, int n) { if (p) { p->fail_writes = n; } }
const fh_ble_stats_t *fh_ble_peer_stats(fh_ble_peer_t *p) { return p ? &p->st : NULL; }
int fh_ble_listeners(void) { return nlis(); }

/* ---- the loop ---- */
static void run_ops(fh_ble_peer_t *p) {
    uint32_t now = fh_now();
    while (p->nq && !p->stall && (int32_t)(now - p->q[0].due) >= 0) {
        op_t o = p->q[0];
        memmove(&p->q[0], &p->q[1], sizeof(op_t) * (size_t)(p->nq - 1));
        p->nq--;
        if (p->nq) { p->q[0].due = now + p->c.op_ms; }
        if (o.kind == PEX_BT_WRITE && p->fail_writes > 0) {
            /* like the gateway's op timeout (reason 1 << 8): nothing reaches the printer */
            p->fail_writes--;
            if (o.want_done) { push(PEX_BT_WRITE, p->addr, o.chr, NULL, 0, 1 << 8); }
        } else if (o.kind == PEX_BT_WRITE) {
            p->st.writes++;
            p->st.bytes += o.len;
            if (p->c.on_write) { p->c.on_write(p, o.chr, o.d, o.len, o.with_resp, p->c.ctx); }
            if (o.want_done) {
                if (p->drop_done > 0) { p->drop_done--; } else { push(PEX_BT_WRITE, p->addr, o.chr, NULL, 0, 0); }
            }
        } else if (o.kind == PEX_BT_READ) {
            push(PEX_BT_READ, p->addr, o.chr, p->val[o.chr], p->vlen[o.chr], 0);
        } else {
            /* subscribe: confirmed as a write */
            if (o.want_done) { push(PEX_BT_WRITE, p->addr, o.chr, NULL, 0, 0); }
        }
    }
}
void fh_ble_poll(void) {
    uint32_t now = fh_now();
    if (s_scanning && s_scan_seen_at && (int32_t)(now - s_scan_seen_at) >= 0) {
        for (int i = 0; i < s_np; i++) { s_p[i].seen = 1; }
        s_scan_seen_at = 0;
    }
    if (s_scanning && s_scan_end && (int32_t)(now - s_scan_end) >= 0) { s_scanning = 0; }
    for (int i = 0; i < s_np; i++) {
        fh_ble_peer_t *p = &s_p[i];
        if (p->state == PEX_BT_CONNECTING && (int32_t)(now - p->up_at) >= 0) {
            if (p->c.connect_fails) {
                p->state = PEX_BT_DISCONNECTED;
                push(PEX_BT_FAILED, p->addr, -1, NULL, 0, 0x3e);
            } else {
                p->state = PEX_BT_CONNECTED;
                p->st.connects++;
                p->st.connected = 1;
                p->ready_at = now + 20;
                push(PEX_BT_LINK_UP, p->addr, -1, NULL, 0, 0);
            }
        }
        if (p->state == PEX_BT_CONNECTED && !p->ready && p->ready_at && (int32_t)(now - p->ready_at) >= 0) {
            p->ready = 1;
            push(PEX_BT_READY, p->addr, -1, NULL, 0, 0);
        }
        if (p->ready) { run_ops(p); }
    }
    while (s_qt != s_qh) {
        qe_t *e = &s_q[s_qt++ % QDEPTH];
        pex_bt_event_t ev = e->ev;
        uint8_t d[QDATA];
        memcpy(d, e->d, ev.len);
        ev.data = ev.len ? d : NULL;
        for (int i = 0; i < LISTENERS; i++) {
            lis_t *l = &s_l[i];
            if (!l->fn || (!l->any && memcmp(l->addr, ev.addr, 6) != 0)) { continue; }
            pex_mod_t *o = fh_swap(l->m);
            l->fn(l->arg, &ev);
            fh_swap(o);
        }
    }
}
void fh_ble_reset(void) {
    memset(s_p, 0, sizeof s_p);
    memset(s_l, 0, sizeof s_l);
    s_np = 0;
    s_qh = s_qt = s_dropped = 0;
    s_scanning = 0;
    s_scan_end = s_scan_seen_at = 0;
}
void fh_ble_unload(pex_mod_t *m) {
    for (int i = 0; i < LISTENERS; i++) { if (s_l[i].m == m) { s_l[i].fn = NULL; } }
}

/* ---- the group ---- */
#define OFF() do { if (fh_group_off("bt")) { return PEX_ENOTSUP; } } while (0)

int pex_bt_scan(uint32_t ms, int active) {
    OFF();
    (void)active;
    if (s_scanning) { return PEX_OK; }
    s_scanning = 1;
    s_scan_end = ms ? fh_now() + ms : 0;
    s_scan_seen_at = fh_now() + 60;
    return PEX_OK;
}
void pex_bt_scan_stop(void) { s_scanning = 0; }
int pex_bt_scanning(void) { return s_scanning; }
int pex_bt_devices(pex_bt_dev_t *out, uint32_t max, const char *prefix, const char *uuid128) {
    OFF();
    uint32_t n = 0;
    size_t pl = prefix ? strlen(prefix) : 0;
    for (int i = 0; i < s_np && n < max; i++) {
        fh_ble_peer_t *p = &s_p[i];
        if (!p->seen) { continue; }
        int rules = pl || (uuid128 && *uuid128), match = !rules;
        if (pl && p->c.name && strncmp(p->c.name, prefix, pl) == 0) { match = 1; }
        if (!match && uuid128 && *uuid128 && p->c.uuid128 && strcasecmp(p->c.uuid128, uuid128) == 0) { match = 1; }
        if (!match) { continue; }
        pex_bt_dev_t *d = &out[n++];
        memset(d, 0, sizeof *d);
        memcpy(d->addr, p->addr, 6);
        d->rssi = p->c.rssi ? p->c.rssi : -60;
        snprintf(d->name, sizeof d->name, "%s", p->c.name ? p->c.name : "");
        d->connectable = 1;
        d->connected = p->state == PEX_BT_CONNECTED;
        d->n_uuid16 = (uint8_t)p->c.n_uuid16;
        memcpy(d->uuid16, p->c.uuid16, sizeof d->uuid16);
        d->mfg_len = p->c.mfg_len;
        memcpy(d->mfg, p->c.mfg, sizeof d->mfg);
    }
    return (int)n;
}
int pex_bt_uuids128(const uint8_t *addr, char (*out)[37], int max) {
    OFF();
    fh_ble_peer_t *p = peer(addr);
    if (!p || !p->c.uuid128 || max < 1) { return 0; }
    snprintf(out[0], 37, "%s", p->c.uuid128);
    return 1;
}
int pex_bt_addr(const char *s, uint8_t *a) { return parse(s, a) ? PEX_OK : PEX_EINVAL; }
void pex_bt_addr_str(const uint8_t *a, char *out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
}
int pex_bt_connect(const uint8_t *addr) {
    OFF();
    fh_ble_peer_t *p = peer(addr);
    if (!p || !p->seen) { return PEX_ENOENT; }
    if (p->state == PEX_BT_CONNECTED) { return PEX_OK; }
    if (p->state == PEX_BT_CONNECTING) { return PEX_EBUSY; }
    p->state = PEX_BT_CONNECTING;
    p->up_at = fh_now() + p->c.connect_ms;
    return PEX_EPENDING;
}
int pex_bt_disconnect(const uint8_t *addr) {
    OFF();
    fh_ble_peer_t *p = peer(addr);
    if (!p || p->state == PEX_BT_DISCONNECTED) { return PEX_ENOENT; }
    link_down(p);
    return PEX_OK;
}
int pex_bt_state(const uint8_t *addr, int *ready) {
    OFF();
    fh_ble_peer_t *p = peer(addr);
    if (ready) { *ready = p ? p->ready : 0; }
    return p ? p->state : PEX_BT_DISCONNECTED;
}
int pex_bt_mtu(const uint8_t *addr) {
    OFF();
    fh_ble_peer_t *p = peer(addr);
    return p && p->state == PEX_BT_CONNECTED ? p->c.mtu : 0;
}
int pex_bt_find(const uint8_t *addr, const char *svc, const char *chr, int instance) {
    OFF();
    fh_ble_peer_t *p = peer(addr);
    if (!p || !p->ready) { return PEX_ESTATE; }
    for (int i = 0; i < p->c.nchr; i++) {
        if ((!svc || strcasecmp(p->c.chrs[i].svc, svc) == 0) && strcasecmp(p->c.chrs[i].chr, chr) == 0) {
            if (instance-- == 0) { return i; }
        }
    }
    return PEX_ENOENT;
}
int pex_bt_props(const uint8_t *addr, int chr) {
    OFF();
    fh_ble_peer_t *p = peer(addr);
    if (!p || chr < 0 || chr >= p->c.nchr) { return PEX_ENOENT; }
    return p->c.chrs[chr].props;
}
static int enqueue(fh_ble_peer_t *p, int kind, int chr, const void *d, size_t n, int wr, int done) {
    if (!p || p->state != PEX_BT_CONNECTED) { return PEX_ESTATE; }
    if (!p->ready || chr < 0 || chr >= p->c.nchr) { return PEX_ENOENT; }
    if (p->nq >= OPQ) { return PEX_EBUSY; }
    op_t *o = &p->q[p->nq++];
    memset(o, 0, sizeof *o);
    o->kind = kind;
    o->chr = chr;
    o->with_resp = wr;
    o->want_done = done;
    o->len = (uint16_t)n;
    if (d && n) { memcpy(o->d, d, n); }
    if (p->nq == 1) { o->due = fh_now() + p->c.op_ms; }
    return PEX_OK;
}
int pex_bt_read(const uint8_t *addr, int chr) {
    OFF();
    return enqueue(peer(addr), PEX_BT_READ, chr, NULL, 0, 0, 0);
}
int pex_bt_write(const uint8_t *addr, int chr, const void *d, size_t len, int wr, int done) {
    OFF();
    fh_ble_peer_t *p = peer(addr);
    if (chr < 0 || chr > 255) { return PEX_EINVAL; }
    int mtu = p ? p->c.mtu : 23;
    if (len > QDATA || (mtu > 3 && len > (size_t)(mtu - 3))) { return PEX_EINVAL; }
    return enqueue(p, PEX_BT_WRITE, chr, d, len, wr, done);
}
int pex_bt_subscribe(const uint8_t *addr, int chr, int mode) {
    OFF();
    fh_ble_peer_t *p = peer(addr);
    if (chr < 0 || chr > 255 || mode < 0 || mode > 2) { return PEX_EINVAL; }
    int rc = enqueue(p, PEX_BT_NOTIFY, chr, NULL, 0, 1, 0);
    if (rc == PEX_OK) { p->st.subscribed[chr] = mode; }
    return rc;
}
int pex_bt_listen(const uint8_t *addr, pex_bt_fn fn, void *arg) {
    OFF();
    if (!fn) { return PEX_EINVAL; }
    for (int i = 0; i < LISTENERS; i++) {
        if (!s_l[i].fn) {
            s_l[i] = (lis_t){ s_lid++, pex_self(), fn, arg, { 0 }, addr == NULL };
            if (addr) { memcpy(s_l[i].addr, addr, 6); }
            return s_l[i].id;
        }
    }
    return PEX_ENOMEM;
}
int pex_bt_unlisten(int id) {
    for (int i = 0; i < LISTENERS; i++) {
        if (s_l[i].fn && s_l[i].id == id) { s_l[i].fn = NULL; return PEX_OK; }
    }
    return PEX_ENOENT;
}
int pex_gatt_begin(void) { return PEX_ENOTSUP; }
int pex_gatt_service(const char *uuid) { (void)uuid; return PEX_ENOTSUP; }
int pex_gatt_characteristic(const char *uuid, int props) { (void)uuid; (void)props; return PEX_ENOTSUP; }
int pex_gatt_commit(void) { return PEX_ENOTSUP; }
