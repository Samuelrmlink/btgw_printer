/* The classic group on the fake host. Session rules come from the firmware's
 * own cl_sess.c (compiled in). This file is the radio: it pages, answers
 * SDP, runs legacy PIN / SSP pairing and opens RFCOMM against simulated
 * peers, driving cl_sess the way btgw/src/bt/classic/spp.c does, and hands
 * queued session events to their modules the way src/pex/pex_gw_classic.c
 * does (after the radio work, from the main loop). */
#include "fh_internal.h"
#include "fh_classic.h"
#include "bt/classic/cl_sess.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PEERS 8
#define ACTS 32

struct fh_cl_peer {
    fh_cl_peer_cfg_t c;
    uint8_t addr[6];
    char name[32];
    char pin[16];
    int has_pin;
    int peer_key;            /* the peer holds a link key for us */
    int absent, stall;
    int handle;              /* the session RFCOMM is open on, or 0 */
    uint32_t name_due;       /* when the remote name request answers, or 0 */
    fh_cl_stats_t st;
};

static fh_cl_peer_t s_peers[PEERS];
static int s_npeers;
static uint8_t s_host_key[PEERS];   /* indexed like s_peers */
static int s_inq_count;
static uint32_t s_inq_end, s_inq_note_at;

/* the standalone SDP lookup */
static pex_mod_t *s_sdp_mod;
static pex_cl_fn s_sdp_fn;
static void *s_sdp_arg;
static int s_sdp_busy;
static uint32_t s_sdp_due;
static uint8_t s_sdp_addr[6];
static uint16_t s_sdp_uuid;

/* timed radio work */
enum { A_SDP = 1, A_RFCOMM, A_SECURE, A_RF_UP, A_KICK, A_ANSWER };
typedef struct { int live, kind, handle; uint32_t due; char pin[16]; } act_t;
static act_t s_act[ACTS];
static uint16_t s_next_acl = 0x40, s_next_cid = 0x100;

static int parse_addr(const char *s, uint8_t *a) {
    unsigned v[6];
    if (!s || sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) { return 0; }
    for (int i = 0; i < 6; i++) { a[i] = (uint8_t)v[i]; }
    return 1;
}
static fh_cl_peer_t *peer_of(const uint8_t *a) {
    for (int i = 0; i < s_npeers; i++) { if (memcmp(s_peers[i].addr, a, 6) == 0) { return &s_peers[i]; } }
    return NULL;
}
static int peer_ix(const fh_cl_peer_t *p) { return (int)(p - s_peers); }

static void act(int kind, int handle, uint32_t delay) {
    for (int i = 0; i < ACTS; i++) {
        if (!s_act[i].live) {
            s_act[i] = (act_t){ 1, kind, handle, fh_now() + delay, "" };
            return;
        }
    }
}
static void drop_acts(int handle) {
    for (int i = 0; i < ACTS; i++) { if (s_act[i].live && s_act[i].handle == handle) { s_act[i].live = 0; } }
}

/* ---- the peer side, for tests ---- */
fh_cl_peer_t *fh_cl_peer_add(const fh_cl_peer_cfg_t *cfg) {
    if (s_npeers >= PEERS) { return NULL; }
    fh_cl_peer_t *p = &s_peers[s_npeers++];
    memset(p, 0, sizeof *p);
    p->c = *cfg;
    parse_addr(cfg->addr, p->addr);
    snprintf(p->name, sizeof p->name, "%s", cfg->name ? cfg->name : "");
    if (cfg->pin) { snprintf(p->pin, sizeof p->pin, "%s", cfg->pin); p->has_pin = 1; }
    if (!p->c.mtu) { p->c.mtu = 990; }
    if (!p->c.page_ms) { p->c.page_ms = 30; }
    if (!p->c.bytes_per_ms) { p->c.bytes_per_ms = 120; }
    p->absent = cfg->absent;
    return p;
}
void fh_cl_peer_send(fh_cl_peer_t *p, const void *d, size_t n) {
    if (p && p->handle) { cl_rx_put(p->handle, d, n); }
}
void fh_cl_peer_close(fh_cl_peer_t *p) {
    if (!p || !p->handle) { return; }
    int h = p->handle;
    p->handle = 0;
    p->st.connected = 0;
    cl_set_cid(h, 0);
    cl_set_acl(h, 0);
    cl_fail(h, CL_WHY_REMOTE, "rfcomm closed");
}
void fh_cl_peer_forget(fh_cl_peer_t *p) { if (p) { p->peer_key = 0; } }
void fh_cl_peer_set_pin(fh_cl_peer_t *p, const char *pin) {
    if (!p) { return; }
    p->has_pin = pin != NULL;
    snprintf(p->pin, sizeof p->pin, "%s", pin ? pin : "");
}
void fh_cl_peer_stall(fh_cl_peer_t *p, int on) { if (p) { p->stall = on; } }
void fh_cl_peer_absent(fh_cl_peer_t *p, int on) { if (p) { p->absent = on; } }
const fh_cl_stats_t *fh_cl_peer_stats(fh_cl_peer_t *p) { return p ? &p->st : NULL; }
int fh_cl_host_has_key(const char *addr) {
    uint8_t a[6];
    fh_cl_peer_t *p = parse_addr(addr, a) ? peer_of(a) : NULL;
    return p ? s_host_key[peer_ix(p)] : 0;
}
void fh_cl_host_set_key(const char *addr, int on) {
    uint8_t a[6];
    fh_cl_peer_t *p = parse_addr(addr, a) ? peer_of(a) : NULL;
    if (p) { s_host_key[peer_ix(p)] = (uint8_t)on; p->peer_key = on; }
}
int fh_cl_sessions(void) {
    int hs[CL_SESS_MAX];
    return cl_handles(hs, CL_SESS_MAX);
}
int fh_cl_inquiries(void) { return s_inq_count; }

/* ---- the radio, as spp.c ---- */
static int page_ok(int handle, fh_cl_peer_t **out) {
    cl_info_t in;
    if (cl_info(handle, &in) != CL_OK) { return 0; }
    fh_cl_peer_t *p = peer_of(in.addr);
    *out = p;
    if (in.acl) { return 1; }
    if (!p || p->absent) { return 0; }
    p->st.pages++;
    cl_set_acl(handle, s_next_acl++);
    return 1;
}

static void open_rfcomm(int handle) {
    cl_info_t in;
    if (cl_info(handle, &in) != CL_OK) { return; }
    if (!in.channel) { cl_fail(handle, CL_WHY_SDP, "no channel"); return; }
    cl_set_phase(handle, in.acl ? CL_PHASE_RFCOMM : CL_PHASE_PAGE);
    cl_set_cid(handle, s_next_cid++);
    fh_cl_peer_t *p = peer_of(in.addr);
    act(A_RFCOMM, handle, in.acl ? 5 : (p ? p->c.page_ms : 30));
}

static void finish_sdp(int handle, int status, uint8_t ch) {
    cl_info_t in;
    if (!ch && cl_info(handle, &in) == CL_OK && in.fallback) { ch = in.fallback; }
    if (!ch) { cl_fail(handle, CL_WHY_SDP, status ? "SDP query failed" : "no channel"); return; }
    cl_set_channel(handle, ch);
    open_rfcomm(handle);
}

static int kick(int handle) {
    cl_info_t in;
    if (cl_info(handle, &in) != CL_OK) { return CL_ENOENT; }
    /* Level 0/1 asks for no authentication from the host, but a printer that
     * wants a legacy PIN asks for one itself when RFCOMM opens (as the
     * PM220 does and mcusim's legacy_only peer does): that path still pairs. */
    fh_cl_peer_t *pp = peer_of(in.addr);
    if (in.level <= 1 && !(pp && pp->has_pin)) { cl_security(handle, in.level, 0, 0, 1); }
    if (!in.channel) {
        if (!in.uuid) { cl_fail(handle, CL_WHY_SDP, "no uuid"); return CL_EINVAL; }
        cl_set_phase(handle, CL_PHASE_SDP);
        fh_cl_peer_t *p = peer_of(in.addr);
        act(A_SDP, handle, (in.acl ? 0 : (p ? p->c.page_ms : 30)) + 8);
        return CL_OK;
    }
    open_rfcomm(handle);
    return CL_OK;
}

/* Restart after a failed authentication with drop_key: the link key goes,
 * RFCOMM and the ACL come down, and the session pages again (spp.c
 * want_restart + on_disc). */
static void restart(int handle) {
    cl_info_t in;
    if (cl_info(handle, &in) != CL_OK) { return; }
    fh_cl_peer_t *p = peer_of(in.addr);
    if (p) { s_host_key[peer_ix(p)] = 0; }
    cl_set_cid(handle, 0);
    cl_set_acl(handle, 0);
    act(A_KICK, handle, 20);
}

static void auth_failed(int handle) {
    int rc = cl_auth_fail(handle);
    if (rc == CL_RETRY) { restart(handle); }
}

/* Pairing finished well: both sides hold a key; encryption comes up at level 2. */
static void paired(int handle, fh_cl_peer_t *p) {
    s_host_key[peer_ix(p)] = 1;
    p->peer_key = 1;
    cl_set_key(handle, 1, 0, 0);
    int rc = cl_security(handle, 2, 0, 0, 1);
    if (rc == CL_OK) { cl_io_done(handle); }
    else if (rc == CL_RETRY) { restart(handle); }
}

static void try_pin(int handle, fh_cl_peer_t *p, const char *pin) {
    p->st.pin_requests++;
    if (p->st.pin_requests <= 8) { snprintf(p->st.pins_tried[p->st.pin_requests - 1], 16, "%s", pin); }
    if (strcmp(pin, p->pin) == 0) { paired(handle, p); }
    else { auth_failed(handle); }
}

/* RFCOMM is being opened: authenticate first (BTstack asks for level 2). */
static void secure(int handle) {
    cl_info_t in;
    fh_cl_peer_t *p;
    if (!page_ok(handle, &p)) {
        cl_fail(handle, CL_WHY_PAGE, "page failed");
        return;
    }
    if (cl_info(handle, &in) != CL_OK || in.state != CL_OPENING) { return; }
    if (in.secure) { act(A_RF_UP, handle, 5); return; }
    int ix = peer_ix(p);
    if (s_host_key[ix]) {
        if (p->peer_key) {
            cl_set_key(handle, 1, 0, 0);
            int rc = cl_security(handle, 2, 0, 0, 1);
            if (rc == CL_OK) { cl_io_done(handle); }
            else if (rc == CL_RETRY) { restart(handle); return; }
        } else {
            auth_failed(handle);            /* the peer lost its key: HCI status 0x06 */
            return;
        }
    } else if (!p->has_pin) {
        paired(handle, p);                  /* SSP just-works */
    } else {
        char pin[CL_PIN_LEN];
        int r = cl_pin_pull(handle, pin, sizeof pin);
        if (r == 1) { try_pin(handle, p, pin); }
        else { return; }                    /* waits for pex_bt_classic_pin; A_ANSWER resumes */
    }
    if (cl_info(handle, &in) == CL_OK && in.state == CL_OPENING && in.secure) { act(A_RF_UP, handle, 5); }
}

static void rf_up(int handle) {
    cl_info_t in;
    if (cl_info(handle, &in) != CL_OK || in.state != CL_OPENING || !in.cid) { return; }
    fh_cl_peer_t *p = peer_of(in.addr);
    if (!p || p->c.sdp_channel == 0 || in.channel != p->c.sdp_channel) {
        /* nothing listens on that channel */
        cl_set_cid(handle, 0);
        cl_fail(handle, CL_WHY_RFCOMM, "RFCOMM open failed");
        return;
    }
    p->handle = handle;
    p->st.opens++;
    p->st.connected = 1;
    cl_rfcomm_up(handle, p->c.mtu);
}

static void run_act(act_t *a) {
    cl_info_t in;
    fh_cl_peer_t *p;
    if (cl_info(a->handle, &in) != CL_OK) { return; }
    if (in.state != CL_OPENING && a->kind != A_ANSWER) { return; }
    switch (a->kind) {
    case A_SDP:
        if (!page_ok(a->handle, &p)) {
            finish_sdp(a->handle, 4, 0);    /* the page under the query failed */
            return;
        }
        finish_sdp(a->handle, p->c.sdp_fails, p->c.sdp_fails ? 0 : p->c.sdp_channel);
        break;
    case A_RFCOMM: secure(a->handle); break;
    case A_RF_UP: rf_up(a->handle); break;
    case A_KICK: kick(a->handle); break;
    }
}

/* A PIN the module submitted (spp.c send_answer). */
static void send_answers(void) {
    int hs[CL_SESS_MAX];
    int n = cl_handles(hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) {
        int kind = 0, neg = 0;
        char pin[CL_PIN_LEN];
        uint32_t num = 0;
        cl_info_t in;
        if (!cl_take_answer(hs[i], &kind, &neg, pin, sizeof pin, &num)) { continue; }
        if (cl_info(hs[i], &in) != CL_OK) { continue; }
        fh_cl_peer_t *p = peer_of(in.addr);
        if (!p || kind != CL_ASK_PIN) { continue; }
        if (neg) { auth_failed(hs[i]); continue; }
        try_pin(hs[i], p, pin);
        if (cl_info(hs[i], &in) == CL_OK && in.state == CL_OPENING && in.secure) { act(A_RF_UP, hs[i], 5); }
    }
}

static void drive_down(void) {
    int hs[CL_SESS_MAX];
    int n = cl_handles(hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) {
        cl_info_t in;
        if (cl_info(hs[i], &in) != CL_OK) { continue; }
        if (in.state != CL_CLOSING && in.state != CL_FAILED) { continue; }
        fh_cl_peer_t *p = peer_of(in.addr);
        if (p && p->handle == hs[i]) {
            p->handle = 0;
            p->st.connected = 0;
            if (in.state == CL_CLOSING) { p->st.closes++; }
        }
        drop_acts(hs[i]);
        cl_set_cid(hs[i], 0);
        cl_set_acl(hs[i], 0);
        if (in.owner && cl_mod_pending(hs[i])) { continue; }
        cl_reap(hs[i]);
    }
}

static void pump_tx(void) {
    int hs[CL_SESS_MAX];
    int n = cl_handles(hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) {
        cl_info_t in;
        if (!cl_tx_eligible(hs[i]) || cl_info(hs[i], &in) != CL_OK) { continue; }
        fh_cl_peer_t *p = peer_of(in.addr);
        if (!p || p->handle != hs[i] || p->stall) { continue; }
        uint32_t budget = p->c.bytes_per_ms;
        while (budget) {
            const uint8_t *d = NULL;
            size_t max = in.mtu < budget ? in.mtu : budget;
            int got = cl_tx_peek(hs[i], &d, max);
            if (got <= 0 || !d) { break; }
            p->st.rx_bytes += (size_t)got;
            uint8_t tmp[2048];
            memcpy(tmp, d, (size_t)got);
            cl_tx_advance(hs[i], (size_t)got);
            if (p->c.on_rx) { p->c.on_rx(p, tmp, (size_t)got, p->c.ctx); }
            budget -= (uint32_t)got;
        }
    }
}

static void inquiry(void) {
    uint32_t now = fh_now();
    /* remote name requests run on after the inquiry */
    for (int i = 0; i < s_npeers; i++) {
        fh_cl_peer_t *p = &s_peers[i];
        if (p->name_due && (int32_t)(now - p->name_due) >= 0) {
            p->name_due = 0;
            if (!p->absent) { cl_inq_set_name(p->addr, p->name); }
        }
    }
    if (!cl_inq_active()) { return; }
    if (s_inq_note_at && (int32_t)(now - s_inq_note_at) >= 0) {
        s_inq_note_at = 0;
        for (int i = 0; i < s_npeers; i++) {
            fh_cl_peer_t *p = &s_peers[i];
            if (p->absent) { continue; }
            cl_inq_note(p->addr, p->c.rssi ? p->c.rssi : -60, p->c.cod, p->c.name_by_rnr ? NULL : p->name);
            if (p->c.name_by_rnr) { p->name_due = (now + (p->c.rnr_ms ? p->c.rnr_ms : 60u)) | 1u; }
        }
    }
    if (s_inq_end && (int32_t)(now - s_inq_end) >= 0) { cl_inq_stop(); }
}

static void deliver(int handle) {
    cl_info_t in;
    pex_cl_event_t ev;
    uint8_t rx[CL_LISTEN_MAX];
    if (cl_info(handle, &in) != CL_OK) { return; }
    int bits = cl_take_mod(handle);
    if (!bits || !in.cb || !in.owner) { return; }
    pex_mod_t *m = (pex_mod_t *)in.owner;
    pex_cl_fn fn = (pex_cl_fn)in.cb;
    memset(&ev, 0, sizeof ev);
    ev.handle = in.handle;
    ev.why = in.why;
    ev.number = in.confirm_num;
    ev.channel = in.channel;
    ev.uuid = in.uuid;
    memcpy(ev.addr, in.addr, 6);
    pex_mod_t *o = fh_swap(m);
    if (bits & CL_EV_STATE) {
        int kind = 0;
        if (in.state == CL_OPEN) { kind = PEX_CL_OPEN; }
        else if (in.state == CL_FAILED) { kind = PEX_CL_FAILED; }
        else if (in.state == CL_CLOSING) { kind = PEX_CL_CLOSED; }
        if (kind) { ev.kind = kind; fn((void *)in.cb_arg, &ev); }
    }
    if (bits & CL_EV_PIN) { ev.kind = PEX_CL_PIN; fn((void *)in.cb_arg, &ev); }
    if (bits & CL_EV_PASSKEY) { ev.kind = PEX_CL_PASSKEY; fn((void *)in.cb_arg, &ev); }
    if (bits & CL_EV_CONFIRM) { ev.kind = PEX_CL_CONFIRM; fn((void *)in.cb_arg, &ev); }
    if (bits & CL_EV_RX) {
        int n = cl_rx_peek(handle, rx, sizeof rx);
        ev.kind = PEX_CL_RX;
        ev.data = n > 0 ? rx : NULL;
        ev.len = n > 0 ? (uint16_t)n : 0;
        fn((void *)in.cb_arg, &ev);
        ev.data = NULL;
        ev.len = 0;
    }
    if (bits & CL_EV_TX_ROOM) { ev.kind = PEX_CL_TX_ROOM; fn((void *)in.cb_arg, &ev); }
    fh_swap(o);
}

void fh_classic_poll(void) {
    uint32_t now = fh_now();
    for (int i = 0; i < ACTS; i++) {
        if (s_act[i].live && (int32_t)(now - s_act[i].due) >= 0) {
            act_t a = s_act[i];
            s_act[i].live = 0;
            run_act(&a);
        }
    }
    inquiry();
    if (s_sdp_busy && (int32_t)(now - s_sdp_due) >= 0) {
        fh_cl_peer_t *p = peer_of(s_sdp_addr);
        pex_cl_event_t ev;
        memset(&ev, 0, sizeof ev);
        ev.kind = PEX_CL_SDP;
        memcpy(ev.addr, s_sdp_addr, 6);
        ev.uuid = s_sdp_uuid;
        ev.channel = p && !p->absent && !p->c.sdp_fails ? p->c.sdp_channel : 0;
        ev.status = ev.channel ? 0 : 1;
        s_sdp_busy = 0;
        if (s_sdp_mod && s_sdp_fn) {
            pex_mod_t *o = fh_swap(s_sdp_mod);
            s_sdp_fn(s_sdp_arg, &ev);
            fh_swap(o);
        }
        s_sdp_mod = NULL;
    }
    cl_tick(now);
    send_answers();
    drive_down();
    pump_tx();
    int hs[CL_SESS_MAX];
    int n = cl_handles(hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) { deliver(hs[i]); }
}

void fh_classic_reset(void) {
    cl_sess_reset();
    memset(s_peers, 0, sizeof s_peers);
    memset(s_host_key, 0, sizeof s_host_key);
    memset(s_act, 0, sizeof s_act);
    s_npeers = 0;
    s_inq_count = 0;
    s_inq_end = s_inq_note_at = 0;
    s_sdp_busy = 0;
    s_sdp_mod = NULL;
}

void fh_classic_unload(pex_mod_t *m) {
    if (s_sdp_mod == m) { s_sdp_mod = NULL; s_sdp_fn = NULL; }
    cl_release_owner((uintptr_t)m);
}

/* ---- the group ---- */
#define OFF() do { if (fh_group_off("classic")) { return PEX_ENOTSUP; } } while (0)

static int own(int handle, cl_info_t *out) {
    pex_mod_t *m = pex_self();
    if (!m || cl_info(handle, out) != CL_OK) { return PEX_ENOENT; }
    if (out->owner != (uintptr_t)m) { return PEX_ENOENT; }
    return PEX_OK;
}

int pex_bt_classic_scan(uint32_t ms) {
    OFF();
    if (cl_inq_active()) { return PEX_EBUSY; }
    int rc = cl_inq_start();
    if (rc) { return rc; }
    s_inq_count++;
    if (!ms) { ms = 10240; }
    s_inq_end = fh_now() + ms;
    s_inq_note_at = fh_now() + (ms < 300 ? ms / 2 : 300);
    return PEX_OK;
}
void pex_bt_classic_scan_stop(void) { cl_inq_stop(); }
int pex_bt_classic_scanning(void) { return fh_group_off("classic") ? 0 : cl_inq_active(); }
int pex_bt_classic_devices(pex_cl_dev_t *out, uint32_t max, uint32_t cod_mask, uint32_t cod_value,
                           const char *prefix) {
    OFF();
    cl_dev_t tmp[CL_DEV_MAX];
    if (max > CL_DEV_MAX) { max = CL_DEV_MAX; }
    if (max && !out) { return PEX_EINVAL; }
    int n = cl_inq_filter(cod_mask, cod_value, prefix, tmp, (int)max);
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof out[i]);
        memcpy(out[i].addr, tmp[i].addr, 6);
        memcpy(out[i].name, tmp[i].name, sizeof out[i].name);
        out[i].name[sizeof out[i].name - 1] = 0;
        out[i].rssi = tmp[i].rssi;
        out[i].cod = tmp[i].cod;
    }
    return n;
}
int pex_bt_classic_sdp(const uint8_t *addr, uint16_t uuid, pex_cl_fn fn, void *arg) {
    OFF();
    if (!pex_self() || !fn || !addr || !uuid) { return PEX_EINVAL; }
    if (s_sdp_busy) { return PEX_EBUSY; }
    fh_cl_peer_t *p = peer_of(addr);
    s_sdp_busy = 1;
    s_sdp_due = fh_now() + (p ? p->c.page_ms : 30) + 8;
    memcpy(s_sdp_addr, addr, 6);
    s_sdp_uuid = uuid;
    s_sdp_mod = pex_self();
    s_sdp_fn = fn;
    s_sdp_arg = arg;
    return PEX_OK;
}

static int radio_block(const cl_open_t *o) {
    int needs_sdp = o->channel == 0;
    int needs_page = !cl_addr_held(o->addr);
    int hs[CL_SESS_MAX];
    if (needs_sdp && !o->uuid) { return CL_EINVAL; }
    if (needs_page && cl_inq_active()) { return CL_EBUSY; }
    if (needs_sdp && s_sdp_busy) { return CL_EBUSY; }
    int n = cl_handles(hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) {
        cl_info_t in;
        if (cl_info(hs[i], &in) != CL_OK) { continue; }
        if (needs_page && in.phase == CL_PHASE_PAGE) { return CL_EBUSY; }
        if (needs_sdp && in.phase == CL_PHASE_SDP) { return CL_EBUSY; }
    }
    return CL_OK;
}

int pex_bt_classic_open(const pex_cl_open_t *opt) {
    OFF();
    cl_open_t o;
    pex_mod_t *m = pex_self();
    if (!opt || !m) { return PEX_EINVAL; }
    if (opt->npin > PEX_CL_PIN_MAX) { return PEX_EINVAL; }
    memset(&o, 0, sizeof o);
    memcpy(o.addr, opt->addr, 6);
    o.uuid = opt->uuid;
    o.channel = opt->channel;
    o.fallback = opt->fallback;
    o.level = opt->level;
    o.io = opt->io;
    o.bond = opt->bond;
    o.drop_key = opt->drop_key ? 1 : 0;
    o.deadline_ms = opt->deadline_ms;
    o.owner = (uintptr_t)m;
    o.cb = (uintptr_t)opt->fn;
    o.cb_arg = (uintptr_t)opt->arg;
    o.npin = opt->npin;
    for (int i = 0; i < opt->npin; i++) {
        size_t n = 0;
        while (opt->pin[i][n] && n < CL_PIN_LEN) { n++; }
        if (n == 0 || n >= CL_PIN_LEN) { return PEX_EINVAL; }
        memcpy(o.pin[i], opt->pin[i], n);
    }
    int blocked = radio_block(&o);
    if (blocked) { return blocked; }
    int h = cl_open(&o, fh_now());
    if (h < 0) { return h; }
    int k = kick(h);
    if (k) { cl_close(h); cl_reap(h); return k; }
    return h;
}
int pex_bt_classic_pin(int h, const char *pin) {
    OFF();
    cl_info_t in;
    int rc = own(h, &in);
    return rc ? rc : cl_submit_pin(h, pin);
}
int pex_bt_classic_passkey(int h, int negative, uint32_t pk) {
    OFF();
    cl_info_t in;
    int rc = own(h, &in);
    return rc ? rc : cl_submit_passkey(h, negative, pk);
}
int pex_bt_classic_confirm(int h, int accept) {
    OFF();
    cl_info_t in;
    int rc = own(h, &in);
    return rc ? rc : cl_submit_confirm(h, accept);
}
int pex_bt_classic_send(int h, const void *data, size_t len) {
    OFF();
    cl_info_t in;
    int rc = own(h, &in);
    return rc ? rc : cl_tx_put(h, data, len);
}
int pex_bt_classic_tx_room(int h) {
    OFF();
    cl_info_t in;
    int rc = own(h, &in);
    return rc ? rc : cl_tx_room(h);
}
int pex_bt_classic_listen(int h, void *buf, size_t cap) {
    OFF();
    cl_info_t in;
    int rc = own(h, &in);
    return rc ? rc : cl_listen(h, buf, cap);
}
int pex_bt_classic_close(int h) {
    OFF();
    cl_info_t in;
    int rc = own(h, &in);
    return rc ? rc : cl_close(h);
}
int pex_bt_classic_state(int h, pex_cl_info_t *out) {
    OFF();
    cl_info_t in;
    int rc = own(h, &in);
    if (rc) { return rc; }
    if (!out) { return PEX_EINVAL; }
    memset(out, 0, sizeof *out);
    out->handle = in.handle;
    memcpy(out->addr, in.addr, 6);
    out->uuid = in.uuid;
    out->channel = in.channel;
    out->state = in.state;
    out->ask = in.ask;
    out->level = in.level;
    out->secure = in.secure;
    out->why = in.why;
    out->mtu = in.mtu;
    out->cod = in.cod;
    out->rx_drop = in.rx_drop;
    out->confirm_num = in.confirm_num;
    memcpy(out->error, in.error, sizeof out->error);
    return PEX_OK;
}
int pex_bt_classic_forget(const uint8_t *addr) {
    OFF();
    if (!addr) { return PEX_EINVAL; }
    fh_cl_peer_t *p = peer_of(addr);
    if (p) { s_host_key[peer_ix(p)] = 0; }
    return PEX_OK;
}
