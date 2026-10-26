/* What the Nelko PM220 says back over SPP, as pure functions so the tests
 * can feed byte vectors. Formats from nelko-pm220 (the owner's Python
 * reference, ~/Projects/nelko-pm220/src/nelko_pm220/protocol.py), which was
 * checked against a live PM220 (fw 1.39):
 *   BATTERY?\r\n  -> "BATTERY " <BCD percent> <charging> "\r\n"
 *   CONFIG?\r\n   -> "CONFIG " <dpi u16 BE> <hw 3> <fw 3> [settings] "\r\n"
 *   ESC !?\r\n    -> one readiness byte (bits: 1 lid open, 4 out of paper, 32 busy)
 *   ESC !o\r\n    -> 16-byte status: [0] readiness, [7] paper type, [11] label
 *                    length mm, [13] label width mm (RFID), [14..15] CRC-16/MODBUS
 *                    of [0..13], big-endian */
#ifndef PM220_PROTO_H
#define PM220_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define PM_READY_LID 0x01
#define PM_READY_PAPER 0x04
#define PM_READY_BUSY 0x20

typedef struct { int percent, charging; } pm_battery_t;
typedef struct { int dpi; char hw[12], fw[12]; int settings; } pm_config_t;
typedef struct { int readiness, paper, width_mm, length_mm, crc_ok; } pm_status_t;

enum { PM_NONE = 0, PM_BATTERY, PM_CONFIG, PM_READY, PM_STATUS };

typedef struct {
    int kind;                /* PM_* */
    union { pm_battery_t battery; pm_config_t config; int ready; pm_status_t status; } u;
} pm_reply_t;

uint16_t pm_crc16_modbus(const uint8_t *p, size_t n);
int pm_parse_battery(const uint8_t *p, size_t n, pm_battery_t *out);   /* 1 ok, 0 not one */
int pm_parse_config(const uint8_t *p, size_t n, pm_config_t *out);
int pm_parse_status(const uint8_t *p, size_t n, pm_status_t *out);

/* An RX accumulator. pm_rx_take() looks for one whole reply at the front:
 * expect says what was asked last (PM_NONE: anything recognisable); idle is
 * 1 when no byte came for a while, which closes a reply that has no
 * terminator. Returns the bytes used (0: need more). Unrecognised bytes are
 * dropped once idle. */
typedef struct { uint8_t b[96]; uint16_t n; } pm_rx_t;
void pm_rx_push(pm_rx_t *rx, const uint8_t *p, size_t n);
size_t pm_rx_take(pm_rx_t *rx, int expect, int idle, pm_reply_t *out);
const char *pm_paper_name(int paper);

#endif
