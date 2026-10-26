/* Host test: P21 job bytes against golden files made by julienvincent/
 * nelko-p21-rs (MIT; tools in docs/notes/p21-d30.md), and the reply parsers
 * against the vectors in docs/protocols/nelko-p21.md. */
#include "../p21_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL cannot open %s\n", path); fails++; *n = 0; return NULL; }
    fseek(f, 0, SEEK_END);
    long m = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)m);
    *n = fread(b, 1, (size_t)m, f);
    fclose(f);
    return b;
}

static void hex(const char *s, uint8_t *out, size_t *n) {
    size_t k = 0;
    for (; s[0] && s[1]; s += 2) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[k++] = (uint8_t)v;
    }
    *n = k;
}

static void golden(const char *dir, int density) {
    char path[512];
    size_t bn, gn;
    snprintf(path, sizeof path, "%s/golden_p21_14x40.bitmap", dir);
    uint8_t *bm = slurp(path, &bn);
    snprintf(path, sizeof path, "%s/golden_p21_14x40_d%d.bin", dir, density);
    uint8_t *g = slurp(path, &gn);
    if (!bm || !g) { return; }
    CHECK(bn == 12 * 284);
    uint8_t *job = malloc(gn + 256);
    size_t n = 0;
    memcpy(job, P21_Q_LABEL, strlen(P21_Q_LABEL));
    n += strlen(P21_Q_LABEL);
    p21_job_t j = {140, 400, 50, 284, density};
    int h = p21_job_head(&j, (char *)job + n, 256);
    CHECK(h > 0);
    n += (size_t)h;
    memcpy(job + n, bm, bn);
    n += bn;
    int t = p21_job_tail((char *)job + n, 64);
    CHECK(t > 0);
    n += (size_t)t;
    CHECK(n == gn);
    if (n == gn) {
        for (size_t i = 0; i < n; i++) {
            if (job[i] != g[i]) { printf("FAIL density %d: byte %zu is %02x, golden %02x\n", density, i, job[i], g[i]); fails++; break; }
        }
    }
    free(job);
    free(bm);
    free(g);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    golden(dir, 8);
    golden(dir, 15);

    /* density clamps to 1..15 and other sizes format as tenths */
    char b[256];
    p21_job_t j = {120, 300, 30, 240, 99};
    CHECK(p21_job_head(&j, b, sizeof b) > 0);
    CHECK(strstr(b, "SIZE 12.0 mm,30.0 mm\r\n") == b);
    CHECK(strstr(b, "GAP 3.0 mm,0 mm\r\n"));
    CHECK(strstr(b, "DENSITY 15\r\n"));
    CHECK(strstr(b, "BITMAP 0,0,12,240,1,"));
    j.density = 0;
    p21_job_head(&j, b, sizeof b);
    CHECK(strstr(b, "DENSITY 1\r\n"));
    CHECK(p21_job_head(&j, b, 10) == -1);

    /* CRC vectors: nelko-p21-rs and the AGPL notes (facts) */
    uint8_t d[64];
    size_t n;
    hex("200c011203000301121215280f0e", d, &n);
    CHECK(p21_crc16(d, n) == 0x0d22);
    hex("000c011203000301121215280f0eed03", d, &n);
    p21_label_t l;
    CHECK(p21_parse_label(d, n, &l));
    CHECK(l.status == 0 && l.width_mm == 14 && l.length_mm == 40 && l.paper_type == 1 && l.color == 3);
    CHECK(l.tag_present);
    d[15] ^= 1;
    CHECK(!p21_parse_label(d, n, &l));

    hex("434f4e4649472000cb00000304020402010d0a", d, &n);
    p21_config_t c;
    CHECK(p21_parse_config(d, n, &c));
    CHECK(c.dpi == 203 && c.fw[0] == 4 && c.fw[1] == 2 && c.fw[2] == 4 && c.timer == 2 && c.beep == 1);

    p21_battery_t bt;
    const uint8_t bat[] = {'B', 'A', 'T', 'T', 'E', 'R', 'Y', ' ', 0x87, 0x00, '\r', '\n'};
    CHECK(p21_parse_battery(bat, sizeof bat, &bt));
    CHECK(bt.percent == 87 && !bt.charging);
    CHECK(!p21_parse_battery(bat, sizeof bat - 1, &bt));

    CHECK(strcmp(p21_status_name(0x24), "out_of_paper") == 0);
    CHECK(p21_status_error(0x01) && !p21_status_error(0x00) && !p21_status_error(0x20));

    printf("%s p21_proto (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
