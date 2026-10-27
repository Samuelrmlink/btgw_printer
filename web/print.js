/** Print jobs: render → bitmap file on the board → printer.print → progress.
 *
 * The browser renders and packs (pack.js); the bitmap goes to the board as a
 * file because op params are capped near 960 bytes; printer_hub streams that
 * file to the driver and reports printer.job events (docs/architecture.md).
 * Labels whose content differs (counters, {{n}}) are one job each; identical
 * labels are one job with copies. */

import { call, on, sdAvailable, writeFile } from "./device.js";
import { buildJobBitmap } from "./pack.js";
import { effectiveDefs, fillObjects, resolveValues, advanceCounters } from "./vars.js";
import { whenImagesReady } from "./image.js";
import { counterStore } from "./templates.js";

const $ = (id) => document.getElementById(id);
const JOB_IDLE_MS = 60000;

export function jobPath() {
    return sdAvailable() ? "sd/printer/var/job.bin" : "printer/var/job.bin";
}

/** The printer.print params for one bitmap (exported for tests). */
export function printParams({ printer, model, media, page, job, copies, path }) {
    return {
        driver: printer.driver,
        addr: printer.addr,
        model: model.id,
        path,
        width_dots: job.width,
        rows: job.rows,
        origin: job.origin,
        media: {
            w_mm: media.w_mm,
            h_mm: media.h_mm,
            gap_mm: page.cal ? page.cal.gap_mm : (media.gap_mm || 0),
            type: media.type || "gap",
        },
        density: printer.density != null ? printer.density : (model.density ? model.density.default : -1),
        speed: printer.speed != null ? printer.speed : -1,
        copies: Math.max(1, copies | 0),
        rotate: 0,
        options: {},
    };
}

/** ctx: {current() → {printer, model, media, page, objects, vars, docName},
 *        render(objects) → mono (editor orientation), setStatus, isConnected(addr), openPrinters()} */
export function bindPrint(ctx) {
    let running = null;   // {cancelled, jobId}
    const waiters = new Map();   // job id -> {resolve, reject, timer, onProgress}
    const early = new Map();     // job id -> events that came before printer.print replied

    on("printer.job", (d) => {
        if (!d || d.job == null) {
            return;
        }
        const w = waiters.get(d.job);
        if (!w) {
            const list = early.get(d.job) || [];
            list.push(d);
            early.set(d.job, list);
            if (early.size > 16) {
                early.delete(early.keys().next().value);
            }
            return;
        }
        handle(w, d);
    });

    function handle(w, d) {
        clearTimeout(w.timer);
        w.timer = setTimeout(() => w.fail(new Error("the printer stopped answering")), JOB_IDLE_MS);
        w.onProgress && w.onProgress(d);
        if (d.state === "done") {
            w.done(d);
        } else if (d.state === "failed") {
            w.fail(new Error(d.message || "print failed"));
        } else if (d.state === "cancelled") {
            w.fail(Object.assign(new Error("cancelled"), { cancelled: true }));
        }
    }

    function bar(text, frac) {
        $("job-bar").hidden = text == null;
        if (text != null) {
            $("job-text").textContent = text;
            $("job-progress").value = Math.max(0, Math.min(1, frac || 0));
        }
    }

    function waitJob(id, onProgress) {
        return new Promise((resolve, reject) => {
            const w = {
                onProgress,
                done: (d) => {
                    clearTimeout(w.timer);
                    waiters.delete(id);
                    resolve(d);
                },
                fail: (e) => {
                    clearTimeout(w.timer);
                    waiters.delete(id);
                    reject(e);
                },
                timer: setTimeout(() => w.fail(new Error("the printer stopped answering")), JOB_IDLE_MS),
            };
            waiters.set(id, w);
            for (const d of early.get(id) || []) {
                if (waiters.has(id)) {
                    handle(w, d);
                }
            }
            early.delete(id);
        });
    }

    async function sendOne(job, copies, label, frac0, fracSpan) {
        const cur = ctx.current();
        const path = jobPath();
        await writeFile(path, job.bytes);
        if (running && running.cancelled) {
            throw Object.assign(new Error("cancelled"), { cancelled: true });
        }
        const params = printParams({ ...cur, job, copies, path });
        const r = await call("printer.print", params, 15000);
        running.jobId = r.job;
        await waitJob(r.job, (d) => {
            const f = d.total ? d.sent / d.total : 0;
            bar(`${label} · ${d.state}${d.copies > 1 ? ` · copy ${(d.copy || 0) + 1}/${d.copies}` : ""}`, frac0 + fracSpan * f);
        });
    }

    async function ready() {
        const cur = ctx.current();
        if (!cur.printer) {
            ctx.openPrinters();
            throw new Error("pick a printer first");
        }
        if (!ctx.isConnected(cur.printer.addr) && !(ctx.ensureConnected && (await ctx.ensureConnected(cur.printer)))) {
            throw new Error(`print cancelled: ${cur.printer.nickname || cur.printer.addr} is not connected`);
        }
        if (cur.model.driver !== cur.printer.driver) {
            throw new Error("the label is set up for another printer model");
        }
        return cur;
    }

    /** list: [{values, copies}] — one entry per distinct label (batch row or prompt). */
    async function printLabels(list) {
        if (running) {
            ctx.setStatus("a print is already running", "err");
            return false;
        }
        running = { cancelled: false, jobId: null };   // also while ready() asks to connect
        ctx.changed && ctx.changed();
        let cur;
        try {
            cur = await ready();
        } catch (err) {
            running = null;
            ctx.changed && ctx.changed();
            ctx.setStatus(String(err.message || err), "err");
            return false;
        }
        const defs = effectiveDefs(cur.objects, cur.vars);
        const varies = defs.some((d) => d.type === "counter") || cur.objects.some((o) => /\{\{\s*n\s*(:[^}]*)?\}\}/.test(o.text || o.payload || ""));
        const cs = counterStore(cur.docName);
        const counters = cs.get();
        const total = list.reduce((n, x) => n + Math.max(1, x.copies | 0), 0);
        let index = 0;
        let printed = 0;
        try {
            await whenImagesReady(cur.objects);
            for (const entry of list) {
                const n = Math.max(1, entry.copies | 0);
                const groups = varies ? Array.from({ length: n }, () => 1) : [n];
                for (const copies of groups) {
                    if (running.cancelled) {
                        throw Object.assign(new Error("cancelled"), { cancelled: true });
                    }
                    const resolved = resolveValues(defs, entry.values || {}, index, { counters });
                    const objs = fillObjects(cur.objects, resolved);
                    const mono = ctx.render(objs);
                    const job = buildJobBitmap(mono, cur.page, cur.model);
                    const label = total > 1 ? `label ${index + 1}${copies > 1 ? `–${index + copies}` : ""} of ${total}` : "label";
                    bar(`${label} · sending`, index / total);
                    await sendOne(job, copies, label, index / total, copies / total);
                    index += copies;
                    printed += copies;
                }
            }
            ctx.setStatus(`printed ${printed} label${printed === 1 ? "" : "s"}`, "ok");
            return true;
        } catch (err) {
            ctx.setStatus(err.cancelled ? `cancelled after ${printed} label(s)` : String(err.message || err), err.cancelled ? "" : "err");
            return false;
        } finally {
            if (printed) {
                cs.set(advanceCounters(defs, counters, printed));
            }
            running = null;
            bar(null);
            ctx.changed && ctx.changed();
        }
    }

    /** One bitmap in head coordinates (the calibration test frame). */
    async function printHeadMono(mono, page) {
        if (running) {
            throw new Error("a print is already running");
        }
        running = { cancelled: false, jobId: null };
        let cur;
        try {
            cur = await ready();
        } catch (err) {
            running = null;
            throw err;
        }
        try {
            const job = buildJobBitmap(mono, { ...page, orient: 0 }, cur.model);
            bar("test print · sending", 0);
            await sendOne(job, 1, "test print", 0, 1);
            ctx.setStatus("test print done", "ok");
        } finally {
            running = null;
            bar(null);
            ctx.changed && ctx.changed();
        }
    }

    async function cancel() {
        if (!running) {
            return;
        }
        running.cancelled = true;
        try {
            await call("printer.cancel", running.jobId != null ? { job: running.jobId } : {});
        } catch {
            /* the loop stops anyway */
        }
    }

    $("job-cancel").addEventListener("click", () => cancel());

    return { printLabels, printHeadMono, cancel, busy: () => !!running };
}
