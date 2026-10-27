import { test } from "node:test";
import assert from "node:assert/strict";
import { FORMAT, LEGACY_MEDIA, fromDocument, toDocument } from "../../web/doc.js";
import {
    advanceCounters, effectiveDefs, fillObjects, findVars, formatDate, mapColumns, resolveValues, substitute,
} from "../../web/vars.js";
import { guessDelimiter, parseCsv, parseTable } from "../../web/csv.js";
import { clashes, helpHtml, keyName, KEYMAP } from "../../web/keymap.js";

// ---- documents ----

test("a pm220 v2 label imports as a 50x30 PM220 label with its objects", () => {
    const pm = { format: "pm220-label", version: 2, media: { width_dots: 384, height_dots: 240 },
        objects: [{ id: 1, type: "text", x: 100, y: 50, width: 80, height: 40, text: "Hi" }] };
    const d = fromDocument(pm);
    assert.equal(d.legacy, true);
    assert.deepEqual(d.media, LEGACY_MEDIA);
    assert.deepEqual(d.model, { driver: "nelko_tspl", id: "pm220" });
    assert.equal(d.objects[0].x, 100);
    assert.equal(d.objects[0].ignoreSafe, false);
});

test("a pm220 v1 label (top-left origin, corner form) is converted to centres", () => {
    const d = fromDocument([{ x1: 10, y1: 20, x2: 49, y2: 39, text: "a" }]);
    assert.equal(d.legacy, true);
    assert.equal(d.objects[0].type, "text");
    assert.deepEqual([d.objects[0].x, d.objects[0].y, d.objects[0].width, d.objects[0].height], [30, 30, 40, 20]);
});

test("v3 documents round-trip media, orientation, model and variables", () => {
    const media = { id: "nelko-4x6", name: "4 × 6", w_mm: 101.6, h_mm: 152.4, gap_mm: 3, type: "gap" };
    const doc = toDocument({ page: { width_dots: 812, height_dots: 1218, dpi: 203 }, media, orient: 90,
        model: { driver: "nelko_pl70e", id: "pl70e", name: "x" }, vars: [{ name: "sku", type: "text" }],
        objects: [{ id: 3, type: "qr", x: 50, y: 50, width: 40, height: 40, payload: "{{sku}}", _pending: 1 }] });
    assert.equal(doc.format, FORMAT);
    assert.equal(doc.version, 3);
    assert.equal(doc.objects[0]._pending, undefined);
    const back = fromDocument(JSON.parse(JSON.stringify(doc)));
    assert.equal(back.legacy, false);
    assert.deepEqual(back.media, media);
    assert.equal(back.orient, 90);
    assert.deepEqual(back.model, { driver: "nelko_pl70e", id: "pl70e" });
    assert.deepEqual(back.vars, [{ name: "sku", type: "text" }]);
});

test("foreign files are refused", () => {
    assert.throws(() => fromDocument({ format: "something-else", objects: [] }), /not a label file/);
    assert.throws(() => fromDocument(null), /not a label file/);
});

// ---- variables ----

const OBJS = [
    { type: "text", text: "SKU {{sku}} lot {{lot}}" },
    { type: "qr", payload: "https://x/{{sku}}?n={{serial:6}}" },
    { type: "barcode1d", payload: "{{serial}}" },
    { type: "image", png: "" },
    { type: "text", text: "made {{made}} · {{n}} · {{date}}" },
];
const DEFS = [
    { name: "serial", type: "counter", start: 41, step: 2, pad: 4, prefix: "S" },
    { name: "made", type: "date", format: "DD/MM/YY" },
    { name: "lot", type: "number", decimals: 1 },
];

test("findVars lists placeholders in first-use order, skipping images", () => {
    assert.deepEqual(findVars(OBJS), ["sku", "lot", "serial", "made", "n", "date"]);
});

test("effectiveDefs adds plain text vars and leaves built-ins out", () => {
    const d = effectiveDefs(OBJS, DEFS);
    assert.deepEqual(d.map((x) => [x.name, x.type]), [["sku", "text"], ["lot", "number"], ["serial", "counter"], ["made", "date"]]);
});

test("values resolve per label: counters step, dates format, numbers round", () => {
    const defs = effectiveDefs(OBJS, DEFS);
    const now = new Date(2026, 9, 26, 9, 5, 7);
    const r0 = resolveValues(defs, { sku: "A-7", lot: "3.14159" }, 0, { now });
    const r2 = resolveValues(defs, { sku: "A-7", lot: "3.14159" }, 2, { now, counters: { serial: 100 } });
    const f0 = fillObjects(OBJS, r0);
    const f2 = fillObjects(OBJS, r2);
    assert.equal(f0[0].text, "SKU A-7 lot 3.1");
    assert.equal(f0[1].payload, "https://x/A-7?n=S000041");
    assert.equal(f0[2].payload, "S0041");
    assert.equal(f2[2].payload, "S0104");
    assert.equal(f0[4].text, "made 26/10/26 · 1 · 2026-10-26");
    assert.equal(f2[4].text, "made 26/10/26 · 3 · 2026-10-26");
    assert.equal(f0[3], OBJS[3]);
});

test("advanceCounters moves each counter by step × labels printed", () => {
    const defs = effectiveDefs(OBJS, DEFS);
    assert.deepEqual(advanceCounters(defs, {}, 5), { serial: 51 });
    assert.deepEqual(advanceCounters(defs, { serial: 100 }, 3), { serial: 106 });
});

test("formatDate tokens and literals", () => {
    const d = new Date(2026, 0, 5, 14, 3, 9);
    assert.equal(formatDate(d, "YYYY-MM-DD HH:mm:ss"), "2026-01-05 14:03:09");
    assert.equal(formatDate(d, "D/M/YY h:mm A"), "5/1/26 2:03 PM");
    assert.equal(formatDate(d, "ddd D MMM [at] HH"), "Mon 5 Jan at 14");
});

test("an unknown placeholder renders empty, text without placeholders is untouched", () => {
    assert.equal(substitute("a{{zz}}b", {}), "ab");
    assert.equal(substitute("plain", {}), "plain");
});

test("mapColumns matches headers to names or prompts, case- and space-insensitively", () => {
    const defs = [{ name: "sku" }, { name: "ship_to", label: "Ship To" }, { name: "qty" }];
    assert.deepEqual(mapColumns(["SKU", "ship to", "other"], defs), { sku: 0, ship_to: 1 });
});

// ---- CSV ----

test("CSV with quotes, embedded commas, newlines and doubled quotes", () => {
    const rows = parseCsv('a,b,c\r\n1,"x, y",3\n"multi\nline","he said ""hi""",\n');
    assert.deepEqual(rows, [["a", "b", "c"], ["1", "x, y", "3"], ["multi\nline", 'he said "hi"', ""]]);
});

test("delimiter guessing and a pasted list", () => {
    assert.equal(guessDelimiter("a\tb\tc"), "\t");
    assert.equal(guessDelimiter("a;b;c,d"), ";");
    assert.equal(guessDelimiter('"x,y";z'), ";");
    const t = parseTable("one\ntwo\n\nthree\n", { hasHeader: false });
    assert.deepEqual(t.header, ["col1"]);
    assert.deepEqual(t.rows, [["one"], ["two"], ["three"]]);
});

test("parseTable pads short rows and names blank headers", () => {
    const t = parseTable("﻿sku,,qty\nA,1\nB,2,3,4");
    assert.deepEqual(t.header, ["sku", "col2", "qty", "col4"]);
    assert.deepEqual(t.rows, [["A", "1", "", ""], ["B", "2", "3", "4"]]);
});

// ---- keys ----

test("no key binding clashes with another live in the same context", () => {
    assert.deepEqual(clashes(), []);
});

test("the clash check notices a clash", () => {
    const bad = KEYMAP.concat([{ ctx: "browse", keys: ["n"], action: "oops", label: "x", section: "x" }]);
    assert.ok(clashes(bad).some((c) => c.key === "n"));
    const bad2 = KEYMAP.concat([{ ctx: "object", keys: ["Ctrl+S"], action: "oops", label: "x", section: "x" }]);
    assert.ok(clashes(bad2).some((c) => c.key === "Ctrl+S"));
});

test("keyName", () => {
    assert.equal(keyName({ key: "s", ctrlKey: true, shiftKey: true }), "Ctrl+Shift+S");
    assert.equal(keyName({ key: "N", shiftKey: true }), "N");
    assert.equal(keyName({ key: "ArrowUp", shiftKey: true }), "Shift+Up");
    assert.equal(keyName({ key: " " }), "Space");
    assert.equal(keyName({ key: "Escape" }), "Esc");
    assert.equal(keyName({ key: "?", shiftKey: true }), "?");
});

test("every pm220 shortcut is still bound", () => {
    const has = (ctx, k) => KEYMAP.some((b) => b.ctx === ctx && b.keys.includes(k));
    for (const k of ["Ctrl+O", "Ctrl+S", "Ctrl+Shift+S", "Ctrl+P", "Ctrl+Z", "Ctrl+Shift+Z", "?", "F1"]) {
        assert.ok(has("global", k), k);
    }
    for (const k of ["n", "N", "Space", "Enter"]) {
        assert.ok(has("browse", k), k);
    }
    for (const k of ["m", "r", "d", "Delete", "Backspace", "Esc", "q"]) {
        assert.ok(has("object", k), k);
    }
    for (const k of ["h", "j", "k", "l", "Left", "Up", "Shift+Left"]) {
        assert.ok(has("nudge", k), k);
    }
    assert.ok(!has("global", "Ctrl+X"), "Ctrl+X no longer hijacks cut");
    assert.match(helpHtml(), /<section><h3>Document<\/h3>/);
});
