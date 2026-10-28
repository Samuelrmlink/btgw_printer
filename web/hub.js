/** printer_hub's live state: is it running, and what each printer is doing.
 *
 * The part of the printers code the page needs from boot (the status line,
 * auto-reconnect); the Printers dialog (printers.js) loads on first use.
 * Ops: printer.drivers, printer.status, printer.connect; events printer.state
 * and printer.status (docs/architecture.md). */

import { call, on } from "./device.js";
import { currentPrinterAddr, findModel, findPrinter, isDriverLive, setLiveDrivers } from "./catalog.js";
import { FALLBACK_DRIVERS, HUB_MODULE, alreadyDone, askBeforeConnect, lostAgain } from "./store.js";

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

// ---- the printer service: printer_hub at boot, a driver when it is needed --

let ours = [...FALLBACK_DRIVERS];   // our driver modules (app.json's pex list)
const pause = (ms) => new Promise((r) => setTimeout(r, ms));

export function setOurDrivers(list) {
    ours = [...list];
}
export const ourDrivers = () => [...ours];

/** The state pex.list gives `name` ("loaded", "running", "stopped"), null
 *  when it isn't loaded, undefined when the list can't be had. */
async function pexState(name) {
    try {
        const list = await call("pex.list");
        const m = (list.modules || []).find((x) => x && x.name === name);
        return m ? m.state : null;
    } catch {
        return undefined;
    }
}

/** pex.load only a module that isn't loaded: the board doesn't refuse a
 *  second pex.load, it loads another instance (<name>#2) into the pool. */
async function loadAndStart(name) {
    const st = await pexState(name);
    const ops = st === "running" ? [] : st ? ["pex.start"] : ["pex.load", "pex.start"];
    for (const op of ops) {
        try {
            await call(op, { name });
        } catch (err) {
            if (!alreadyDone(err)) {
                throw Object.assign(new Error(err.message || String(err)), { code: err.code, module: name });
            }
        }
    }
}

/** Ask printer.drivers once a second, ≤ 5 s, until ok() says so. */
async function settle(ok) {
    for (let i = 0; i < 5; i++) {
        await refreshDrivers();
        if (ok()) {
            return true;
        }
        await pause(1000);
    }
    await refreshDrivers();
    return ok();
}

/** pex.load + pex.start printer_hub, then wait for it to answer. Throws the
 *  board's message. */
export async function startHub() {
    await loadAndStart(HUB_MODULE);
    if (!(await settle(() => up))) {
        throw new Error("printer_hub did not answer");
    }
}

/** A driver nothing of ours is using: no printer on it connected, printing or on its way. */
function idleDriver(driver) {
    for (const [, s] of live) {
        if (s.driver === driver && ["connected", "printing", "connecting", "pairing", "idle", "waiting"].includes(s.state)) {
            return false;
        }
    }
    return true;
}

/** Load and start a driver unless printer.drivers lists it. Only the driver
 *  for the printer in hand stays loaded: first every other driver of ours
 *  that is loaded and has no printer in use is stopped and unloaded (two
 *  drivers loaded side by side left the bench board's PEX pool 32 bytes,
 *  and every print then failed). Throws the board's message. */
export async function ensureDriver(driver) {
    if (isDriverLive(driver)) {
        return false;
    }
    await refreshStatus();
    let loaded = [];
    try {
        const list = await call("pex.list");
        loaded = (list.modules || []).filter((m) => m && ["loaded", "running", "stopped"].includes(m.state)).map((m) => m.name);
    } catch {
        /* no list: unload nothing */
    }
    for (const other of ours) {
        if (other !== driver && loaded.includes(other) && idleDriver(other)) {
            await call("pex.stop", { name: other }).catch(() => {});   // a loaded, stopped one says so: fine
            await call("pex.unload", { name: other });
        }
    }
    await refreshDrivers();
    await loadAndStart(driver);
    if (!(await settle(() => isDriverLive(driver)))) {
        throw new Error(`the ${driver} driver did not register`);
    }
    return true;
}

// ---- asking before a Classic printer is paged (store.js askBeforeConnect) --

const seen = new Set();       // addrs a scan in this page found, not lost since
let prompt = null;            // async (printer) → true to connect (app.js: the "Turn on the printer" dialog)
let asking = null;            // the prompt on screen, if any

/** fn(printer) → Promise<boolean>: shown before a saved Classic printer is paged. */
export function setConnectPrompt(fn) {
    prompt = fn;
}

export function transportOf(p) {
    const m = findModel({ driver: p.driver, id: p.model });
    return (m && m.transport) || p.transport || "";
}

/** fresh: just picked from a scan (not paired before), so it is on. */
export function mustAsk(p, { fresh = false } = {}) {
    return !fresh && askBeforeConnect({ transport: transportOf(p), saved: !!findPrinter(p.addr), seen: seen.has(p.addr) });
}

/** Connect p, asking first when mustAsk(p), loading its driver first when
 *  it is not running. → true once printer.connect went out (it throws what
 *  the board or the driver said), false when the user said not now. */
export async function requestConnect(p, opts = {}) {
    if (mustAsk(p, opts)) {
        if (!asking) {
            asking = Promise.resolve(prompt ? prompt(p) : false).finally(() => {
                asking = null;
            });
        }
        if (!(await asking)) {
            return false;
        }
    }
    await ensureDriver(p.driver);
    await connect(p);
    return true;
}

/** Resolves true once addr is connected, false when the connect failed, the
 *  link went away or ms passed. */
export function waitConnected(addr, ms = 45000) {
    if (isUp(addr)) {
        return Promise.resolve(true);
    }
    return new Promise((resolve) => {
        const timer = setTimeout(() => finish(false), ms);
        const off = onHub(() => {
            const st = stateOf(addr).state;
            if (isUp(addr)) {
                finish(true);
            } else if (st === "error" || st === "disconnected" || st === "idle_disconnected") {
                finish(false);
            }
        });
        function finish(v) {
            clearTimeout(timer);
            off();
            resolve(v);
        }
    });
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

/** Can a connect be started now: the hub is up and p is not up or on its way. */
export function isDown(p) {
    const st = stateOf(p.addr).state;
    return up && st !== "connected" && st !== "printing" && st !== "connecting" && st !== "pairing";
}

/** requestConnect(p) unless it is up or on its way. → false when nothing went out. */
export function connectIfDown(p, opts) {
    return isDown(p) ? requestConnect(p, opts).catch(() => false) : Promise.resolve(false);
}

/** What page load does with the current printer: null (nothing: none, no
 *  auto-reconnect, or up already), "ask" (a saved Classic printer: never
 *  paged without the user) or "connect". The caller runs requestConnect. */
export function autoConnect() {
    const addr = currentPrinterAddr();
    const p = addr && findPrinter(addr);
    if (!p || p.auto_reconnect === false || !isDown(p)) {
        return null;
    }
    return mustAsk(p) ? "ask" : "connect";
}

on("printer.found", (d) => {
    if (d && d.addr && !d.gone) {
        seen.add(d.addr);
    }
});
on("printer.state", (d) => {
    if (d && d.addr) {
        if (lostAgain(d.why)) {
            seen.delete(d.addr);
        }
        setState(d.addr, d);
    }
});
on("printer.status", (d) => {
    if (d && d.addr) {
        setState(d.addr, d);
    }
});
