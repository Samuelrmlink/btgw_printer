/** 1-bit images ("mono": {w, h, bits}, bits[y*w+x] = 1 for black) and the job bitmap.
 *
 * DOM-free. The editor renders a mono in editor orientation; buildJobBitmap
 * turns it into what the driver sends: head orientation, the pm220 crop and
 * shift (job.c layout_and_build), the all-black guard, then packed rows with
 * the model's bit sense. */

import { computeLayout } from "./layout.js";

export function monoNew(w, h) {
    return { w, h, bits: new Uint8Array(w * h) };
}

/** Editor orientation → head orientation (layout.js rectToEditor is the inverse). */
export function toHead(e, rot) {
    const r = ((rot % 360) + 360) % 360;
    if (r === 0) {
        return e;
    }
    const swap = r === 90 || r === 270;
    const W = swap ? e.h : e.w;
    const H = swap ? e.w : e.h;
    const s = monoNew(W, H);
    for (let y = 0; y < H; y++) {
        for (let x = 0; x < W; x++) {
            let ex;
            let ey;
            if (r === 90) {
                ex = y;
                ey = e.h - 1 - x;
            } else if (r === 180) {
                ex = e.w - 1 - x;
                ey = e.h - 1 - y;
            } else {
                ex = e.w - 1 - y;
                ey = x;
            }
            s.bits[y * W + x] = e.bits[ey * e.w + ex];
        }
    }
    return s;
}

/** The thermal-head guard from job.c: a solid black bitmap gets a white dot
 *  every 16 rows in column 0 so the head is never driven fully on. */
export function guardAllBlack(m) {
    for (let i = 0; i < m.bits.length; i++) {
        if (!m.bits[i]) {
            return m;
        }
    }
    for (let y = 0; y < m.h; y += 16) {
        m.bits[y * m.w] = 0;
    }
    return m;
}

/** Pack rows MSB first. blackBit = the bit value that burns (PM220: 0). */
export function packMono(m, blackBit = 0) {
    const rb = (m.w + 7) >> 3;
    const out = new Uint8Array(rb * m.h);
    out.fill(blackBit ? 0x00 : 0xff);
    for (let y = 0; y < m.h; y++) {
        for (let x = 0; x < m.w; x++) {
            if (m.bits[y * m.w + x]) {
                const i = y * rb + (x >> 3);
                const b = 0x80 >> (x & 7);
                if (blackBit) {
                    out[i] |= b;
                } else {
                    out[i] &= ~b;
                }
            }
        }
    }
    return out;
}

export function unpackMono(bytes, w, h, blackBit = 0) {
    const m = monoNew(w, h);
    const rb = (w + 7) >> 3;
    for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
            const set = !!(bytes[y * rb + (x >> 3)] & (0x80 >> (x & 7)));
            m.bits[y * w + x] = set === !!blackBit ? 1 : 0;
        }
    }
    return m;
}

/** The job for one label. page = layout.pageFor(...), model = driver caps model.
 *  Models that place the bitmap themselves (TSPL BITMAP x,y; caps.placement
 *  true, the default) get the cropped bitmap and its origin. A model that
 *  can't place it gets a head-wide bitmap with the origin baked in. */
export function buildJobBitmap(editorMono, page, model) {
    const s = toHead(editorMono, page.orient || 0);
    const L = computeLayout(page.geo, page.cal);
    const outW = L.printW;
    const outH = L.printH;
    let out = monoNew(outW, outH);
    for (let y = 0; y < outH; y++) {
        const sy = y - L.shiftY;
        if (sy < 0 || sy >= s.h) {
            continue;
        }
        for (let x = 0; x < outW; x++) {
            const sx = x - L.shiftX;
            if (sx < 0 || sx >= s.w) {
                continue;
            }
            out.bits[y * outW + x] = s.bits[sy * s.w + sx];
        }
    }
    guardAllBlack(out);
    let originX = L.originX;
    let originY = L.originY;
    if (model && model.placement === false) {
        const head = model.head_dots || outW;
        const full = monoNew(head, originY + outH);
        for (let y = 0; y < outH; y++) {
            for (let x = 0; x < outW; x++) {
                const fx = x + originX;
                if (fx < head) {
                    full.bits[(y + originY) * head + fx] = out.bits[y * outW + x];
                }
            }
        }
        out = full;
        originX = 0;
        originY = 0;
    }
    const blackBit = model && model.black_bit != null ? model.black_bit : 0;
    return {
        width: out.w,
        rows: out.h,
        rowBytes: (out.w + 7) >> 3,
        origin: [originX, originY],
        bytes: packMono(out, blackBit),
        mono: out,
        layout: L,
    };
}
