/** What can be printed on: models (driver caps), label sizes, saved printers.
 *
 * Models come from printer.drivers when printer_hub is running, else from the
 * shipped presets/models.json, so a label can be designed with no board.
 * Label sizes are the shipped presets/media/*.json plus the user's own in
 * media.json. Saved printers and their calibration live in printers.json
 * (docs/architecture.md); both under printer/ on flash or sd/printer/ on the
 * card (store.js decides). */

import { readJsonOr, sdAvailable, writeJson } from "./device.js";
import { defaultCalibration, labelGeometry, normaliseCalibration } from "./layout.js";
import { migrateDriver, migratePrinters, servedFromSd, settingsStore } from "./store.js";
import { fetchRetry } from "./lazy.js";

const MEDIA = "media.json";
const PRINTERS = "printers.json";
const store = settingsStore({
    read: (path) => readJsonOr(path, null),
    write: writeJson,
    sdUp: sdAvailable,
    served: servedFromSd(globalThis.location && location.pathname),
});

/** Where a settings file is saved now, e.g. "sd/printer/printers.json". */
export const printersPath = () => store.path(PRINTERS);
export const mediaPath = () => store.path(MEDIA);

let presetDrivers = [];
let liveDrivers = null;
let presetMedia = [];
let userMedia = [];
let printers = { version: 1, printers: [] };

const listeners = new Set();
export function onCatalog(fn) {
    listeners.add(fn);
    return () => listeners.delete(fn);
}
function changed() {
    for (const fn of listeners) {
        try {
            fn();
        } catch (e) {
            console.error(e);
        }
    }
}

async function fetchJson(url) {
    const r = await fetchRetry(url, { cache: "no-store" });
    if (!r.ok) {
        throw new Error(`${url}: HTTP ${r.status}`);
    }
    return r.json();
}

export async function loadPresets(base = "presets/") {
    try {
        presetDrivers = (await fetchJson(base + "models.json")).drivers || [];
    } catch {
        presetDrivers = [];
    }
    presetMedia = [];
    try {
        const idx = await fetchJson(base + "media/index.json");
        for (const f of idx.files || []) {
            try {
                const j = await fetchJson(base + "media/" + f);
                for (const m of j.media || []) {
                    presetMedia.push({ ...m, preset: true });
                }
            } catch {
                /* skip a bad file */
            }
        }
    } catch {
        /* none */
    }
    changed();
}

export function setLiveDrivers(drivers) {
    liveDrivers = Array.isArray(drivers) ? drivers : null;
    changed();
}

function flatten(drivers, live) {
    const out = [];
    for (const d of drivers || []) {
        for (const m of d.models || []) {
            out.push({ ...m, driver: d.driver, key: `${d.driver}/${m.id}`, live: !!live });
        }
    }
    return out;
}

/** Every model: live caps first, presets for the rest. */
export function models() {
    const live = flatten(liveDrivers, true);
    const keys = new Set(live.map((m) => m.key));
    return live.concat(flatten(presetDrivers, false).filter((m) => !keys.has(m.key)));
}

export function findModel(ref) {
    if (!ref) {
        return null;
    }
    const all = models();
    if (typeof ref === "string") {
        return all.find((m) => m.key === ref || m.id === ref) || null;
    }
    const driver = migrateDriver(ref.driver);
    return all.find((m) => m.driver === driver && m.id === ref.id) ||
        all.find((m) => m.id === ref.id) || null;
}

export function defaultModel() {
    return findModel("nelko_pl70e/pl70e") || models()[0] || {
        key: "printer_virtual/virtual", driver: "printer_virtual", id: "virtual", name: "Virtual", dpi: 203, head_dots: 384,
        black_bit: 0, placement: true, media: ["gap"], presets: [],
    };
}

export function isDriverLive(driver) {
    return !!(liveDrivers && liveDrivers.some((d) => d.driver === driver));
}

// ---- label sizes --------------------------------------------------------

export async function loadUserMedia() {
    const j = await store.load(MEDIA).catch(() => null);
    userMedia = j && Array.isArray(j.media) ? j.media.map((m) => ({ ...m, preset: false })) : [];
    changed();
}

export function allMedia() {
    const ids = new Set(userMedia.map((m) => m.id));
    return userMedia.concat(presetMedia.filter((m) => !ids.has(m.id)));
}

export function mediaFor(model) {
    const id = model && model.id;
    return allMedia().filter((m) => !m.models || !m.models.length || m.models.includes(id) || m.all_models);
}

export function findMedia(id) {
    return allMedia().find((m) => m.id === id) || null;
}

export function fitsModel(media, model) {
    if (!model || !media) {
        return true;
    }
    const lenOk = !model.max_len_mm || media.h_mm <= model.max_len_mm;
    const typeOk = !model.media || !media.type || model.media.includes(media.type);
    return lenOk && typeOk;
}

export function newMediaId(name) {
    const base = "user-" + String(name || "size").toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "").slice(0, 24);
    let id = base || "user-size";
    let i = 2;
    while (allMedia().some((m) => m.id === id)) {
        id = `${base}-${i++}`;
    }
    return id;
}

export async function saveUserMedia(media) {
    const m = { ...media, preset: undefined };
    delete m.preset;
    const i = userMedia.findIndex((x) => x.id === m.id);
    if (i >= 0) {
        userMedia[i] = { ...m, preset: false };
    } else {
        userMedia.push({ ...m, preset: false });
    }
    const path = await store.save(MEDIA, { version: 1, media: userMedia.map(({ preset, ...rest }) => rest) });
    changed();
    return path;
}

export async function deleteUserMedia(id) {
    userMedia = userMedia.filter((m) => m.id !== id);
    const path = await store.save(MEDIA, { version: 1, media: userMedia.map(({ preset, ...rest }) => rest) });
    changed();
    return path;
}

// ---- saved printers -------------------------------------------------------

export async function loadPrinters() {
    printers = migratePrinters(await store.load(PRINTERS).catch(() => null));
    changed();
}

export function savedPrinters() {
    return printers.printers;
}

export function findPrinter(addr) {
    return printers.printers.find((p) => p.addr === addr) || null;
}

export async function savePrinter(p) {
    const i = printers.printers.findIndex((x) => x.addr === p.addr);
    if (i >= 0) {
        printers.printers[i] = { ...printers.printers[i], ...p };
    } else {
        printers.printers.push({ calibration: {}, ...p });
    }
    const path = await store.save(PRINTERS, printers);
    changed();
    return path;
}

export async function forgetPrinter(addr) {
    printers.printers = printers.printers.filter((p) => p.addr !== addr);
    if (printers.current === addr) {
        delete printers.current;
    }
    const path = await store.save(PRINTERS, printers);
    changed();
    return path;
}

export function currentPrinterAddr() {
    return printers.current || null;
}

export async function setCurrentPrinter(addr) {
    printers.current = addr || undefined;
    const path = await store.save(PRINTERS, printers);
    changed();
    return path;
}

/** The calibration for one printer × label size: saved, else the media's factory values. */
export function calibrationFor(addr, model, media) {
    const p = addr ? findPrinter(addr) : null;
    const saved = p && p.calibration && p.calibration[media.id];
    const geo = labelGeometry(model, media);
    return saved ? normaliseCalibration({ ...defaultCalibration(model, media), ...saved }, geo) : defaultCalibration(model, media);
}

export async function saveCalibration(addr, media, cal) {
    const p = findPrinter(addr);
    if (!p) {
        throw new Error("save the printer first");
    }
    p.calibration = { ...(p.calibration || {}), [media.id]: cal };
    const path = await store.save(PRINTERS, printers);
    changed();
    return path;
}

/** A JSON-safe, test-friendly reset (tests only). */
export function _resetForTests() {
    presetDrivers = [];
    liveDrivers = null;
    presetMedia = [];
    userMedia = [];
    printers = { version: 1, printers: [] };
}
