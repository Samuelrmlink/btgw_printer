/** Calibration: the pm220 dialog, now per printer × label size.
 *
 * Fields and limits are the pm220 ones (settings/print.json there); the
 * layout math is layout.js computeLayout. Saved in printer/printers.json under
 * the printer's address and the label size's id. With no saved printer the
 * values only last for this session. */

import { describeLayout, pageFor, normaliseCalibration, labelGeometry, defaultCalibration, CAL_ADJ_LIM, CAL_MM_LIM } from "./layout.js";
import { isNumField, setDynamicBounds, stepNumField } from "./numfield.js";
import { monoNew } from "./pack.js";

const $ = (id) => document.getElementById(id);

const FIELDS = [
    ["cal-offx", "offset_x_mm", "mm"],
    ["cal-offy", "offset_y_mm", "mm"],
    ["cal-gap", "gap_mm", "gap"],
    ["cal-ox", "origin_x_adj", "adj"],
    ["cal-oy", "origin_y_adj", "adj"],
    ["cal-ph", "print_height_adj", "adj"],
    ["cal-sx0", "safe_x0", "sx0"],
    ["cal-sy0", "safe_y0", "sy0"],
    ["cal-sx1", "safe_x1", "sx1"],
    ["cal-sy1", "safe_y1", "sy1"],
];

/** The pm220 firmware's test frame (job.c draw_frame): a 3-dot frame inset
 *  2 mm (5 mm at the bottom), a centre cross, and the safe rect dotted. In
 *  head coordinates, srcW × srcH, so it prints the same whatever the label's
 *  orientation in the editor. */
export function testFrameMono(geo, cal) {
    const w = geo.srcW;
    const h = geo.srcH;
    const m = monoNew(w, h);
    const set = (x, y, v = 1) => {
        if (x >= 0 && y >= 0 && x < w && y < h) {
            m.bits[y * w + x] = v;
        }
    };
    const d = (mm) => Math.trunc(mm * geo.dpi / 25.4 + 0.5);
    const thickness = 3;
    const ix = Math.max(d(2), thickness + 1);
    const iy0 = Math.max(d(2), thickness + 1);
    const iy1 = Math.max(d(5), thickness + 1);
    const x0 = ix;
    const y0 = iy0;
    const x1 = w - 1 - ix;
    const y1 = h - 1 - iy1;
    if (x1 > x0 && y1 > y0) {
        for (let t = 0; t < thickness; t++) {
            for (let x = x0; x <= x1; x++) {
                set(x, y0 + t);
                set(x, y1 - t);
            }
            for (let y = y0; y <= y1; y++) {
                set(x0 + t, y);
                set(x1 - t, y);
            }
        }
        const cx = w >> 1;
        const cy = h >> 1;
        const arm = Math.trunc(Math.min(x1 - x0, y1 - y0) / 4);
        for (let i = -arm; i <= arm; i++) {
            set(cx + i, cy);
            set(cx + i, cy + 1);
            set(cx, cy + i);
            set(cx + 1, cy + i);
        }
    }
    for (let t = 0; t < 2; t++) {
        const sx0 = cal.safe_x0 + t;
        const sy0 = cal.safe_y0 + t;
        const sx1 = cal.safe_x1 - t;
        const sy1 = cal.safe_y1 - t;
        if (sx1 <= sx0 || sy1 <= sy0) {
            break;
        }
        for (let x = sx0; x <= sx1; x++) {
            if ((x & 7) < 4) {
                set(x, sy0);
                set(x, sy1);
            }
        }
        for (let y = sy0; y <= sy1; y++) {
            if ((y & 7) < 4) {
                set(sx0, y);
                set(sx1, y);
            }
        }
    }
    return m;
}

/** ctx: {setStatus, current() → {model, media, addr, printer}, saveCal(addr, media, cal),
 *        loadCal(addr, model, media), applied(cal), testPrint(cal) } */
export function bindCalibrate(ctx) {
    const overlay = $("cal");
    const errEl = $("cal-err");
    let geo = null;
    let model = null;
    let media = null;

    setDynamicBounds((el) => {
        if (!geo || !el.id.startsWith("cal-")) {
            return null;
        }
        const f = FIELDS.find((x) => x[0] === el.id);
        if (!f) {
            return null;
        }
        switch (f[2]) {
            case "mm":
                return { lo: -CAL_MM_LIM, hi: CAL_MM_LIM, step: 0.25 };
            case "gap":
                return { lo: 0, hi: CAL_MM_LIM, step: 0.25 };
            case "adj":
                return { lo: -CAL_ADJ_LIM, hi: CAL_ADJ_LIM, step: 1 };
            case "sx0":
                return { lo: 0, hi: geo.srcW - 4, step: 1 };
            case "sy0":
                return { lo: 0, hi: geo.srcH - 4, step: 1 };
            case "sx1":
                return { lo: 3, hi: geo.srcW - 1, step: 1 };
            case "sy1":
                return { lo: 3, hi: geo.srcH - 1, step: 1 };
            default:
                return null;
        }
    });

    function showErr(msg) {
        errEl.hidden = !msg;
        errEl.textContent = msg || "";
    }

    function readCal() {
        const out = {};
        for (const [id, key] of FIELDS) {
            const v = Number(String($(id).value).trim());
            out[key] = Number.isFinite(v) ? v : 0;
        }
        return normaliseCalibration(out, geo);
    }

    function fill(cal) {
        for (const [id, key] of FIELDS) {
            $(id).value = String(cal[key]);
        }
    }

    function updateEffective() {
        if (!model || !media) {
            return;
        }
        const page = pageFor(model, media, readCal(), 0);
        $("cal-effective").textContent = describeLayout(page);
    }

    function open() {
        const cur = ctx.current();
        showErr("");
        model = cur.model;
        media = cur.media;
        if (!model || !media) {
            ctx.setStatus("pick a printer model and label size first", "err");
            return;
        }
        geo = labelGeometry(model, media);
        $("cal-title").textContent = `Calibration · ${cur.printer ? (cur.printer.nickname || cur.printer.addr) : model.name} · ${media.name}`;
        $("cal-for").textContent = cur.printer
            ? `Saved for ${cur.printer.nickname || cur.printer.addr} with ${media.name} labels (printer/printers.json).`
            : "No saved printer is selected: these values apply until the page is reloaded. Save a printer to keep them.";
        fill(ctx.loadCal(cur.addr, model, media));
        updateEffective();
        overlay.hidden = false;
        $("cal-offx").focus();
        $("cal-offx").select();
    }

    function close() {
        overlay.hidden = true;
    }

    async function save() {
        showErr("");
        const cal = readCal();
        fill(cal);
        try {
            await ctx.saveCal(ctx.current().addr, media, cal);
            ctx.applied(cal);
            updateEffective();
            ctx.setStatus("calibration saved", "ok");
            return true;
        } catch (err) {
            showErr(String(err.message || err));
            return false;
        }
    }

    overlay.addEventListener("input", () => updateEffective());
    $("calibrate").addEventListener("click", () => open());
    $("cal-close").addEventListener("click", () => close());
    $("cal-save").addEventListener("click", () => save());
    $("cal-reset").addEventListener("click", () => {
        fill(defaultCalibration(model, media));
        updateEffective();
    });
    $("cal-test").addEventListener("click", async () => {
        showErr("");
        if (!(await save())) {
            return;
        }
        try {
            await ctx.testPrint(readCal());
        } catch (err) {
            showErr(String(err.message || err));
        }
    });
    overlay.addEventListener("click", (e) => {
        if (e.target === overlay || e.target.classList.contains("picker-backdrop")) {
            close();
        }
    });
    const fieldIds = FIELDS.map((f) => f[0]);
    const actionIds = ["cal-test", "cal-reset", "cal-save", "cal-close"];

    function calFocusables() {
        return [...fieldIds, ...actionIds].map($).filter(Boolean);
    }

    document.addEventListener("keydown", (e) => {
        if (overlay.hidden) {
            return;
        }
        if (e.key === "Escape" || e.key === "q") {
            e.preventDefault();
            close();
            return;
        }
        const mod = e.ctrlKey || e.metaKey;
        if (mod && !e.altKey && e.key.toLowerCase() === "s") {
            e.preventDefault();
            save();
            return;
        }
        if (e.key === "Tab") {
            e.preventDefault();
            const els = calFocusables();
            const i = els.indexOf(document.activeElement);
            const n = els.length;
            let next;
            if (i < 0) {
                next = e.shiftKey ? n - 1 : 0;
            } else {
                next = e.shiftKey ? (i - 1 + n) % n : (i + 1) % n;
            }
            els[next].focus();
            if (els[next].select) {
                els[next].select();
            }
            return;
        }
        if (isNumField(e.target)) {
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
        const onActions = actionIds.includes(e.target && e.target.id);
        const right = e.key === "ArrowRight" || e.key === "l" || e.key === "ArrowDown" || e.key === "j";
        const left = e.key === "ArrowLeft" || e.key === "h" || e.key === "ArrowUp" || e.key === "k";
        if (onActions && (right || left)) {
            e.preventDefault();
            const els = actionIds.map($).filter(Boolean);
            const i = Math.max(0, els.indexOf(e.target));
            const n = els.length;
            const next = right ? (i + 1) % n : (i - 1 + n) % n;
            els[next].focus();
            return;
        }
        if (e.key === "Enter" && e.target && e.target.tagName === "INPUT") {
            e.preventDefault();
            save();
        }
    });

    return { isOpen: () => !overlay.hidden, open, close };
}
