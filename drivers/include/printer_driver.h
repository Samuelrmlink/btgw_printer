/* The contract between printer_hub and the printer driver modules.
 *
 * Each driver is its own PEX app. printer_hub owns the printer.* ops and
 * events (the op table is shared by every module and holds 16), keeps the
 * list of drivers, and streams print jobs to them. Calls between the modules
 * go through pex_call(), which takes four words and returns 64 bits, so each
 * side exports ONE dispatch function and the command picks what it does:
 *
 *   driver exports  int prn_drv(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2)
 *   hub exports     int prn_hub(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2)
 *
 * Pointers passed in a0..a2 point into the caller's RAM and stay valid only
 * for the call. Strings are NUL-terminated. JSON is UTF-8 without a BOM.
 * Every call returns >= 0 on success or a negative PEX_E* code.
 *
 * The hub is started first (cfg/launch.json order). A driver registers in
 * PEX_APP_RUN and unregisters in PEX_APP_STOP; the hub also drops a driver
 * whose module is unloaded. docs/architecture.md has the op and event shapes. */
#ifndef PRINTER_DRIVER_H
#define PRINTER_DRIVER_H

#include <stdint.h>
#include <stddef.h>

#define PRN_HUB_MODULE "printer_hub"
#define PRN_HUB_SYM "prn_hub"
#define PRN_DRV_SYM "prn_drv"
#define PRN_ABI 1

/* ---- driver -> hub (prn_hub) ---- */
enum {
    /* a0 = const char *driver module name, a1 = PRN_ABI. The hub finds the
     * module with pex_find(name) and its prn_drv with pex_sym. */
    PRN_HUB_REGISTER = 1,
    /* a0 = const char *driver module name */
    PRN_HUB_UNREGISTER = 2,
    /* a0 = const char *kind ("state" | "found" | "status" | "log"),
     * a1 = const char *json object. The hub adds "driver" and re-emits it as
     * printer.<kind>. */
    PRN_HUB_EMIT = 3,
    /* a0 = job id, a1 = PRN_JOB_* outcome, a2 = const char *message or 0.
     * The driver calls this when the printer has taken (or refused) the
     * whole job, after PRN_DRV_JOB_END. */
    PRN_HUB_JOB_DONE = 4,
    /* The driver has room again after returning PEX_EBUSY from JOB_FEED.
     * Optional: the hub also retries on its own tick. */
    PRN_HUB_KICK = 5,
};

/* ---- hub -> driver (prn_drv) ---- */
enum {
    /* a0 = char *out, a1 = cap. Writes the driver's caps JSON (below).
     * Returns the length. */
    PRN_DRV_INFO = 1,
    /* a0 = scan ms (0 = stop). Results come back as PRN_HUB_EMIT "found". */
    PRN_DRV_SCAN = 2,
    /* a0 = char *out, a1 = cap: JSON array of
     * {"driver","addr","name","rssi","model","transport"} the driver would
     * accept. */
    PRN_DRV_DEVICES = 3,
    /* a0 = const char *addr "AA:BB:CC:DD:EE:FF", a1 = const char *cfg JSON
     * (the printer config from printer/printers.json, may be "{}").
     * Returns PEX_EPENDING; progress is reported with "state" emits. */
    PRN_DRV_CONNECT = 4,
    /* a0 = const char *addr, or 0 for every link of this driver */
    PRN_DRV_DISCONNECT = 5,
    /* a0 = const char *addr or 0, a1 = char *out, a2 = cap: status JSON */
    PRN_DRV_STATUS = 6,
    /* a0 = const prn_job_t *job (copy it: it is valid for the call only).
     * Prepare to accept job->rows_total rows. Returns 1 if the driver prints
     * job->copies itself from one pass of the rows (TSPL "PRINT n"), 0 if
     * the hub must feed the rows once per copy (job->copy counts), or a
     * negative PEX_E* to refuse the job. */
    PRN_DRV_JOB_BEGIN = 7,
    /* a0 = const uint8_t *rows, a1 = byte count (always whole rows of
     * job->row_bytes). Returns the bytes taken (whole rows; may be fewer
     * than offered) or PEX_EBUSY. */
    PRN_DRV_JOB_FEED = 8,
    /* All rows were fed for this pass. a0 = copy index (0-based). After the
     * last pass the driver flushes and reports PRN_HUB_JOB_DONE. */
    PRN_DRV_JOB_END = 9,
    /* Abandon the current job. */
    PRN_DRV_CANCEL = 10,
};

enum {
    PRN_JOB_OK = 0,
    PRN_JOB_FAILED = 1,
    PRN_JOB_CANCELLED = 2,
};

enum { PRN_MEDIA_GAP = 0, PRN_MEDIA_CONTINUOUS = 1, PRN_MEDIA_BLACKMARK = 2 };

/* One print job as the hub hands it over. The web app has already rendered
 * and laid out the page: the bitmap is exactly what goes on the label, in the
 * model's head orientation, 1 bit per dot, MSB first, row 0 first. The bit
 * value for black is in the model caps ("black_bit"); the web app packs it
 * that way so drivers do not invert. */
typedef struct {
    uint32_t id;
    char addr[18];
    char model[24];          /* a model id from the driver's caps */
    uint16_t width_dots;     /* bitmap width */
    uint16_t row_bytes;      /* (width_dots + 7) / 8 */
    uint32_t rows_total;     /* bitmap height */
    /* placement of the bitmap on the label, in dots (calibration applied by
     * the web app; a driver that cannot place the bitmap ignores these) */
    int16_t origin_x, origin_y;
    /* media, in hundredths of a mm (50 mm = 5000); height 0 = continuous */
    uint32_t width_mm100, height_mm100, gap_mm100;
    uint8_t media_type;      /* PRN_MEDIA_* */
    /* settings, already clamped to the model's ranges by the hub */
    int8_t density;          /* -1 = model default */
    int8_t speed;            /* -1 = model default */
    uint8_t copies;          /* >= 1 */
    uint8_t copy;            /* 0-based copy being fed */
    uint8_t rotate;          /* 0,1,2,3 x 90 deg, for drivers whose printer rotates */
    uint8_t black_bit;       /* the bit value that burns a dot, from the model caps */
    uint8_t all_black;       /* the hub found no white dot and punched some in (head protection) */
    char options[96];        /* driver-specific JSON object from the job, or "{}" */
} prn_job_t;

/* Caps JSON a driver returns from PRN_DRV_INFO:
 * {"driver":"nelko_pm220","abi":1,
 *  "models":[{"id":"pm220","vendor":"Nelko","name":"PM220","transport":"classic",
 *    "dpi":203,"head_dots":384,"min_len_mm":10,"max_len_mm":200,
 *    "media":["gap","continuous"],"density":{"min":0,"max":15,"default":8},
 *    "speed":null,"black_bit":0,"copies":true,
 *    "status":["battery","paper","lid","media"],"presets":["50x30mm","40x30mm"]}]}
 * "speed" is null when the printer takes no speed command. The hub caches
 * id, black_bit and the density/speed ranges of up to four models per driver
 * and clamps job settings to them. Preset ids are media profile ids from the
 * web app's presets (web/presets/media/). */

#endif
