/* Nelko P21 wire format: a TSPL subset plus raw ESC status queries.
 * docs/protocols/nelko-p21.md. Pure functions, no host calls, so the host
 * tests build them natively. */
#ifndef P21_PROTO_H
#define P21_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define P21_HEAD_DOTS 96
#define P21_ROW_BYTES 12
#define P21_DPI 203

/* Raw queries. ESC sequences take no terminator; the text ones end in CRLF. */
#define P21_Q_LABEL "\x1b!o\r\n"   /* 16-byte label block */
#define P21_Q_STATUS "\x1b!?"      /* 1 status byte */
#define P21_Q_BATTERY "BATTERY?\r\n"
#define P21_Q_CONFIG "CONFIG?\r\n"

typedef struct {
    uint16_t width_mm10, height_mm10, gap_mm10; /* tenths of a mm */
    uint32_t rows;                              /* bitmap height in dots */
    int density;                                /* 1..15 */
} p21_job_t;

/* Writes everything before the bitmap bytes (SIZE .. "BITMAP x,y,12,rows,1,").
 * Returns the length, or -1 if cap is too small. */
int p21_job_head(const p21_job_t *j, char *out, size_t cap);
/* Writes what follows the bitmap: "\r\nPRINT 1\r\n". */
int p21_job_tail(char *out, size_t cap);

uint16_t p21_crc16(const uint8_t *d, size_t n);   /* CRC-16/MODBUS */

typedef struct {
    uint8_t status;        /* byte 0 */
    uint8_t color, paper_type, gap_mm, length_mm, usable_mm, width_mm;
    uint8_t tag_present;   /* bytes 2..13 not all zero */
    uint8_t code[12];
} p21_label_t;

/* 16 bytes with a valid big-endian CRC over 0..13 → 1, else 0. */
int p21_parse_label(const uint8_t *d, size_t n, p21_label_t *out);

typedef struct { uint8_t percent, charging; } p21_battery_t;
/* "BATTERY " + 2 bytes + CRLF → 1 */
int p21_parse_battery(const uint8_t *d, size_t n, p21_battery_t *out);

typedef struct {
    uint16_t dpi;
    uint8_t hw[3], fw[3], timer, beep;
} p21_config_t;
/* "CONFIG " + 10 bytes (or 20) + CRLF → 1 */
int p21_parse_config(const uint8_t *d, size_t n, p21_config_t *out);

/* Name for a status byte ("ready", "lid_open", ...). */
const char *p21_status_name(uint8_t s);
/* 1 if the status byte means the printer can't print now. */
int p21_status_error(uint8_t s);
const char *p21_paper_name(uint8_t t);
const char *p21_color_name(uint8_t c);

#endif
