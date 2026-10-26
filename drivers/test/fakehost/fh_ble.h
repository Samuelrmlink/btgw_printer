/* Simulated BLE peripherals for the fake host: an advertisement in the scan
 * table, a GATT table, writes, reads, notifications, link loss. Modelled on
 * btgw/src/pex/pex_gw_bt.c + src/bt/le/central.c: connect needs the device
 * in the scan table first, then PEX_BT_LINK_UP and PEX_BT_READY; four GATT
 * ops in flight per link (PEX_EBUSY past that); a write is at most MTU - 3
 * and 252 bytes; listener events go through an 8-deep queue that drops the
 * oldest. */
#ifndef FH_BLE_H
#define FH_BLE_H

#include <stddef.h>
#include <stdint.h>

#define FH_BLE_CHR_MAX 16

typedef struct fh_ble_peer fh_ble_peer_t;

typedef struct {
    const char *svc;         /* service UUID as the module passes it to pex_bt_find */
    const char *chr;
    uint8_t props;           /* ATT property bits (0x02 read, 0x04 wnr, 0x08 write, 0x10 notify, 0x20 indicate) */
} fh_ble_chr_cfg_t;

/* chr is the index into cfg.chrs. */
typedef void (*fh_ble_write_fn)(fh_ble_peer_t *p, int chr, const uint8_t *data, size_t n,
                                int with_response, void *ctx);

typedef struct {
    const char *addr;
    const char *name;
    int8_t rssi;
    uint16_t uuid16[4];
    int n_uuid16;
    const char *uuid128;     /* advertised 128-bit service, or NULL */
    uint8_t mfg[31];
    uint8_t mfg_len;
    uint16_t mtu;            /* negotiated ATT MTU (default 247) */
    fh_ble_chr_cfg_t chrs[FH_BLE_CHR_MAX];
    int nchr;
    uint32_t connect_ms;     /* LINK_UP after this (default 40); READY 20 ms later */
    uint32_t op_ms;          /* time one GATT op takes (default 8) */
    int connect_fails;       /* 1: PEX_BT_FAILED instead of READY */
    fh_ble_write_fn on_write;
    void *ctx;
} fh_ble_peer_cfg_t;

fh_ble_peer_t *fh_ble_peer_add(const fh_ble_peer_cfg_t *cfg);
void fh_ble_peer_notify(fh_ble_peer_t *p, int chr, const void *data, size_t n);
void fh_ble_peer_set_value(fh_ble_peer_t *p, int chr, const void *data, size_t n);  /* for reads */
void fh_ble_peer_disconnect(fh_ble_peer_t *p);   /* link loss */
void fh_ble_peer_stall(fh_ble_peer_t *p, int on);   /* ops stop completing */

typedef struct {
    int connects;
    int writes;              /* write ops completed */
    size_t bytes;            /* payload bytes written */
    int subscribed[FH_BLE_CHR_MAX];
    int connected;
} fh_ble_stats_t;
const fh_ble_stats_t *fh_ble_peer_stats(fh_ble_peer_t *p);
int fh_ble_listeners(void);

#endif
