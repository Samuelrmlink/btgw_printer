/* The reference for the web app's label layout and calibration.
 *
 * The old PM220 firmware laid the bitmap out on the label itself
 * (pm220_rp2_webapp src/printer/job.c: job_compute_layout, the crop in
 * layout_and_build, draw_frame for the test print). In btgw_printer the web
 * app does that work and sends the result; this is the same arithmetic with
 * every compile-time constant turned into a parameter. test_tspl checks it
 * against the old code byte for byte, and writes golden/layout_vectors.json
 * so the JavaScript port can be checked against the same numbers. */
#ifndef REF_LAYOUT_H
#define REF_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int dpi;                 /* 203 */
    int32_t page_w_mm100, page_h_mm100;   /* the label: 5000 x 3000 */
    int32_t offset_x_mm100, offset_y_mm100;  /* calibration: -125, 300 */
    int origin_x_adj, origin_y_adj, print_height_adj;   /* dots, +-80 */
} ref_cal_t;

typedef struct {
    int origin_x, origin_y;
    int print_width_dots, print_height_dots;
    int shift_x, shift_y;
} ref_layout_t;

void ref_layout(const ref_cal_t *c, int src_w, int src_h, ref_layout_t *out);

/* Crop/shift src (packed, 0 = black) into out per the layout; then the
 * all-black protection. out holds ((w+7)/8)*h bytes. Returns its size. */
size_t ref_crop(const ref_layout_t *L, const uint8_t *src, int src_wbytes, int src_w, int src_h,
                uint8_t *out, size_t cap);

/* The old test print: inset frame, centre cross, dotted safe rectangle. */
void ref_test_frame(uint8_t *fb, int wbytes, int w, int h, int dpi,
                    int safe_x0, int safe_y0, int safe_x1, int safe_y1);

int ref_mm100_to_dots(int32_t mm100, int dpi);

#endif
