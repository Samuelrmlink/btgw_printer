import { test } from "node:test";
import assert from "node:assert/strict";
import { buildJobBitmap, guardAllBlack, monoNew, packMono, toHead, unpackMono } from "../../web/pack.js";
import { defaultCalibration, pageFor, rectToEditor } from "../../web/layout.js";

function randomMono(w, h, seed = 1) {
    const m = monoNew(w, h);
    let s = seed;
    for (let i = 0; i < m.bits.length; i++) {
        s = (s * 1103515245 + 12345) & 0x7fffffff;
        m.bits[i] = (s >> 16) & 1;
    }
    return m;
}

test("packMono: black_bit 0 is the pm220 polarity (1 = white), black_bit 1 inverts", () => {
    const m = monoNew(10, 2);
    m.bits[0] = 1;       // (0,0)
    m.bits[9] = 1;       // (9,0)
    m.bits[10 + 3] = 1;  // (3,1)
    assert.deepEqual([...packMono(m, 0)], [0x7f, 0xbf, 0xef, 0xff]);
    assert.deepEqual([...packMono(m, 1)], [0x80, 0x40, 0x10, 0x00]);
    assert.deepEqual(unpackMono(packMono(m, 0), 10, 2, 0).bits, m.bits);
    assert.deepEqual(unpackMono(packMono(m, 1), 10, 2, 1).bits, m.bits);
});

test("toHead rotations agree with rectToEditor", () => {
    const W = 24; // head
    const H = 40;
    for (const rot of [0, 90, 180, 270]) {
        const swap = rot === 90 || rot === 270;
        const e = monoNew(swap ? H : W, swap ? W : H);
        // mark the editor image of head pixel (5, 7)
        const r = rectToEditor({ x0: 5, y0: 7, x1: 5, y1: 7 }, W, H, rot);
        e.bits[r.y0 * e.w + r.x0] = 1;
        const s = toHead(e, rot);
        assert.equal(s.w, W);
        assert.equal(s.h, H);
        assert.equal(s.bits[7 * W + 5], 1, `rot ${rot}`);
        assert.equal(s.bits.reduce((a, b) => a + b, 0), 1);
    }
});

/** job.c layout_and_build's crop, written out independently. */
function pm220Crop(src, L) {
    const out = monoNew(L.printW, L.printH);
    for (let y = 0; y < L.printH; y++) {
        for (let x = 0; x < L.printW; x++) {
            const sx = x - L.shiftX;
            const sy = y - L.shiftY;
            if (sx >= 0 && sy >= 0 && sx < src.w && sy < src.h && src.bits[sy * src.w + sx]) {
                out.bits[y * L.printW + x] = 1;
            }
        }
    }
    return out;
}

test("buildJobBitmap reproduces the pm220 firmware crop, shift and origin", () => {
    const PM220 = { id: "pm220", dpi: 203, head_dots: 384, black_bit: 0 };
    const media = { id: "m", w_mm: 50, h_mm: 30, gap_mm: 2, safe_mm: [4, 4, 4, 4], cal: { offset_x_mm: -1.25, offset_y_mm: 3 } };
    for (const adj of [{}, { offset_x_mm: -4, origin_x_adj: -7, offset_y_mm: -2, print_height_adj: -30 }, { origin_y_adj: 40 }]) {
        const cal = { ...defaultCalibration(PM220, media), ...adj };
        const page = pageFor(PM220, media, cal, 0);
        const src = randomMono(384, 240, 7);
        const job = buildJobBitmap(src, page, PM220);
        const want = pm220Crop(src, page.layout);
        assert.equal(job.width, page.layout.printW);
        assert.equal(job.rows, page.layout.printH);
        assert.deepEqual(job.origin, [page.layout.originX, page.layout.originY]);
        assert.deepEqual(job.bytes, packMono(want, 0));
        assert.equal(job.bytes.length, job.rowBytes * job.rows);
    }
});

test("the all-black guard punches a white dot every 16 rows", () => {
    const m = monoNew(16, 40);
    m.bits.fill(1);
    guardAllBlack(m);
    const white = [];
    for (let i = 0; i < m.bits.length; i++) {
        if (!m.bits[i]) {
            white.push(i);
        }
    }
    assert.deepEqual(white, [0, 16 * 16, 32 * 16]);
    const n = monoNew(4, 4);
    n.bits.fill(1);
    n.bits[5] = 0;
    guardAllBlack(n);
    assert.equal(n.bits[0], 1);
});

test("a model without placement gets a head-wide bitmap with the origin baked in", () => {
    const P21 = { id: "p21", dpi: 203, head_dots: 96, black_bit: 1, placement: false };
    const media = { id: "p", w_mm: 14, h_mm: 40, safe_mm: [1, 1, 1, 1] };
    const cal = { ...defaultCalibration(P21, media), origin_x_adj: 3, origin_y_adj: 2 };
    const page = pageFor(P21, media, cal, 90);
    const e = monoNew(page.width_dots, page.height_dots);
    e.bits[0] = 1; // editor (0,0) -> head (95, 0) at 90°
    const job = buildJobBitmap(e, page, P21);
    assert.equal(job.width, 96);
    assert.deepEqual(job.origin, [0, 0]);
    assert.equal(job.rows, page.layout.printH + 2);
    // origin x = (112 - 96) / 2 centring + 3 = 11: head (95,0) falls off the head's right edge
    assert.equal(job.mono.bits.reduce((a, b) => a + b, 0), 0);
    e.bits[0] = 0;
    e.bits[30 * e.w + 0] = 1; // editor (0,30) -> head (65, 0) -> (76, 2)
    const j2 = buildJobBitmap(e, page, P21);
    assert.equal(j2.mono.bits[2 * 96 + 76], 1);
    assert.equal(j2.bytes[2 * 12 + 9], 0x08);
});
