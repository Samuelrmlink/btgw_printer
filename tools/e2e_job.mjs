#!/usr/bin/env node
// Builds one print job the way the web app does (web/layout.js pageFor +
// web/pack.js buildJobBitmap, the same code the browser runs) for a test
// pattern, for tools/e2e_sim.py.
//
//   node tools/e2e_job.mjs <driver> <model> <media-id> <out.bin>
//
// Writes the packed bitmap to out.bin and prints one JSON line: the
// printer.print params (minus driver/addr/path) and the expected burned dots
// in head orientation as rows of '#'/'.' ("expect"), with the origin they go at.
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { pageFor } from "../web/layout.js";
import { buildJobBitmap, monoNew } from "../web/pack.js";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const [driver, modelId, mediaId, out] = process.argv.slice(2);
if (!out) {
    console.error("usage: e2e_job.mjs <driver> <model> <media-id> <out.bin>");
    process.exit(2);
}
const models = JSON.parse(readFileSync(join(ROOT, "web/presets/models.json"), "utf8"));
const drv = models.drivers.find((d) => d.driver === driver);
const model = drv && drv.models.find((m) => m.id === modelId);
if (!model) {
    console.error(`no ${driver}/${modelId} in web/presets/models.json`);
    process.exit(2);
}
let media = null;
const idx = JSON.parse(readFileSync(join(ROOT, "web/presets/media/index.json"), "utf8"));
for (const f of idx.files) {
    const d = JSON.parse(readFileSync(join(ROOT, "web/presets/media", f), "utf8"));
    media = media || d.media.find((m) => m.id === mediaId) || null;
}
if (!media) {
    console.error(`no media ${mediaId}`);
    process.exit(2);
}

const page = pageFor(model, media, null, media.orient || 0);
// The test pattern in editor orientation: a frame, both diagonals, a solid
// block in the top-left quarter, and a row of bars (text-like detail).
const W = page.width_dots;
const H = page.height_dots;
const e = monoNew(W, H);
const set = (x, y) => { if (x >= 0 && y >= 0 && x < W && y < H) { e.bits[y * W + x] = 1; } };
for (let x = 0; x < W; x++) { set(x, 0); set(x, 1); set(x, H - 1); set(x, H - 2); }
for (let y = 0; y < H; y++) { set(0, y); set(1, y); set(W - 1, y); set(W - 2, y); }
for (let i = 0; i < Math.max(W, H); i++) {
    set(Math.round(i * (W - 1) / Math.max(W, H)), Math.round(i * (H - 1) / Math.max(W, H)));
    set(W - 1 - Math.round(i * (W - 1) / Math.max(W, H)), Math.round(i * (H - 1) / Math.max(W, H)));
}
for (let y = 6; y < Math.floor(H / 4); y++) { for (let x = 6; x < Math.floor(W / 4); x++) { set(x, y); } }
for (let x = Math.floor(W / 2); x < W - 8; x += 6) {
    for (let y = Math.floor(H / 2); y < Math.floor(H / 2) + Math.max(4, Math.floor(H / 10)); y++) { set(x, y); set(x + 1, y); }
}

const job = buildJobBitmap(e, page, model);
writeFileSync(out, job.bytes);
const rows = [];
for (let y = 0; y < job.rows; y++) {
    let r = "";
    for (let x = 0; x < job.width; x++) { r += job.mono.bits[y * job.width + x] ? "#" : "."; }
    rows.push(r);
}
const params = {
    model: model.id,
    width_dots: job.width,
    rows: job.rows,
    origin: job.origin,
    media: { w_mm: media.w_mm, h_mm: media.h_mm, gap_mm: page.cal.gap_mm, type: media.type || "gap" },
    density: model.density ? model.density.default : -1,
    speed: -1,
    copies: 1,
    rotate: 0,
    options: {},
};
console.log(JSON.stringify({ params, bytes: job.bytes.length, expect: rows }));
