/* nelko_tspl's inquiry filter on the fake host, with the real printer_hub.
 * A device is claimed only when a model's name rule matches. A device with
 * no name yet is held until the inquiry ends (plus a short wait for late
 * names) and reported with "model":null only if it is still unnamed; a
 * name that comes after that re-reports it (with its model, or "gone").
 *
 * The bench case that started this: a PL70e-BT (Classic DC:0D:30:89:6C:C5,
 * CoD 0x040680, imaging) shows up in the inquiry without a name, and its
 * name "PL70e-BT_6CC5" comes a moment later. The old PM220 driver emitted it
 * as a PM220 printer.found. */
#include "t.h"
#include "drv.h"
#include "fh_classic.h"

static char R[3000];
#define PL70E "DC:0D:30:89:6C:C5"
#define P21 "C0:01:02:03:04:21"
#define PM220 "11:22:33:44:55:66"
#define QUIET "44:44:44:44:44:44"
#define PHONE "55:55:55:55:55:55"

static void peer(const char *addr, const char *name, uint32_t cod, int rnr, uint32_t rnr_ms) {
    fh_cl_peer_cfg_t c = { .addr = addr, .name = name, .cod = cod, .rssi = -50, .name_by_rnr = rnr,
                           .rnr_ms = rnr_ms, .sdp_channel = 1 };
    fh_cl_peer_add(&c);
}

static void start(void) {
    load_hub();
    load_nelko();
    fh_run(20);
}

static int found(const char *addr) {
    int n = 0;
    for (int i = 0; i < fh_events("printer.found"); i++) { n += strstr(fh_event("printer.found", i), addr) != NULL; }
    return n;
}

static const char *found_ev(const char *addr) {
    for (int i = 0; i < fh_events("printer.found"); i++) {
        if (strstr(fh_event("printer.found", i), addr)) { return fh_event("printer.found", i); }
    }
    return "";
}

static const char *found_last(const char *addr) {
    const char *e = "";
    for (int i = 0; i < fh_events("printer.found"); i++) {
        if (strstr(fh_event("printer.found", i), addr)) { e = fh_event("printer.found", i); }
    }
    return e;
}

static void t_unnamed_then_named_not_claimed(void) {
    /* the name comes 300 ms after the inquiry result, while the inquiry runs */
    fh_reset();
    peer(PL70E, "PL70e-BT_6CC5", 0x040680, 1, 300);
    start();
    CHECK(fh_op("printer.scan", "{\"ms\":2000}", R, sizeof R) > 0);
    fh_run(1000);
    CHECK(fh_op("printer.devices", "{}", R, sizeof R) > 0);
    CHECK(!strstr(R, PL70E));
    fh_run(7500);
    CHECK_EQ(found(PL70E), 0);
    CHECK(fh_op("printer.devices", "{}", R, sizeof R) > 0);
    CHECK(!strstr(R, PL70E));
    CHECK_HAS(fh_event_last("printer.state"), "\"idle\"");

    /* the name comes after the inquiry has ended (within the wait for names) */
    fh_reset();
    peer(PL70E, "PL70e-BT_6CC5", 0x040680, 1, 5000);
    start();
    CHECK(fh_op("printer.scan", "{\"ms\":2000}", R, sizeof R) > 0);
    fh_run(2200);
    CHECK(fh_op("printer.devices", "{}", R, sizeof R) > 0);
    CHECK(!strstr(R, PL70E));      /* still unnamed, but the wait is not over */
    fh_run(7000);
    CHECK_EQ(found(PL70E), 0);
    CHECK(fh_op("printer.devices", "{}", R, sizeof R) > 0);
    CHECK(!strstr(R, PL70E));
}

static void t_names_pick_the_model(void) {
    fh_reset();
    peer(P21, "P21", 0x000680, 0, 0);
    peer(PM220, "PM220_1A2B", 0x000680, 0, 0);
    peer(QUIET, "nelko pm220", 0x000000, 1, 100);   /* any case, anywhere; CoD doesn't matter */
    peer(PHONE, "Pixel P21", 0x5a020c, 0, 0);        /* not a model of this driver */
    start();
    CHECK(fh_op("printer.scan", "{\"ms\":2000}", R, sizeof R) > 0);
    CHECK_STR(R, "{\"started\":[\"nelko_tspl\"]}");
    /* named matches are reported while the inquiry runs */
    CHECK(wait_ev("printer.found", PM220, NULL, 1000));
    CHECK(wait_ev("printer.found", QUIET, NULL, 500));
    CHECK_HAS(found_ev(PM220), "\"model\":\"pm220\"");
    CHECK_HAS(found_ev(QUIET), "\"model\":\"pm220\"");
    CHECK_HAS(found_ev(PM220), "\"driver\":\"nelko_tspl\"");
    fh_run(8000);
    /* once each; the P21 pages over Classic too but is nelko_ble's (BLE) */
    CHECK_EQ(found(P21), 0);
    CHECK_EQ(found(PM220), 1);
    CHECK_EQ(found(QUIET), 1);
    CHECK_EQ(found(PHONE), 0);
    CHECK(fh_op("printer.devices", "{}", R, sizeof R) > 0);
    CHECK(!strstr(R, P21));
    CHECK(!strstr(R, PHONE));
    CHECK_HAS(R, "\"addr\":\"" PM220 "\",\"name\":\"PM220_1A2B\",\"rssi\":-50,\"model\":\"pm220\"");
    /* a second scan reports them again, once */
    CHECK(fh_op("printer.scan", "{\"ms\":1000}", R, sizeof R) > 0);
    fh_run(7500);
    CHECK_EQ(found(PM220), 2);
    CHECK_EQ(found(P21), 0);

    /* connect with no model in the config: the inquiry's name picks it */
    int from = fh_events("printer.state");
    CHECK(fh_op("printer.connect", "{\"driver\":\"nelko_tspl\",\"addr\":\"" PM220 "\"}", R, sizeof R) > 0);
    CHECK(wait_ev_after(from, "printer.state", "\"connected\"", "\"model\":\"pm220\"", 3000));
    CHECK_EQ(fh_events("printer.log"), 0);
}

/* On the bench the radio asked for names one at a time and the PM220's came
 * after the wait: it was reported with "model":null and never corrected. */
static void t_late_name_re_reports(void) {
    fh_reset();
    peer(PM220, "PM220", 0x040680, 1, 11000);
    peer(PL70E, "PL70e-BT_6CC5", 0x040680, 1, 12000);
    start();
    CHECK(fh_op("printer.scan", "{\"ms\":2000}", R, sizeof R) > 0);
    fh_run(9000);
    CHECK_EQ(found(PM220), 1);
    CHECK_HAS(found_ev(PM220), "\"model\":null");
    CHECK_EQ(found(PL70E), 1);
    CHECK_HAS(fh_event_last("printer.state"), "\"idle\"");
    fh_run(5000);
    CHECK_EQ(found(PM220), 2);
    CHECK_EQ(found(PL70E), 2);
    CHECK_HAS(found_last(PM220), "\"model\":\"pm220\"");
    CHECK_HAS(found_last(PM220), "\"name\":\"PM220\"");
    CHECK_HAS(found_last(PL70E), "\"gone\":true");
    CHECK(fh_op("printer.devices", "{}", R, sizeof R) > 0);
    CHECK_HAS(R, "\"addr\":\"" PM220 "\",\"name\":\"PM220\"");
    CHECK(!strstr(R, PL70E));
    fh_run(25000);
    CHECK_EQ(found(PM220), 2);                       /* not again */
    CHECK_EQ(found(PL70E), 2);
}

static void t_unnamed_to_the_end(void) {
    fh_reset();
    peer(QUIET, NULL, 0x000680, 1, 0);              /* imaging, and never a name */
    peer(PHONE, NULL, 0x5a020c, 1, 0);               /* no name, not imaging: never */
    start();
    CHECK(fh_op("printer.scan", "{\"ms\":2000}", R, sizeof R) > 0);
    fh_run(2100);
    CHECK_EQ(found(QUIET), 0);                       /* held: its name may still come */
    CHECK(fh_op("printer.devices", "{}", R, sizeof R) > 0);
    CHECK(!strstr(R, QUIET));
    CHECK_HAS(fh_event_last("printer.state"), "\"scanning\"");
    fh_run(6500);
    CHECK_EQ(found(QUIET), 1);
    CHECK_HAS(found_ev(QUIET), "\"name\":\"\"");
    CHECK_HAS(found_ev(QUIET), "\"model\":null");
    CHECK_EQ(found(PHONE), 0);
    CHECK_HAS(fh_event_last("printer.state"), "\"idle\"");
    CHECK(fh_op("printer.devices", "{}", R, sizeof R) > 0);
    CHECK_HAS(R, "\"addr\":\"" QUIET "\",\"name\":\"\",\"rssi\":-50,\"model\":null");
    CHECK(!strstr(R, PHONE));
    fh_run(3000);
    CHECK_EQ(found(QUIET), 1);                       /* not again */

    /* the user picks the model in the connect config */
    int from = fh_events("printer.state");
    CHECK(fh_op("printer.connect", "{\"driver\":\"nelko_tspl\",\"addr\":\"" QUIET "\",\"config\":{\"model\":\"pm220\"}}",
                R, sizeof R) > 0);
    CHECK(wait_ev_after(from, "printer.state", "\"connected\"", "\"model\":\"pm220\"", 3000));
    CHECK_EQ(fh_events("printer.log"), 0);
    fh_op("printer.disconnect", "{}", R, sizeof R);
    fh_run(50);
    /* no model and no name: the first model, said in printer.log */
    from = fh_events("printer.state");
    CHECK(fh_op("printer.connect", "{\"driver\":\"nelko_tspl\",\"addr\":\"" QUIET "\"}", R, sizeof R) > 0);
    CHECK(wait_ev_after(from, "printer.state", "\"connected\"", "\"model\":\"pm220\"", 3000));
    CHECK_EQ(fh_events("printer.log"), 1);
    CHECK_HAS(fh_event_last("printer.log"), "using pm220");
    CHECK_HAS(fh_event_last("printer.log"), "\"addr\":\"" QUIET "\"");
}

int main(void) {
    RUN(t_unnamed_then_named_not_claimed);
    RUN(t_names_pick_the_model);
    RUN(t_unnamed_to_the_end);
    RUN(t_late_name_re_reports);
    T_DONE();
}
