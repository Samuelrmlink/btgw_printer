/* The PM220 driver end to end on the fake host: printer_hub + nelko_pm220 +
 * a simulated PM220 on the Classic radio (legacy PIN, SPP on channel 1, the
 * status replies the real one gives, a TSPL job parser).
 *
 * The golden check prints through the whole new path (reference layout ->
 * job file -> hub -> driver -> RFCOMM) and compares what reached the
 * printer with the old firmware's job_from_bitmap() for the same picture. */
#include "t.h"
#include "drv.h"
#include "fh_classic.h"
#include "core/prn_core.h"
#include "ref/layout.h"
#include "ref/old_fs_stub.h"
#include "vendors/nelko/pm220/pm220_proto.h"

static char R[3000];
#define ADDR "11:22:33:44:55:66"

/* ---- the simulated printer ---- */
typedef struct {
    fh_cl_peer_t *p;
    uint8_t *in;
    size_t n, cap, pos;
    uint8_t ready;
    int queries[5];          /* BATTERY? CONFIG? ESC!? ESC!o-alone */
    size_t job_start[8], job_end[8];
    int jobs;
    int printed;             /* labels: sum of PRINT n */
    int in_job;
    long bitmap_left;
    int mute;                /* answer nothing */
} sim_t;
static sim_t S;

static const uint8_t STATUS16[16] = { 0x00, 0x0c, 0x01, 0x22, 0x03, 0x00, 0x03, 0x01,
                                      0x06, 0x12, 0x15, 0x1e, 0x0f, 0x32, 0xe6, 0x1f };

static void reply(const void *d, size_t n) { if (!S.mute) { fh_cl_peer_send(S.p, d, n); } }

static int at(const char *s) {
    size_t k = strlen(s);
    return S.n - S.pos >= k && memcmp(S.in + S.pos, s, k) == 0;
}
static size_t line_end(void) {
    for (size_t i = S.pos; i + 1 < S.n; i++) {
        if (S.in[i] == '\r' && S.in[i + 1] == '\n') { return i + 2; }
    }
    return 0;
}

static void sim_parse(void) {
    for (;;) {
        if (S.bitmap_left > 0) {
            size_t k = S.n - S.pos;
            if ((long)k > S.bitmap_left) { k = (size_t)S.bitmap_left; }
            S.pos += k;
            S.bitmap_left -= (long)k;
            if (S.bitmap_left) { return; }
            continue;
        }
        if (S.pos >= S.n) { return; }
        if (at("BATTERY?\r\n")) { S.queries[0]++; reply("BATTERY \x75\x00\r\n", 11); S.pos += 10; continue; }
        if (at("CONFIG?\r\n")) {
            S.queries[1]++;
            reply("CONFIG \x00\xcb\x00\x00\x03\x01\x03\x09\x02\x01\r\n", 19);
            S.pos += 9;
            continue;
        }
        if (at("\x1b!?\r\n")) { S.queries[2]++; reply(&S.ready, 1); S.pos += 5; continue; }
        if (at("\x1b!o\r\n")) {
            uint8_t st[16];
            memcpy(st, STATUS16, 16);
            st[0] = S.ready;
            uint16_t c = pm_crc16_modbus(st, 14);
            st[14] = (uint8_t)(c >> 8);
            st[15] = (uint8_t)c;
            reply(st, 16);
            if (S.n - S.pos < 10) { return; }        /* wait: is a job behind it? */
            if (memcmp(S.in + S.pos + 5, "SIZE ", 5) == 0) {
                S.in_job = 1;
                if (S.jobs < 8) { S.job_start[S.jobs] = S.pos; }
            } else {
                S.queries[3]++;
            }
            S.pos += 5;
            continue;
        }
        if (at("\r\nPRINT ")) {
            size_t e = 0;
            for (size_t i = S.pos + 2; i + 1 < S.n; i++) {
                if (S.in[i] == '\r' && S.in[i + 1] == '\n') { e = i + 2; break; }
            }
            if (!e) { return; }
            S.printed += atoi((const char *)S.in + S.pos + 8);
            if (S.in_job && S.jobs < 8) { S.job_end[S.jobs++] = e; }
            S.in_job = 0;
            S.pos = e;
            continue;
        }
        if (at("BITMAP ")) {
            size_t e = S.pos;
            int commas = 0;
            while (e < S.n && commas < 5) { commas += S.in[e++] == ','; }
            if (commas < 5) { return; }
            int x, y, wb, h, mode;
            char t[64];
            size_t k = e - S.pos < sizeof t - 1 ? e - S.pos : sizeof t - 1;
            memcpy(t, S.in + S.pos, k);
            t[k] = 0;
            if (sscanf(t, "BITMAP %d,%d,%d,%d,%d,", &x, &y, &wb, &h, &mode) != 5) { S.pos++; continue; }
            S.pos = e;
            S.bitmap_left = (long)wb * h;
            continue;
        }
        size_t e = line_end();
        if (!e) { return; }
        S.pos = e;
    }
}

static void sim_rx(fh_cl_peer_t *p, const uint8_t *d, size_t n, void *ctx) {
    (void)p; (void)ctx;
    if (S.n + n > S.cap) {
        S.cap = (S.n + n) * 2;
        S.in = realloc(S.in, S.cap);
    }
    memcpy(S.in + S.n, d, n);
    S.n += n;
    sim_parse();
}

static fh_cl_peer_t *add_pm220(const char *pin, uint32_t rate) {
    free(S.in);
    memset(&S, 0, sizeof S);
    fh_cl_peer_cfg_t c = { .addr = ADDR, .name = "PM220", .cod = 0x000680, .rssi = -55, .sdp_channel = 1,
                           .pin = pin, .mtu = 990, .bytes_per_ms = rate, .on_rx = sim_rx };
    S.p = fh_cl_peer_add(&c);
    return S.p;
}

static void setup(const char *pin, uint32_t rate) {
    fh_reset();
    add_pm220(pin, rate);
    load_hub();
    load_pm220();
    fh_run(20);
}

static int connect_pm220(const char *cfg) {
    char p[300];
    snprintf(p, sizeof p, "{\"driver\":\"nelko_pm220\",\"addr\":\"" ADDR "\",\"config\":%s}", cfg ? cfg : "{}");
    int from = fh_events("printer.state");
    CHECK(fh_op("printer.connect", p, R, sizeof R) > 0);
    return wait_ev_after(from, "printer.state", "\"connected\"", NULL, 3000);
}

/* ---- tests ---- */
static void t_scan_filters(void) {
    fh_reset();
    add_pm220("0000", 0);
    fh_cl_peer_cfg_t other = { .addr = "22:22:22:22:22:22", .name = "P21-ABCD", .cod = 0x000680, .sdp_channel = 1 };
    fh_cl_peer_cfg_t headset = { .addr = "33:33:33:33:33:33", .name = "Headset", .cod = 0x240404, .sdp_channel = 1 };
    fh_cl_peer_cfg_t quiet = { .addr = "44:44:44:44:44:44", .name = "PM220-9F", .cod = 0x000680, .name_by_rnr = 1,
                               .sdp_channel = 1 };
    fh_cl_peer_add(&other);
    fh_cl_peer_add(&headset);
    fh_cl_peer_add(&quiet);
    load_hub();
    load_pm220();
    fh_run(10);
    CHECK(fh_op("printer.scan", "{\"ms\":2000}", R, sizeof R) > 0);
    CHECK_STR(R, "{\"started\":[\"nelko_pm220\"]}");
    CHECK(wait_ev("printer.found", ADDR, NULL, 1000));
    fh_run(2200);
    CHECK(fh_op("printer.devices", "{}", R, sizeof R) > 0);
    CHECK_HAS(R, "\"addr\":\"" ADDR "\",\"name\":\"PM220\"");
    CHECK_HAS(R, "\"name\":\"PM220-9F\"");
    CHECK(!strstr(R, "P21-ABCD"));
    CHECK(!strstr(R, "Headset"));
    CHECK_EQ(fh_cl_inquiries(), 1);
    CHECK_HAS(fh_event_last("printer.state"), "\"idle\"");
}

static void t_pin_fallback_in_one_open(void) {
    setup("1234", 0);
    CHECK(connect_pm220(NULL));
    const fh_cl_stats_t *st = fh_cl_peer_stats(S.p);
    CHECK_EQ(st->pin_requests, 2);
    CHECK_STR(st->pins_tried[0], "0000");
    CHECK_STR(st->pins_tried[1], "1234");
    CHECK_HAS(fh_event_last("printer.state"), "\"pin\":\"1234\"");
    CHECK_HAS(fh_event_last("printer.state"), "\"channel\":1");
    fh_run(6000);
    CHECK_EQ(S.queries[0], 1);
    CHECK_EQ(S.queries[1], 1);
    CHECK_EQ(S.queries[2], 1);
    CHECK(fh_op("printer.status", "{}", R, sizeof R) > 0);
    CHECK_HAS(R, "\"battery\":75");
    CHECK_HAS(R, "\"charging\":false");
    CHECK_HAS(R, "\"dpi\":203");
    CHECK_HAS(R, "\"fw\":\"1.3.9\"");
    CHECK_HAS(R, "\"paper\":true");
    CHECK_HAS(R, "\"lid\":false");
}

static void t_remembered_pin_first(void) {
    setup("1234", 0);
    CHECK(connect_pm220("{\"pin\":\"1234\",\"channel\":1}"));
    CHECK_EQ(fh_cl_peer_stats(S.p)->pin_requests, 1);
    CHECK_STR(fh_cl_peer_stats(S.p)->pins_tried[0], "1234");
}

static void t_stale_key(void) {
    setup("0000", 0);
    fh_cl_host_set_key(ADDR, 1);
    fh_cl_peer_forget(S.p);          /* the printer was reset: it forgot us */
    CHECK(connect_pm220(NULL));
    CHECK_EQ(fh_cl_peer_stats(S.p)->pages, 2);
    CHECK_EQ(fh_cl_peer_stats(S.p)->pin_requests, 1);
    CHECK(fh_cl_host_has_key(ADDR));
    /* a later connect uses the new key: no PIN */
    fh_op("printer.disconnect", "{}", R, sizeof R);
    fh_run(50);
    CHECK(connect_pm220(NULL));
    CHECK_EQ(fh_cl_peer_stats(S.p)->pin_requests, 1);
}

static void t_wrong_pins_give_up(void) {
    setup("9999", 0);
    CHECK(!connect_pm220(NULL));
    CHECK(wait_ev("printer.state", "pairing failed", NULL, 200));
    /* three rounds (each tries both PINs), then it stops */
    fh_run(200000);
    CHECK_EQ(fh_cl_peer_stats(S.p)->pin_requests, 6);
    CHECK_HAS(fh_event_last("printer.state"), "\"idle\"");
}

static void t_reconnect_backoff(void) {
    setup("0000", 0);
    CHECK(connect_pm220(NULL));
    fh_cl_peer_absent(S.p, 1);
    fh_cl_peer_close(S.p);
    CHECK(wait_ev("printer.state", "\"waiting\"", "link lost", 100));
    CHECK_HAS(fh_event_last("printer.state"), "\"retry_ms\":8000");
    int pages = fh_cl_peer_stats(S.p)->pages;
    fh_run(7900);
    CHECK_EQ(fh_cl_peer_stats(S.p)->pages, pages);
    CHECK(wait_ev("printer.state", "not reachable", NULL, 200));
    /* the next wait is 16 s, then 32 s */
    fh_run(15000);
    CHECK(fh_log_has("retry in 16000 ms"));
    fh_run(2000);
    CHECK(fh_log_has("retry in 32000 ms"));
    int st = fh_events("printer.state");
    fh_cl_peer_absent(S.p, 0);
    CHECK(wait_ev_after(st, "printer.state", "\"connected\"", NULL, 40000));
}

/* the golden print: the whole new path against the old firmware */
static uint8_t s_src[48 * 240], s_old[16384], s_crop[48 * 240];
static void make_source(void) {
    uint32_t seed = 7;
    memset(s_src, 0xFF, sizeof s_src);
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 384; x++) {
            seed = seed * 1103515245u + 12345u;
            int black = (x > 20 && x < 360 && y > 20 && y < 220 && ((seed >> 16) & 3) == 0) || x == y;
            if (black) { s_src[y * 48 + x / 8] &= (uint8_t)~(1u << (7 - x % 8)); }
        }
    }
}

static int print_layout(const ref_cal_t *cal, const char *gap, int copies, ref_layout_t *L) {
    ref_layout(cal, 384, 240, L);
    size_t n = ref_crop(L, s_src, 48, 384, 240, s_crop, sizeof s_crop);
    /* the hub punches an all-black bitmap itself: send it unpunched */
    fh_fs_put("lfs:/printer/var/job.bin", s_crop, n);
    char p[500];
    snprintf(p, sizeof p, "{\"driver\":\"nelko_pm220\",\"addr\":\"" ADDR "\",\"model\":\"pm220\","
                          "\"path\":\"printer/var/job.bin\",\"width_dots\":%d,\"rows\":%d,\"origin\":[%d,%d],"
                          "\"media\":{\"w_mm\":50,\"h_mm\":30,\"gap_mm\":%s,\"type\":\"gap\"},\"copies\":%d}",
             L->print_width_dots, L->print_height_dots, L->origin_x, L->origin_y, gap, copies);
    return fh_op("printer.print", p, R, sizeof R);
}

static void t_golden_print(void) {
    static const struct { const char *json; ref_cal_t cal; const char *gap; } C[] = {
        { NULL, { 203, 5000, 3000, -125, 300, 0, 0, 0 }, "2" },
        { "{\"offset_x_mm\":-3.5,\"offset_y_mm\":-2.25,\"gap_mm\":3,\"origin_x_adj\":-20,\"origin_y_adj\":-30}",
          { 203, 5000, 3000, -350, -225, -20, -30, 0 }, "3" },
        { "{\"offset_x_mm\":1.1,\"offset_y_mm\":0.3,\"gap_mm\":1.75,\"print_height_adj\":-12}",
          { 203, 5000, 3000, 110, 30, 0, 0, -12 }, "1.75" },
    };
    make_source();
    setup("0000", 0);
    CHECK(connect_pm220(NULL));
    fh_run(3000);
    for (int c = 0; c < 3; c++) {
        old_cal = C[c].json;
        size_t on = 0;
        CHECK(old_job_from_bitmap(s_src, 48, 240, s_old, sizeof s_old, &on, NULL));
        ref_layout_t L;
        int from = fh_events("printer.job");
        CHECK(print_layout(&C[c].cal, C[c].gap, 1, &L) > 0);
        CHECK(wait_ev_after(from, "printer.job", "\"done\"", NULL, 5000));
        CHECK_EQ(S.jobs, c + 1);
        size_t a = S.job_start[c], b = S.job_end[c];
        CHECK_EQ(b - a, on);
        CHECK(b - a == on && memcmp(S.in + a, s_old, on) == 0);
    }
    /* each job was preceded by the readiness check */
    CHECK_EQ(S.queries[2], 1 + 3);
    CHECK_EQ(S.printed, 3);
    CHECK(fh_op("printer.status", "{}", R, sizeof R) > 0);
    CHECK_HAS(R, "\"media\":{\"type\":\"gap\",\"w_mm\":50,\"h_mm\":30,\"crc_ok\":true}");
}

static void t_copies_and_settings(void) {
    make_source();
    setup("0000", 0);
    CHECK(connect_pm220(NULL));
    fh_run(3000);
    ref_cal_t cal = { 203, 5000, 3000, -125, 300, 0, 0, 0 };
    ref_layout_t L;
    ref_layout(&cal, 384, 240, &L);
    size_t n = ref_crop(&L, s_src, 48, 384, 240, s_crop, sizeof s_crop);
    fh_fs_put("lfs:/printer/var/job.bin", s_crop, n);
    char p[500];
    snprintf(p, sizeof p, "{\"driver\":\"nelko_pm220\",\"model\":\"pm220\",\"path\":\"printer/var/job.bin\","
                          "\"width_dots\":%d,\"rows\":%d,\"origin\":[%d,%d],\"media\":{\"w_mm\":40,\"h_mm\":30,"
                          "\"gap_mm\":2.5,\"type\":\"blackmark\"},\"density\":99,\"speed\":3,\"copies\":3,\"rotate\":2}",
             L.print_width_dots, L.print_height_dots, L.origin_x, L.origin_y);
    CHECK(fh_op("printer.print", p, R, sizeof R) > 0);
    CHECK(wait_ev("printer.job", "\"done\"", NULL, 5000));
    CHECK_EQ(S.printed, 3);
    CHECK_EQ(S.jobs, 1);
    char head[200];
    size_t k = S.job_end[0] - S.job_start[0];
    memcpy(head, S.in + S.job_start[0], 150);
    head[150] = 0;
    CHECK_HAS(head, "SIZE 40 mm,30 mm\r\nBLINE 2.5 mm,0 mm\r\nDIRECTION 1,0\r\nDENSITY 15\r\nCLS\r\n");
    CHECK(!strstr(head, "SPEED"));
    CHECK(k == 5 + n + 120 || k > n);   /* one pass of the rows for all three copies */
    CHECK(k < 2 * n);
}

static void t_stream_under_backpressure(void) {
    static const int ROWS[] = { 341, 2720 };      /* 16 KB and 130 KB of rows */
    for (int t = 0; t < 2; t++) {
        setup("0000", 40);                        /* the link drains 40 bytes per ms */
        CHECK(connect_pm220(NULL));
        fh_run(3000);
        size_t n = 48u * (size_t)ROWS[t];
        uint8_t *rows = malloc(n);
        for (size_t i = 0; i < n; i++) { rows[i] = (uint8_t)(i * 131u + (i >> 9)); }
        fh_fs_put("lfs:/printer/var/job.bin", rows, n);
        char p[400];
        snprintf(p, sizeof p, "{\"driver\":\"nelko_pm220\",\"path\":\"printer/var/job.bin\",\"width_dots\":384,"
                              "\"rows\":%d,\"media\":{\"w_mm\":48,\"type\":\"continuous\"},\"options\":{\"preflight\":false}}",
                 ROWS[t]);
        CHECK(fh_op("printer.print", p, R, sizeof R) > 0);
        /* stall the link for a while in the middle */
        fh_run(150);
        fh_cl_peer_stall(S.p, 1);
        fh_run(400);
        fh_cl_peer_stall(S.p, 0);
        CHECK(wait_ev("printer.job", "\"done\"", NULL, 20000));
        CHECK_EQ(S.jobs, 1);
        /* the bytes between the BITMAP header and the tail are the file */
        size_t a = S.job_start[0], b = S.job_end[0];
        const uint8_t *body = NULL;
        for (size_t i = a; i + 7 < b; i++) {
            if (memcmp(S.in + i, "BITMAP ", 7) == 0) {
                int commas = 0;
                size_t e = i;
                while (commas < 5) { commas += S.in[e++] == ','; }
                body = S.in + e;
                break;
            }
        }
        CHECK(body != NULL);
        CHECK(body && memcmp(body, rows, n) == 0);
        CHECK(body && (size_t)(S.in + b - body) == n + 11);   /* "\r\nPRINT 1\r\n" */
        CHECK(strstr(fh_event_last("printer.job"), "\"sent\":") != NULL);
        free(rows);
    }
}

static void t_link_lost_mid_job(void) {
    setup("0000", 30);
    CHECK(connect_pm220(NULL));
    fh_run(3000);
    size_t n = 48u * 2000u;
    uint8_t *rows = calloc(1, n);
    memset(rows, 0x5A, n);
    fh_fs_put("lfs:/printer/var/job.bin", rows, n);
    CHECK(fh_op("printer.print", "{\"driver\":\"nelko_pm220\",\"path\":\"printer/var/job.bin\",\"width_dots\":384,"
                                 "\"rows\":2000,\"media\":{\"w_mm\":48,\"h_mm\":250}}", R, sizeof R) > 0);
    fh_run(300);
    fh_cl_peer_close(S.p);
    CHECK(wait_ev("printer.job", "\"failed\"", "link lost", 200));
    CHECK(wait_ev("printer.state", "\"waiting\"", NULL, 100));
    CHECK_EQ(fh_fs_open_count(), 0);
    /* it comes back by itself after 8 s and prints again */
    int st = fh_events("printer.state");
    CHECK(wait_ev_after(st, "printer.state", "\"connected\"", NULL, 9000));
    fh_run(2000);
    if (getenv("DBG")) { fh_verbose(1); }
    int from = fh_events("printer.job");
    CHECK(fh_op("printer.print", "{\"driver\":\"nelko_pm220\",\"path\":\"printer/var/job.bin\",\"width_dots\":384,"
                                 "\"rows\":2000,\"media\":{\"w_mm\":48,\"h_mm\":250}}", R, sizeof R) > 0);
    CHECK(wait_ev_after(from, "printer.job", "\"done\"", NULL, 20000));
    fh_verbose(0);
    free(rows);
}

static void t_cancel(void) {
    setup("0000", 20);
    CHECK(connect_pm220(NULL));
    fh_run(3000);
    size_t n = 48u * 2000u;
    uint8_t *rows = malloc(n);
    memset(rows, 0xA5, n);
    fh_fs_put("lfs:/printer/var/job.bin", rows, n);
    CHECK(fh_op("printer.print", "{\"driver\":\"nelko_pm220\",\"path\":\"printer/var/job.bin\",\"width_dots\":384,"
                                 "\"rows\":2000,\"media\":{\"w_mm\":48,\"h_mm\":250}}", R, sizeof R) > 0);
    fh_run(500);
    CHECK(fh_op("printer.cancel", "{}", R, sizeof R) > 0);
    CHECK_HAS(fh_event_last("printer.job"), "\"cancelled\"");
    fh_run(3000);
    CHECK_HAS(fh_event_last("printer.state"), "\"connected\"");
    free(rows);
}

static void t_preflight_refuses(void) {
    make_source();
    setup("0000", 0);
    CHECK(connect_pm220(NULL));
    fh_run(3000);
    ref_cal_t cal = { 203, 5000, 3000, -125, 300, 0, 0, 0 };
    ref_layout_t L;
    S.ready = PM_READY_LID;
    CHECK(print_layout(&cal, "2", 1, &L) > 0);
    CHECK(wait_ev("printer.job", "\"failed\"", "lid open", 3000));
    S.ready = PM_READY_PAPER;
    int from = fh_events("printer.job");
    CHECK(print_layout(&cal, "2", 1, &L) > 0);
    CHECK(wait_ev_after(from, "printer.job", "\"failed\"", "out of paper", 3000));
    CHECK_EQ(S.jobs, 0);
    /* busy for a moment, then ready: it waits and prints */
    S.ready = PM_READY_BUSY;
    from = fh_events("printer.job");
    CHECK(print_layout(&cal, "2", 1, &L) > 0);
    fh_run(1700);
    S.ready = 0;
    CHECK(wait_ev_after(from, "printer.job", "\"done\"", NULL, 12000));
    CHECK_EQ(S.jobs, 1);
    /* a printer that never answers ESC !? still prints after the wait */
    S.mute = 1;
    from = fh_events("printer.job");
    CHECK(print_layout(&cal, "2", 1, &L) > 0);
    CHECK(wait_ev_after(from, "printer.job", "\"done\"", NULL, 5000));
    CHECK_EQ(S.jobs, 2);
}

static void t_keepalive(void) {
    setup("0000", 0);
    CHECK(connect_pm220(NULL));
    fh_run(3000);
    int b = S.queries[0];
    fh_run(25000 * 3);
    CHECK_EQ(S.queries[0], b + 3);
}

static void t_without_classic(void) {
    fh_reset();
    fh_disable_group("classic");
    load_hub();
    load_pm220();
    fh_run(20);
    CHECK(fh_op("printer.drivers", "{}", R, sizeof R) > 0);
    CHECK_HAS(R, "\"available\":false");
    CHECK(fh_op("printer.connect", "{\"driver\":\"nelko_pm220\",\"addr\":\"" ADDR "\"}", R, sizeof R) < 0);
    CHECK(fh_op("printer.scan", "{}", R, sizeof R) > 0);
    CHECK_STR(R, "{\"started\":[]}");
}

static void t_unload_releases(void) {
    fh_reset();
    add_pm220("0000", 20);
    load_hub();
    pex_mod_t *m = load_pm220();
    fh_run(10);
    CHECK(connect_pm220(NULL));
    CHECK_EQ(fh_cl_sessions(), 1);
    size_t n = 48u * 2000u;
    uint8_t *rows = calloc(1, n);
    fh_fs_put("lfs:/printer/var/job.bin", rows, n);
    CHECK(fh_op("printer.print", "{\"driver\":\"nelko_pm220\",\"path\":\"printer/var/job.bin\",\"width_dots\":384,"
                                 "\"rows\":2000,\"media\":{\"w_mm\":48,\"h_mm\":250}}", R, sizeof R) > 0);
    fh_run(200);
    fh_unload(m);
    fh_run(50);
    CHECK_EQ(fh_cl_sessions(), 0);
    CHECK(!fh_cl_peer_stats(S.p)->connected);
    CHECK(wait_ev("printer.job", "\"failed\"", NULL, 100));
    free(rows);
}

static void t_proto_vectors(void) {
    pm_battery_t b;
    CHECK(pm_parse_battery((const uint8_t *)"BATTERY \x75\x00\r\n", 11, &b));
    CHECK_EQ(b.percent, 75);
    CHECK_EQ(b.charging, 0);
    CHECK(pm_parse_battery((const uint8_t *)"BATTERY \x99\x01\r\n", 11, &b));
    CHECK_EQ(b.percent, 99);
    CHECK_EQ(b.charging, 1);
    CHECK(pm_parse_battery((const uint8_t *)"BATTERY \xff\r\n", 10, &b));
    CHECK_EQ(b.percent, 255);
    CHECK(!pm_parse_battery((const uint8_t *)"CONFIG", 6, &b));
    pm_config_t c;
    CHECK(pm_parse_config((const uint8_t *)"CONFIG \x00\xcb\x00\x00\x03\x04\x02\x04\x02\x01\r\n", 19, &c));
    CHECK_EQ(c.dpi, 203);
    CHECK_STR(c.hw, "0.0.3");
    CHECK_STR(c.fw, "4.2.4");
    CHECK_EQ(c.settings, 2);
    static const uint8_t live[16] = { 0x00, 0x0c, 0x01, 0x22, 0x03, 0x00, 0x03, 0x01,
                                      0x06, 0x12, 0x15, 0x1e, 0x0f, 0x32, 0xe6, 0x1f };
    pm_status_t s;
    CHECK(pm_parse_status(live, 16, &s));
    CHECK_EQ(s.readiness, 0);
    CHECK_EQ(s.paper, 1);
    CHECK_EQ(s.width_mm, 50);
    CHECK_EQ(s.length_mm, 30);
    CHECK_EQ(s.crc_ok, 1);
    CHECK(!pm_parse_status(live, 13, &s));
    /* the accumulator: replies split across packets, back to back, and junk */
    pm_rx_t rx = { { 0 }, 0 };
    pm_reply_t r;
    pm_rx_push(&rx, (const uint8_t *)"BATT", 4);
    CHECK_EQ(pm_rx_take(&rx, PM_BATTERY, 0, &r), 0);
    pm_rx_push(&rx, (const uint8_t *)"ERY \x42\x01\r\nCONFIG \x00\xcb", 15);
    CHECK(pm_rx_take(&rx, PM_BATTERY, 0, &r) > 0);
    CHECK_EQ(r.kind, PM_BATTERY);
    CHECK_EQ(r.u.battery.percent, 42);
    CHECK_EQ(pm_rx_take(&rx, PM_CONFIG, 0, &r), 0);
    pm_rx_push(&rx, (const uint8_t *)"\x00\x00\x03\x04\x02\x04\x02\x01\r\n", 10);
    CHECK(pm_rx_take(&rx, PM_CONFIG, 0, &r) > 0);
    CHECK_EQ(r.kind, PM_CONFIG);
    pm_rx_push(&rx, live, 10);
    CHECK_EQ(pm_rx_take(&rx, PM_STATUS, 0, &r), 0);
    pm_rx_push(&rx, live + 10, 6);
    CHECK(pm_rx_take(&rx, PM_STATUS, 0, &r) > 0);
    CHECK_EQ(r.kind, PM_STATUS);
    CHECK_EQ(r.u.status.width_mm, 50);
    pm_rx_push(&rx, (const uint8_t *)"\x00", 1);
    CHECK(pm_rx_take(&rx, PM_READY, 0, &r) > 0);
    CHECK_EQ(r.kind, PM_READY);
    CHECK_EQ(r.u.ready, 0);
    pm_rx_push(&rx, (const uint8_t *)"zz?", 3);
    CHECK_EQ(pm_rx_take(&rx, PM_NONE, 0, &r), 0);
    CHECK(pm_rx_take(&rx, PM_NONE, 1, &r) > 0);
    CHECK_EQ(rx.n, 0);
    /* an unasked status still counts when its CRC is right */
    pm_rx_push(&rx, live, 16);
    CHECK(pm_rx_take(&rx, PM_NONE, 0, &r) > 0);
    CHECK_EQ(r.kind, PM_STATUS);
}

int main(void) {
    RUN(t_proto_vectors);
    RUN(t_scan_filters);
    RUN(t_pin_fallback_in_one_open);
    RUN(t_remembered_pin_first);
    RUN(t_stale_key);
    RUN(t_wrong_pins_give_up);
    RUN(t_reconnect_backoff);
    RUN(t_golden_print);
    RUN(t_copies_and_settings);
    RUN(t_stream_under_backpressure);
    RUN(t_link_lost_mid_job);
    RUN(t_cancel);
    RUN(t_preflight_refuses);
    RUN(t_keepalive);
    RUN(t_without_classic);
    RUN(t_unload_releases);
    T_DONE();
}
