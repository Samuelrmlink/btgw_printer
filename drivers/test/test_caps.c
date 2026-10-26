/* Every driver's caps (PRN_DRV_INFO, through printer.drivers) against the web
 * app's fallback copy in web/presets/models.json, field by field, so the web
 * app designs the same bitmap with or without a board. Also: every caps
 * object carries the fields the web app and the hub read. */
#include "t.h"
#include "drv.h"
#include <stdio.h>
#include <stdlib.h>

int p21__pex_main(const pex_host_t *, uint32_t, void *);
int p21__prn_drv(uint32_t, uint32_t, uint32_t, uint32_t);
int d30__pex_main(const pex_host_t *, uint32_t, void *);
int d30__prn_drv(uint32_t, uint32_t, uint32_t, uint32_t);
int pl70e__pex_main(const pex_host_t *, uint32_t, void *);
int pl70e__prn_drv(uint32_t, uint32_t, uint32_t, uint32_t);

static char R[8192];
static char *s_web;

static void load(const char *name, int (*main_)(const pex_host_t *, uint32_t, void *),
                 int (*drv)(uint32_t, uint32_t, uint32_t, uint32_t)) {
    pex_mod_t *m = fh_module(name, main_);
    fh_export(m, "prn_drv", (void *)drv);
    fh_load(m);
    fh_start(m, NULL);
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { n = 0; }
    b[n] = 0;
    fclose(f);
    return b;
}

/* the web's models.json entry for driver/model, as "drivers.i.models.k" */
static int web_path(const char *driver, const char *model, char *out, size_t cap) {
    char p[96], v[48];
    for (int i = 0; i < 16; i++) {
        snprintf(p, sizeof p, "drivers.%d.driver", i);
        if (!pex_json_str(s_web, p, v, sizeof v)) { return 0; }
        if (strcmp(v, driver)) { continue; }
        for (int k = 0; k < 8; k++) {
            snprintf(p, sizeof p, "drivers.%d.models.%d.id", i, k);
            if (!pex_json_str(s_web, p, v, sizeof v)) { break; }
            if (strcmp(v, model) == 0) { snprintf(out, cap, "drivers.%d.models.%d", i, k); return 1; }
        }
    }
    return 0;
}

/* the raw JSON text of one value, or "" */
static void raw(const char *json, const char *path, char *out, size_t cap) {
    size_t n = 0;
    const char *v = pex_json_find(json, path, &n);
    if (!v) { out[0] = 0; return; }
    if (n >= cap) { n = cap - 1; }
    /* compare without whitespace: models.json is pretty-printed */
    size_t o = 0;
    int in_str = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        char c = v[i];
        if (c == '"' && (i == 0 || v[i - 1] != '\\')) { in_str = !in_str; }
        if (!in_str && (c == ' ' || c == '\n' || c == '\t' || c == '\r')) { continue; }
        out[o++] = c;
    }
    out[o] = 0;
}

static void t_caps_match_web(void) {
    fh_reset();
    load_hub();
    load_virtual();
    load_pm220();
    load("nelko_p21", p21__pex_main, p21__prn_drv);
    load("phomemo_d30", d30__pex_main, d30__prn_drv);
    load("nelko_pl70e", pl70e__pex_main, pl70e__prn_drv);
    fh_run(1200);
    CHECK(fh_op("printer.drivers", "{}", R, sizeof R) > 0);
    static const char *fields[] = { "transport", "dpi", "head_dots", "black_bit", "placement", "copies",
                                    "density", "speed", "media", "presets", "min_len_mm", "max_len_mm" };
    int models = 0;
    for (int i = 0; i < 8; i++) {
        char p[96], drv[48], id[32], wp[64];
        snprintf(p, sizeof p, "drivers.%d.driver", i);
        if (!pex_json_str(R, p, drv, sizeof drv)) { break; }
        for (int k = 0; k < 4; k++) {
            snprintf(p, sizeof p, "drivers.%d.models.%d.id", i, k);
            if (!pex_json_str(R, p, id, sizeof id)) { break; }
            models++;
            if (!web_path(drv, id, wp, sizeof wp)) {
                fprintf(stderr, "web/presets/models.json has no %s/%s\n", drv, id);
                CHECK(0);
                continue;
            }
            for (size_t f = 0; f < sizeof fields / sizeof fields[0]; f++) {
                char a[512], b[512], pa[128], pb[128];
                snprintf(pa, sizeof pa, "drivers.%d.models.%d.%s", i, k, fields[f]);
                snprintf(pb, sizeof pb, "%s.%s", wp, fields[f]);
                raw(R, pa, a, sizeof a);
                raw(s_web, pb, b, sizeof b);
                if (!a[0] && strcmp(fields[f], "copies") && strcmp(fields[f], "speed")) {
                    fprintf(stderr, "%s/%s caps lack \"%s\"\n", drv, id, fields[f]);
                    CHECK(0);
                }
                if (strcmp(a, b)) {
                    fprintf(stderr, "%s/%s %s: driver %s, web %s\n", drv, id, fields[f], a[0] ? a : "(none)", b[0] ? b : "(none)");
                }
                CHECK(strcmp(a, b) == 0);
            }
        }
    }
    CHECK_EQ(models, 5);
}

int main(void) {
    s_web = slurp(WEB_MODELS);
    if (!s_web) { fprintf(stderr, "cannot read %s\n", WEB_MODELS); return 1; }
    RUN(t_caps_match_web);
    T_DONE();
}
