import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import {
    CAL_FORMAT, CAL_KEYS, calibrationFile, calibrationFromFile, computeLayout, defaultCalibration, labelGeometry, mmToDots, normaliseCalibration, pageFor, rectToEditor,
} from "../../web/layout.js";
import * as pm from "../fixtures/pm220_cal_layout.js";

const PM220 = { id: "pm220", dpi: 203, head_dots: 384 };
const M5030 = { id: "nelko-pm220-50x30", w_mm: 50, h_mm: 30, gap_mm: 2, safe_mm: [4, 4, 4, 4], cal: { offset_x_mm: -1.25, offset_y_mm: 3, gap_mm: 2 } };

test("mmToDots rounds half away from zero like tspl_mm_to_dots", () => {
    assert.equal(mmToDots(50, 203), 400);
    assert.equal(mmToDots(30, 203), 240);
    assert.equal(mmToDots(-1.25, 203), -10);
    assert.equal(mmToDots(3, 203), 24);
    assert.equal(mmToDots(101.6, 203), 812);
    assert.equal(mmToDots(0, 203), 0);
});

test("PM220 50x30 geometry and factory calibration match the pm220 firmware", () => {
    const geo = labelGeometry(PM220, M5030);
    assert.deepEqual([geo.labelW, geo.labelH, geo.srcW, geo.srcH], [400, 240, 384, 240]);
    const cal = defaultCalibration(PM220, M5030);
    assert.deepEqual([cal.safe_x0, cal.safe_y0, cal.safe_x1, cal.safe_y1], [32, 32, 351, 207]);
    assert.equal(cal.offset_x_mm, -1.25);
    const L = computeLayout(geo, cal);
    assert.deepEqual(L, { originX: 0, originY: 24, printW: 384, printH: 216, shiftX: -2, shiftY: 0, pageW: 400, pageH: 240 });
});

test("computeLayout equals pm220's computeLayout over a sweep of calibrations", () => {
    const geo = labelGeometry(PM220, M5030);
    let n = 0;
    for (const ox of [-10, -3.5, -1.25, 0, 0.75, 4, 10]) {
        for (const oy of [-10, -2, 0, 3, 7.25, 10]) {
            for (const ax of [-80, -9, 0, 5, 80]) {
                for (const ay of [-80, 0, 13]) {
                    for (const ph of [-80, -10, 0, 40]) {
                        const adj = { offset_x_mm: ox, offset_y_mm: oy, origin_x_adj: ax, origin_y_adj: ay, print_height_adj: ph };
                        const ours = computeLayout(geo, adj);
                        const theirs = pm.computeLayout(adj);
                        assert.deepEqual(ours, theirs, JSON.stringify(adj));
                        n++;
                    }
                }
            }
        }
    }
    assert.ok(n > 2000);
});

test("normaliseCalibration clamps like cal_load / safe_clamp", () => {
    const geo = labelGeometry(PM220, M5030);
    const c = normaliseCalibration({ offset_x_mm: 99, offset_y_mm: -99, gap_mm: -1, origin_x_adj: 500, origin_y_adj: -500,
        print_height_adj: 3.7, safe_x0: 380, safe_y0: -5, safe_x1: 381, safe_y1: 9999 }, geo);
    assert.equal(c.offset_x_mm, 10);
    assert.equal(c.offset_y_mm, -10);
    assert.equal(c.gap_mm, 0);
    assert.equal(c.origin_x_adj, 80);
    assert.equal(c.origin_y_adj, -80);
    assert.equal(c.print_height_adj, 3);
    assert.deepEqual([c.safe_x0, c.safe_x1], [380, 383]);
    assert.deepEqual([c.safe_y0, c.safe_y1], [0, 239]);
});

test("a label narrower than the head prints at the label's width (PL70e 3x1 in)", () => {
    const geo = labelGeometry({ dpi: 203, head_dots: 832 }, { w_mm: 76.2, h_mm: 25.4 });
    assert.deepEqual([geo.srcW, geo.srcH], [609, 203]);
});

test("rectToEditor maps a head rect for every orientation", () => {
    const W = 96;
    const H = 320;
    const r = { x0: 10, y0: 20, x1: 30, y1: 50 };
    assert.deepEqual(rectToEditor(r, W, H, 0), r);
    assert.deepEqual(rectToEditor(r, W, H, 90), { x0: 20, y0: 65, x1: 50, y1: 85 });
    assert.deepEqual(rectToEditor(r, W, H, 180), { x0: 65, y0: 269, x1: 85, y1: 299 });
    assert.deepEqual(rectToEditor(r, W, H, 270), { x0: 269, y0: 10, x1: 299, y1: 30 });
});

test("pageFor swaps the editor page for a landscape P21 label and moves the crop band", () => {
    const P21 = { dpi: 203, head_dots: 96 };
    const media = { id: "p21", w_mm: 14, h_mm: 40, gap_mm: 4, safe_mm: [1, 1, 1, 1] };
    const cal = { ...defaultCalibration(P21, media), print_height_adj: -20 };
    const p = pageFor(P21, media, cal, 90);
    assert.equal(p.width_dots, 320);
    assert.equal(p.height_dots, 96);
    assert.equal(p.layout.printH, 300);
    // rows 300..319 of the head bitmap are the right-hand 20 columns of the editor
    assert.deepEqual(p.crop_rect, { x0: 300, y0: 0, x1: 319, y1: 95 });
});

// ---- calibration files ----

const PRINT_JSON = JSON.parse(readFileSync(new URL("../fixtures/pm220_print.json", import.meta.url), "utf8"));

test("a pm220 settings/print.json loads every key it has, keeps the rest, and lays out like pm220", () => {
    const geo = labelGeometry(PM220, M5030);
    const cur = { ...defaultCalibration(PM220, M5030), origin_y_adj: 7 };
    const r = calibrationFromFile(PRINT_JSON, cur, geo);
    assert.equal(r.pm220, true);
    assert.deepEqual([r.model, r.media_id], ["pm220", "nelko-pm220-50x30"]);
    assert.deepEqual(r.cal, { offset_x_mm: -0.75, offset_y_mm: 2.5, gap_mm: 2.2, origin_x_adj: 3, origin_y_adj: 7, print_height_adj: -4,
        safe_x0: 20, safe_y0: 18, safe_x1: 380, safe_y1: 222 });
    const adj = { ...r.cal, origin_y_adj: 0 };
    assert.deepEqual(computeLayout(geo, adj), pm.computeLayout(adj));
    assert.deepEqual(computeLayout(geo, adj), pm.PRINT_JSON_LAYOUT);
});

test("calibration files are clamped like cal_load, and junk is refused", () => {
    const geo = labelGeometry(PM220, M5030);
    const cur = defaultCalibration(PM220, M5030);
    const r = calibrationFromFile({ offset_x_mm: 50, origin_x_adj: -999, gap_mm: -3, safe_x1: 9000, safe_y0: "12", print_height_adj: 2.9 }, cur, geo);
    assert.deepEqual([r.cal.offset_x_mm, r.cal.origin_x_adj, r.cal.gap_mm, r.cal.safe_x1, r.cal.print_height_adj], [10, -80, 0, 383, 2]);
    assert.equal(r.cal.safe_y0, cur.safe_y0, "a string is not a number the firmware would read");
    assert.equal(r.cal.offset_y_mm, cur.offset_y_mm);
    assert.throws(() => calibrationFromFile({ format: "btgw-printer-label", objects: [] }, cur, geo), /not a calibration/);
    assert.throws(() => calibrationFromFile([1, 2], cur, geo), /not a calibration/);
    assert.throws(() => calibrationFromFile({ hello: 1 }, cur, geo), /no calibration values/);
});

test("our calibration file round-trips and stays readable by the pm220 firmware's 320-byte parser", () => {
    const geo = labelGeometry(PM220, M5030);
    const cal = calibrationFromFile(PRINT_JSON, defaultCalibration(PM220, M5030), geo).cal;
    const f = calibrationFile(cal, "pm220", "nelko-pm220-50x30");
    assert.deepEqual(Object.keys(f).slice(0, CAL_KEYS.length), CAL_KEYS);
    assert.equal(f.format, CAL_FORMAT);
    assert.equal(f.version, 1);
    const text = JSON.stringify(f);
    assert.ok(text.length < 320, `${text.length} bytes`);
    const back = calibrationFromFile(JSON.parse(text), defaultCalibration(PM220, M5030), geo);
    assert.equal(back.pm220, false);
    assert.deepEqual([back.model, back.media_id], ["pm220", "nelko-pm220-50x30"]);
    assert.deepEqual(back.cal, cal);
    // the pm220 firmware's json_*_field: the first "key": number in the text
    for (const k of CAL_KEYS) {
        const m = text.slice(0, 319).match(new RegExp(`"${k}"\\s*:\\s*(-?[0-9.]+)`));
        assert.ok(m && Number(m[1]) === cal[k], k);
    }
});
