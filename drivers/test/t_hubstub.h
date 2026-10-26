/* A stand-in printer_hub for driver tests: it records what a driver tells
 * the hub (register, emits, job done) and calls the driver's prn_drv the way
 * the hub would (pex_call, so the driver runs as itself). Header-only; one
 * test file includes it. */
#ifndef T_HUBSTUB_H
#define T_HUBSTUB_H
#include "fakehost.h"
#include "printer_driver.h"
#include <stdio.h>
#include <string.h>

#define HS_EMITS 256
static struct {
    pex_mod_t *mod;
    char registered[32];
    int registers, unregisters, kicks;
    int ndone;
    uint32_t done_id;
    int done_outcome;
    char done_msg[64];
    int nemit;
    char kind[HS_EMITS][12];
    char json[HS_EMITS][640];
} HS;

static int hs_prn_hub(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2) {
    (void)a2;
    switch (cmd) {
    case PRN_HUB_REGISTER:
        snprintf(HS.registered, sizeof HS.registered, "%s", (const char *)(uintptr_t)a0);
        HS.registers++;
        return a1 == PRN_ABI ? 0 : PEX_EAPI;
    case PRN_HUB_UNREGISTER:
        HS.unregisters++;
        HS.registered[0] = 0;
        return 0;
    case PRN_HUB_EMIT:
        if (HS.nemit < HS_EMITS) {
            snprintf(HS.kind[HS.nemit], sizeof HS.kind[0], "%s", (const char *)(uintptr_t)a0);
            snprintf(HS.json[HS.nemit], sizeof HS.json[0], "%s", (const char *)(uintptr_t)a1);
            HS.nemit++;
        }
        return 0;
    case PRN_HUB_JOB_DONE:
        HS.ndone++;
        HS.done_id = a0;
        HS.done_outcome = (int)a1;
        snprintf(HS.done_msg, sizeof HS.done_msg, "%s", a2 ? (const char *)(uintptr_t)a2 : "");
        return 0;
    case PRN_HUB_KICK:
        HS.kicks++;
        return 0;
    }
    return PEX_ENOTSUP;
}

static int hs_main(const pex_host_t *h, uint32_t reason, void *arg) {
    (void)h; (void)reason; (void)arg;
    return PEX_OK;
}

static void hs_reset(void) {
    memset(&HS, 0, sizeof HS);
    HS.mod = fh_module(PRN_HUB_MODULE, hs_main);
    fh_export(HS.mod, PRN_HUB_SYM, (void *)hs_prn_hub);
    fh_load(HS.mod);
    fh_start(HS.mod, "{}");
}

/* the last emit of a kind, or NULL */
__attribute__((unused)) static const char *hs_last(const char *kind) {
    for (int i = HS.nemit - 1; i >= 0; i--) {
        if (strcmp(HS.kind[i], kind) == 0) { return HS.json[i]; }
    }
    return NULL;
}
__attribute__((unused)) static int hs_count(const char *kind, const char *substr) {
    int n = 0;
    for (int i = 0; i < HS.nemit; i++) {
        if (strcmp(HS.kind[i], kind) == 0 && (!substr || strstr(HS.json[i], substr))) { n++; }
    }
    return n;
}

static int hs_drv(pex_mod_t *drv, uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2) {
    return (int)(int32_t)(uint32_t)pex_call(drv, pex_sym(drv, PRN_DRV_SYM), cmd, a0, a1, a2);
}

static int hs_state_is(void *arg) {
    const char *s = hs_last("state");
    char want[48];
    snprintf(want, sizeof want, "\"state\":\"%s\"", (const char *)arg);
    return s && strstr(s, want) != NULL;
}
static int hs_done(void *arg) { (void)arg; return HS.ndone > 0; }

/* Feed a whole bitmap as the hub would: chunks, retrying on EBUSY while
 * time runs. Returns 0, or the first hard error. */
static int hs_feed_all(pex_mod_t *drv, const uint8_t *bm, size_t n, size_t chunk) {
    size_t off = 0;
    uint32_t guard = 0;
    while (off < n) {
        size_t c = n - off < chunk ? n - off : chunk;
        int r = hs_drv(drv, PRN_DRV_JOB_FEED, (uint32_t)(uintptr_t)(bm + off), (uint32_t)c, 0);
        if (r == PEX_EBUSY || r == 0) {
            fh_run(1);
            if (++guard > 200000) { return PEX_EBUSY; }
            continue;
        }
        if (r < 0) { return r; }
        off += (size_t)r;
    }
    return 0;
}
#endif
