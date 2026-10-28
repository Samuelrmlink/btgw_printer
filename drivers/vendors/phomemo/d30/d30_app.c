/* Phomemo D30 driver (PEX app). Classic SPP, one GS v 0 raster per label,
 * 96-dot head, 1 = black. docs/protocols/phomemo-d30.md is the spec. The
 * printer's replies are not documented: whatever arrives is kept as last_rx
 * (hex) in the status for the bench. */
#include "pex.h"
#include "printer_driver.h"
#include "vendors/common/spp_printer.h"
#include "d30_proto.h"
#include <string.h>

static struct {
    uint8_t preamble_sent;
    uint32_t rx_total;
} D;

static void d30_on_open(void) {
    memset(&D, 0, sizeof D);
    const uint8_t *p;
    size_t n = d30_preamble(&p);
    if (spp_send(p, n) == PEX_OK) { D.preamble_sent = 1; }
}

static void d30_on_rx(const uint8_t *d, size_t n) {
    (void)d;
    D.rx_total += (uint32_t)n;
    spp_emit_status();
}

static int d30_head(const prn_job_t *j, uint8_t *out, size_t cap) {
    size_t k = 0;
    if (!D.preamble_sent) {
        const uint8_t *p;
        size_t n = d30_preamble(&p);
        if (n > cap) { return -1; }
        memcpy(out, p, n);
        k = n;
        D.preamble_sent = 1;
    }
    int h = d30_job_head(j->rows_total, out + k, cap - k);
    return h < 0 ? -1 : (int)k + h;
}

static void d30_fields(pex_jw_t *w) {
    pex_jw_int(w, "rx_bytes", D.rx_total);
}

static const char CAPS[] =
    "{\"driver\":\"phomemo_d30\",\"abi\":1,\"models\":[{\"id\":\"d30\",\"vendor\":\"Phomemo\",\"name\":\"D30\","
    "\"transport\":\"classic\",\"dpi\":203,\"head_dots\":96,\"min_len_mm\":15,\"max_len_mm\":100,"
    "\"media\":[\"gap\"],\"density\":{\"min\":0,\"max\":0,\"default\":0},"
    "\"speed\":null,\"black_bit\":1,\"copies\":false,\"placement\":false,\"orientation\":\"along_feed\","
    "\"status\":[],"
    "\"presets\":[\"phomemo-d30-12x40\",\"phomemo-d30-12x30\",\"phomemo-d30-14x40\",\"phomemo-d30-15x30\",\"phomemo-d30-12x22\"]}]}";

static PEX_PTR_TABLE spp_drv_t DRV = {
    .driver = "phomemo_d30",
    .model = "d30",
    .name_prefix = "D30",
    .channel = 0,
    .fallback = 1,
    .npin = 2,
    .pins = {"0000", "1234"},
    .head_dots = D30_HEAD_DOTS,
    .caps_json = CAPS,
    .keepalive_ms = 0,
    .on_open = d30_on_open,
    .on_rx = d30_on_rx,
    .job_head = d30_head,
    .status_fields = d30_fields,
};

#include "vendors/common/spp_printer.inc.c"
