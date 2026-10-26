/** Label geometry: model caps × media profile × calibration → the editor page and the job layout.
 *
 * DOM-free, so node tests can import it. Ported from pm220_rp2_webapp, where
 * the same math lived twice (firmware job.c job_compute_layout and web cal.js
 * computeLayout); here it lives only in the browser and the driver is told the
 * result.
 *
 * Two coordinate systems:
 *   head (S): x across the print head, y along the feed. Width srcW = the
 *             printable width on this label, height srcH = the label length.
 *   editor (E): what the user sees. E = S rotated by the label's orientation
 *             (0/90/180/270), so a 40×14 mm P21 label can be designed wide.
 */

export const CAL_ADJ_LIM = 80;
export const CAL_MM_LIM = 10;

/** mm → dots, rounded half away from zero (the firmware's tspl_mm_to_dots). */
export function mmToDots(mm, dpi) {
    const v = (Number(mm) || 0) * dpi / 25.4;
    return Math.trunc(v + (v >= 0 ? 0.5 : -0.5));
}

export function dotsToMm(dots, dpi) {
    return Math.round((dots * 25.4 / dpi) * 100) / 100;
}

function clamp(v, lo, hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

/** The label in head coordinates. Continuous media use media.h_mm as the length. */
export function labelGeometry(model, media) {
    const dpi = model.dpi || 203;
    const labelW = Math.max(8, mmToDots(media.w_mm, dpi));
    const labelH = Math.max(8, mmToDots(media.h_mm || model.default_len_mm || 40, dpi));
    const head = model.head_dots || labelW;
    return {
        dpi,
        labelW,
        labelH,
        srcW: Math.min(head, labelW),
        srcH: labelH,
        head,
    };
}

/** Factory calibration for one printer × media: the media profile's own defaults
 *  (the PM220 die-cut registration is −1.25 / +3 mm), else zeros, plus a safe
 *  rect inset by media.safe_mm (default 4 mm, the PM220's SAFE_MARGIN_MM). */
export function defaultCalibration(model, media) {
    const geo = labelGeometry(model, media);
    const d = media.cal || {};
    const m = Array.isArray(media.safe_mm) ? media.safe_mm : [4, 4, 4, 4];
    const [l, t, r, b] = m.map((v) => mmToDots(v, geo.dpi));
    return normaliseCalibration({
        offset_x_mm: d.offset_x_mm ?? 0,
        offset_y_mm: d.offset_y_mm ?? 0,
        gap_mm: d.gap_mm ?? media.gap_mm ?? 0,
        origin_x_adj: 0,
        origin_y_adj: 0,
        print_height_adj: 0,
        safe_x0: l,
        safe_y0: t,
        safe_x1: geo.srcW - 1 - r,
        safe_y1: geo.srcH - 1 - b,
    }, geo);
}

/** Clamp a calibration to the limits the pm220 firmware applied (cal_load / safe_clamp). */
export function normaliseCalibration(cal, geo) {
    const c = { ...cal };
    const n = (v, d = 0) => (Number.isFinite(Number(v)) ? Number(v) : d);
    c.offset_x_mm = clamp(n(c.offset_x_mm), -CAL_MM_LIM, CAL_MM_LIM);
    c.offset_y_mm = clamp(n(c.offset_y_mm), -CAL_MM_LIM, CAL_MM_LIM);
    c.gap_mm = clamp(n(c.gap_mm), 0, CAL_MM_LIM);
    for (const k of ["origin_x_adj", "origin_y_adj", "print_height_adj"]) {
        c[k] = clamp(Math.trunc(n(c[k])), -CAL_ADJ_LIM, CAL_ADJ_LIM);
    }
    const W = geo.srcW;
    const H = geo.srcH;
    let x0 = Math.trunc(n(c.safe_x0, 0));
    let y0 = Math.trunc(n(c.safe_y0, 0));
    let x1 = Math.trunc(n(c.safe_x1, W - 1));
    let y1 = Math.trunc(n(c.safe_y1, H - 1));
    x0 = Math.max(0, x0);
    y0 = Math.max(0, y0);
    x1 = Math.min(W - 1, x1);
    y1 = Math.min(H - 1, y1);
    if (x1 < x0 + 3) {
        x1 = x0 + 3;
        if (x1 >= W) {
            x1 = W - 1;
            x0 = x1 - 3;
        }
    }
    if (y1 < y0 + 3) {
        y1 = y0 + 3;
        if (y1 >= H) {
            y1 = H - 1;
            y0 = y1 - 3;
        }
    }
    Object.assign(c, { safe_x0: x0, safe_y0: y0, safe_x1: x1, safe_y1: y1 });
    return c;
}

/** Where the bitmap lands on the label, and how much of it prints.
 *  A line-for-line port of job_compute_layout() (pm220 src/printer/job.c). */
export function computeLayout(geo, cal) {
    const pageW = geo.labelW;
    const pageH = geo.labelH;
    const srcW = geo.srcW;
    const srcH = geo.srcH;
    const dpi = geo.dpi;
    let originX = Math.trunc((pageW - srcW) / 2);
    let originY = Math.trunc((pageH - srcH) / 2);
    if (originX < 0) {
        originX = 0;
    }
    if (originY < 0) {
        originY = 0;
    }
    const dx = mmToDots(cal.offset_x_mm, dpi);
    const dy = mmToDots(cal.offset_y_mm, dpi);
    let shiftX = 0;
    let shiftY = 0;
    if (originX + dx < 0) {
        shiftX = originX + dx;
        originX = 0;
    } else {
        originX += dx;
    }
    if (originY + dy < 0) {
        shiftY = originY + dy;
        originY = 0;
    } else {
        originY += dy;
    }
    originX += cal.origin_x_adj | 0;
    originY += cal.origin_y_adj | 0;
    if (originX < 0) {
        shiftX += originX;
        originX = 0;
    }
    if (originY < 0) {
        shiftY += originY;
        originY = 0;
    }
    if (originX > pageW - 1) {
        originX = pageW - 1;
    }
    if (originY > pageH - 1) {
        originY = pageH - 1;
    }
    const availW = Math.max(1, pageW - originX);
    const availH = Math.max(1, pageH - originY);
    const printW = srcW < availW ? srcW : availW;
    let printH = srcH < availH ? srcH : availH;
    printH += cal.print_height_adj | 0;
    if (printH < 1) {
        printH = 1;
    }
    if (printH > availH) {
        printH = availH;
    }
    return { originX, originY, printW, printH, shiftX, shiftY, pageW, pageH };
}

/** Rotate a rect given in head coords (w×h space) into editor coords. */
export function rectToEditor(r, W, H, rot) {
    const { x0, y0, x1, y1 } = r;
    switch (((rot % 360) + 360) % 360) {
        case 90:   // E = S rotated counter-clockwise for display: E(x,y) = S(W-1-y, x)
            return { x0: y0, y0: W - 1 - x1, x1: y1, y1: W - 1 - x0 };
        case 180:
            return { x0: W - 1 - x1, y0: H - 1 - y1, x1: W - 1 - x0, y1: H - 1 - y0 };
        case 270:
            return { x0: H - 1 - y1, y0: x0, x1: H - 1 - y0, y1: x1 };
        default:
            return { x0, y0, x1, y1 };
    }
}

export function rotSwaps(rot) {
    const r = ((rot % 360) + 360) % 360;
    return r === 90 || r === 270;
}

/** Everything the editor needs for one label: the page it edits (editor coords),
 *  its safe rect, the band that will not print, and the job layout. */
export function pageFor(model, media, calIn, orient = 0) {
    const geo = labelGeometry(model, media);
    const cal = normaliseCalibration(calIn || defaultCalibration(model, media), geo);
    const L = computeLayout(geo, cal);
    const rot = ((Number(orient) || 0) % 360 + 360) % 360;
    const swap = rotSwaps(rot);
    const width = swap ? geo.srcH : geo.srcW;
    const height = swap ? geo.srcW : geo.srcH;
    const safeS = { x0: cal.safe_x0, y0: cal.safe_y0, x1: cal.safe_x1, y1: cal.safe_y1 };
    const safe = rectToEditor(safeS, geo.srcW, geo.srcH, rot);
    let crop = null;
    if (L.printH < geo.srcH) {
        crop = rectToEditor({ x0: 0, y0: L.printH, x1: geo.srcW - 1, y1: geo.srcH - 1 }, geo.srcW, geo.srcH, rot);
    }
    return {
        width_dots: width,
        height_dots: height,
        width_bytes: (width + 7) >> 3,
        dpi: geo.dpi,
        width_mm: swap ? media.h_mm : media.w_mm,
        height_mm: swap ? media.w_mm : media.h_mm,
        safe_rect: safe,
        crop_rect: crop,
        orient: rot,
        geo,
        cal,
        layout: L,
    };
}

/** Text for the calibration modal's live "effective" line. */
export function describeLayout(page) {
    const L = page.layout;
    const c = page.cal;
    return `BITMAP ${L.originX},${L.originY}  ${L.printW}×${L.printH}  ` +
        `(canvas ${page.geo.srcW}×${page.geo.srcH}, label ${L.pageW}×${L.pageH}, offset ${c.offset_x_mm}/${c.offset_y_mm} mm, ` +
        `gap ${c.gap_mm} mm, safe ${c.safe_x0},${c.safe_y0}–${c.safe_x1},${c.safe_y1})`;
}
