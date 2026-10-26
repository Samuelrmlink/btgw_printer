/* Host test: D30 job bytes against the byte strings in
 * docs/protocols/phomemo-d30.md (the sequences polskafan/phomemo_d30 sends). */
#include "../d30_proto.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static size_t hex(const char *s, uint8_t *out) {
    size_t k = 0;
    for (; s[0] && s[1]; s += 2) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[k++] = (uint8_t)v;
    }
    return k;
}

int main(void) {
    uint8_t want[64], got[64];
    const uint8_t *pre;
    size_t pn = d30_preamble(&pre);
    size_t wn = hex("1f1138" "1f11121f1113" "1f1109" "1f1111" "1f1119" "1f1107" "1f110a1f110202", want);
    CHECK(pn == wn && memcmp(pre, want, wn) == 0);

    /* a 40 mm label: 320 rows */
    wn = hex("1f1124001b401d7630000c004001", want);
    CHECK(d30_job_head(320, got, sizeof got) == 14);
    CHECK(memcmp(got, want, 14) == 0);
    CHECK(d30_job_head(240, got, sizeof got) == 14 && got[12] == 240 && got[13] == 0);
    CHECK(d30_job_head(0, got, sizeof got) == -1);
    CHECK(d30_job_head(320, got, 13) == -1);
    CHECK(d30_job_head(70000, got, sizeof got) == -1);

    printf("%s d30_proto (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
