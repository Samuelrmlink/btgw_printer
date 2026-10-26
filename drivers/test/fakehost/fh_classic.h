/* Simulated Bluetooth Classic peers for the fake host. The session rules are
 * the firmware's own (btgw/src/bt/classic/cl_sess.c); this file plays the
 * radio, SDP, pairing and RFCOMM the way btgw/src/bt/classic/spp.c drives
 * them, against peers a test describes. */
#ifndef FH_CLASSIC_H
#define FH_CLASSIC_H

#include <stddef.h>
#include <stdint.h>

typedef struct fh_cl_peer fh_cl_peer_t;
typedef void (*fh_cl_rx_fn)(fh_cl_peer_t *p, const uint8_t *data, size_t n, void *ctx);

typedef struct {
    const char *addr;        /* "AA:BB:CC:DD:EE:FF" */
    const char *name;
    uint32_t cod;            /* Class of Device */
    int8_t rssi;
    int name_by_rnr;         /* 1: the name only comes from a remote name request */
    uint8_t sdp_channel;     /* channel of the SPP record; 0: no record */
    int sdp_fails;           /* 1: the SDP query itself fails */
    const char *pin;         /* legacy PIN the peer wants; NULL: SSP just-works */
    uint16_t mtu;            /* RFCOMM frame size offered (default 990) */
    int absent;              /* 1: pages time out, inquiry does not see it */
    uint32_t page_ms;        /* default 30 */
    uint32_t bytes_per_ms;   /* how fast the link drains the host's TX (default 120) */
    fh_cl_rx_fn on_rx;       /* bytes the host sent */
    void *ctx;
} fh_cl_peer_cfg_t;

fh_cl_peer_t *fh_cl_peer_add(const fh_cl_peer_cfg_t *cfg);
void fh_cl_peer_send(fh_cl_peer_t *p, const void *data, size_t n);   /* into the host's RX */
void fh_cl_peer_close(fh_cl_peer_t *p);      /* the peer closes RFCOMM / drops the ACL */
void fh_cl_peer_forget(fh_cl_peer_t *p);     /* the peer loses its link key (the host keeps a stale one) */
void fh_cl_peer_set_pin(fh_cl_peer_t *p, const char *pin);
void fh_cl_peer_stall(fh_cl_peer_t *p, int on);   /* stop draining: TX fills, sends get EBUSY */
void fh_cl_peer_absent(fh_cl_peer_t *p, int on);

typedef struct {
    int pages;               /* ACL attempts */
    int pin_requests;
    char pins_tried[8][16];
    int opens;               /* RFCOMM channels opened */
    int closes;              /* RFCOMM channels closed by the host */
    size_t rx_bytes;         /* bytes received from the host, total */
    int connected;           /* RFCOMM up now */
} fh_cl_stats_t;
const fh_cl_stats_t *fh_cl_peer_stats(fh_cl_peer_t *p);

int fh_cl_host_has_key(const char *addr);
void fh_cl_host_set_key(const char *addr, int on);
int fh_cl_sessions(void);    /* live sessions in the table */
int fh_cl_inquiries(void);   /* inquiries started */

#endif
