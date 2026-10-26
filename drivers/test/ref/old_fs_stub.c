#include "ref/old_fs_stub.h"
#include <string.h>

const char *old_cal;
int fs_begin_read(const char *name, size_t *size) {
    if (!old_cal || strcmp(name, "settings/print.json")) { return -1; }
    *size = strlen(old_cal);
    return 1;
}
int fs_read(int h, uint8_t *buf, size_t cap) {
    (void)h;
    size_t n = strlen(old_cal);
    if (n > cap) { n = cap; }
    memcpy(buf, old_cal, n);
    return (int)n;
}
void fs_end_read(int h) { (void)h; }
