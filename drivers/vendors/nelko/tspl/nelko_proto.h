/* What the Nelko TSPL printers (PM220, P21) say back over SPP, as pure
 * functions so the tests can feed byte vectors. Formats from nelko-pm220 (the
 * owner's Python reference, ~/Projects/nelko-pm220/src/nelko_pm220/protocol.py),
 * which was checked against a live PM220 (fw 1.39), and from
 * docs/protocols/nelko-p21.md for the P21:
 *   BATTERY?\r\n  -> "BATTERY " <BCD percent> <charging> "\r\n"
 *   CONFIG?\r\n   -> "CONFIG " <dpi u16 BE> <hw 3> <fw 3> [settings] "\r\n"
 *   ESC !?        -> one status byte. PM220: bits (1 lid open, 4 out of paper,
 *                    32 busy). P21: a code (nk_status_name)
 *   ESC !o\r\n    -> 16-byte status: [0] status byte, [7] paper type, [11] label
 *                    length mm, [13] label width mm (RFID), [14..15] CRC-16/MODBUS
 *                    of [0..13], big-endian */
#ifndef NELKO_PROTO_H
#define NELKO_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define NK_READY_LID 0x01
#define NK_READY_PAPER 0x04
#define NK_READY_BUSY 0x20

typedef struct { int percent, charging; } nk_battery_t;
typedef struct { int dpi; char hw[12], fw[12]; int settings; } nk_config_t;
typedef struct { int readiness, paper, width_mm, length_mm, crc_ok; } nk_status_t;

enum { NK_NONE = 0, NK_BATTERY, NK_CONFIG, NK_READY, NK_STATUS };

typedef struct {
    int kind;                /* NK_* */
    union { nk_battery_t battery; nk_config_t config; int ready; nk_status_t status; } u;
} nk_reply_t;

uint16_t nk_crc16_modbus(const uint8_t *p, size_t n);
int nk_parse_battery(const uint8_t *p, size_t n, nk_battery_t *out);   /* 1 ok, 0 not one */
int nk_parse_config(const uint8_t *p, size_t n, nk_config_t *out);
int nk_parse_status(const uint8_t *p, size_t n, nk_status_t *out);

/* An RX accumulator. nk_rx_take() looks for one whole reply at the front:
 * expect says what was asked last (NK_NONE: anything recognisable); idle is
 * 1 when no byte came for a while, which closes a reply that has no
 * terminator. Returns the bytes used (0: need more). Unrecognised bytes are
 * dropped once idle. */
typedef struct { uint8_t b[96]; uint16_t n; } nk_rx_t;
void nk_rx_push(nk_rx_t *rx, const uint8_t *p, size_t n);
size_t nk_rx_take(nk_rx_t *rx, int expect, int idle, nk_reply_t *out);
const char *nk_paper_name(int paper);
/* The P21's status byte as a code: "ready", "lid_open", "no_paper", ... */
const char *nk_status_name(int code);
/* 1 if the code means the printer can't print now (not ready, feeding or busy). */
int nk_status_error(int code);

#endif
