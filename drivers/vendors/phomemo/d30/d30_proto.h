/* Phomemo D30 wire format: a sniffed preamble, ESC @, and one GS v 0 raster
 * block. docs/protocols/phomemo-d30.md. Pure functions for the host tests. */
#ifndef D30_PROTO_H
#define D30_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define D30_HEAD_DOTS 96
#define D30_ROW_BYTES 12
#define D30_DPI 203

/* The packets the vendor app sends once after connecting (as concatenated
 * bytes). Returns its length. */
size_t d30_preamble(const uint8_t **out);

/* Everything before the raster bytes of one label: 1f 11 24 00, ESC @,
 * GS v 0 0 wbytes(LE16) rows(LE16). Returns the length (always 14) or -1. */
int d30_job_head(uint32_t rows, uint8_t *out, size_t cap);

#endif
