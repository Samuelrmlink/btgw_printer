/* The one file the old PM220 job.c reads (settings/print.json), served from a string. */
#ifndef OLD_FS_STUB_H
#define OLD_FS_STUB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
extern const char *old_cal;   /* the calibration JSON, or NULL for none */
bool old_job_from_bitmap(const uint8_t *bitmap, int width_bytes, int height_dots, uint8_t *out,
                         size_t out_cap, size_t *out_len, void *layout);
bool old_job_test_frame(uint8_t *out, size_t out_cap, size_t *out_len);
#endif
