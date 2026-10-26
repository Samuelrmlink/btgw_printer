/** BTGW Printer: the label editor (ported from pm220_rp2_webapp's app.js) on
 * bluetooth_rp2_webapp, for any printer a driver module supports. */

import { connect as connectDevice, on, readJsonOr, refreshSd, whenInfo, ws, deviceLabel, sdAvailable } from "./device.js";
import { Editor, defaultBarcode, defaultImage, defaultQr, defaultText } from "./editor.js";
import { boxOverflows, rasterize, rotateSwapsAxes } from "./raster.js";
import { fromDocument, readFileText, toDocument } from "./doc.js";
import { bindPicker } from "./files.js";
import { bindNetwork } from "./network.js";
import { bindCalibrate, testFrameMono } from "./cal.js";
import { code128Error } from "./barcode.js";
import { qrError } from "./qr.js";
import { importImageFile } from "./image.js";
import { pageFor } from "./layout.js";
import {
    calibrationFor, currentPrinterAddr, defaultModel, findMedia, findModel, findPrinter, loadPresets,
    loadPrinters, loadUserMedia, mediaFor, onCatalog, saveCalibration,
} from "./catalog.js";
import { bindPrinters } from "./printers.js";
import { bindMedia } from "./media.js";
import { bindTemplates } from "./templates.js";
import { bindPrint } from "./print.js";
import { anyDialogOpen } from "./dialog.js";
import { actionFor, helpHtml } from "./keymap.js";
import { isNumField, parseIntValue, setBounds, stepNumField, wrapNumSpins, bindSpinHolds } from "./numfield.js";
import { effectiveDefs, fillObjects, hasVars, resolveValues } from "./vars.js";
import { counterStore } from "./templates.js";
import "./barcode.js";
import "./qr.js";
import "./image.js";

const SCALE_MAX = 2;
const STORE = "btgw-printer.editor.v1";
const LEGACY_STORE = "pm220-editor-v1";

const TITLES = {
    text: "Text box",
    qr: "QR code",
    barcode1d: "Barcode",
    image: "Image",
};

const $ = (id) => document.getElementById(id);

// ---- pm220: keyboard-nav detection and the mobile viewport lock ----
const NAV_KEYS = new Set([
    "Tab", "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight",
    "Home", "End", "PageUp", "PageDown", "F1",
]);

document.addEventListener("keydown", (e) => {
    if (NAV_KEYS.has(e.key) || e.key === "?" ) {
        document.documentElement.classList.add("kbd-nav");
        return;
    }
    if (e.ctrlKey || e.metaKey) {
        document.documentElement.classList.add("kbd-nav");
        return;
    }
    if (e.key.length === 1 && "hjklnNq".includes(e.key)) {
        document.documentElement.classList.add("kbd-nav");
    }
}, true);

const VIEWPORT_LOCK =
    "width=device-width, initial-scale=1, minimum-scale=1, maximum-scale=1, user-scalable=no, viewport-fit=cover";

function lockViewport() {
    const meta = document.querySelector('meta[name="viewport"]');
    if (!meta) {
        return;
    }
    meta.setAttribute(
        "content",
        "width=device-width, initial-scale=1, minimum-scale=1, maximum-scale=5, user-scalable=yes",
    );
    requestAnimationFrame(() => {
        meta.setAttribute("content", VIEWPORT_LOCK);
    });
}

lockViewport();
window.addEventListener("pageshow", lockViewport);
window.addEventListener("orientationchange", () => setTimeout(lockViewport, 50));

for (const ev of ["gesturestart", "gesturechange", "gestureend"]) {
    document.addEventListener(ev, (e) => e.preventDefault(), { passive: false });
}

document.addEventListener("touchstart", (e) => {
    if (e.touches.length > 1) {
        e.preventDefault();
    }
}, { passive: false });

document.addEventListener("touchmove", (e) => {
    if (e.touches.length > 1 || (typeof e.scale === "number" && e.scale !== 1)) {
        e.preventDefault();
    }
}, { passive: false });

let lastTapAt = 0;
document.addEventListener("touchend", (e) => {
    if (e.target && e.target.closest && e.target.closest("input, textarea, select, button, a")) {
        lastTapAt = Date.now();
        return;
    }
    const now = Date.now();
    if (now - lastTapAt < 350) {
        e.preventDefault();
    }
    lastTapAt = now;
}, { passive: false });

function setField(id, value) {
    const el = $(id);
    if (document.activeElement === el) {
        return;
    }
    el.value = value;
}

function parseIntField(id) {
    return parseIntValue($(id).value);
}

// Bounds for the object fields depend on the page; numfield asks through here
// when no static bound is set (calibration registers its own).
for (const [id, lo, hi] of [["size", 4, 240], ["bc-font", 4, 72], ["bc-off", -240, 240]]) {
    setBounds(id, lo, hi);
}

const preview = $("preview");
const pctx = preview.getContext("2d", { willReadFrequently: true });
pctx.imageSmoothingEnabled = false;

// ---- what the label is for ----
const state = {
    model: null,
    media: null,
    orient: 0,
    vars: [],
    picoName: "",
    sessionCal: new Map(),   // "model|media" -> calibration, when no printer is saved
};
let page = null;
let mono = null;

function calKey(model, media) {
    return `${model.key}|${media.id}`;
}

function currentPrinter() {
    const addr = currentPrinterAddr();
    return addr ? findPrinter(addr) : null;
}

function loadCal(addr, model, media) {
    if (addr && findPrinter(addr)) {
        return calibrationFor(addr, model, media);
    }
    return state.sessionCal.get(calKey(model, media)) || calibrationFor(null, model, media);
}

function computePage() {
    const p = currentPrinter();
    const cal = loadCal(p ? p.addr : null, state.model, state.media);
    page = pageFor(state.model, state.media, cal, state.orient);
    return page;
}

const editor = new Editor($("stage"), {
    page: { width_dots: 384, height_dots: 240, width_bytes: 48 },
    safe: { x0: 0, y0: 0, x1: 383, y1: 239 },
    scale: SCALE_MAX,
    onChange: () => {
        paint();
        fillForm();
        saveScratch();
        markNudgeFocus();
        updateStageKeys();
        if (isPayloadTyping()) {
            clearTimeout(histTypeTimer);
            histTypeTimer = setTimeout(histRecord, 400);
        } else {
            histRecord();
        }
    },
    onSelect: (box) => focusPayload(box),
});

const HIST_MAX = 50;
const history = { past: [], present: null, future: [], applying: false };
let histHold = false;
let histTypeTimer = 0;

function isPayloadTyping() {
    const el = document.activeElement;
    if (!el) {
        return false;
    }
    if (el.tagName === "TEXTAREA") {
        return true;
    }
    if (el.tagName === "INPUT" && el.type !== "checkbox" && el.type !== "button" && !isNumField(el)) {
        return true;
    }
    return false;
}

function histSnap() {
    return JSON.stringify({
        boxes: editor.boxes,
        selectedId: editor.selectedId,
    });
}

function histBoxesKey(snap) {
    if (!snap) {
        return "";
    }
    try {
        return JSON.stringify(JSON.parse(snap).boxes);
    } catch {
        return snap;
    }
}

function syncHistButtons() {
    const undo = $("undo");
    const redo = $("redo");
    if (undo) {
        undo.disabled = history.past.length === 0;
    }
    if (redo) {
        redo.disabled = history.future.length === 0;
    }
}

function histRecord() {
    if (history.applying || editor.drag || histHold) {
        return;
    }
    const snap = histSnap();
    if (histBoxesKey(snap) === histBoxesKey(history.present)) {
        history.present = snap;
        return;
    }
    if (history.present != null) {
        history.past.push(history.present);
        if (history.past.length > HIST_MAX) {
            history.past.shift();
        }
    }
    history.present = snap;
    history.future = [];
    syncHistButtons();
}

function histApply(snap) {
    const data = JSON.parse(snap);
    history.applying = true;
    editor.load(data.boxes);
    if (data.selectedId != null) {
        editor.select(data.selectedId);
    }
    history.applying = false;
}

function histUndo() {
    if (!history.past.length) {
        return;
    }
    history.future.push(histSnap());
    history.present = history.past.pop();
    histApply(history.present);
    syncHistButtons();
}

function histRedo() {
    if (!history.future.length) {
        return;
    }
    history.past.push(histSnap());
    history.present = history.future.pop();
    histApply(history.present);
    syncHistButtons();
}

function histReset() {
    history.past = [];
    history.future = [];
    history.present = histSnap();
    syncHistButtons();
}

function histBeginHold() {
    histHold = true;
}

function histEndHold() {
    if (!histHold) {
        return;
    }
    histHold = false;
    histRecord();
}

function rotationPatch(box, next) {
    const patch = { rotate: next };
    if (rotateSwapsAxes(next) !== rotateSwapsAxes(box.rotate)) {
        patch.width = box.height;
        patch.height = box.width;
    }
    return patch;
}

function cycleRotation() {
    const box = editor.selected();
    if (!box) {
        return;
    }
    const rots = [0, 90, 180, 270];
    const cur = Number(box.rotate) || 0;
    let i = rots.indexOf(cur);
    if (i < 0) {
        i = 0;
    }
    editor.updateSelected(rotationPatch(box, rots[(i + 1) % rots.length]));
}

function duplicateSelected() {
    const box = editor.selected();
    if (!box) {
        return;
    }
    const copy = structuredClone(box);
    delete copy.id;
    copy.x += 8;
    copy.y += 8;
    copy.pristine = false;
    editor.addBox(copy);
}

function enterMoveMode() {
    if (!editor.selected()) {
        return;
    }
    $("nudge-pad").focus();
    markNudgeFocus();
}

let helpIndex = 0;
let helpRegion = "list";

function helpIsOpen() {
    const el = $("help");
    return !!(el && !el.hidden);
}

function helpSections() {
    return [...document.querySelectorAll("#help .help-cols section")];
}

function helpColCount() {
    const cols = $("help")?.querySelector(".help-cols");
    if (!cols) {
        return 1;
    }
    const n = getComputedStyle(cols).gridTemplateColumns.split(" ").filter(Boolean).length;
    return n > 1 ? n : 1;
}

function paintHelpNav() {
    const secs = helpSections();
    if (helpIndex >= secs.length) {
        helpIndex = Math.max(0, secs.length - 1);
    }
    secs.forEach((s, i) => s.classList.toggle("current", helpRegion === "list" && i === helpIndex));
    const closeBtn = $("help-close");
    if (closeBtn) {
        closeBtn.classList.toggle("nav", helpRegion === "actions");
    }
    if (helpRegion === "list") {
        secs[helpIndex]?.scrollIntoView({ block: "nearest" });
    }
}

function openHelp() {
    const el = $("help");
    if (!el) {
        return;
    }
    el.hidden = false;
    helpIndex = 0;
    helpRegion = "list";
    const panel = el.querySelector(".picker-panel");
    if (panel) {
        panel.setAttribute("tabindex", "-1");
        panel.focus();
    }
    paintHelpNav();
}

function closeHelp() {
    const el = $("help");
    if (el) {
        el.hidden = true;
    }
}

function helpMove(dx, dy) {
    const secs = helpSections();
    const n = secs.length;
    if (!n) {
        return;
    }
    const cols = helpColCount();
    if (helpRegion === "actions") {
        if (dy < 0 || dx < 0) {
            helpRegion = "list";
            helpIndex = n - 1;
            paintHelpNav();
        }
        return;
    }
    const col = helpIndex % cols;
    const row = Math.floor(helpIndex / cols);
    if (dx) {
        const nextCol = col + dx;
        if (nextCol >= 0 && nextCol < cols) {
            const next = row * cols + nextCol;
            if (next < n) {
                helpIndex = next;
            }
        }
    } else if (dy > 0) {
        const next = helpIndex + cols;
        if (next < n) {
            helpIndex = next;
        } else {
            helpRegion = "actions";
        }
    } else if (dy < 0) {
        const next = helpIndex - cols;
        if (next >= 0) {
            helpIndex = next;
        }
    }
    paintHelpNav();
}

function overlayOpen() {
    return anyDialogOpen();
}

function updateStageKeys() {
    const el = $("stage-keys");
    if (!el) {
        return;
    }
    if (editor.selected()) {
        el.innerHTML =
            "<span><kbd>m</kbd> move</span>" +
            "<span><kbd>r</kbd> rotate</span>" +
            "<span><kbd>d</kbd> duplicate</span>" +
            "<span><kbd>←↑→↓</kbd> nudge</span>" +
            "<span><kbd>⇧</kbd>+arrows resize</span>" +
            "<span><kbd>Del</kbd> delete</span>" +
            "<span><kbd>Esc</kbd>/<kbd>q</kbd> deselect</span>" +
            "<span><kbd>?</kbd> all keys</span>";
    } else {
        el.innerHTML =
            "<span><kbd>n</kbd> next</span>" +
            "<span><kbd>N</kbd> prev</span>" +
            "<span><kbd>Space</kbd> select</span>" +
            "<span><kbd>?</kbd> all keys</span>";
    }
}

function viewScale() {
    const wrap = $("stage-wrap");
    if (!wrap || wrap.clientWidth < 32) {
        return SCALE_MAX;
    }
    const cs = getComputedStyle(wrap);
    const padX = parseFloat(cs.paddingLeft) + parseFloat(cs.paddingRight);
    const padY = parseFloat(cs.paddingTop) + parseFloat(cs.paddingBottom);
    const availW = Math.max(64, wrap.clientWidth - padX);
    const stacked = window.matchMedia("(max-width: 900px)").matches;
    let availH;
    if (stacked) {
        const headerH = document.querySelector("header").offsetHeight;
        availH = Math.max(80, window.innerHeight - headerH - padY - 72);
    } else {
        availH = Math.max(80, wrap.clientHeight - padY);
    }
    const s = Math.min(availW / page.width_dots, availH / page.height_dots);
    return Math.min(SCALE_MAX, Math.max(0.5, s));
}

function layoutGuides() {
    if (!page) {
        return;
    }
    const s = viewScale();
    editor.scale = s;
    const stage = $("stage");
    stage.style.width = `${page.width_dots * s}px`;
    stage.style.height = `${page.height_dots * s}px`;
    preview.style.width = `${page.width_dots * s}px`;
    preview.style.height = `${page.height_dots * s}px`;
    const r = page.safe_rect;
    const safe = $("safe");
    safe.style.left = `${r.x0 * s}px`;
    safe.style.top = `${r.y0 * s}px`;
    safe.style.width = `${(r.x1 - r.x0 + 1) * s}px`;
    safe.style.height = `${(r.y1 - r.y0 + 1) * s}px`;
    const c = page.crop_rect;
    const crop = $("crop");
    if (c) {
        crop.style.left = `${c.x0 * s}px`;
        crop.style.top = `${c.y0 * s}px`;
        crop.style.width = `${(c.x1 - c.x0 + 1) * s}px`;
        crop.style.height = `${(c.y1 - c.y0 + 1) * s}px`;
        crop.hidden = false;
    } else {
        crop.hidden = true;
    }
    editor.syncDom();
}

/** Re-derive the page (model, label size, orientation or calibration changed).
 *  With rescale, boxes keep their place proportionally on the new page. */
function applyPage({ rescale = false } = {}) {
    const old = page;
    computePage();
    preview.width = page.width_dots;
    preview.height = page.height_dots;
    $("media-name").textContent = state.media ? state.media.name : "–";
    if (rescale && old && editor.boxes.length && (old.width_dots !== page.width_dots || old.height_dots !== page.height_dots)) {
        const sx = page.width_dots / old.width_dots;
        const sy = page.height_dots / old.height_dots;
        const sz = Math.min(sx, sy);
        const boxes = editor.boxes.map((b) => {
            const n = { ...b, x: Math.round(b.x * sx), y: Math.round(b.y * sy), width: Math.round(b.width * sz), height: Math.round(b.height * sz) };
            if (n.type === "text" && n.size) {
                n.size = Math.max(4, Math.round(n.size * sz));
            }
            if (n.type === "barcode1d" && n.textSize) {
                n.textSize = Math.max(4, Math.round(n.textSize * sz));
            }
            return n;
        });
        editor.page = page;
        editor.safe = page.safe_rect;
        editor.load(boxes);
    } else {
        editor.setPage(page, page.safe_rect);
    }
    layoutGuides();
    paint();
    const over = editor.boxes.filter((b) => boxOverflows(b, editor.safe)).length;
    if (rescale && over) {
        setStatus(`${over} object(s) now extend outside the printable area`, "err");
    }
}

function setModel(model, { keepMedia = false } = {}) {
    state.model = model;
    // Keep the label size if it is meant for this model, or if it came with a
    // document and is not in the library at all.
    const keep = keepMedia && state.media &&
        (mediaFor(model).some((m) => m.id === state.media.id) || !findMedia(state.media.id));
    if (!keep) {
        const list = mediaFor(model);
        const pref = (model.presets || []).map((id) => findMedia(id)).find(Boolean);
        const m = pref || list[0] || state.media;
        if (m && (!state.media || m.id !== state.media.id)) {
            state.media = m;
            state.orient = m.orient || 0;
        }
    }
}

function render(objects) {
    const r = rasterize(page, objects, () => paint());
    return r.mono;
}

function paint() {
    if (!page) {
        return;
    }
    mono = render(editor.boxes);
    const w = page.width_dots;
    const h = page.height_dots;
    const img = pctx.createImageData(w, h);
    const d = img.data;
    for (let i = 0, p = 0; i < mono.bits.length; i++, p += 4) {
        const v = mono.bits[i] ? 0 : 255;
        d[p] = d[p + 1] = d[p + 2] = v;
        d[p + 3] = 255;
    }
    pctx.putImageData(img, 0, 0);
}

function payloadEl(box) {
    if (!box) {
        return null;
    }
    if (box.type === "qr") {
        return $("qr-payload");
    }
    if (box.type === "barcode1d") {
        return $("bc-payload");
    }
    if (box.type === "text") {
        return $("text");
    }
    return null;
}

function focusPayload(box) {
    if (!box) {
        return;
    }
    setTimeout(() => {
        $("nudge-pad").focus();
        markNudgeFocus();
    }, 0);
}

function isNudgeFocused() {
    const pad = $("nudge-pad");
    const el = document.activeElement;
    return el === pad || (pad && pad.contains(el));
}

function markNudgeFocus() {
    const on = isNudgeFocused();
    const boxEl = document.querySelector(".tbox.selected");
    if (boxEl) {
        boxEl.classList.toggle("nudge", on);
    }
}

function nudgeSelected(dx, dy) {
    const box = editor.selected();
    if (!box) {
        return;
    }
    editor.updateSelected({ x: box.x + dx, y: box.y + dy });
}

function resizeSelected(dw, dh) {
    const box = editor.selected();
    if (!box) {
        return;
    }
    if (box.type === "qr") {
        const d = dw !== 0 ? dw : dh;
        editor.updateSelected({ width: box.width + d, height: box.height + d });
        return;
    }
    editor.updateSelected({ width: box.width + dw, height: box.height + dh });
}

function isShownControl(el) {
    if (!el || el.disabled || el.tabIndex < 0) {
        return false;
    }
    if (el.closest("[hidden]")) {
        return false;
    }
    const st = getComputedStyle(el);
    if (st.display === "none" || st.visibility === "hidden") {
        return false;
    }
    return true;
}

function editorTabOrder(box) {
    const pad = $("nudge-pad");
    const pay = payloadEl(box);
    const skip = new Set(["font", "del", "pane-close"]);
    const rest = [];
    const root = $("editor-pane");
    for (const el of root.querySelectorAll("input, select, textarea, button, [tabindex]")) {
        if (el === pad || el === pay || skip.has(el.id) || el.classList.contains("nudge-btn")) {
            continue;
        }
        if (!isShownControl(el)) {
            continue;
        }
        rest.push(el);
    }
    const order = [];
    order.push(pad);
    if (pay) {
        order.push(pay);
    }
    order.push(...rest);
    return order;
}

function moveEditorFocus(dir) {
    const box = editor.selected();
    if (!box) {
        return false;
    }
    const order = editorTabOrder(box);
    if (!order.length) {
        return false;
    }
    const active = document.activeElement;
    let i = order.findIndex((el) => el === active || el.contains(active));
    if (i < 0) {
        i = dir > 0 ? -1 : 0;
    }
    const next = order[(i + dir + order.length) % order.length];
    if (next) {
        next.focus();
        markNudgeFocus();
        return true;
    }
    return false;
}

function payloadWarning(box) {
    if (!box) {
        return "";
    }
    if (box.type === "qr") {
        return qrError(box.payload);
    }
    if (box.type === "barcode1d") {
        return code128Error(box.payload);
    }
    return "";
}

function fillForm() {
    const box = editor.selected();
    $("add-pane").hidden = !!box;
    $("editor-pane").hidden = !box;
    $("del").disabled = !box;
    const paneHead = $("pane-head");
    const paneClose = $("pane-close");
    const paneTitle = $("pane-title");
    if (paneHead) {
        paneHead.hidden = !box;
    }
    if (paneClose) {
        paneClose.hidden = !box;
    }
    if (paneTitle) {
        paneTitle.textContent = box ? (TITLES[box.type] || box.type) : "Add object";
    }
    const clip = !!(box && box.type === "text" && !box.wrap && !box.autoSize);
    const overflow = !!(box && boxOverflows(box, editor.safe));
    const payload = payloadWarning(box);
    $("warn-clip").hidden = !clip;
    $("warn-overflow").hidden = !overflow;
    $("warn-payload").hidden = !payload;
    $("warn-payload").textContent = payload || "";
    $("warn").hidden = !clip && !overflow && !payload;
    $("form-text").hidden = !box || box.type !== "text";
    $("form-qr").hidden = !box || box.type !== "qr";
    $("form-barcode").hidden = !box || box.type !== "barcode1d";
    $("form-image").hidden = !box || box.type !== "image";
    $("advanced-qr").hidden = !$("advanced").checked || !box || box.type !== "qr";
    if (!box) {
        return;
    }
    $("rotate").value = String(box.rotate || 0);
    $("ignore-safe").checked = !!box.ignoreSafe;
    setField("ox", box.x);
    setField("oy", box.y);
    setField("ow", box.width);
    setField("oh", box.height);
    if (box.type === "text") {
        $("size").disabled = !!box.autoSize;
        setField("text", box.text);
        $("font").value = box.font || "Arial";
        setField("size", box.size);
        $("autoSize").checked = !!box.autoSize;
        $("wrap").checked = !!box.wrap;
        $("bold").checked = !!box.bold;
        $("italic").checked = !!box.italic;
        $("underline").checked = !!box.underline;
        $("align").value = box.align;
        $("valign").value = box.valign;
    } else if (box.type === "qr") {
        setField("qr-payload", box.payload || "");
        $("qr-pixel").checked = !!box.pixelPerfect;
        $("qr-ecc").value = box.ecc || "M";
    } else if (box.type === "barcode1d") {
        setField("bc-payload", box.payload || "");
        $("bc-sym").value = box.symbology || "code128";
        $("bc-text").checked = box.showText !== false;
        setField("bc-font", box.textSize ?? 12);
        setField("bc-off", box.textOffset ?? 7);
    } else if (box.type === "image") {
        $("img-dither").checked = box.dither !== false;
    }
}

function readForm() {
    const box = editor.selected();
    if (!box) {
        return {};
    }
    const newRot = Number($("rotate").value);
    const patch = {
        rotate: newRot,
        ignoreSafe: $("ignore-safe").checked,
    };
    const x = parseIntField("ox");
    const y = parseIntField("oy");
    const w = parseIntField("ow");
    const h = parseIntField("oh");
    if (x != null) {
        patch.x = x;
    }
    if (y != null) {
        patch.y = y;
    }
    if (w != null) {
        patch.width = w;
    }
    if (h != null) {
        patch.height = h;
    }
    const swapped = rotationPatch(box, newRot);
    if (swapped.width != null) {
        patch.width = swapped.width;
        patch.height = swapped.height;
    }
    if (box.type === "text") {
        Object.assign(patch, {
            text: $("text").value,
            font: $("font").value,
            autoSize: $("autoSize").checked,
            wrap: $("wrap").checked,
            bold: $("bold").checked,
            italic: $("italic").checked,
            underline: $("underline").checked,
            align: $("align").value,
            valign: $("valign").value,
        });
        const size = parseIntField("size");
        if (size != null) {
            patch.size = size;
        }
    } else if (box.type === "qr") {
        Object.assign(patch, {
            payload: $("qr-payload").value,
            ecc: $("qr-ecc").value,
            pixelPerfect: $("qr-pixel").checked,
        });
        if (patch.width != null && patch.width !== patch.height) {
            patch.height = patch.width;
        }
    } else if (box.type === "barcode1d") {
        Object.assign(patch, {
            payload: $("bc-payload").value,
            symbology: $("bc-sym").value,
            showText: $("bc-text").checked,
        });
        const textSize = parseIntField("bc-font");
        const textOffset = parseIntField("bc-off");
        if (textSize != null) {
            patch.textSize = textSize;
        }
        if (textOffset != null) {
            patch.textOffset = textOffset;
        }
    } else if (box.type === "image") {
        patch.dither = $("img-dither").checked;
    }
    return patch;
}

function scratchDoc() {
    return toDocument({ page, objects: editor.boxes, media: state.media, orient: state.orient, model: state.model, vars: state.vars });
}

function saveScratch() {
    if (!page) {
        return;
    }
    try {
        localStorage.setItem(STORE, JSON.stringify(scratchDoc()));
    } catch {
        /* quota */
    }
}

/** The last session's label, or a pm220_rp2_webapp one left in this browser. */
function restoreScratch() {
    let raw = null;
    try {
        raw = localStorage.getItem(STORE);
        if (!raw) {
            raw = localStorage.getItem(LEGACY_STORE);
            if (raw) {
                localStorage.setItem(STORE + "-advanced", localStorage.getItem(LEGACY_STORE + "-advanced") || "0");
            }
        }
    } catch {
        return null;
    }
    if (!raw) {
        return null;
    }
    try {
        return fromDocument(JSON.parse(raw));
    } catch {
        return null;
    }
}

/** Load a parsed document: its model and label size first (so the boxes are
 *  clamped to the right page, which pm220 got wrong on boot), then the boxes. */
function loadDocument(d) {
    const model = (d.model && findModel(d.model)) || state.model || defaultModel();
    state.model = model;
    let media = d.media && (findMedia(d.media.id) || null);
    if (!media && d.media && d.media.w_mm) {
        media = { ...d.media, id: d.media.id || `doc-${d.media.w_mm}x${d.media.h_mm}`, name: d.media.name || `${d.media.w_mm} × ${d.media.h_mm} mm` };
    }
    if (media) {
        state.media = media;
        state.orient = d.orient || 0;
    } else {
        setModel(model);
    }
    state.vars = d.vars || [];
    computePage();
    editor.page = page;
    editor.safe = page.safe_rect;
    applyPage();
    editor.load(d.objects);
}

function syncAdvanced() {
    const on = $("advanced").checked;
    const box = editor.selected();
    $("coords").hidden = !on;
    $("advanced-qr").hidden = !on || !box || box.type !== "qr";
}

$("editor-pane").addEventListener("input", (e) => {
    const box = editor.selected();
    if (!box) {
        return;
    }
    if (e.target && (e.target.id === "text" || e.target.id === "qr-payload" || e.target.id === "bc-payload")) {
        box.pristine = false;
    }
    if (e.target && e.target.id === "advanced") {
        try {
            localStorage.setItem(STORE + "-advanced", $("advanced").checked ? "1" : "0");
        } catch {
            /* ignore */
        }
        syncAdvanced();
        return;
    }
    editor.updateSelected(readForm());
});

$("editor-pane").addEventListener("focusout", () => {
    setTimeout(fillForm, 0);
});

$("add-text").addEventListener("click", () => editor.addBox(defaultText(page, editor.safe)));
$("add-qr").addEventListener("click", () => editor.addBox(defaultQr(page, editor.safe)));
$("add-barcode").addEventListener("click", () => editor.addBox(defaultBarcode(page, editor.safe)));
$("add-image").addEventListener("click", () => $("image-file").click());
$("replace-image").addEventListener("click", () => $("image-file").click());

$("image-file").addEventListener("change", async (e) => {
    const file = e.target.files && e.target.files[0];
    e.target.value = "";
    if (!file) {
        return;
    }
    try {
        const { png } = await importImageFile(file, () => paint(), Math.max(page.width_dots, page.height_dots));
        const sel = editor.selected();
        if (sel && sel.type === "image") {
            editor.updateSelected({ png });
        } else {
            editor.addBox(defaultImage(page, editor.safe, png));
        }
    } catch (err) {
        setStatus(String(err.message || err), "err");
    }
});

$("del").addEventListener("click", () => editor.removeSelected());
$("dup").addEventListener("click", () => duplicateSelected());
$("undo").addEventListener("click", () => histUndo());
$("redo").addEventListener("click", () => histRedo());
$("help-btn").addEventListener("click", () => openHelp());
$("help-close").addEventListener("click", () => closeHelp());
$("help-backdrop").addEventListener("click", () => closeHelp());
$("pane-close")?.addEventListener("click", () => editor.select(null));

$("obj-next").addEventListener("click", () => editor.cyclePreview(1));
$("obj-prev").addEventListener("click", () => editor.cyclePreview(-1));
$("obj-select").addEventListener("click", () => editor.confirmPreview());

// ---- dialogs ----
let picoStore = "flash";
const picker = bindPicker({
    getDoc: () => scratchDoc(),
    loadDoc: (d) => {
        loadDocument(d);
        histReset();
    },
    picoName: () => state.picoName,
    setPicoName: (name, store) => {
        state.picoName = name;
        if (store) {
            picoStore = store;
        }
        document.title = name ? `${name} · BTGW Printer` : "BTGW Printer";
    },
    setStatus,
});
const netUi = bindNetwork({ setStatus });

function current() {
    const printer = currentPrinter();
    return {
        printer,
        addr: printer ? printer.addr : null,
        model: state.model,
        media: state.media,
        orient: state.orient,
        page,
        objects: editor.boxes,
        vars: state.vars,
        docName: state.picoName,
    };
}

const printersUi = bindPrinters({
    setStatus,
    onChange: (o) => {
        if (o && o.printerChanged) {
            const p = currentPrinter();
            const m = p && findModel({ driver: p.driver, id: p.model });
            if (m && (!state.model || m.key !== state.model.key)) {
                setModel(m, { keepMedia: true });
                applyPage({ rescale: true });
            } else {
                applyPage();
            }
        }
        refreshPrinterUi();
    },
});
const mediaUi = bindMedia({
    current,
    setStatus,
    choose: (m) => {
        state.media = m;
        state.orient = m.orient || 0;
        applyPage({ rescale: true });
        histRecord();
    },
    setOrient: (deg) => {
        state.orient = deg;
        applyPage({ rescale: true });
    },
});
const calUi = bindCalibrate({
    setStatus,
    current,
    loadCal,
    saveCal: async (addr, media, cal) => {
        if (addr && findPrinter(addr)) {
            await saveCalibration(addr, media, cal);
        } else {
            state.sessionCal.set(calKey(state.model, media), cal);
        }
    },
    applied: () => applyPage(),
    testPrint: async (cal) => {
        const pg = pageFor(state.model, state.media, cal, 0);
        await printer.printHeadMono(testFrameMono(pg.geo, pg.cal), pg);
    },
});
const templ = bindTemplates({
    objects: () => editor.boxes,
    vars: () => state.vars,
    setVars: (defs) => {
        state.vars = defs;
        saveScratch();
    },
    docName: () => state.picoName,
    setStatus,
    printLabels: (list) => printer.printLabels(list),
    renderPreview: (canvas, values, index) => renderPreview(canvas, values, index),
});
const printer = bindPrint({
    current,
    render,
    setStatus,
    isConnected: (addr) => {
        const st = printersUi.stateOf(addr).state;
        return st === "connected" || st === "printing" || st === "idle";
    },
    openPrinters: () => printersUi.open(),
    changed: () => refreshPrinterUi(),
});


function renderPreview(canvas, values, index) {
    const defs = effectiveDefs(editor.boxes, state.vars);
    const r = resolveValues(defs, values, index, { counters: counterStore(state.picoName).get() });
    const m = render(fillObjects(editor.boxes, r));
    canvas.width = m.w;
    canvas.height = m.h;
    const c = canvas.getContext("2d");
    const img = c.createImageData(m.w, m.h);
    for (let i = 0, p = 0; i < m.bits.length; i++, p += 4) {
        const v = m.bits[i] ? 0 : 255;
        img.data[p] = img.data[p + 1] = img.data[p + 2] = v;
        img.data[p + 3] = 255;
    }
    c.putImageData(img, 0, 0);
}

async function sendPrint() {
    if (printer.busy()) {
        return;
    }
    if (hasVars(editor.boxes)) {
        const r = await templ.promptValues();
        if (!r) {
            return;
        }
        await printer.printLabels([{ values: r.values, copies: r.count }]);
        return;
    }
    const p = currentPrinter();
    await printer.printLabels([{ values: {}, copies: (p && p.copies) || 1 }]);
}

$("print").addEventListener("click", () => sendPrint());
$("mode-switch").addEventListener("click", () => {
    location.href = "/";
});

$("file").addEventListener("change", async (e) => {
    const file = e.target.files && e.target.files[0];
    e.target.value = "";
    if (picker && picker.close) {
        picker.close();
    }
    if (!file) {
        return;
    }
    try {
        const d = fromDocument(JSON.parse(await readFileText(file)));
        loadDocument(d);
        histReset();
        state.picoName = file.name.replace(/\.gz$/i, "").replace(/\.pm220\.json$/i, ".label.json");
        setStatus(`opened ${file.name}${d.legacy ? " (pm220 label, 50 × 30 mm)" : ""}`, "ok");
    } catch (err) {
        setStatus(String(err.message || err), "err");
    }
});

// ---- keyboard (pm220 handler, map in keymap.js) ----
function runAction(action) {
    switch (action) {
        case "open":
            picker.open("open");
            return true;
        case "save":
            if (!state.picoName) {
                picker.open("save");
            } else {
                picker.saveCurrent();
            }
            return true;
        case "saveAs":
            picker.open("save");
            return true;
        case "print":
            sendPrint();
            return true;
        case "batch":
            templ.openBatch();
            return true;
        case "printers":
            printersUi.open();
            return true;
        case "media":
            mediaUi.open();
            return true;
        case "vars":
            templ.openVars();
            return true;
        case "calibrate":
            calUi.open();
            return true;
        case "network":
            netUi.open();
            return true;
        case "addText":
            editor.addBox(defaultText(page, editor.safe));
            return true;
        default:
            return false;
    }
}

document.addEventListener("keydown", (e) => {
    if (helpIsOpen()) {
        if (e.key === "Escape" || e.key === "q" || e.key === "?" || e.key === "F1") {
            e.preventDefault();
            closeHelp();
            return;
        }
        if (e.key === "Tab") {
            e.preventDefault();
            helpRegion = e.shiftKey
                ? (helpRegion === "actions" ? "list" : "actions")
                : (helpRegion === "list" ? "actions" : "list");
            paintHelpNav();
            return;
        }
        if (e.key === "ArrowDown" || e.key === "j") {
            e.preventDefault();
            helpMove(0, 1);
            return;
        }
        if (e.key === "ArrowUp" || e.key === "k") {
            e.preventDefault();
            helpMove(0, -1);
            return;
        }
        if (e.key === "ArrowRight" || e.key === "l") {
            e.preventDefault();
            helpMove(1, 0);
            return;
        }
        if (e.key === "ArrowLeft" || e.key === "h") {
            e.preventDefault();
            helpMove(-1, 0);
            return;
        }
        if (e.key === "Enter" || e.key === " ") {
            e.preventDefault();
            if (helpRegion === "actions") {
                closeHelp();
            }
        }
        return;
    }
    if (overlayOpen()) {
        return;
    }
    const g = actionFor("global", e);
    if (g === "undo" || g === "redo") {
        if (isPayloadTyping()) {
            return;   // the browser's own undo while typing
        }
        e.preventDefault();
        if (g === "undo") {
            histUndo();
        } else {
            histRedo();
        }
        return;
    }
    if (g === "help") {
        if (isPayloadTyping()) {
            return;
        }
        e.preventDefault();
        openHelp();
        return;
    }
    if (g && runAction(g)) {
        e.preventDefault();
        return;
    }
    const box = editor.selected();
    if (box && e.key === "Tab") {
        e.preventDefault();
        moveEditorFocus(e.shiftKey ? -1 : 1);
        return;
    }
    const typing = e.target && (
        e.target.tagName === "TEXTAREA" ||
        (e.target.tagName === "INPUT" && !isNumField(e.target) && e.target.type !== "checkbox")
    );
    if (!typing && !e.ctrlKey && !e.metaKey && (e.key === "n" || e.key === "N")) {
        e.preventDefault();
        if (box) {
            moveEditorFocus(e.key === "N" ? -1 : 1);
        } else {
            editor.cyclePreview(e.key === "N" ? -1 : 1);
        }
        return;
    }
    const tag = e.target && e.target.tagName;
    const onControl = tag === "BUTTON" || tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT";
    if (!box && !onControl && !e.ctrlKey && !e.metaKey && !e.altKey) {
        const a = actionFor("browse", e);
        if (a === "select") {
            e.preventDefault();
            editor.confirmPreview();
            return;
        }
        if (a && runAction(a)) {
            e.preventDefault();
            return;
        }
    }
    const inField = e.target && (e.target.tagName === "TEXTAREA" || e.target.tagName === "INPUT");
    if (inField) {
        if (isNumField(e.target) && !e.target.disabled) {
            let d = 0;
            if (e.key === "ArrowUp" || e.key === "k") {
                d = e.shiftKey ? 8 : 1;
            } else if (e.key === "ArrowDown" || e.key === "j") {
                d = e.shiftKey ? -8 : -1;
            } else if (e.key === "K") {
                d = 8;
            } else if (e.key === "J") {
                d = -8;
            }
            if (d) {
                e.preventDefault();
                stepNumField(e.target, d);
                return;
            }
        }
        if (e.key === "Escape" || (e.key === "q" && !isPayloadTyping())) {
            e.target.blur();
            editor.select(null);
        }
        // Delete and Backspace edit the field; they never delete the object
        // (pm220 deleted the whole object on Delete inside the text box).
        return;
    }
    if (isNudgeFocused() && box) {
        const keys = {
            ArrowLeft: [-1, 0],
            ArrowRight: [1, 0],
            ArrowUp: [0, -1],
            ArrowDown: [0, 1],
            h: [-1, 0],
            l: [1, 0],
            k: [0, -1],
            j: [0, 1],
            H: [-1, 0],
            L: [1, 0],
            K: [0, -1],
            J: [0, 1],
        };
        const delta = keys[e.key];
        if (delta) {
            e.preventDefault();
            histBeginHold();
            if (e.shiftKey) {
                resizeSelected(delta[0], delta[1]);
            } else {
                nudgeSelected(delta[0], delta[1]);
            }
            return;
        }
        if (e.key === "Backspace") {
            e.preventDefault();
            return;
        }
    }
    if (!box) {
        return;
    }
    const a = actionFor("object", e);
    if (a === "delete") {
        e.preventDefault();
        editor.removeSelected();
    } else if (a === "duplicate") {
        e.preventDefault();
        duplicateSelected();
    } else if (a === "move") {
        e.preventDefault();
        enterMoveMode();
    } else if (a === "rotate") {
        e.preventDefault();
        cycleRotation();
    } else if (a === "deselect") {
        editor.select(null);
    }
});

document.addEventListener("keyup", (e) => {
    if (histHold && (e.key === "ArrowLeft" || e.key === "ArrowRight" || e.key === "ArrowUp" ||
            e.key === "ArrowDown" || "hjklHJKL".includes(e.key))) {
        histEndHold();
    }
});

$("nudge-pad").addEventListener("focusin", markNudgeFocus);
$("nudge-pad").addEventListener("focusout", () => setTimeout(markNudgeFocus, 0));

{
    let holdWait = 0;
    let holdRep = 0;
    const stopHold = () => {
        clearTimeout(holdWait);
        clearInterval(holdRep);
        holdWait = 0;
        holdRep = 0;
        histEndHold();
    };
    $("nudge-pad").addEventListener("pointerdown", (e) => {
        const btn = e.target.closest(".nudge-btn");
        if (!btn || !editor.selected()) {
            return;
        }
        e.preventDefault();
        const dx = Number(btn.dataset.dx) || 0;
        const dy = Number(btn.dataset.dy) || 0;
        $("nudge-pad").focus();
        histBeginHold();
        nudgeSelected(dx, dy);
        clearTimeout(holdWait);
        clearInterval(holdRep);
        holdWait = setTimeout(() => {
            holdRep = setInterval(() => nudgeSelected(dx, dy), 50);
        }, 300);
        btn.setPointerCapture?.(e.pointerId);
    });
    $("nudge-pad").addEventListener("pointerup", stopHold);
    $("nudge-pad").addEventListener("pointercancel", stopHold);
    $("nudge-pad").addEventListener("lostpointercapture", stopHold);
}

{
    let holdWait = 0;
    let holdRep = 0;
    const stopHold = () => {
        clearTimeout(holdWait);
        clearInterval(holdRep);
        holdWait = 0;
        holdRep = 0;
        histEndHold();
    };
    $("resize-pad").addEventListener("pointerdown", (e) => {
        const btn = e.target.closest(".nudge-btn");
        if (!btn || !editor.selected()) {
            return;
        }
        e.preventDefault();
        const dw = Number(btn.dataset.dw) || 0;
        const dh = Number(btn.dataset.dh) || 0;
        histBeginHold();
        resizeSelected(dw, dh);
        clearTimeout(holdWait);
        clearInterval(holdRep);
        holdWait = setTimeout(() => {
            holdRep = setInterval(() => resizeSelected(dw, dh), 50);
        }, 300);
        btn.setPointerCapture?.(e.pointerId);
    });
    $("resize-pad").addEventListener("pointerup", stopHold);
    $("resize-pad").addEventListener("pointercancel", stopHold);
    $("resize-pad").addEventListener("lostpointercapture", stopHold);
}

wrapNumSpins("aside input[inputmode='numeric'], #cal input[inputmode='numeric'], #cal input[inputmode='decimal']");
bindSpinHolds();

// A message (saved, printed, an error) stays up a while before the line goes
// back to showing the printer's state.
const MSG_MS = 8000;
let msgAt = 0;
function setStatus(msg, kind) {
    msgAt = Date.now();
    showStatus(msg, kind);
}
function showStatus(msg, kind) {
    const el = $("status");
    el.textContent = msg;
    el.className = kind || "";
}

// ---- printer / board status ----
function refreshPrinterUi() {
    const p = currentPrinter();
    $("printer-name").textContent = p ? (p.nickname || p.addr) : (state.model ? `${state.model.name} (design)` : "none");
    const st = p ? printersUi.stateOf(p.addr).state : null;
    const up = st === "connected" || st === "idle" || st === "printing";
    $("printer-btn").classList.toggle("ok", !!up);
    $("print").disabled = !up || printer.busy();
    if (Date.now() - msgAt < MSG_MS) {
        return;
    }
    if (!ws.connected) {
        showStatus(`board unreachable (${deviceLabel()})`, "err");
    } else if (!printersUi.hubUp()) {
        showStatus(`no printer_hub on ${deviceLabel()} · designing only`, "");
    } else if (!p) {
        showStatus("no printer selected", "");
    } else {
        showStatus(`${p.nickname || p.addr}: ${printersUi.stateText(p.addr)}`, up ? "ok" : "");
    }
}

let statusTimer = 0;
function scheduleStatus() {
    clearTimeout(statusTimer);
    statusTimer = setTimeout(async () => {
        if (ws.connected && !document.hidden) {
            await printersUi.refreshStatus();
        }
        refreshPrinterUi();
        scheduleStatus();
    }, document.hidden ? 20000 : 10000);
}
document.addEventListener("visibilitychange", () => scheduleStatus());

// mode.json sits next to the app (tools/deploy.py), wherever it is served
// from: /printer/ on flash or the card's static route. Older deploys only
// wrote it to flash, so that is the fallback.
async function readMode() {
    try {
        const r = await fetch("mode.json", { cache: "no-store" });
        if (r.ok) {
            return await r.json();
        }
    } catch {
        // not served here: fall through
    }
    return readJsonOr("printer/mode.json", null).catch(() => null);
}

async function onBoard() {
    await whenInfo();
    await refreshSd().catch(() => false);
    const mode = await readMode();
    $("mode-switch").hidden = !(mode && mode.mode === "companion");
    await Promise.all([loadUserMedia(), loadPrinters()]);
    await printersUi.refreshDrivers();
    // The selected printer decides the model; a live driver's caps replace the
    // shipped ones (same key, maybe different numbers).
    const p = currentPrinter();
    const m = (p && findModel({ driver: p.driver, id: p.model })) || (state.model && findModel(state.model.key)) || defaultModel();
    const switched = !state.model || m.key !== state.model.key;
    setModel(m, { keepMedia: true });
    applyPage({ rescale: switched });
    await printersUi.refreshStatus();
    printersUi.autoConnect();
    refreshPrinterUi();
    state.boardReady = true;
}

ws.on("_open", () => {
    onBoard().catch((err) => setStatus(String(err.message || err), "err"));
});
ws.on("_close", () => refreshPrinterUi());
onCatalog(() => {
    if (state.media && !findMedia(state.media.id) && !String(state.media.id).startsWith("doc-")) {
        // the size was deleted: keep the label as it is, but stop naming a size that is gone
    }
});

async function boot() {
    $("help-cols").innerHTML = helpHtml();
    try {
        $("advanced").checked = localStorage.getItem(STORE + "-advanced") === "1";
    } catch {
        $("advanced").checked = false;
    }
    syncAdvanced();
    await loadPresets();
    const d = restoreScratch();
    if (d) {
        loadDocument(d);
    } else {
        setModel(defaultModel());
        applyPage();
    }
    histReset();
    updateStageKeys();
    refreshPrinterUi();
    layoutGuides();
    new ResizeObserver(() => layoutGuides()).observe($("stage-wrap"));
    window.addEventListener("resize", () => layoutGuides());
    connectDevice();
    scheduleStatus();
}

boot().catch((err) => {
    setStatus(String(err.message || err), "err");
});

// For tests and the console.
window.btgwPrinter = { editor, state, current, page: () => page, applyPage, sdAvailable };
