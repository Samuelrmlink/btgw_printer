/* Nelko P21 driver (PEX app). Classic SPP, TSPL jobs with a 96-dot head.
 * docs/protocols/nelko-p21.md is the spec; p21_proto.c builds the bytes.
 *
 * Replies come in three shapes and none says what it answers, so the driver
 * remembers what it asked last: a 16-byte label block (CRC-checked), one
 * status byte, or a CRLF-terminated "BATTERY "/"CONFIG " line. */
#include "pex.h"
#include "printer_driver.h"
#include "vendors/common/spp_printer.h"
#include "p21_proto.h"
#include <string.h>

enum { Q_NONE = 0, Q_LABEL, Q_STATUS, Q_BATTERY, Q_CONFIG };

static struct {
    uint8_t rx[40];
    uint8_t rx_len;
    uint8_t asked;
    uint32_t asked_t;
    uint8_t have_label, have_battery, have_config, have_status;
    p21_label_t label;
    p21_battery_t battery;
    p21_config_t config;
    uint8_t status;
    uint8_t queue[4];
    uint8_t nqueue;
    uint8_t job_wait;
} P;

static void p21_on_rx(const uint8_t *d, size_t n);

static void ask_next(void) {
    if (P.asked != Q_NONE || P.nqueue == 0) { return; }
    uint8_t q = P.queue[0];
    memmove(P.queue, P.queue + 1, --P.nqueue);
    const char *s = q == Q_LABEL ? P21_Q_LABEL : q == Q_STATUS ? P21_Q_STATUS
                  : q == Q_BATTERY ? P21_Q_BATTERY : P21_Q_CONFIG;
    if (spp_send(s, strlen(s)) == PEX_OK) {
        P.asked = q;
        P.asked_t = pex_ms();
        P.rx_len = 0;
    } else {
        P.queue[P.nqueue++] = q;   /* try again on the next keepalive */
    }
}

static void ask(uint8_t q) {
    for (uint8_t i = 0; i < P.nqueue; i++) {
        if (P.queue[i] == q) { return; }
    }
    if (P.nqueue < sizeof P.queue) { P.queue[P.nqueue++] = q; }
    ask_next();
}

static void p21_on_open(void) {
    memset(&P, 0, sizeof P);
    ask(Q_CONFIG);
    ask(Q_BATTERY);
    ask(Q_LABEL);
}

static void p21_keepalive(void) {
    if (P.asked != Q_NONE && pex_ms() - P.asked_t > 3000) {
        P.asked = Q_NONE;   /* the reply never came */
        P.rx_len = 0;
    }
    if (P.nqueue) { ask_next(); return; }
    ask(Q_BATTERY);
    ask(Q_LABEL);
}

static void answered(void) {
    P.asked = Q_NONE;
    P.rx_len = 0;
    spp_emit_status();
    if (P.job_wait && P.have_status) {
        P.job_wait = 0;
        if (p21_status_error(P.status)) {
            spp_job_finished(PRN_JOB_FAILED, p21_status_name(P.status));
        } else {
            spp_job_finished(PRN_JOB_OK, NULL);
        }
    }
    ask_next();
}

static void p21_on_rx(const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (P.rx_len < sizeof P.rx) { P.rx[P.rx_len++] = d[i]; }
        switch (P.asked) {
        case Q_STATUS:
            P.status = P.rx[0];
            P.have_status = 1;
            answered();
            break;
        case Q_LABEL:
            if (P.rx_len == 16 && p21_parse_label(P.rx, 16, &P.label)) {
                P.have_label = 1;
                P.status = P.label.status;
                P.have_status = 1;
                answered();
            } else if (P.rx_len >= 16) {
                memmove(P.rx, P.rx + 1, --P.rx_len);   /* slide until a block checks */
            }
            break;
        case Q_BATTERY:
        case Q_CONFIG:
            if (P.rx_len >= 2 && P.rx[P.rx_len - 2] == '\r' && P.rx[P.rx_len - 1] == '\n') {
                if (P.asked == Q_BATTERY) {
                    P.have_battery = (uint8_t)p21_parse_battery(P.rx, P.rx_len, &P.battery) | P.have_battery;
                } else {
                    P.have_config = (uint8_t)p21_parse_config(P.rx, P.rx_len, &P.config) | P.have_config;
                }
                answered();
            }
            break;
        default:
            P.rx_len = 0;   /* unasked bytes: kept only in last_rx */
            break;
        }
    }
}

static int p21_head(const prn_job_t *j, uint8_t *out, size_t cap) {
    size_t q = strlen(P21_Q_LABEL);
    if (cap < q) { return -1; }
    /* the label block goes first, as the vendor app and nelko-p21-rs do;
     * its reply is parsed like any other */
    memcpy(out, P21_Q_LABEL, q);
    p21_job_t pj;
    /* the job carries hundredths of a mm; the TSPL builder takes tenths */
    pj.width_mm10 = (uint16_t)(j->width_mm100 ? (j->width_mm100 + 5) / 10 : 140);
    pj.height_mm10 = (uint16_t)(j->height_mm100 ? (j->height_mm100 + 5) / 10 : 400);
    pj.gap_mm10 = (uint16_t)(j->media_type == PRN_MEDIA_CONTINUOUS ? 0 : (j->gap_mm100 ? (j->gap_mm100 + 5) / 10 : 50));
    pj.rows = j->rows_total;
    pj.density = j->density < 0 ? 8 : j->density;
    int h = p21_job_head(&pj, (char *)out + q, cap - q);
    if (h < 0) { return -1; }
    P.asked = Q_LABEL;
    P.asked_t = pex_ms();
    P.rx_len = 0;
    return (int)q + h;
}

static int p21_tail(const prn_job_t *j, uint8_t *out, size_t cap) {
    (void)j;
    return p21_job_tail((char *)out, cap);
}

static int p21_sent(const prn_job_t *j) {
    (void)j;
    /* Ask for the status byte: busy/ready means the job was taken. */
    P.job_wait = 1;
    P.have_status = 0;
    P.asked = Q_NONE;
    P.nqueue = 0;
    ask(Q_STATUS);
    return 0;
}

static void p21_fields(pex_jw_t *w) {
    if (P.have_status) {
        pex_jw_str(w, "printer", p21_status_name(P.status));
        pex_jw_bool(w, "lid", P.status == 0x01);
        pex_jw_str(w, "paper", (P.status == 0x04 || P.status == 0x24) ? "out" : "ok");
    }
    if (P.have_battery) {
        pex_jw_int(w, "battery", P.battery.percent);
        pex_jw_bool(w, "charging", P.battery.charging);
    }
    if (P.have_label && P.label.tag_present) {
        pex_jw_obj(w, "media");
        pex_jw_num(w, "w_mm", P.label.width_mm);
        pex_jw_num(w, "h_mm", P.label.length_mm);
        pex_jw_num(w, "gap_mm", P.label.gap_mm);
        pex_jw_str(w, "type", p21_paper_name(P.label.paper_type));
        pex_jw_str(w, "color", p21_color_name(P.label.color));
        pex_jw_hex(w, "code", P.label.code, sizeof P.label.code);
        pex_jw_end(w);
    }
    if (P.have_config) {
        char fw[16];
        pex_snprintf(fw, sizeof fw, "%u.%u.%u", P.config.fw[0], P.config.fw[1], P.config.fw[2]);
        pex_jw_str(w, "firmware", fw);
        pex_jw_int(w, "dpi", P.config.dpi);
    }
}

static const char CAPS[] =
    "{\"driver\":\"nelko_p21\",\"abi\":1,\"models\":[{\"id\":\"p21\",\"vendor\":\"Nelko\",\"name\":\"P21\","
    "\"transport\":\"classic\",\"dpi\":203,\"head_dots\":96,\"min_len_mm\":15,\"max_len_mm\":100,"
    "\"media\":[\"gap\",\"continuous\"],\"density\":{\"min\":1,\"max\":15,\"default\":8},"
    "\"speed\":null,\"black_bit\":0,\"copies\":false,\"placement\":false,\"orientation\":\"along_feed\","
    "\"status\":[\"battery\",\"paper\",\"lid\",\"media\"],"
    "\"presets\":[\"nelko-p21-14x40\",\"nelko-p21-12x40\",\"nelko-p21-14x50\",\"nelko-p21-14x75\",\"nelko-p21-15x30\",\"nelko-p21-14x30\"]}]}";

static const spp_drv_t DRV = {
    .driver = "nelko_p21",
    .model = "p21",
    .name_prefix = "P21",
    .channel = 0,
    .fallback = 1,
    .npin = 2,
    .pins = {"0000", "1234"},
    .head_dots = P21_HEAD_DOTS,
    .caps_json = CAPS,
    .keepalive_ms = 25000,
    .on_open = p21_on_open,
    .on_rx = p21_on_rx,
    .on_keepalive = p21_keepalive,
    .job_head = p21_head,
    .job_tail = p21_tail,
    .job_sent = p21_sent,
    .status_fields = p21_fields,
};

#include "vendors/common/spp_printer.inc.c"
