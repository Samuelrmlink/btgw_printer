/* Shared between the fake host's files. Not for tests. */
#ifndef FH_INTERNAL_H
#define FH_INTERNAL_H

#include "fakehost.h"

#define FH_EXPORTS 8

struct pex_mod {
    char name[32];
    fh_main_fn main;
    int used, loaded, running, exiting;
    struct { char sym[32]; void *fn; } exports[FH_EXPORTS];
    int nexports;
    size_t mem, peak;
    int refs;
};

pex_mod_t *fh_cur(void);
pex_mod_t *fh_swap(pex_mod_t *m);   /* set the current module, return the old one */
int fh_group_off(const char *group);
void fh_log_line(pex_mod_t *m, const char *fmt, ...);

/* per-pass hooks in the order the firmware's loop runs them */
void fh_ble_poll(void);
void fh_classic_poll(void);
void fh_ble_reset(void);
void fh_classic_reset(void);
void fh_ble_unload(pex_mod_t *m);
void fh_classic_unload(pex_mod_t *m);

#endif
