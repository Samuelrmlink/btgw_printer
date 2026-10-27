/* A native fake of the BTGW PEX host, for testing printer modules on the
 * build machine (gcc -m32).
 *
 * Several modules link into one test binary. Each is compiled with its own
 * names for the entry points, e.g.
 *   -Dpex_main=nelko__pex_main -Dprn_drv=nelko__prn_drv
 * and registered with fh_module(), and its exports with fh_export(). Module
 * code must not have other non-static globals with clashing names; shared
 * libraries (drivers/core, drivers/proto) are linked once and must be
 * stateless.
 *
 * Time is simulated: fh_run(ms) advances the clock 1 ms at a time and runs
 * what the real main loop would (tasks, then BLE events, then Classic
 * session events, then card callbacks).
 *
 * What is real, not faked:
 *   - the Classic session rules: btgw/src/bt/classic/cl_sess.c is compiled
 *     in, and fh_classic.c plays the radio side the way spp.c does;
 *   - the json group: btgw/pex/runtime/groups/pex_json.c.
 * What is modelled on the firmware: the 8-deep BLE event queue that drops
 * the oldest, the 4-deep GATT op queue per link, write length limits
 * (MTU - 3 and 252), 4 open files, lfs reads of at most 4096 bytes, sd:/
 * calls that need a callback (PEX_EPENDING) and answer later. */
#ifndef FAKEHOST_H
#define FAKEHOST_H

#include "pex.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*fh_main_fn)(const pex_host_t *host, uint32_t reason, void *arg);

void fh_reset(void);
void fh_verbose(int on);              /* print module logs and events to stderr */

/* ---- modules ---- */
pex_mod_t *fh_module(const char *name, fh_main_fn main);
void fh_export(pex_mod_t *m, const char *sym, void *fn);
int fh_load(pex_mod_t *m);            /* PEX_INIT */
int fh_start(pex_mod_t *m, const char *cfg_json);   /* PEX_APP_RUN */
int fh_stop(pex_mod_t *m);            /* PEX_APP_STOP, then its tasks are cancelled */
int fh_unload(pex_mod_t *m);          /* PEX_DEINIT; ops, listeners, sessions, files go */
int fh_loaded(pex_mod_t *m);
/* Make m the calling module (for a test calling the host API as m); returns
 * the previous one to restore. */
pex_mod_t *fh_swap(pex_mod_t *m);
void fh_disable_group(const char *group);  /* "classic", "bt": pex_avail false, calls ENOTSUP */

/* ---- time ---- */
uint32_t fh_now(void);
void fh_step(void);                   /* one main-loop pass, then +1 ms */
void fh_run(uint32_t ms);
/* Run until cond(arg) is true or ms pass. 1 if cond became true. */
int fh_run_until(int (*cond)(void *arg), void *arg, uint32_t ms);

/* ---- bus ---- */
/* Calls the op as the WebSocket would. Returns the handler's rc (reply
 * length, or a negative PEX_E*), or PEX_ENOENT when no module has it. */
int fh_op(const char *op, const char *params, char *reply, size_t cap);
int fh_op_exists(const char *op);
int fh_events(const char *topic);     /* emitted so far with this topic */
const char *fh_event(const char *topic, int i);   /* i < 0 counts from the end */
const char *fh_event_last(const char *topic);
void fh_events_clear(void);
int fh_log_has(const char *substr);   /* any module log line containing it */

/* ---- files: "lfs:/a/b" and "sd:/a/b" ---- */
void fh_fs_put(const char *path, const void *data, size_t n);
const uint8_t *fh_fs_get(const char *path, size_t *n);   /* NULL if absent */
void fh_fs_sd_present(int on);        /* default on */
void fh_fs_sd_latency(uint32_t ms);   /* default 2 ms per card request */
int fh_fs_open_count(void);

/* ---- memory accounting ---- */
size_t fh_mem_used(pex_mod_t *m);
size_t fh_mem_peak(pex_mod_t *m);

#ifdef __cplusplus
}
#endif
#endif
