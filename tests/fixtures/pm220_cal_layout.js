// pm220_rp2_webapp web/cal.js (1a7cd31): its client-side copy of the firmware's
// job_compute_layout, verbatim, as the reference layout.js must reproduce.
// MIT, (c) 2026 Samuel Hedrick, the same author as this repo.
const DPI = 203;
const WIDTH_MM = 50;
const HEIGHT_MM = 30;
const SRC_W = 384;
const SRC_H = 240;
function mmToDots(mm) {
    return Math.round(mm * DPI / 25.4);
}
function computeLayout(adj) {
    const pageW = mmToDots(WIDTH_MM);
    const pageH = mmToDots(HEIGHT_MM);
    let originX = Math.floor((pageW - SRC_W) / 2);
    let originY = Math.floor((pageH - SRC_H) / 2);
    if (originX < 0) {
        originX = 0;
    }
    if (originY < 0) {
        originY = 0;
    }
    const dx = mmToDots(adj.offset_x_mm);
    const dy = mmToDots(adj.offset_y_mm);
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
    originX += adj.origin_x_adj;
    originY += adj.origin_y_adj;
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
    let printW = SRC_W < availW ? SRC_W : availW;
    let printH = SRC_H < availH ? SRC_H : availH;
    printH += adj.print_height_adj;
    if (printH < 1) {
        printH = 1;
    }
    if (printH > availH) {
        printH = availH;
    }
    return { originX, originY, printW, printH, shiftX, shiftY, pageW, pageH };
}

export { computeLayout, mmToDots, SRC_W, SRC_H, WIDTH_MM, HEIGHT_MM, DPI };
