#include "pl70e_proto.h"
#include <string.h>

/* not const: a table of pointers is RW under PEX model B (pex.h PEX_PTR_TABLE;
 * extern linkage keeps it out of rodata without the attribute) */
pl70e_pipe_t pl70e_pipes[4] = {
    { "18f0", "2af1", "2af0" },
    { "fff0", "fff2", "fff1" },
    { "49535343-fe7d-4ae5-8fa9-9fafd205e455", "49535343-8841-43f4-a8d4-ecbe34729bb3",
      "49535343-1e4d-4bd9-ba61-23c647249616" },
    { "e7810a71-73ae-499d-8c15-faa9aef0c3f2", "bef8d6c9-9c21-4c9e-b632-bd58c1009f9f",
      "bef8d6c9-9c21-4c9e-b632-bd58c1009f9f" },
};

const char *pl70e_status_name(int st) {
    switch (st) {
    case PL70E_ST_READY: return "ready";
    case PL70E_ST_NOPAPER: return "no_paper";
    case PL70E_ST_LIDOPEN: return "lid_open";
    case PL70E_ST_OVERHEAT: return "overheat";
    case PL70E_ST_PRINTING: return "printing";
    case PL70E_ST_PAUSED: return "paused";
    case PL70E_ST_ERROR: return "error";
    default: return "unknown";
    }
}

static int starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

int pl70e_status_from_line(const char *line) {
    if (!starts(line, "READSTA")) { return -1; }
    const char *v = line + 7;
    while (*v == ' ') { v++; }
    /* NOPAPER before PAPER: one is a prefix of the other's tail. */
    if (starts(v, "NOPAPER")) { return PL70E_ST_NOPAPER; }
    if (starts(v, "PAPER")) { return PL70E_ST_READY; }
    if (starts(v, "LIBOPEN") || starts(v, "LIDOPEN")) { return PL70E_ST_LIDOPEN; }
    if (starts(v, "OVERHEAT")) { return PL70E_ST_OVERHEAT; }
    return PL70E_ST_UNKNOWN;
}

/* The ESC !? byte. The Nelko app compares it as a code, not as bits
 * (docs/protocols/nelko-states.md): 0x30 is low battery and still prints.
 * A byte not in the table falls back to the TSPL bits seen on this family
 * (orbsa: 0x04 paper out with 0x02 riding along, 0x01 lid, 0x20 printing). */
int pl70e_status_from_byte(uint8_t b) {
    switch (b) {
    case 0x00: case 0x30: return PL70E_ST_READY;
    case 0x01: return PL70E_ST_LIDOPEN;
    case 0x02: case 0x20: return PL70E_ST_PRINTING;
    case 0x03: case 0x08: case 0x90: return PL70E_ST_ERROR;
    case 0x04: case 0x24: return PL70E_ST_NOPAPER;
    case 0x10: return PL70E_ST_PAUSED;
    case 0x80: return PL70E_ST_OVERHEAT;
    default: break;
    }
    if (b & 0x04) { return PL70E_ST_NOPAPER; }
    if (b & 0x01) { return PL70E_ST_LIDOPEN; }
    if (b & 0x80) { return PL70E_ST_OVERHEAT; }
    if (b & 0x20) { return PL70E_ST_PRINTING; }
    return PL70E_ST_UNKNOWN;
}

int pl70e_rx_feed(pl70e_rx_t *rx, const uint8_t *data, size_t n) {
    int got = -1;
    /* The status byte is a code below 'A' or 0x80/0x90; a text reply is
     * letters, and a split line can leave a lone CR or LF in a notification. */
    if (n == 1 && rx->n == 0 && (data[0] < 0x41 || data[0] >= 0x80) && data[0] != '\r' && data[0] != '\n') {
        return pl70e_status_from_byte(data[0]);
    }
    for (size_t i = 0; i < n; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (rx->n) {
                rx->line[rx->n] = 0;
                int st = pl70e_status_from_line(rx->line);
                if (st >= 0) { got = st; }
                rx->n = 0;
            }
            continue;
        }
        if (rx->n < sizeof rx->line - 1) {
            rx->line[rx->n++] = c;
        } else {
            rx->n = 0;           /* not a status line: drop it */
        }
    }
    return got;
}

int pl70e_name_match(const char *name) {
    return name && strncmp(name, PL70E_NAME_PREFIX, strlen(PL70E_NAME_PREFIX)) == 0;
}
