/* TSPL job text for 1-bit bitmaps, in pieces so a job can stream:
 *   tspl_head()  setup lines and "BITMAP x,y,wb,h,1,"
 *   rows         raw bitmap bytes, row 0 first
 *   tspl_tail()  "\r\nPRINT n\r\n"
 * With the PM220's settings the bytes are the same as the old firmware's
 * tspl_build_job() (pm220_rp2_webapp src/printer/tspl.c); test_tspl checks
 * that byte for byte. Everything that file fixed at compile time is a
 * field here. mm_tenths and mirror give the P21's text, byte for byte as
 * nelko-p21-rs sends it (test_p21). */
#ifndef TSPL_H
#define TSPL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *prefix;      /* sent first, e.g. "\x1b!o\r\n" (the PM220 status query); NULL: none */
    uint32_t width_mm100, height_mm100, gap_mm100;   /* SIZE and GAP */
    uint8_t media;           /* PRN_MEDIA_GAP / CONTINUOUS / BLACKMARK */
    uint16_t dpi;            /* for a continuous label's length (height_mm100 0) */
    int density;             /* DENSITY n; < 0: no line */
    int speed;               /* SPEED n; < 0: no line */
    uint8_t direction;       /* DIRECTION d,0 */
    int x, y;                /* BITMAP origin in dots */
    uint16_t row_bytes;
    uint32_t rows;
    int copies;              /* PRINT n */
    uint8_t mm_tenths;       /* 1: SIZE/GAP mm always with one decimal ("14.0"), as
                              * nelko-p21-rs sends them; 0: like %g ("50", "2.5") */
    uint8_t mirror;          /* DIRECTION d,<mirror> */
} tspl_job_t;

/* Lengths, or -1 if cap is too small. */
int tspl_head(char *out, size_t cap, const tspl_job_t *j);
int tspl_tail(char *out, size_t cap, const tspl_job_t *j);

#endif
