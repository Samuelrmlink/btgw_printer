/* The fake host: core, log, mem, sched, pex, sys, bus, fs. See fakehost.h. */
#include "fh_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODS 12
#define TASKS 48
#define OPS 16                /* the firmware's BUS_OPS: one table for every module */
#define FILES 4               /* the firmware's open-file limit */
#define POOL_BYTES (48u * 1024u)

static struct pex_mod s_mods[MODS];
static pex_mod_t *s_cur;
static uint32_t s_now;
static int s_verbose;
static char s_off[4][16];

pex_mod_t *fh_cur(void) { return s_cur; }
pex_mod_t *fh_swap(pex_mod_t *m) { pex_mod_t *o = s_cur; s_cur = m; return o; }
void fh_verbose(int on) { s_verbose = on; }

void fh_disable_group(const char *g) {
    for (int i = 0; i < 4; i++) {
        if (!s_off[i][0]) { snprintf(s_off[i], sizeof s_off[i], "%s", g); return; }
    }
}
int fh_group_off(const char *g) {
    for (int i = 0; i < 4; i++) { if (s_off[i][0] && strcmp(s_off[i], g) == 0) { return 1; } }
    return 0;
}
int fh_avail(const char *sym) {
    if (strncmp(sym, "pex_bt_classic", 14) == 0) { return !fh_group_off("classic"); }
    if (strncmp(sym, "pex_bt_", 7) == 0 || strncmp(sym, "pex_gatt", 8) == 0) { return !fh_group_off("bt"); }
    return 1;
}

/* ---- log ---- */
#define LOGS 4096
static char *s_log[LOGS];
static int s_nlog;

void fh_log_line(pex_mod_t *m, const char *fmt, ...) {
    char b[512];
    va_list ap;
    int o = snprintf(b, sizeof b, "[%6u] %s: ", s_now, m ? m->name : "host");
    va_start(ap, fmt);
    vsnprintf(b + o, sizeof b - (size_t)o, fmt, ap);
    va_end(ap);
    if (s_verbose) { fprintf(stderr, "%s\n", b); }
    if (s_nlog < LOGS) { s_log[s_nlog++] = strdup(b); }
}
int fh_log_has(const char *sub) {
    for (int i = 0; i < s_nlog; i++) { if (strstr(s_log[i], sub)) { return 1; } }
    return 0;
}

void pex_logv(int level, const char *fmt, va_list ap) {
    char b[400];
    vsnprintf(b, sizeof b, fmt, ap);
    static const char *L[] = { "E", "W", "I", "D" };
    fh_log_line(s_cur, "%s %s", L[level & 3], b);
}
void pex_log(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pex_logv(level, fmt, ap);
    va_end(ap);
}
void pex_log_hex(int level, const char *what, const void *data, size_t len) {
    char b[200];
    size_t o = 0;
    const uint8_t *p = data;
    for (size_t i = 0; i < len && o + 3 < sizeof b; i++) { o += (size_t)snprintf(b + o, sizeof b - o, "%02x", p[i]); }
    b[o] = 0;
    pex_log(level, "%s %s", what, b);
}

/* ---- core ---- */
uint32_t pex_api_version(void) { return (PEX_API_MAJOR << 16) | PEX_API_MINOR; }
uint32_t pex_has(uint32_t g) {
    if (g == PEX_GROUP_CLASSIC && fh_group_off("classic")) { return 0; }
    if (g == PEX_GROUP_BT && fh_group_off("bt")) { return 0; }
    return 1u << 16 | 1u;
}
uint32_t pex_target(void) { return PEX_TARGET_ARMV8M_MAIN; }
uint32_t pex_ms(void) { return s_now; }
uint64_t pex_us(void) { return (uint64_t)s_now * 1000u; }
int pex_vsnprintf(char *d, size_t cap, const char *fmt, va_list ap) { return vsnprintf(d, cap, fmt, ap); }
int pex_snprintf(char *d, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(d, cap, fmt, ap);
    va_end(ap);
    return n;
}
void pex_assert_fail(const char *e, const char *f, int l) { fh_log_line(s_cur, "ASSERT %s at %s:%d", e, f, l); }

/* ---- mem: malloc with a header, charged to the calling module, inside a 48 KB pool ---- */
typedef struct { pex_mod_t *m; size_t n; uint32_t magic; uint32_t pad; } mhdr_t;
static size_t s_pool_used;

static void *fh_alloc(size_t n, int zero) {
    if (s_pool_used + n > POOL_BYTES) { return NULL; }
    mhdr_t *h = zero ? calloc(1, sizeof *h + n) : malloc(sizeof *h + n);
    if (!h) { return NULL; }
    h->m = s_cur;
    h->n = n;
    h->magic = 0x50455841u;
    s_pool_used += n;
    if (s_cur) {
        s_cur->mem += n;
        if (s_cur->mem > s_cur->peak) { s_cur->peak = s_cur->mem; }
    }
    return h + 1;
}
void *pex_alloc_long(size_t n) { return fh_alloc(n, 1); }
void *pex_alloc_ex(size_t n, uint32_t flags) { (void)flags; return fh_alloc(n, 1); }
void *pex_malloc(size_t n) { return fh_alloc(n, 0); }
void *pex_calloc(size_t n, size_t s) { return fh_alloc(n * s, 1); }
void pex_free(void *p) {
    if (!p) { return; }
    mhdr_t *h = (mhdr_t *)p - 1;
    if (h->magic != 0x50455841u) { fh_log_line(s_cur, "pex_free: bad pointer"); return; }
    h->magic = 0;
    s_pool_used -= h->n;
    if (h->m) { h->m->mem -= h->n; }
    free(h);
}
void *pex_realloc(void *p, size_t n) {
    if (!p) { return pex_malloc(n); }
    mhdr_t *h = (mhdr_t *)p - 1;
    void *q = pex_malloc(n);
    if (!q) { return NULL; }
    memcpy(q, p, h->n < n ? h->n : n);
    pex_free(p);
    return q;
}
struct pex_arena { size_t cap, used; struct pex_arena *prev; uint8_t mem[]; };
static struct pex_arena *s_arena;
pex_arena_t *pex_scratch_begin(size_t cap) {
    struct pex_arena *a = fh_alloc(sizeof *a + cap, 0);
    if (!a) { return NULL; }
    a->cap = cap;
    a->used = 0;
    a->prev = s_arena;
    s_arena = a;
    return a;
}
void *pex_scratch_alloc(pex_arena_t *a, size_t n) {
    n = (n + 7u) & ~7u;
    if (!a || a->used + n > a->cap) { return NULL; }
    void *p = a->mem + a->used;
    a->used += n;
    return p;
}
int pex_scratch_end(pex_arena_t *a) {
    if (a != s_arena) { return PEX_EORDER; }
    s_arena = a->prev;
    pex_free(a);
    return PEX_OK;
}
size_t pex_scratch_avail(void) { return POOL_BYTES - s_pool_used; }
struct pex_pool { size_t sz; };
pex_pool_t *pex_pool_new(size_t sz, uint32_t count) {
    (void)count;
    struct pex_pool *p = fh_alloc(sizeof *p, 0);
    if (p) { p->sz = sz; }
    return p;
}
void *pex_pool_get(pex_pool_t *p) { return p ? fh_alloc(p->sz, 1) : NULL; }
void pex_pool_put(pex_pool_t *p, void *o) { (void)p; pex_free(o); }
void pex_pool_delete(pex_pool_t *p) { pex_free(p); }
int pex_mem_stats(pex_mem_stats_t *o) {
    if (!o) { return PEX_EINVAL; }
    memset(o, 0, sizeof *o);
    o->long_bytes = s_cur ? (uint32_t)s_cur->mem : 0;
    o->pool_size = POOL_BYTES;
    o->pool_free = (uint32_t)(POOL_BYTES - s_pool_used);
    o->pool_largest = o->pool_free;
    return PEX_OK;
}
size_t fh_mem_used(pex_mod_t *m) { return m ? m->mem : s_pool_used; }
size_t fh_mem_peak(pex_mod_t *m) { return m ? m->peak : 0; }

/* ---- sched ---- */
typedef struct { int id; pex_mod_t *m; pex_task_fn fn; void *arg; uint32_t period, due; int once, live; } task_t;
static task_t s_tasks[TASKS];
static int s_task_id = 1;

static int add_task(uint32_t period, uint32_t delay, int once, pex_task_fn fn, void *arg) {
    if (!fn || !s_cur) { return PEX_EINVAL; }
    for (int i = 0; i < TASKS; i++) {
        if (!s_tasks[i].live) {
            s_tasks[i] = (task_t){ s_task_id++, s_cur, fn, arg, period, s_now + delay, once, 1 };
            return s_tasks[i].id;
        }
    }
    return PEX_ENOMEM;
}
int pex_sched_every(uint32_t ms, pex_task_fn fn, void *arg) { return add_task(ms ? ms : 1, ms, 0, fn, arg); }
int pex_sched_after(uint32_t ms, pex_task_fn fn, void *arg) { return add_task(0, ms, 1, fn, arg); }
int pex_sched_post(pex_task_fn fn, void *arg) { return add_task(0, 0, 1, fn, arg); }
int pex_sched_cancel(int id) {
    for (int i = 0; i < TASKS; i++) {
        if (s_tasks[i].live && s_tasks[i].id == id) { s_tasks[i].live = 0; return PEX_OK; }
    }
    return PEX_ENOENT;
}
uint32_t pex_sched_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < TASKS; i++) { n += s_tasks[i].live && s_tasks[i].m == s_cur; }
    return n;
}
static void cancel_tasks(pex_mod_t *m) {
    for (int i = 0; i < TASKS; i++) { if (s_tasks[i].m == m) { s_tasks[i].live = 0; } }
}

/* ---- pex ---- */
static const pex_host_t s_host = { sizeof(pex_host_t), PEX_API_MAJOR, PEX_API_MINOR,
                                   PEX_TARGET_ARMV8M_MAIN, PEX_FORMAT_MINOR, "fakehost", NULL };

pex_mod_t *pex_self(void) { return s_cur; }
void pex_ref(pex_mod_t *m) { if (m) { m->refs++; } }
void pex_unref(pex_mod_t *m) { if (m && m->refs) { m->refs--; } }
const char *pex_name(pex_mod_t *m) { return m ? m->name : NULL; }
void pex_app_exit(int code) {
    (void)code;
    if (s_cur) { s_cur->exiting = 1; }
}
pex_mod_t *pex_find(const char *name) {
    for (int i = 0; i < MODS; i++) {
        if (s_mods[i].used && s_mods[i].loaded && strcmp(s_mods[i].name, name) == 0) { return &s_mods[i]; }
    }
    return NULL;
}
uint32_t pex_sym(pex_mod_t *m, const char *name) {
    if (!m) { return 0; }
    for (int i = 0; i < m->nexports; i++) {
        if (strcmp(m->exports[i].sym, name) == 0) { return (uint32_t)(uintptr_t)m->exports[i].fn; }
    }
    return 0;
}
uint64_t pex_call(pex_mod_t *m, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    typedef int (*f4)(uint32_t, uint32_t, uint32_t, uint32_t);
    if (!m || !fn || !m->loaded) { return (uint64_t)(uint32_t)PEX_ENOENT; }
    pex_mod_t *o = fh_swap(m);
    int r = ((f4)(uintptr_t)fn)(a0, a1, a2, a3);
    fh_swap(o);
    return (uint64_t)(uint32_t)r;
}

pex_mod_t *fh_module(const char *name, fh_main_fn main) {
    for (int i = 0; i < MODS; i++) {
        if (!s_mods[i].used) {
            memset(&s_mods[i], 0, sizeof s_mods[i]);
            s_mods[i].used = 1;
            snprintf(s_mods[i].name, sizeof s_mods[i].name, "%s", name);
            s_mods[i].main = main;
            return &s_mods[i];
        }
    }
    return NULL;
}
void fh_export(pex_mod_t *m, const char *sym, void *fn) {
    if (!m || m->nexports >= FH_EXPORTS) { return; }
    snprintf(m->exports[m->nexports].sym, sizeof m->exports[0].sym, "%s", sym);
    m->exports[m->nexports++].fn = fn;
}
static int enter(pex_mod_t *m, uint32_t reason, void *arg) {
    pex_mod_t *o = fh_swap(m);
    pex_host_t h = s_host;
    h.self = m;
    int rc = m->main(&h, reason, arg);
    fh_swap(o);
    return rc;
}
int fh_loaded(pex_mod_t *m) { return m && m->loaded; }
int fh_load(pex_mod_t *m) {
    if (!m || m->loaded) { return PEX_ESTATE; }
    m->loaded = 1;
    int rc = enter(m, PEX_INIT, NULL);
    if (rc < 0) { m->loaded = 0; }
    return rc;
}
int fh_start(pex_mod_t *m, const char *cfg) {
    if (!m || !m->loaded) { return PEX_ESTATE; }
    m->running = 1;
    m->exiting = 0;
    return enter(m, PEX_APP_RUN, (void *)cfg);
}
static void drop_ops(pex_mod_t *m);
static void close_files(pex_mod_t *m);
int fh_stop(pex_mod_t *m) {
    if (!m || !m->loaded) { return PEX_ESTATE; }
    int rc = enter(m, PEX_APP_STOP, NULL);
    cancel_tasks(m);
    m->running = 0;
    return rc;
}
int fh_unload(pex_mod_t *m) {
    if (!m || !m->loaded) { return PEX_ESTATE; }
    if (m->running) { fh_stop(m); }
    int rc = enter(m, PEX_DEINIT, NULL);
    cancel_tasks(m);
    drop_ops(m);
    fh_ble_unload(m);
    fh_classic_unload(m);
    close_files(m);
    m->loaded = 0;
    return rc;
}

/* ---- sys ---- */
static char s_secret_json[2048];
int64_t pex_sys_time(void) { return 0; }
const char *pex_sys_hostname(void) { return "btgw-fake"; }
const char *pex_sys_version(void) { return "1.999"; }
int pex_sys_mem(pex_sys_mem_t *o) {
    if (!o) { return PEX_EINVAL; }
    memset(o, 0, sizeof *o);
    o->pex_size = POOL_BYTES;
    o->pex_free = (uint32_t)(POOL_BYTES - s_pool_used);
    return PEX_OK;
}
int pex_sys_secret(const char *name, const char *key, char *out, size_t cap) {
    char p[96];
    size_t n = 0;
    snprintf(p, sizeof p, "lfs:/cfg/%s.json", name);
    const uint8_t *d = fh_fs_get(p, &n);
    if (!d) { return PEX_ENOENT; }
    if (n >= sizeof s_secret_json) { n = sizeof s_secret_json - 1; }
    memcpy(s_secret_json, d, n);
    s_secret_json[n] = 0;
    if (!pex_json_str(s_secret_json, key, out, cap)) { return PEX_ENOENT; }
    return (int)strlen(out);
}

/* ---- bus ---- */
typedef struct { char name[48]; pex_mod_t *m; pex_op_fn fn; void *arg; } op_t;
static op_t s_ops[OPS];
typedef struct { char *topic, *json; } ev_t;
static ev_t *s_ev;
static int s_nev, s_cap_ev;

int pex_bus_op(const char *name, pex_op_fn fn, void *arg) {
    if (!name || !fn || !strchr(name, '.')) { return PEX_EINVAL; }
    if (strncmp(name, "pex.", 4) == 0) { return PEX_EEXIST; }
    for (int i = 0; i < OPS; i++) {
        if (s_ops[i].fn && strcmp(s_ops[i].name, name) == 0) { return PEX_EEXIST; }
    }
    for (int i = 0; i < OPS; i++) {
        if (!s_ops[i].fn) {
            snprintf(s_ops[i].name, sizeof s_ops[i].name, "%s", name);
            s_ops[i].m = s_cur;
            s_ops[i].fn = fn;
            s_ops[i].arg = arg;
            return PEX_OK;
        }
    }
    return PEX_ENOMEM;
}
int pex_bus_op_remove(const char *name) {
    for (int i = 0; i < OPS; i++) {
        if (s_ops[i].fn && s_ops[i].m == s_cur && strcmp(s_ops[i].name, name) == 0) {
            s_ops[i].fn = NULL;
            return PEX_OK;
        }
    }
    return PEX_ENOENT;
}
static void drop_ops(pex_mod_t *m) {
    for (int i = 0; i < OPS; i++) { if (s_ops[i].m == m) { s_ops[i].fn = NULL; } }
}
int pex_bus_emit(const char *topic, const char *json) {
    if (!topic) { return PEX_EINVAL; }
    if (s_nev == s_cap_ev) {
        s_cap_ev = s_cap_ev ? s_cap_ev * 2 : 256;
        s_ev = realloc(s_ev, sizeof *s_ev * (size_t)s_cap_ev);
    }
    s_ev[s_nev].topic = strdup(topic);
    s_ev[s_nev].json = strdup(json ? json : "null");
    s_nev++;
    if (s_verbose) { fprintf(stderr, "[%6u] evt %s %s\n", s_now, topic, json ? json : "null"); }
    return PEX_OK;
}
int fh_op_exists(const char *op) {
    for (int i = 0; i < OPS; i++) { if (s_ops[i].fn && strcmp(s_ops[i].name, op) == 0) { return 1; } }
    return 0;
}
int fh_op(const char *op, const char *params, char *reply, size_t cap) {
    for (int i = 0; i < OPS; i++) {
        if (s_ops[i].fn && strcmp(s_ops[i].name, op) == 0) {
            pex_mod_t *o = fh_swap(s_ops[i].m);
            if (cap) { reply[0] = 0; }
            int rc = s_ops[i].fn(s_ops[i].arg, params ? params : "{}", reply, cap);
            fh_swap(o);
            if (s_verbose) { fprintf(stderr, "[%6u] op %s %s -> %d %s\n", s_now, op, params ? params : "{}", rc, reply); }
            return rc;
        }
    }
    if (cap) { snprintf(reply, cap, "unknown op"); }
    return PEX_ENOENT;
}
int fh_events(const char *topic) {
    int n = 0;
    for (int i = 0; i < s_nev; i++) { n += strcmp(s_ev[i].topic, topic) == 0; }
    return n;
}
const char *fh_event(const char *topic, int idx) {
    int n = fh_events(topic);
    if (idx < 0) { idx += n; }
    if (idx < 0 || idx >= n) { return NULL; }
    for (int i = 0; i < s_nev; i++) {
        if (strcmp(s_ev[i].topic, topic) == 0 && idx-- == 0) { return s_ev[i].json; }
    }
    return NULL;
}
const char *fh_event_last(const char *topic) { return fh_event(topic, -1); }
void fh_events_clear(void) {
    for (int i = 0; i < s_nev; i++) { free(s_ev[i].topic); free(s_ev[i].json); }
    s_nev = 0;
}

/* ---- fs ---- */
typedef struct { char path[140]; uint8_t *d; size_t n, cap; int dir; } file_t;
#define FSMAX 256
static file_t s_fs[FSMAX];
static int s_sd_present = 1;
static uint32_t s_sd_lat = 2;
typedef struct { pex_mod_t *m; int used, sd, mode; file_t *f; size_t pos; } fd_t;
static fd_t s_fd[FILES];
typedef struct { int live; uint32_t due; pex_mod_t *m; pex_fs_cb cb; void *arg; int rc;
                 int op; int fd; void *buf; size_t n; } sdq_t;
#define SDQ 16
static sdq_t s_sdq[SDQ];

void fh_fs_sd_present(int on) { s_sd_present = on; }
void fh_fs_sd_latency(uint32_t ms) { s_sd_lat = ms; }

static int vol_of(const char *p, const char **rel) {
    if (strncmp(p, "lfs:", 4) == 0) { p += 4; while (*p == '/') { p++; } *rel = p; return 1; }
    if (strncmp(p, "sd:", 3) == 0) { p += 3; while (*p == '/') { p++; } *rel = p; return 2; }
    return 0;
}
static void norm(const char *in, char *out, size_t cap) {
    const char *rel;
    int v = vol_of(in, &rel);
    if (!v) { snprintf(out, cap, "?%s", in); return; }
    snprintf(out, cap, "%s:%s", v == 1 ? "lfs" : "sd", rel);
    size_t n = strlen(out);
    while (n && out[n - 1] == '/') { out[--n] = 0; }
}
static file_t *ffind(const char *path) {
    char k[140];
    norm(path, k, sizeof k);
    for (int i = 0; i < FSMAX; i++) { if (s_fs[i].path[0] && strcmp(s_fs[i].path, k) == 0) { return &s_fs[i]; } }
    return NULL;
}
static file_t *fcreate(const char *path) {
    file_t *f = ffind(path);
    if (f) { return f; }
    for (int i = 0; i < FSMAX; i++) {
        if (!s_fs[i].path[0]) { norm(path, s_fs[i].path, sizeof s_fs[i].path); return &s_fs[i]; }
    }
    return NULL;
}
void fh_fs_put(const char *path, const void *data, size_t n) {
    file_t *f = fcreate(path);
    if (!f) { return; }
    free(f->d);
    f->d = malloc(n ? n : 1);
    memcpy(f->d, data, n);
    f->n = f->cap = n;
    f->dir = 0;
}
const uint8_t *fh_fs_get(const char *path, size_t *n) {
    file_t *f = ffind(path);
    if (!f || f->dir) { return NULL; }
    if (n) { *n = f->n; }
    return f->d ? f->d : (const uint8_t *)"";
}
int fh_fs_open_count(void) {
    int n = 0;
    for (int i = 0; i < FILES; i++) { n += s_fd[i].used; }
    return n;
}

static int sd_queue(int op, int fd, pex_fs_cb cb, void *arg, int rc, void *buf, size_t n) {
    if (!cb) { return PEX_EAGAIN; }
    for (int i = 0; i < SDQ; i++) {
        if (!s_sdq[i].live) {
            s_sdq[i] = (sdq_t){ 1, s_now + s_sd_lat, s_cur, cb, arg, rc, op, fd, buf, n };
            return PEX_EPENDING;
        }
    }
    return PEX_EBUSY;
}
enum { Q_DONE = 0, Q_READ = 1 };
static void sd_poll(void) {
    for (int i = 0; i < SDQ; i++) {
        sdq_t *q = &s_sdq[i];
        if (!q->live || (int32_t)(s_now - q->due) < 0) { continue; }
        q->live = 0;
        int rc = q->rc;
        if (q->op == Q_READ && rc >= 0) {
            fd_t *d = &s_fd[q->fd];
            if (!d->used || !d->f) { rc = PEX_EINVAL; }
            else {
                size_t left = d->f->n > d->pos ? d->f->n - d->pos : 0;
                size_t k = q->n < left ? q->n : left;
                memcpy(q->buf, d->f->d + d->pos, k);
                d->pos += k;
                rc = (int)k;
            }
        }
        pex_mod_t *o = fh_swap(q->m);
        q->cb(q->arg, rc);
        fh_swap(o);
    }
}

int pex_fs_open(const char *path, int mode, pex_fs_cb cb, void *arg) {
    const char *rel;
    int v = path ? vol_of(path, &rel) : 0;
    if (!v || mode < PEX_FS_READ || mode > PEX_FS_APPEND || !*rel) { return PEX_EINVAL; }
    if (v == 2 && !s_sd_present) { return PEX_EIO; }
    int fd = -1;
    for (int i = 0; i < FILES; i++) { if (!s_fd[i].used) { fd = i; break; } }
    if (fd < 0) { return PEX_ENOMEM; }
    file_t *f = ffind(path);
    if (mode == PEX_FS_READ && (!f || f->dir)) {
        return v == 2 ? sd_queue(Q_DONE, fd, cb, arg, PEX_ENOENT, NULL, 0) : PEX_ENOENT;
    }
    if (mode != PEX_FS_READ) {
        f = fcreate(path);
        if (!f) { return PEX_ENOMEM; }
        if (mode == PEX_FS_WRITE) { f->n = 0; }
    }
    s_fd[fd] = (fd_t){ s_cur, 1, v == 2, mode, f, mode == PEX_FS_APPEND ? f->n : 0 };
    if (v == 2) {
        int rc = sd_queue(Q_DONE, fd, cb, arg, fd, NULL, 0);
        if (rc != PEX_EPENDING) { s_fd[fd].used = 0; }
        return rc;
    }
    return fd;
}
static fd_t *fd_of(int fd) {
    if (fd < 0 || fd >= FILES || !s_fd[fd].used || s_fd[fd].m != s_cur) { return NULL; }
    return &s_fd[fd];
}
int pex_fs_read(int fd, void *buf, size_t n, pex_fs_cb cb, void *arg) {
    fd_t *d = fd_of(fd);
    if (!d || !buf) { return PEX_EINVAL; }
    if (d->sd) { return sd_queue(Q_READ, fd, cb, arg, 0, buf, n); }
    if (n > 4096) { n = 4096; }
    size_t left = d->f->n > d->pos ? d->f->n - d->pos : 0;
    size_t k = n < left ? n : left;
    memcpy(buf, d->f->d + d->pos, k);
    d->pos += k;
    return (int)k;
}
int pex_fs_write(int fd, const void *buf, size_t n, pex_fs_cb cb, void *arg) {
    fd_t *d = fd_of(fd);
    if (!d || !buf || d->mode == PEX_FS_READ) { return PEX_EINVAL; }
    if (!d->sd && n > 4096) { n = 4096; }
    file_t *f = d->f;
    if (d->pos + n > f->cap) {
        f->cap = (d->pos + n) * 2;
        f->d = realloc(f->d, f->cap);
    }
    memcpy(f->d + d->pos, buf, n);
    d->pos += n;
    if (d->pos > f->n) { f->n = d->pos; }
    if (d->sd) { return sd_queue(Q_DONE, fd, cb, arg, (int)n, NULL, 0); }
    return (int)n;
}
int pex_fs_seek(int fd, uint32_t pos, pex_fs_cb cb, void *arg) {
    fd_t *d = fd_of(fd);
    if (!d) { return PEX_EINVAL; }
    d->pos = pos;
    if (d->sd) { return sd_queue(Q_DONE, fd, cb, arg, (int)pos, NULL, 0); }
    return (int)pos;
}
int32_t pex_fs_size(int fd) {
    fd_t *d = fd_of(fd);
    return d ? (int32_t)d->f->n : PEX_EINVAL;
}
int pex_fs_close(int fd) {
    fd_t *d = fd_of(fd);
    if (!d) { return PEX_EINVAL; }
    d->used = 0;
    return PEX_OK;
}
static void close_files(pex_mod_t *m) {
    for (int i = 0; i < FILES; i++) { if (s_fd[i].m == m) { s_fd[i].used = 0; } }
}
int pex_fs_stat(const char *path, pex_fs_entry_t *out, pex_fs_cb cb, void *arg) {
    const char *rel;
    int v = path ? vol_of(path, &rel) : 0;
    if (!v) { return PEX_EINVAL; }
    file_t *f = ffind(path);
    int rc = f ? PEX_OK : PEX_ENOENT;
    if (f && out) {
        memset(out, 0, sizeof *out);
        const char *b = strrchr(f->path, '/');
        snprintf(out->name, sizeof out->name, "%s", b ? b + 1 : strchr(f->path, ':') + 1);
        out->is_dir = (uint8_t)f->dir;
        out->size = (uint32_t)f->n;
    }
    return v == 2 ? sd_queue(Q_DONE, -1, cb, arg, rc, NULL, 0) : rc;
}
int pex_fs_list(const char *dir, pex_fs_list_cb each, void *arg) {
    char k[140];
    norm(dir, k, sizeof k);
    size_t kl = strlen(k);
    int n = 0;
    for (int i = 0; i < FSMAX; i++) {
        const char *p = s_fs[i].path;
        if (!p[0] || strncmp(p, k, kl) != 0) { continue; }
        const char *rest = p + kl;
        if (k[kl - 1] != ':') { if (*rest != '/') { continue; } rest++; }
        if (!*rest || strchr(rest, '/')) { continue; }
        pex_fs_entry_t e;
        memset(&e, 0, sizeof e);
        snprintf(e.name, sizeof e.name, "%s", rest);
        e.is_dir = (uint8_t)s_fs[i].dir;
        e.size = (uint32_t)s_fs[i].n;
        if (each) { each(arg, &e); }
        n++;
    }
    if (each) { each(arg, NULL); }
    return n;
}
int pex_fs_mkdir(const char *path, pex_fs_cb cb, void *arg) {
    file_t *f = fcreate(path);
    if (!f) { return PEX_ENOMEM; }
    f->dir = 1;
    const char *rel;
    return vol_of(path, &rel) == 2 ? sd_queue(Q_DONE, -1, cb, arg, PEX_OK, NULL, 0) : PEX_OK;
}
int pex_fs_remove(const char *path, pex_fs_cb cb, void *arg) {
    file_t *f = ffind(path);
    int rc = PEX_ENOENT;
    if (f) { free(f->d); memset(f, 0, sizeof *f); rc = PEX_OK; }
    const char *rel;
    return vol_of(path, &rel) == 2 ? sd_queue(Q_DONE, -1, cb, arg, rc, NULL, 0) : rc;
}
int pex_fs_rename(const char *from, const char *to, pex_fs_cb cb, void *arg) {
    file_t *f = ffind(from);
    int rc = PEX_ENOENT;
    if (f) { norm(to, f->path, sizeof f->path); rc = PEX_OK; }
    const char *rel;
    return vol_of(from, &rel) == 2 ? sd_queue(Q_DONE, -1, cb, arg, rc, NULL, 0) : rc;
}
int pex_fs_media(pex_fs_media_t *o) {
    if (!o) { return PEX_EINVAL; }
    memset(o, 0, sizeof *o);
    o->lfs_ok = 1;
    o->lfs_total = 1200u * 1024u;
    o->lfs_free = 900u * 1024u;
    snprintf(o->sd_state, sizeof o->sd_state, "%s", s_sd_present ? "ok" : "none");
    if (s_sd_present) { o->sd_total = 32ull << 30; o->sd_free = 30ull << 30; }
    return PEX_OK;
}

/* ---- the loop ---- */
uint32_t fh_now(void) { return s_now; }
void fh_step(void) {
    for (int i = 0; i < TASKS; i++) {
        task_t *t = &s_tasks[i];
        if (!t->live || (int32_t)(s_now - t->due) < 0) { continue; }
        pex_mod_t *m = t->m;
        pex_mod_t *o = fh_swap(m);
        int r = t->fn(t->arg);
        fh_swap(o);
        if (!t->live) { continue; }   /* cancelled itself */
        if (t->once || r == PEX_TASK_DONE) { t->live = 0; }
        else { t->due = s_now + t->period; }
        if (m->exiting) { cancel_tasks(m); m->exiting = 0; m->running = 0; }
    }
    fh_ble_poll();
    fh_classic_poll();
    sd_poll();
    s_now++;
}
void fh_run(uint32_t ms) { for (uint32_t i = 0; i < ms; i++) { fh_step(); } }
int fh_run_until(int (*cond)(void *), void *arg, uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        if (cond(arg)) { return 1; }
        fh_step();
    }
    return cond(arg);
}

void fh_reset(void) {
    for (int i = 0; i < MODS; i++) { memset(&s_mods[i], 0, sizeof s_mods[i]); }
    memset(s_tasks, 0, sizeof s_tasks);
    memset(s_ops, 0, sizeof s_ops);
    fh_events_clear();
    for (int i = 0; i < s_nlog; i++) { free(s_log[i]); }
    s_nlog = 0;
    for (int i = 0; i < FSMAX; i++) { free(s_fs[i].d); }
    memset(s_fs, 0, sizeof s_fs);
    memset(s_fd, 0, sizeof s_fd);
    memset(s_sdq, 0, sizeof s_sdq);
    memset(s_off, 0, sizeof s_off);
    s_sd_present = 1;
    s_sd_lat = 2;
    s_cur = NULL;
    s_now = 1000;
    s_pool_used = 0;
    s_arena = NULL;
    fh_ble_reset();
    fh_classic_reset();
}
