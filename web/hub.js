/** printer_hub's live state: is it running, and what each printer is doing.
 *
 * The part of the printers code the page needs from boot (the status line,
 * auto-reconnect); the Printers dialog (printers.js) loads on first use.
 * Ops: printer.drivers, printer.status, printer.connect; events printer.state
 * and printer.status (docs/architecture.md). */

import { call, on } from "./device.js";
import { currentPrinterAddr, findPrinter, setLiveDrivers } from "./catalog.js";

const live = new Map();       // addr -> {state, why, battery, paper, lid, media, model, driver}
let up = false;
const listeners = new Set();

/** fn() on every state change. */
export function onHub(fn) {
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

export const hubUp = () => up;

export function stateOf(addr) {
    return live.get(addr) || { state: "unknown" };
}

export function setState(addr, patch) {
    live.set(addr, { ...stateOf(addr), ...patch });
    changed();
}

export function stateText(addr) {
    const s = stateOf(addr);
    const bits = [s.state || "unknown"];
    if (s.battery != null) {
        bits.push(`battery ${s.battery}%`);
    }
    if (s.paper === false || s.paper === "out") {
        bits.push("no paper");
    }
    if (s.lid === "open" || s.lid === true) {
        bits.push("lid open");
    }
    if (s.why) {
        bits.push(s.why);
    }
    return bits.join(" · ");
}

export function isUp(addr) {
    const st = stateOf(addr).state;
    return st === "connected" || st === "printing" || st === "idle";
}

export async function refreshDrivers() {
    try {
        const r = await call("printer.drivers");
        setLiveDrivers(r.drivers || []);
        up = true;
    } catch {
        setLiveDrivers(null);
        up = false;
    }
    return up;
}

export async function refreshStatus() {
    if (!up) {
        return;
    }
    try {
        const r = await call("printer.status", {});
        for (const p of r.printers || []) {
            live.set(p.addr, { ...stateOf(p.addr), ...p });
        }
    } catch {
        /* hub gone; the next drivers call says so */
    }
    changed();
}

function configOf(p) {
    return { model: p.model, density: p.density, speed: p.speed, nickname: p.nickname };
}

/** Throws what printer.connect said; the state shows it too. */
export async function connect(p) {
    setState(p.addr, { state: "connecting", why: "" });
    try {
        await call("printer.connect", { driver: p.driver, addr: p.addr, config: configOf(p) });
    } catch (err) {
        setState(p.addr, { state: "error", why: String(err.message || err) });
        throw err;
    }
}

/** Connect p unless it is up or on its way. */
export function connectIfDown(p) {
    const st = stateOf(p.addr).state;
    if (up && st !== "connected" && st !== "printing" && st !== "connecting") {
        return connect(p).catch(() => {});
    }
    return Promise.resolve();
}

/** Connect the current printer if it wants auto-reconnect. */
export function autoConnect() {
    const addr = currentPrinterAddr();
    const p = addr && findPrinter(addr);
    if (p && p.auto_reconnect !== false) {
        connectIfDown(p);
    }
}

on("printer.state", (d) => {
    if (d && d.addr) {
        setState(d.addr, d);
    }
});
on("printer.status", (d) => {
    if (d && d.addr) {
        setState(d.addr, d);
    }
});
