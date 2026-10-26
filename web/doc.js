/** Label document: one JSON file (format btgw-printer-label). Templates are the same files with {{vars}}. */

import { geom } from "./raster.js";

export const FORMAT = "btgw-printer-label";
export const VERSION = 3;

function adoptLegacyCorners(obj) {
    if (obj.x == null && obj.x1 != null) {
        obj.x = obj.x1;
        obj.y = obj.y1;
        obj.width = obj.x2 - obj.x1 + 1;
        obj.height = obj.y2 - obj.y1 + 1;
    }
}

function aabbOriginToCenter(obj) {
    const w = Math.max(1, Number(obj.width) || 1);
    const h = Math.max(1, Number(obj.height) || 1);
    obj.x = Math.round((Number(obj.x) || 0) + w / 2);
    obj.y = Math.round((Number(obj.y) || 0) + h / 2);
}

export function migrateObject(raw, docVersion = VERSION) {
    const obj = { ...raw };
    adoptLegacyCorners(obj);
    if ((Number(docVersion) || 1) < 2) {
        aabbOriginToCenter(obj);
    }
    geom(obj);
    delete obj.x1;
    delete obj.y1;
    delete obj.x2;
    delete obj.y2;
    if (!obj.type) {
        obj.type = "text";
    }
    if (obj.type === "text" && obj.pristine !== true) {
        obj.pristine = false;
    }
    obj.ignoreSafe = !!obj.ignoreSafe;
    return obj;
}

export const LEGACY_FORMAT = "pm220-label";
/** The PM220 label every legacy (pm220_rp2_webapp) file was drawn for. */
export const LEGACY_MEDIA = { id: "nelko-pm220-50x30", name: "50 × 30 mm", w_mm: 50, h_mm: 30, gap_mm: 2, type: "gap" };
export const LEGACY_MODEL = { driver: "nelko_pm220", id: "pm220" };

function mediaBlock(m) {
    if (!m) {
        return null;
    }
    const out = { id: m.id || "", name: m.name || "", w_mm: Number(m.w_mm), h_mm: Number(m.h_mm) };
    if (m.gap_mm != null) {
        out.gap_mm = Number(m.gap_mm);
    }
    out.type = m.type || "gap";
    return out;
}

/** state: {page, objects, media, orient, model, vars} */
export function toDocument(state) {
    const page = state.page || {};
    return {
        format: FORMAT,
        version: VERSION,
        media: mediaBlock(state.media),
        orient: Number(state.orient) || 0,
        model: state.model ? { driver: state.model.driver || "", id: state.model.id || "" } : null,
        page: { width_dots: page.width_dots, height_dots: page.height_dots, dpi: page.dpi },
        vars: (state.vars || []).map((v) => ({ ...v })),
        objects: (state.objects || []).map((o) => {
            const copy = migrateObject(o, VERSION);
            delete copy._pending;
            return copy;
        }),
    };
}

/** → {objects, media, orient, model, vars, legacy}. Accepts this app's files,
 *  pm220_rp2_webapp's (pm220-label v1/v2) and a bare objects array. */
export function fromDocument(data) {
    if (!data || (typeof data !== "object")) {
        throw new Error("not a label file");
    }
    const legacy = Array.isArray(data) || data.format === LEGACY_FORMAT || (!data.format && Array.isArray(data.objects));
    if (!legacy && data.format !== FORMAT) {
        throw new Error("not a label file (format " + JSON.stringify(data.format) + ")");
    }
    const list = Array.isArray(data) ? data : data.objects;
    if (!Array.isArray(list)) {
        throw new Error("label file has no objects list");
    }
    const ver = Array.isArray(data) ? 1 : (Number(data.version) || 1);
    // pm220 files are v1/v2 of their own format; their geometry rules are this format's v2.
    const objVer = legacy ? Math.min(ver, 2) : ver;
    const objects = list.map((o) => migrateObject(o, objVer));
    if (legacy) {
        return { objects, media: { ...LEGACY_MEDIA }, orient: 0, model: { ...LEGACY_MODEL }, vars: [], legacy: true };
    }
    return {
        objects,
        media: mediaBlock(data.media),
        orient: Number(data.orient) || 0,
        model: data.model || null,
        vars: Array.isArray(data.vars) ? data.vars : [],
        legacy: false,
    };
}

export function downloadDocument(doc, name) {
    const blob = new Blob([JSON.stringify(doc, null, 2)], { type: "application/json" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = name || "label.label.json";
    a.rel = "noopener";
    a.style.display = "none";
    document.body.appendChild(a);
    a.click();
    setTimeout(() => {
        URL.revokeObjectURL(a.href);
        a.remove();
    }, 1000);
}

/** Text of a picked file, gunzipped when it is gzip (the pm220 app read .gz as text and failed). */
export async function readFileText(file) {
    const buf = new Uint8Array(await file.arrayBuffer());
    if (buf.length >= 2 && buf[0] === 0x1f && buf[1] === 0x8b) {
        const ds = new DecompressionStream("gzip");
        const out = await new Response(new Blob([buf]).stream().pipeThrough(ds)).arrayBuffer();
        return new TextDecoder().decode(out);
    }
    return new TextDecoder().decode(buf);
}

export async function readLabelFile(file) {
    return fromDocument(JSON.parse(await readFileText(file)));
}
