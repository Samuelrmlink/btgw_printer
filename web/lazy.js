/** Every HTTP request the app makes to the board, and loading its modules.
 *
 * The board serves a handful of connections in all, shared with other
 * browsers and WebSockets, and answers 409 busy (503 card busy on its /sd
 * route) or drops the request past that. So nothing here pushes it: every
 * request goes through one queue, at most two in flight (one after the board
 * has said busy once), and a busy answer or a network error is retried after
 * 300 ms, 1 s and 3 s (with jitter) through the same queue, the whole queue
 * waiting out the backoff.
 *
 * Modules: a plain <script type="module" src="app.js"> would have the browser
 * fetch app.js's whole import graph at once. index.html loads only this
 * module; boot() imports the boot graph through the queue, leaves first, and
 * everything else is import()ed on first use (load) or, once the page is
 * idle, one file at a time with a pause between (prefetch).
 *
 * MODULES lists each module's static imports (tests/unit/lazy.test.js checks
 * it against the sources). A module is imported only after its imports are in
 * the module map, so each import() fetches exactly one file.
 *
 * A failed import is retried with a cache-busting query (x.js?r=1): the
 * browser remembers a failed module URL for the life of the page and never
 * fetches it again. So a module that only loaded on a retry cannot satisfy
 * another module's static import of its plain URL; the dependent fails and
 * the error says to reload (boot reloads the page itself, twice at most). */

export const MODULES = {
    // ---- the boot graph, in load order ----
    "js/btgw/wsapi": [],
    "js/btgw/fs": [],
    raster: [],
    layout: [],
    store: [],
    keymap: [],
    numfield: [],
    doc: ["raster"],
    device: ["js/btgw/wsapi", "js/btgw/fs", "lazy"],
    editor: ["raster", "doc"],
    catalog: ["device", "layout", "store", "lazy"],
    hub: ["device", "catalog"],
    app: ["device", "editor", "raster", "doc", "layout", "catalog", "hub", "keymap", "numfield", "lazy"],
    // ---- on first use ----
    dialog: ["numfield"],
    pack: ["layout"],
    vars: [],
    csv: [],
    image: ["raster"],
    qrcodegen: [],
    qr: ["raster", "qrcodegen"],
    barcode: ["raster"],
    printers: ["device", "dialog", "catalog", "numfield", "hub"],
    media: ["dialog", "catalog", "numfield"],
    cal: ["layout", "numfield", "pack", "catalog", "doc", "device"],
    files: ["device", "doc", "dialog"],
    network: ["device", "dialog"],
    templates: ["dialog", "vars", "csv", "doc", "numfield"],
    print: ["device", "pack", "vars", "image", "templates"],
};

export const BOOT = Object.keys(MODULES).slice(0, Object.keys(MODULES).indexOf("app") + 1);
/** Prefetch order: what a user opens first comes first. */
export const LAZY = ["dialog", "printers", "media", "pack", "cal", "vars", "csv", "templates", "image", "print", "files",
    "network", "qrcodegen", "qr", "barcode"];

export const RETRY_MS = [300, 1000, 3000];
export const MAX_IN_FLIGHT = 2;

/** The board's "too many at once" answers: 409 busy (its connection limit)
 *  and 503 card busy (the /sd queue). */
export const isBusy = (status) => status === 409 || status === 503;

const realSleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** The request queue. run(fn, busy): fn() once a slot is free; busy(result,
 *  error) says the board was busy or the request was lost, and then the
 *  whole queue waits out the backoff and fn is queued again. */
export function makeQueue({ max = MAX_IN_FLIGHT, delays = RETRY_MS, sleep = realSleep, now = () => Date.now(), random = Math.random } = {}) {
    let limit = max;
    let active = 0;
    let peak = 0;
    let busySeen = false;
    let until = 0;            // no new request before this (a backoff)
    const waiting = [];

    function pump() {
        while (active < limit && waiting.length) {
            const wait = until - now();
            if (wait > 0) {
                sleep(wait).then(pump);
                return;
            }
            active++;
            peak = Math.max(peak, active);
            waiting.shift()();
        }
    }
    function slot() {
        return new Promise((r) => {
            waiting.push(r);
            pump();
        });
    }
    function done() {
        active--;
        pump();
    }

    async function run(fn, busy = () => false) {
        for (let i = 0; ; i++) {
            await slot();
            let res;
            let err = null;
            try {
                res = await fn(i);
            } catch (e) {
                err = e || new Error("failed");
            } finally {
                done();
            }
            if (!busy(res, err) || i >= delays.length) {
                if (err) {
                    throw err;
                }
                return res;
            }
            busySeen = true;
            limit = 1;
            const ms = Math.round(delays[i] * (0.8 + 0.4 * random()));
            until = Math.max(until, now() + ms);
            await sleep(ms);
        }
    }

    return {
        run,
        idle: () => active === 0 && waiting.length === 0,
        busySeen: () => busySeen,
        stats: () => ({ peak, limit, active, waiting: waiting.length }),
    };
}

export const queue = makeQueue();

/** fetch() through the queue; a busy answer or a lost request is retried.
 *  Resolves to the last Response; rejects only when the last try got none. */
export function fetchRetry(url, init, { q = queue, fetchImpl = (u, i) => fetch(u, i) } = {}) {
    return q.run(async () => {
        const res = await fetchImpl(url, init);
        // the body is read while the slot is held: the transfer is the request
        const body = await res.arrayBuffer();
        return new Response(res.status === 204 ? null : body, { status: res.status, statusText: res.statusText, headers: res.headers });
    }, (res, err) => !!err || isBusy(res.status));
}

/** importer(url) → module promise; the default is the browser's import(). */
export function makeLoader({ importer = (url) => import(url), q = queue } = {}) {
    const done = new Map();      // name -> module promise
    const mods = new Map();      // name -> module, once loaded

    // A module another one imports statically cannot be retried under a new
    // URL (the importer asks for the plain one, which the page now holds as
    // failed), so it fails at once and boot reloads the page.
    const shared = new Set(Object.values(MODULES).flat());

    async function importOne(name) {
        try {
            // any failure counts as busy: import() does not say why
            return await q.run((i) => importer(`./${name}.js${i ? "?r=" + i : ""}`), (res, err) => !!err && !shared.has(name));
        } catch (last) {
            const err = new Error(`could not load ${name}.js (${(last && last.message) || last}) — check the connection to the board and reload the page`);
            err.cause = last;
            throw err;
        }
    }

    /** The module, its static imports loaded first (one at a time). Memoised;
     *  a failure is forgotten, so a later call tries again. */
    function load(name) {
        if (name === "lazy") {
            return Promise.resolve(null);   // the entry: always in the module map
        }
        if (!done.has(name)) {
            if (!MODULES[name]) {
                return Promise.reject(new Error(`unknown module ${name}`));
            }
            const p = (async () => {
                for (const d of MODULES[name]) {
                    await load(d);
                }
                const m = await importOne(name);
                mods.set(name, m);
                return m;
            })();
            done.set(name, p);
            p.catch(() => done.delete(name));
        }
        return done.get(name);
    }

    /** Load names, `parallel` chains at a time (the queue still caps the requests). */
    async function loadAll(names, parallel = 2) {
        const list = [...names];
        const worker = async () => {
            while (list.length) {
                await load(list.shift());
            }
        };
        await Promise.all(Array.from({ length: Math.min(parallel, list.length) }, worker));
    }

    return { load, loadAll, loaded: (name) => mods.get(name) || null };
}

const loader = makeLoader();
export const load = loader.load;
export const loaded = loader.loaded;

/** index.html's entry: the boot graph, then app.js runs. A boot module that
 *  failed cannot be retried in this page (see above), so the page reloads
 *  itself, twice at most, before it gives up and says why. */
export async function boot() {
    const KEY = "btgw-printer.boot-reloads";
    const store = (fn) => {
        try {
            return fn(sessionStorage);
        } catch {
            return null;
        }
    };
    // app.css is the page's own request: wait for it, so the queue's two are all
    const css = document.querySelector('link[rel="stylesheet"]');
    if (css && !css.sheet) {
        await new Promise((r) => {
            css.addEventListener("load", r, { once: true });
            css.addEventListener("error", r, { once: true });
        });
    }
    try {
        await loader.loadAll(BOOT, 2);
        store((s) => s.removeItem(KEY));
    } catch (err) {
        const n = Number(store((s) => s.getItem(KEY))) || 0;
        const reload = n < 2 && store((s) => s.setItem(KEY, String(n + 1))) !== null;
        const el = document.getElementById("status");
        if (el) {
            el.textContent = reload ? "loading failed, reloading…" : String(err.message || err);
            el.className = "err";
        }
        if (reload) {
            setTimeout(() => location.reload(), 1000 + Math.round(Math.random() * 1000));
        }
        throw err;
    }
}

/** The lazy modules, one file at a time with a pause between, only while the
 *  queue is otherwise idle. Stops for good when the tab is hidden or once the
 *  board has said busy: what is left loads on first use. */
export function prefetch(names = LAZY, { pauseMs = 300 } = {}) {
    (async () => {
        for (const n of names) {
            do {
                await realSleep(pauseMs);
            } while (!queue.idle());
            if (document.hidden || queue.busySeen()) {
                return;
            }
            await load(n).catch(() => null);
        }
    })();
}

/** For tests and the console. */
globalThis.btgwQueue = queue;
