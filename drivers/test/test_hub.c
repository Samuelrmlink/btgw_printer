/* printer_hub with the virtual driver: ops, registry, jobs, copies, the
 * all-black punch, card files, cancel, unload. */
#include "t.h"
#include "drv.h"
#include "fh_classic.h"
#include "core/prn_core.h"

static char R[3000];

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = ~0u;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) { c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    }
    return ~c;
}

static void put_bitmap(const char *path, int wb, int rows, int black) {
    size_t n = (size_t)wb * rows;
    uint8_t *b = malloc(n);
    for (size_t i = 0; i < n; i++) { b[i] = black ? 0x00 : (uint8_t)(i * 37u + 11u); }
    fh_fs_put(path, b, n);
    free(b);
}

static void setup(void) {
    fh_reset();
    load_hub();
    load_virtual();
    fh_run(20);
}

static void connect_virtual(void) {
    CHECK(fh_op("printer.connect", "{\"driver\":\"printer_virtual\",\"addr\":\"00:00:00:00:5E:01\"}", R, sizeof R) > 0);
    CHECK(wait_ev("printer.state", "\"connected\"", NULL, 200));
}

static void t_ops_and_registry(void) {
    setup();
    static const char *ops[] = { "printer.drivers", "printer.scan", "printer.devices", "printer.connect",
                                 "printer.disconnect", "printer.status", "printer.print", "printer.cancel" };
    for (int i = 0; i < 8; i++) { CHECK(fh_op_exists(ops[i])); }
    CHECK(fh_op("printer.drivers", "{}", R, sizeof R) > 0);
    CHECK_HAS(R, "{\"drivers\":[{\"driver\":\"printer_virtual\"");
    CHECK_HAS(R, "\"id\":\"virtual\"");
    CHECK(fh_op("printer.scan", "{\"ms\":1000}", R, sizeof R) > 0);
    CHECK_STR(R, "{\"started\":[\"printer_virtual\"]}");
    CHECK(fh_events("printer.found") == 1);
    CHECK(fh_op("printer.devices", "{}", R, sizeof R) > 0);
    CHECK_HAS(R, "{\"devices\":[{\"driver\":\"printer_virtual\",\"addr\":\"00:00:00:00:5E:01\"");
    CHECK(fh_op("printer.status", "{}", R, sizeof R) > 0);
    CHECK_HAS(R, "\"printers\":[{\"driver\":\"printer_virtual\"");
    CHECK_HAS(R, "\"job\":null");
    CHECK_EQ(fh_op("printer.connect", "{\"driver\":\"nope\",\"addr\":\"00:00:00:00:00:01\"}", R, sizeof R), PEX_ENOENT);
    CHECK_EQ(fh_op("printer.connect", "{\"addr\":\"00:00:00:00:00:01\"}", R, sizeof R), PEX_EINVAL);
}

static const char *JOB = "{\"driver\":\"printer_virtual\",\"addr\":\"00:00:00:00:5E:01\",\"model\":\"virtual\","
                         "\"path\":\"printer/var/job.bin\",\"width_dots\":832,\"rows\":1218,\"origin\":[0,0],"
                         "\"media\":{\"w_mm\":101.6,\"h_mm\":152.4,\"gap_mm\":3,\"type\":\"gap\"},"
                         "\"density\":30,\"copies\":%d,\"options\":{\"rate\":%d}}";

static void t_print_copies(void) {
    setup();
    connect_virtual();
    put_bitmap("lfs:/printer/var/job.bin", 104, 1218, 0);
    char p[600];
    snprintf(p, sizeof p, JOB, 2, 4096);
    CHECK(fh_op("printer.print", p, R, sizeof R) > 0);
    CHECK_STR(R, "{\"job\":1}");
    CHECK_EQ(fh_op("printer.print", p, R, sizeof R), PEX_EBUSY);
    CHECK(wait_ev("printer.job", "\"done\"", NULL, 5000));
    size_t n;
    const uint8_t *src = fh_fs_get("lfs:/printer/var/job.bin", &n);
    char want[16];
    snprintf(want, sizeof want, "%08x", crc32(src, n));
    /* the virtual printer reports each copy's CRC: both must be the file's */
    CHECK_EQ(fh_events("printer.status"), 2);
    CHECK_HAS(fh_event("printer.status", 0), want);
    CHECK_HAS(fh_event("printer.status", 1), want);
    CHECK_HAS(fh_event("printer.status", 1), "\"copy\":1");
    const uint8_t *got = fh_fs_get("lfs:/printer/var/virtual.bin", &n);
    CHECK(got && n == 104u * 1218u && memcmp(got, src, n) == 0);
    const char *last = fh_event_last("printer.job");
    CHECK_HAS(last, "\"sent\":253344");
    CHECK_HAS(last, "\"total\":253344");
    CHECK(fh_events("printer.job") >= 4);
    CHECK_HAS(fh_event("printer.job", 0), "\"queued\"");
    /* back-pressure ran: the driver takes 4096 B per 5 ms tick */
    CHECK(fh_now() > 1000 + 250);
}

static void t_all_black_is_punched(void) {
    setup();
    connect_virtual();
    put_bitmap("lfs:/printer/var/job.bin", 48, 240, 1);
    CHECK(fh_op("printer.print", "{\"driver\":\"printer_virtual\",\"model\":\"virtual\",\"path\":\"printer/var/job.bin\","
                                 "\"width_dots\":384,\"rows\":240,\"media\":{\"w_mm\":50,\"h_mm\":30,\"gap_mm\":2}}",
                R, sizeof R) > 0);
    CHECK(wait_ev("printer.job", "\"done\"", NULL, 3000));
    size_t n;
    const uint8_t *got = fh_fs_get("lfs:/printer/var/virtual.bin", &n);
    CHECK(got && n == 48u * 240u);
    if (!got) { return; }
    for (int y = 0; y < 240; y++) {
        CHECK_EQ(got[y * 48], (y % 16) ? 0x00 : 0x80);
    }
}

static void t_bad_jobs(void) {
    setup();
    connect_virtual();
    put_bitmap("lfs:/printer/var/job.bin", 48, 240, 0);
    CHECK_EQ(fh_op("printer.print", "{\"driver\":\"printer_virtual\",\"path\":\"printer/var/job.bin\",\"width_dots\":384,"
                                    "\"rows\":241,\"media\":{\"w_mm\":50,\"h_mm\":30}}", R, sizeof R), PEX_EINVAL);
    CHECK_HAS(R, "file size");
    CHECK_EQ(fh_op("printer.print", "{\"driver\":\"printer_virtual\",\"path\":\"printer/var/none.bin\",\"width_dots\":384,"
                                    "\"rows\":240,\"media\":{\"w_mm\":50,\"h_mm\":30}}", R, sizeof R), PEX_ENOENT);
    CHECK_EQ(fh_op("printer.print", "{\"driver\":\"printer_virtual\",\"model\":\"pm9\",\"path\":\"printer/var/job.bin\","
                                    "\"width_dots\":384,\"rows\":240,\"media\":{\"w_mm\":50}}", R, sizeof R), PEX_EINVAL);
    CHECK_HAS(R, "unknown model");
    CHECK_EQ(fh_op("printer.print", "{\"driver\":\"printer_virtual\",\"path\":\"printer/var/job.bin\",\"width_dots\":0,"
                                    "\"rows\":240,\"media\":{\"w_mm\":50}}", R, sizeof R), PEX_EINVAL);
    CHECK_EQ(fh_op("printer.print", "{\"driver\":\"printer_virtual\",\"path\":\"printer/var/job.bin\",\"width_dots\":384,"
                                    "\"rows\":240}", R, sizeof R), PEX_EINVAL);
    /* not connected: the driver refuses at JOB_BEGIN, the job fails */
    fh_op("printer.disconnect", "{}", R, sizeof R);
    CHECK(fh_op("printer.print", "{\"driver\":\"printer_virtual\",\"path\":\"printer/var/job.bin\",\"width_dots\":384,"
                                 "\"rows\":240,\"media\":{\"w_mm\":50,\"h_mm\":30}}", R, sizeof R) > 0);
    CHECK(wait_ev("printer.job", "\"failed\"", "not connected", 200));
}

static void t_card_file(void) {
    setup();
    connect_virtual();
    put_bitmap("sd:/printer/var/job.bin", 48, 400, 0);
    fh_fs_sd_latency(3);
    CHECK(fh_op("printer.print", "{\"driver\":\"printer_virtual\",\"path\":\"sd/printer/var/job.bin\",\"width_dots\":384,"
                                 "\"rows\":400,\"media\":{\"w_mm\":48,\"h_mm\":0,\"type\":\"continuous\"}}", R, sizeof R) > 0);
    CHECK(wait_ev("printer.job", "\"done\"", NULL, 5000));
    size_t n, m;
    const uint8_t *a = fh_fs_get("sd:/printer/var/job.bin", &n);
    const uint8_t *b = fh_fs_get("lfs:/printer/var/virtual.bin", &m);
    CHECK(a && b && n == m && memcmp(a, b, n) == 0);
    /* a card file that is not there */
    CHECK(fh_op("printer.print", "{\"driver\":\"printer_virtual\",\"path\":\"sd/printer/var/nope.bin\",\"width_dots\":384,"
                                 "\"rows\":400,\"media\":{\"w_mm\":48}}", R, sizeof R) > 0);
    CHECK(wait_ev("printer.job", "\"failed\"", "no such file", 200));
    CHECK_EQ(fh_fs_open_count(), 0);
}

static void t_cancel_and_failure(void) {
    setup();
    connect_virtual();
    put_bitmap("lfs:/printer/var/job.bin", 104, 1218, 0);
    char p[600];
    snprintf(p, sizeof p, JOB, 1, 512);
    CHECK(fh_op("printer.print", p, R, sizeof R) > 0);
    fh_run(100);
    CHECK(fh_op("printer.status", "{}", R, sizeof R) > 0);
    CHECK_HAS(R, "\"state\":\"sending\"");
    CHECK(fh_op("printer.cancel", "{}", R, sizeof R) > 0);
    CHECK_HAS(fh_event_last("printer.job"), "\"cancelled\"");
    CHECK_EQ(fh_fs_open_count(), 0);
    /* the driver fails mid-job */
    snprintf(p, sizeof p, JOB, 1, 4096);
    char q[700];
    snprintf(q, sizeof q, "%.*s,\"fail_at\":300}}", (int)(strlen(p) - 2), p);
    CHECK(fh_op("printer.print", q, R, sizeof R) > 0);
    CHECK(wait_ev("printer.job", "\"failed\"", "paper jam", 2000));
    /* and a third prints */
    CHECK(fh_op("printer.print", p, R, sizeof R) > 0);
    CHECK(wait_ev("printer.job", "\"done\"", NULL, 5000));
}

static void t_unload_driver_mid_job(void) {
    fh_reset();
    pex_mod_t *hub = load_hub();
    pex_mod_t *v = load_virtual();
    fh_run(20);
    connect_virtual();
    put_bitmap("lfs:/printer/var/job.bin", 104, 1218, 0);
    char p[600];
    snprintf(p, sizeof p, JOB, 1, 256);
    CHECK(fh_op("printer.print", p, R, sizeof R) > 0);
    fh_run(50);
    fh_unload(v);
    fh_run(20);
    CHECK(wait_ev("printer.job", "\"cancelled\"", "driver stopped", 100));
    CHECK(fh_op("printer.drivers", "{}", R, sizeof R) > 0);
    CHECK_STR(R, "{\"drivers\":[]}");
    CHECK_EQ(fh_fs_open_count(), 0);
    /* the hub goes and comes back: the driver registers again by itself */
    fh_unload(hub);
    CHECK(!fh_op_exists("printer.print"));
    v = load_virtual();
    hub = load_hub();
    fh_run(2000);
    CHECK(fh_op("printer.drivers", "{}", R, sizeof R) > 0);
    CHECK_HAS(R, "printer_virtual");
}

/* Saved printers come back at boot, except Bluetooth Classic ones: those
 * wait for the user (paging one that is off stalls the gateway's Wi-Fi,
 * parent-requirements B4). */
static void t_autoconnect(void) {
    fh_reset();
    fh_cl_peer_cfg_t pc = { .addr = "31:9D:68:1F:38:A4", .name = "PM220", .cod = 0x040680, .rssi = -50,
                            .sdp_channel = 1 };
    fh_cl_peer_t *pm = fh_cl_peer_add(&pc);
    const char *cfg = "{\"version\":1,\"printers\":[{\"addr\":\"00:00:00:00:5E:01\",\"driver\":\"printer_virtual\","
                      "\"model\":\"virtual\",\"auto_reconnect\":true},{\"addr\":\"11:11:11:11:11:11\","
                      "\"driver\":\"other\"},{\"addr\":\"31:9D:68:1F:38:A4\",\"driver\":\"nelko_tspl\","
                      "\"model\":\"pm220\",\"auto_reconnect\":true}]}";
    fh_fs_put("lfs:/printer/printers.json", cfg, strlen(cfg));
    load_hub();
    load_virtual();
    load_nelko();
    CHECK(wait_ev("printer.state", "\"connected\"", "00:00:00:00:5E:01", 200));
    fh_run(10000);
    CHECK(fh_log_has("auto-connect nelko_tspl 31:9D:68:1F:38:A4 skipped"));
    CHECK_EQ(fh_cl_peer_stats(pm)->pages, 0);
    for (int i = 0; i < fh_events("printer.state"); i++) {
        const char *e = fh_event("printer.state", i);
        CHECK(!(strstr(e, "31:9D:68:1F:38:A4") && strstr(e, "connecting")));
    }
}

static void t_helpers(void) {
    uint8_t a[6];
    char s[18];
    CHECK(prn_addr_parse("aa:bb:CC:01:02:03", a));
    prn_addr_fmt(a, s);
    CHECK_STR(s, "AA:BB:CC:01:02:03");
    CHECK(!prn_addr_parse("aa:bb:CC:01:02", a));
    CHECK_EQ(prn_backoff_ms(0, 8000, 60000), 8000);
    CHECK_EQ(prn_backoff_ms(1, 8000, 60000), 16000);
    CHECK_EQ(prn_backoff_ms(2, 8000, 60000), 32000);
    CHECK_EQ(prn_backoff_ms(3, 8000, 60000), 60000);
    CHECK_EQ(prn_backoff_ms(9, 8000, 60000), 60000);
    CHECK_EQ(prn_mm100_to_dots(5000, 203), 400);
    CHECK_EQ(prn_mm100_to_dots(-125, 203), -10);
}

int main(void) {
    RUN(t_ops_and_registry);
    RUN(t_print_copies);
    RUN(t_all_black_is_punched);
    RUN(t_bad_jobs);
    RUN(t_card_file);
    RUN(t_cancel_and_failure);
    RUN(t_unload_driver_mid_job);
    RUN(t_autoconnect);
    RUN(t_helpers);
    T_DONE();
}
