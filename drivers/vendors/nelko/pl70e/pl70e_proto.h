/* Nelko PL70e-BT: the parts of the protocol that need no radio. Pure
 * functions, so the native tests and the mcusim printer model can check
 * them. docs/protocols/pl70e-bt.md is the spec. */
#ifndef PL70E_PROTO_H
#define PL70E_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define PL70E_DPI 203
#define PL70E_HEAD_DOTS 832          /* 104 mm at 8 dots/mm */
#define PL70E_NAME_PREFIX "PL70e"

/* The four GATT pipes reach the same TSPL parser; tried in this order. */
typedef struct { const char *svc, *write, *notify; } pl70e_pipe_t;
extern const pl70e_pipe_t pl70e_pipes[4];

enum {
    PL70E_ST_UNKNOWN = 0,
    PL70E_ST_READY,          /* READSTA PAPER / ESC !? 0x00 */
    PL70E_ST_NOPAPER,
    PL70E_ST_LIDOPEN,
    PL70E_ST_OVERHEAT,
    PL70E_ST_PRINTING,       /* ESC !? 0x20 (busy) or 0x02 (feeding) */
    PL70E_ST_PAUSED,         /* ESC !? 0x10 */
    PL70E_ST_ERROR,          /* ESC !? 0x03 paper error, 0x08 ribbon, 0x90 no power */
};
const char *pl70e_status_name(int st);

/* Line assembly for notify data: READSTA replies end in "\r\n" and may be
 * split across notifications. */
typedef struct {
    char line[64];
    uint8_t n;
} pl70e_rx_t;

/* Feed notify bytes. Returns a PL70E_ST_* each time a whole status reply has
 * been seen (the last one wins if several are in one call), or -1 when none
 * completed. A lone byte that isn't part of a text line is the ESC !? reply. */
int pl70e_rx_feed(pl70e_rx_t *rx, const uint8_t *data, size_t n);
int pl70e_status_from_line(const char *line);
int pl70e_status_from_byte(uint8_t b);

/* 1 if an advertised name belongs to this driver. */
int pl70e_name_match(const char *name);

/* Status query sent to the printer. */
#define PL70E_QUERY "READSTA \r\n"

#endif
