/* The fake host on its own: modules, ops, tasks, files, a Classic peer, a BLE peer. */
#include "t.h"
#include "fakehost.h"
#include "fh_classic.h"
#include "fh_ble.h"

/* ---- a tiny module defined here ---- */
static int s_ticks, s_cl_open, s_cl_failed, s_cl_rx_bytes, s_handle;
static int s_why;
static uint8_t s_rx[2048];
static int tick(void *a) { (void)a; s_ticks++; return PEX_TASK_AGAIN; }
static int op_echo(void *a, const char *p, char *r, size_t cap) {
    (void)a;
    return pex_snprintf(r, cap, "{\"got\":%s}", p);
}
static void on_cl(void *a, const pex_cl_event_t *ev) {
    (void)a;
    if (ev->kind == PEX_CL_OPEN) { s_cl_open++; }
    if (ev->kind == PEX_CL_FAILED) { s_cl_failed++; s_why = ev->why; }
    if (ev->kind == PEX_CL_RX) {
        int n;
        while ((n = pex_bt_classic_listen(ev->handle, s_rx + s_cl_rx_bytes, 64)) > 0) { s_cl_rx_bytes += n; }
    }
}
static int mini_main(const pex_host_t *h, uint32_t reason, void *arg) {
    (void)h; (void)arg;
    if (reason == PEX_INIT) { return pex_bus_op("mini.echo", op_echo, NULL); }
    if (reason == PEX_APP_RUN) { pex_sched_every(10, tick, NULL); }
    return PEX_OK;
}
static int mini_add(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { (void)c; (void)d; return (int)(a + b); }

static uint8_t s_peer_got[64];
static size_t s_peer_n;
static void peer_rx(fh_cl_peer_t *p, const uint8_t *d, size_t n, void *ctx) {
    (void)ctx;
    if (s_peer_n + n <= sizeof s_peer_got) { memcpy(s_peer_got + s_peer_n, d, n); s_peer_n += n; }
    if (n >= 3 && memcmp(d, "hi?", 3) == 0) { fh_cl_peer_send(p, "yes\r\n", 5); }
}

static void t_modules(void) {
    fh_reset();
    pex_mod_t *m = fh_module("mini", mini_main);
    fh_export(m, "mini_add", (void *)mini_add);
    CHECK_EQ(fh_load(m), PEX_OK);
    CHECK_EQ(fh_start(m, "{}"), PEX_OK);
    fh_run(100);
    CHECK(s_ticks >= 9);
    char r[64];
    CHECK(fh_op("mini.echo", "{\"a\":1}", r, sizeof r) > 0);
    CHECK_STR(r, "{\"got\":{\"a\":1}}");
    pex_mod_t *f = pex_find("mini");
    CHECK(f == m);
    CHECK_EQ((int)pex_call(f, pex_sym(f, "mini_add"), 2, 3, 0, 0), 5);
    fh_unload(m);
    CHECK(!fh_op_exists("mini.echo"));
    int t0 = s_ticks;
    fh_run(50);
    CHECK_EQ(s_ticks, t0);
}

static void t_files(void) {
    fh_reset();
    pex_mod_t *m = fh_module("mini", mini_main);
    fh_load(m);
    pex_mod_t *o = fh_swap(m);
    int fd = pex_fs_open("lfs:/a/b.bin", PEX_FS_WRITE, NULL, NULL);
    CHECK(fd >= 0);
    CHECK_EQ(pex_fs_write(fd, "hello", 5, NULL, NULL), 5);
    pex_fs_close(fd);
    size_t n;
    const uint8_t *d = fh_fs_get("lfs:/a/b.bin", &n);
    CHECK(d && n == 5 && memcmp(d, "hello", 5) == 0);
    CHECK_EQ(pex_fs_open("sd:/x", PEX_FS_READ, NULL, NULL), PEX_EAGAIN);
    fh_swap(o);
}

static void t_classic(void) {
    fh_reset();
    fh_cl_peer_cfg_t pc = { .addr = "11:22:33:44:55:66", .name = "PM220", .cod = 0x000680,
                            .sdp_channel = 1, .pin = "1234", .on_rx = peer_rx };
    fh_cl_peer_t *p = fh_cl_peer_add(&pc);
    pex_mod_t *m = fh_module("mini", mini_main);
    fh_load(m);
    pex_mod_t *o = fh_swap(m);
    CHECK_EQ(pex_bt_classic_scan(1000), PEX_OK);
    fh_swap(o);
    fh_run(1100);
    o = fh_swap(m);
    pex_cl_dev_t d[4];
    CHECK_EQ(pex_bt_classic_devices(d, 4, 0x1F00, 0x0600, NULL), 1);
    CHECK_STR(d[0].name, "PM220");
    pex_cl_open_t op;
    memset(&op, 0, sizeof op);
    memcpy(op.addr, d[0].addr, 6);
    op.uuid = 0x1101;
    op.level = PEX_CL_LEVEL_OMIT;
    op.bond = PEX_CL_BOND_OMIT;
    op.drop_key = 1;
    op.deadline_ms = 5000;
    op.npin = 2;
    strcpy(op.pin[0], "0000");
    strcpy(op.pin[1], "1234");
    op.fn = on_cl;
    s_handle = pex_bt_classic_open(&op);
    CHECK(s_handle > 0);
    fh_swap(o);
    fh_run(500);
    /* the firmware's rule: the retry starts the list again, so "1234" is never reached */
    CHECK_EQ(s_cl_failed, 1);
    CHECK_EQ(s_why, 3);
    CHECK_EQ(fh_cl_peer_stats(p)->pin_requests, 2);
    CHECK_STR(fh_cl_peer_stats(p)->pins_tried[1], "0000");
    fh_run(10);
    CHECK_EQ(fh_cl_sessions(), 0);
    /* open again with 1234 first */
    o = fh_swap(m);
    strcpy(op.pin[0], "1234");
    strcpy(op.pin[1], "0000");
    s_handle = pex_bt_classic_open(&op);
    fh_swap(o);
    fh_run(300);
    CHECK_EQ(s_cl_open, 1);
    CHECK(fh_cl_host_has_key("11:22:33:44:55:66"));
    o = fh_swap(m);
    CHECK_EQ(pex_bt_classic_send(s_handle, "hi?", 3), 3);
    fh_swap(o);
    fh_run(20);
    CHECK_EQ(s_peer_n, 3);
    CHECK_EQ(s_cl_rx_bytes, 5);
    CHECK(memcmp(s_rx, "yes\r\n", 5) == 0);
    fh_cl_peer_close(p);
    fh_run(10);
    CHECK_EQ(s_cl_failed, 2);
    CHECK_EQ(s_why, 9);
}

static int s_ble_up, s_ble_ready, s_ble_notes;
static void on_ble(void *a, const pex_bt_event_t *ev) {
    (void)a;
    if (ev->kind == PEX_BT_LINK_UP) { s_ble_up++; }
    if (ev->kind == PEX_BT_READY) { s_ble_ready++; }
    if (ev->kind == PEX_BT_NOTIFY) { s_ble_notes++; }
}
static size_t s_ble_written;
static void ble_wr(fh_ble_peer_t *p, int chr, const uint8_t *d, size_t n, int wr, void *ctx) {
    (void)p; (void)chr; (void)d; (void)wr; (void)ctx;
    s_ble_written += n;
}

static void t_ble(void) {
    fh_reset();
    fh_ble_peer_cfg_t c = { .addr = "AA:BB:CC:00:11:22", .name = "PL70e-BT", .mtu = 185,
                            .chrs = { { "ff00", "ff02", 0x0c }, { "ff00", "ff01", 0x10 } }, .nchr = 2,
                            .on_write = ble_wr };
    fh_ble_peer_t *p = fh_ble_peer_add(&c);
    pex_mod_t *m = fh_module("mini", mini_main);
    fh_load(m);
    pex_mod_t *o = fh_swap(m);
    uint8_t a[6];
    pex_bt_addr("AA:BB:CC:00:11:22", a);
    CHECK_EQ(pex_bt_connect(a), PEX_ENOENT);
    pex_bt_listen(NULL, on_ble, NULL);
    pex_bt_scan(500, 1);
    fh_swap(o);
    fh_run(100);
    o = fh_swap(m);
    pex_bt_dev_t d[4];
    CHECK_EQ(pex_bt_devices(d, 4, "PL70", NULL), 1);
    CHECK_EQ(pex_bt_connect(a), PEX_EPENDING);
    fh_swap(o);
    fh_run(100);
    CHECK_EQ(s_ble_up, 1);
    CHECK_EQ(s_ble_ready, 1);
    o = fh_swap(m);
    int w = pex_bt_find(a, "ff00", "ff02", 0);
    int n = pex_bt_find(a, "ff00", "ff01", 0);
    CHECK_EQ(w, 0);
    CHECK_EQ(n, 1);
    uint8_t buf[200] = { 0 };
    CHECK_EQ(pex_bt_write(a, w, buf, 183, 0, 1), PEX_EINVAL);
    for (int i = 0; i < 4; i++) { CHECK_EQ(pex_bt_write(a, w, buf, 182, 0, 1), PEX_OK); }
    CHECK_EQ(pex_bt_write(a, w, buf, 182, 0, 1), PEX_EBUSY);
    CHECK_EQ(pex_bt_subscribe(a, n, 1), PEX_EBUSY);
    fh_swap(o);
    fh_run(50);
    CHECK_EQ(s_ble_written, 4 * 182);
    o = fh_swap(m);
    CHECK_EQ(pex_bt_subscribe(a, n, 1), PEX_OK);
    fh_swap(o);
    fh_run(20);
    fh_ble_peer_notify(p, n, "\x01", 1);
    fh_run(2);
    CHECK_EQ(s_ble_notes, 1);
}

int main(void) {
    RUN(t_modules);
    RUN(t_files);
    RUN(t_classic);
    RUN(t_ble);
    T_DONE();
}
