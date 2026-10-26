/* Helpers every printer module links: talking to printer_hub, number
 * formatting without floats, bitmap checks. Stateless, so one copy can serve
 * several modules in the native tests. */
#ifndef PRN_CORE_H
#define PRN_CORE_H

#include <stddef.h>
#include <stdint.h>
#include "pex.h"
#include "printer_driver.h"

/* The hub as a driver sees it. */
typedef struct {
    pex_mod_t *m;
    uint32_t fn;
} prn_hub_t;

/* Find printer_hub and register self_name with it. 0, or PEX_ENOENT when
 * the hub is not loaded (try again later). */
int prn_hub_attach(prn_hub_t *h, const char *self_name);
void prn_hub_detach(prn_hub_t *h, const char *self_name);
int prn_hub_ok(prn_hub_t *h);    /* the hub is still loaded */
int prn_hub_emit(prn_hub_t *h, const char *kind, const char *json);
int prn_hub_job_done(prn_hub_t *h, uint32_t job, int outcome, const char *msg);
int prn_hub_kick(prn_hub_t *h);

/* Hundredths as the shortest decimal: 5000 -> "50", 225 -> "2.25",
 * 10160 -> "101.6". Same text as printf("%g") for these values. Returns the
 * length. */
int prn_fmt_mm100(char *out, size_t cap, int32_t v);
/* Tenths of a mm from mm100, rounded */
int32_t prn_mm100_to_dots(int32_t mm100, int dpi);

/* 1 if no byte of rows contains a non-burning dot. */
int prn_rows_all_black(const uint8_t *rows, size_t n, int black_bit);
/* The old firmware's head protection: in an all-black bitmap, clear the
 * first dot of every 16th row. row0 is the index of rows[0] in the bitmap. */
void prn_punch_rows(uint8_t *rows, uint32_t row0, uint32_t nrows, uint16_t row_bytes, int black_bit);

/* "AA:BB:.." <-> bytes */
int prn_addr_parse(const char *s, uint8_t *a);
void prn_addr_fmt(const uint8_t *a, char *out);   /* 18 bytes */

/* 8 s, 16 s, 32 s, 60 s, 60 s ... for attempt 0, 1, 2 ... */
uint32_t prn_backoff_ms(uint32_t attempt, uint32_t first_ms, uint32_t max_ms);

/* Copy at most cap-1 bytes of a JSON value at path (raw text) into out.
 * Returns the length, or -1 if absent. */
int prn_json_raw(const char *json, const char *path, char *out, size_t cap);

#endif
