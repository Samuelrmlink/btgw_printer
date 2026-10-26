/* Phomemo D30 job bytes. The byte sequences are adapted from
 * polskafan/phomemo_d30 (MIT, see THIRD_PARTY.md), which took them from the
 * vendor app's traffic. */
#include "d30_proto.h"

static const uint8_t PREAMBLE[] = {
    0x1f, 0x11, 0x38,
    0x1f, 0x11, 0x12, 0x1f, 0x11, 0x13,
    0x1f, 0x11, 0x09,
    0x1f, 0x11, 0x11,
    0x1f, 0x11, 0x19,
    0x1f, 0x11, 0x07,
    0x1f, 0x11, 0x0a, 0x1f, 0x11, 0x02, 0x02,
};

size_t d30_preamble(const uint8_t **out) {
    *out = PREAMBLE;
    return sizeof PREAMBLE;
}

int d30_job_head(uint32_t rows, uint8_t *out, size_t cap) {
    if (cap < 14 || rows == 0 || rows > 0xffff) { return -1; }
    static const uint8_t pre[] = {0x1f, 0x11, 0x24, 0x00, 0x1b, 0x40, 0x1d, 0x76, 0x30, 0x00};
    for (int i = 0; i < 10; i++) { out[i] = pre[i]; }
    out[10] = D30_ROW_BYTES;
    out[11] = 0;
    out[12] = (uint8_t)rows;
    out[13] = (uint8_t)(rows >> 8);
    return 14;
}
