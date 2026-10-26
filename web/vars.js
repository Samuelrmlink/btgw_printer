/** Dynamic templates: {{name}} placeholders in text, QR and barcode payloads.
 *
 * DOM-free. A label document carries `vars`, one definition per name:
 *   {name, type: "text"|"number"|"counter"|"date", label?, default?,
 *    start?, step?, pad?, prefix?, suffix?, format?}
 * A placeholder whose name has no definition is a plain text variable.
 * Built-ins need no definition: {{date}} (YYYY-MM-DD), {{time}} (HH:mm),
 * {{n}} (1-based label number in this print run).
 * Optional inline format: {{name:FORMAT}} — a date pattern for date vars,
 * a pad width for counters/numbers (e.g. {{serial:5}}). */

export const VAR_RE = /\{\{\s*([A-Za-z_][A-Za-z0-9_.-]*)\s*(?::([^}]*))?\}\}/g;
export const VAR_TYPES = ["text", "number", "counter", "date"];
export const BUILTINS = new Set(["date", "time", "n"]);

const PAYLOAD_KEYS = { text: "text", qr: "payload", barcode1d: "payload" };

export function payloadKey(obj) {
    return PAYLOAD_KEYS[obj && obj.type] || null;
}

/** Names used by the objects, in first-use order. */
export function findVars(objects) {
    const seen = [];
    for (const o of objects || []) {
        const k = payloadKey(o);
        if (!k || typeof o[k] !== "string") {
            continue;
        }
        for (const m of o[k].matchAll(VAR_RE)) {
            if (!seen.includes(m[1])) {
                seen.push(m[1]);
            }
        }
    }
    return seen;
}

export function hasVars(objects) {
    return findVars(objects).length > 0;
}

/** Definitions for every used name: the document's, else a default text var. */
export function effectiveDefs(objects, defs) {
    const byName = new Map((defs || []).map((d) => [d.name, d]));
    return findVars(objects)
        .filter((n) => !BUILTINS.has(n) || byName.has(n))
        .map((n) => normaliseDef(byName.get(n) || { name: n, type: "text" }));
}

export function normaliseDef(d) {
    const type = VAR_TYPES.includes(d.type) ? d.type : "text";
    const out = { name: String(d.name), type, label: d.label || "" };
    if (type === "counter") {
        out.start = Number.isFinite(Number(d.start)) ? Math.trunc(Number(d.start)) : 1;
        out.step = Number.isFinite(Number(d.step)) && Number(d.step) !== 0 ? Math.trunc(Number(d.step)) : 1;
        out.pad = Math.max(0, Math.min(20, Math.trunc(Number(d.pad) || 0)));
        out.prefix = d.prefix || "";
        out.suffix = d.suffix || "";
    } else if (type === "date") {
        out.format = d.format || "YYYY-MM-DD";
        out.offset_days = Math.trunc(Number(d.offset_days) || 0);
    } else if (type === "number") {
        out.default = d.default ?? "";
        out.decimals = d.decimals == null || d.decimals === "" ? null : Math.max(0, Math.min(10, Math.trunc(Number(d.decimals))));
    } else {
        out.default = d.default ?? "";
    }
    return out;
}

function pad2(n) {
    return String(n).padStart(2, "0");
}

/** Date tokens: YYYY YY MM M DD D HH H hh h mm ss A MMM ddd. Text in [brackets] is literal. */
export function formatDate(date, fmt) {
    const MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
    const DAYS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
    const h12 = date.getHours() % 12 || 12;
    const tok = {
        YYYY: String(date.getFullYear()),
        YY: String(date.getFullYear()).slice(-2),
        MMM: MONTHS[date.getMonth()],
        MM: pad2(date.getMonth() + 1),
        M: String(date.getMonth() + 1),
        DD: pad2(date.getDate()),
        D: String(date.getDate()),
        ddd: DAYS[date.getDay()],
        HH: pad2(date.getHours()),
        H: String(date.getHours()),
        hh: pad2(h12),
        h: String(h12),
        mm: pad2(date.getMinutes()),
        ss: pad2(date.getSeconds()),
        A: date.getHours() < 12 ? "AM" : "PM",
    };
    return String(fmt).replace(/\[([^\]]*)\]|YYYY|YY|MMM|MM|M|DD|D|ddd|HH|H|hh|h|mm|ss|A/g, (m, lit) =>
        lit !== undefined ? lit : tok[m]);
}

function counterText(def, value, padOverride) {
    const pad = padOverride != null ? padOverride : def.pad;
    const neg = value < 0;
    let s = String(Math.abs(value)).padStart(pad, "0");
    if (neg) {
        s = "-" + s;
    }
    return (def.prefix || "") + s + (def.suffix || "");
}

/** The value of every placeholder for label number `index` (0-based) of a run.
 *  values: {name: string} typed by the user or taken from a batch row.
 *  counters: {name: next value} — where each counter starts this run. */
export function resolveValues(defs, values, index, { now = new Date(), counters = {} } = {}) {
    const out = {};
    for (const d of defs) {
        const v = values ? values[d.name] : undefined;
        if (d.type === "counter") {
            const start = counters[d.name] != null ? Number(counters[d.name]) : d.start;
            out[d.name] = { def: d, num: start + d.step * index };
        } else if (d.type === "date") {
            const t = new Date(now.getTime() + (d.offset_days || 0) * 86400000);
            out[d.name] = { def: d, date: v ? new Date(v) : t };
        } else if (d.type === "number") {
            out[d.name] = { def: d, text: v != null && v !== "" ? String(v) : String(d.default ?? "") };
        } else {
            out[d.name] = { def: d, text: v != null ? String(v) : String(d.default ?? "") };
        }
    }
    out.n = { text: String(index + 1) };
    out.date = out.date || { text: formatDate(now, "YYYY-MM-DD") };
    out.time = out.time || { text: formatDate(now, "HH:mm") };
    return out;
}

function render(entry, fmt) {
    if (!entry) {
        return "";
    }
    if (entry.num != null) {
        const pad = fmt != null && fmt !== "" && /^\d+$/.test(fmt) ? Number(fmt) : null;
        return counterText(entry.def, entry.num, pad);
    }
    if (entry.date) {
        return formatDate(entry.date, fmt || entry.def.format);
    }
    if (entry.def && entry.def.type === "number") {
        const n = Number(entry.text);
        const dec = fmt != null && /^\d+$/.test(fmt) ? Number(fmt) : entry.def.decimals;
        if (entry.text !== "" && Number.isFinite(n) && dec != null) {
            return n.toFixed(dec);
        }
    }
    return entry.text ?? "";
}

export function substitute(str, resolved) {
    return String(str).replace(VAR_RE, (m, name, fmt) => (name in resolved ? render(resolved[name], fmt) : ""));
}

/** Copies of the objects with every placeholder filled in. */
export function fillObjects(objects, resolved) {
    return objects.map((o) => {
        const k = payloadKey(o);
        if (!k || typeof o[k] !== "string" || !o[k].includes("{{")) {
            return o;
        }
        return { ...o, [k]: substitute(o[k], resolved) };
    });
}

/** Where each counter should start next time, after `printed` labels. */
export function advanceCounters(defs, counters, printed) {
    const next = { ...counters };
    for (const d of defs) {
        if (d.type === "counter") {
            const start = counters[d.name] != null ? Number(counters[d.name]) : d.start;
            next[d.name] = start + d.step * printed;
        }
    }
    return next;
}

/** Match batch columns to variable names (case-insensitive, spaces → _). */
export function mapColumns(header, defs) {
    const norm = (s) => String(s).trim().toLowerCase().replace(/\s+/g, "_");
    const map = {};
    header.forEach((h, i) => {
        const d = defs.find((x) => norm(x.name) === norm(h) || (x.label && norm(x.label) === norm(h)));
        if (d) {
            map[d.name] = i;
        }
    });
    return map;
}
