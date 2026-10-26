/* Shared body of a printer driver that talks over one Bluetooth Classic SPP
 * link: hub registration, inquiry, the session with its PIN list and
 * reconnect backoff, a small pending buffer that keeps job rows whole, and
 * the prn_drv dispatch. A driver fills in spp_drv_t and #includes
 * spp_printer.inc.c once, so every module gets its own copy of the state
 * (several modules link into one host-test binary).
 *
 * docs/architecture.md and drivers/include/printer_driver.h are the
 * contract this implements. */
#ifndef SPP_PRINTER_H
#define SPP_PRINTER_H

#include "pex.h"
#include "printer_driver.h"

typedef struct {
    const char *driver;          /* module name, e.g. "nelko_p21" */
    const char *model;           /* model id in the caps */
    const char *name_prefix;     /* inquiry filter, e.g. "P21" */
    uint32_t cod_mask, cod_value;/* inquiry filter on Class of Device (0 = any) */
    uint8_t channel;             /* 0: SDP lookup of 0x1101 */
    uint8_t fallback;            /* channel to try when SDP has nothing */
    uint8_t npin;
    const char *pins[PEX_CL_PIN_MAX];
    uint16_t head_dots;          /* the bitmap width a job must have */
    const char *caps_json;       /* PRN_DRV_INFO reply */
    uint32_t keepalive_ms;       /* 0: none */

    /* Called with the link open, before any job: send status queries. */
    void (*on_open)(void);
    /* Bytes from the printer, in arrival order (any split). */
    void (*on_rx)(const uint8_t *d, size_t n);
    /* Periodic, only while open and idle. */
    void (*on_keepalive)(void);
    /* Bytes that go before a copy's rows / after them. Return the length,
     * or < 0 to refuse the job. */
    int (*job_head)(const prn_job_t *j, uint8_t *out, size_t cap);
    int (*job_tail)(const prn_job_t *j, uint8_t *out, size_t cap);
    /* Called once the last copy's bytes left the host. Return 1 if the job
     * is done now, or 0 to wait for spp_job_finished() (e.g. after a status
     * reply). */
    int (*job_sent)(const prn_job_t *j);
    /* Extra status fields for PRN_DRV_STATUS (a JSON writer is open on the
     * printer's object). */
    void (*status_fields)(pex_jw_t *w);
} spp_drv_t;

/* For the driver's callbacks (static: one copy per module): */
static int spp_send(const void *data, size_t len);   /* queued whole, or PEX_EBUSY */
static void spp_job_finished(int outcome, const char *msg);  /* PRN_JOB_* */
static void spp_emit_status(void);                   /* push printer.status now */
__attribute__((unused)) static int spp_job_active(void);
static void spp_log_rx(const uint8_t *d, size_t n);  /* keep as last_rx (hex) */

#endif
