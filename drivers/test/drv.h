/* Shared by the driver tests: the modules under test, and waiting on events. */
#ifndef DRV_H
#define DRV_H
#include "fakehost.h"

int hub__pex_main(const pex_host_t *, uint32_t, void *);
int hub__prn_hub(uint32_t, uint32_t, uint32_t, uint32_t);
int virt__pex_main(const pex_host_t *, uint32_t, void *);
int virt__prn_drv(uint32_t, uint32_t, uint32_t, uint32_t);
int nelko__pex_main(const pex_host_t *, uint32_t, void *);
int nelko__prn_drv(uint32_t, uint32_t, uint32_t, uint32_t);

static inline pex_mod_t *load_hub(void) {
    pex_mod_t *m = fh_module("printer_hub", hub__pex_main);
    fh_export(m, "prn_hub", (void *)hub__prn_hub);
    fh_load(m);
    fh_start(m, NULL);
    return m;
}
static inline pex_mod_t *load_virtual(void) {
    pex_mod_t *m = fh_module("printer_virtual", virt__pex_main);
    fh_export(m, "prn_drv", (void *)virt__prn_drv);
    fh_load(m);
    fh_start(m, NULL);
    return m;
}
/* nelko_tspl: the Nelko TSPL driver over Classic (PM220) */
static inline pex_mod_t *load_nelko(void) {
    pex_mod_t *m = fh_module("nelko_tspl", nelko__pex_main);
    fh_export(m, "prn_drv", (void *)nelko__prn_drv);
    fh_load(m);
    fh_start(m, NULL);
    return m;
}

/* Wait until an event with this topic whose JSON contains every needle (up
 * to 3, NULL-terminated) has been emitted after index `from`. */
typedef struct { const char *topic; const char *n[3]; int from; } ev_wait_t;
static inline int ev_match(void *arg) {
    ev_wait_t *w = arg;
    int n = fh_events(w->topic);
    for (int i = w->from; i < n; i++) {
        const char *e = fh_event(w->topic, i);
        int ok = 1;
        for (int k = 0; k < 3 && w->n[k]; k++) { ok &= strstr(e, w->n[k]) != NULL; }
        if (ok) { return 1; }
    }
    return 0;
}
static inline int wait_ev(const char *topic, const char *a, const char *b, uint32_t ms) {
    ev_wait_t w = { topic, { a, b, NULL }, 0 };
    return fh_run_until(ev_match, &w, ms);
}
static inline int wait_ev_after(int from, const char *topic, const char *a, const char *b, uint32_t ms) {
    ev_wait_t w = { topic, { a, b, NULL }, from };
    return fh_run_until(ev_match, &w, ms);
}
#endif
