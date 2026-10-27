import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { BOOT, LAZY, MODULES, RETRY_MS, fetchRetry, makeLoader, makeQueue } from "../../web/lazy.js";

const WEB = new URL("../../web/", import.meta.url);
const PARENT = new URL("../../btgw/web/js/", import.meta.url);

function staticImports(src) {
    const out = [];
    for (const m of src.matchAll(/^\s*import\s+(?:[^"';]*?\s+from\s+)?["']\.\/([^"']+)\.js["']/gm)) {
        out.push(m[1]);
    }
    for (const m of src.matchAll(/^\}\s+from\s+["']\.\/([^"']+)\.js["']/gm)) {
        out.push(m[1]);
    }
    return [...new Set(out)];
}

test("MODULES lists every module and exactly its static imports", () => {
    const files = readdirSync(WEB).filter((f) => f.endsWith(".js")).map((f) => f.slice(0, -3));
    const known = new Set([...Object.keys(MODULES), "lazy"]);
    assert.deepEqual(files.filter((f) => !known.has(f)), [], "a module lazy.js does not know about");
    for (const [name, deps] of Object.entries(MODULES)) {
        const url = name.startsWith("js/btgw/") ? new URL(name.slice(8) + ".js", PARENT) : new URL(name + ".js", WEB);
        assert.deepEqual([...staticImports(readFileSync(url, "utf8"))].sort(), [...deps].sort(), name);
    }
    assert.deepEqual(staticImports(readFileSync(new URL("lazy.js", WEB), "utf8")), []);
});

test("the boot graph is closed and in dependency order; the rest is prefetched", () => {
    BOOT.forEach((name, i) => {
        for (const d of MODULES[name]) {
            assert.ok(d === "lazy" || BOOT.indexOf(d) < i, `${name} needs ${d} first`);
        }
    });
    assert.equal(BOOT.at(-1), "app");
    assert.deepEqual([...LAZY].sort(), Object.keys(MODULES).filter((n) => !BOOT.includes(n)).sort());
    LAZY.forEach((name, i) => {
        for (const d of MODULES[name]) {
            assert.ok(BOOT.includes(d) || LAZY.indexOf(d) < i, `prefetch ${d} before ${name}`);
        }
    });
    assert.ok(BOOT.length <= 14, `boot graph ${BOOT.length}`);
});

function fakeImporter(failures = {}) {
    const urls = [];
    return {
        urls,
        importer: async (url) => {
            urls.push(url);
            const name = url.replace(/^\.\//, "").replace(/\.js(\?.*)?$/, "");
            if ((failures[name] || 0) > 0) {
                failures[name]--;
                throw new TypeError("Failed to fetch dynamically imported module: " + url);
            }
            return { name };
        },
    };
}

test("a failed import is retried with a cache-busting query after 300 ms, 1 s, 3 s", async () => {
    const f = fakeImporter({ network: 2 });
    const c = fakeClock();
    const slept = c.slept;
    const q = makeQueue({ sleep: c.sleep, now: c.now, random: () => 0.5 });
    const L = makeLoader({ importer: f.importer, q });
    assert.deepEqual(await L.load("network"), { name: "network" });
    assert.deepEqual(f.urls, ["./js/btgw/wsapi.js", "./js/btgw/fs.js", "./device.js", "./numfield.js", "./dialog.js",
        "./network.js", "./network.js?r=1", "./network.js?r=2"]);
    assert.deepEqual(slept, [300, 1000]);
    assert.deepEqual(RETRY_MS, [300, 1000, 3000]);
    assert.equal(L.loaded("network").name, "network");
    const n = f.urls.length;
    await L.load("network");
    assert.equal(f.urls.length, n, "memoised");
});

test("after 3 retries the error says what failed, and a later load tries again", async () => {
    const f = fakeImporter({ media: 4 });
    const c = fakeClock();
    const slept = c.slept;
    const q = makeQueue({ sleep: c.sleep, now: c.now, random: () => 0.5 });
    const L = makeLoader({ importer: f.importer, q });
    await assert.rejects(L.load("media"), /could not load media\.js .*reload the page/);
    assert.deepEqual(f.urls.filter((u) => u.includes("media")), ["./media.js", "./media.js?r=1", "./media.js?r=2", "./media.js?r=3"]);
    assert.deepEqual(slept, [300, 1000, 3000]);
    assert.equal(L.loaded("media"), null);
    assert.deepEqual(await L.load("media"), { name: "media" });
});

test("a module another imports statically is not retried under a new URL: it fails at once", async () => {
    const f = fakeImporter({ dialog: 1 });
    const c = fakeClock();
    const L = makeLoader({ importer: f.importer, q: makeQueue({ sleep: c.sleep, now: c.now }) });
    await assert.rejects(L.load("printers"), /could not load dialog\.js/);
    assert.deepEqual(f.urls.filter((u) => u.includes("dialog")), ["./dialog.js"]);
    assert.ok(!f.urls.includes("./printers.js"));
});

test("a module's imports load first, one at a time; loadAll keeps to its limit", async () => {
    let inFlight = 0;
    let peak = 0;
    const urls = [];
    const L = makeLoader({
        q: makeQueue(),
        importer: async (url) => {
            urls.push(url);
            inFlight++;
            peak = Math.max(peak, inFlight);
            await new Promise((r) => setTimeout(r, 2));
            inFlight--;
            return {};
        },
    });
    await L.load("templates");
    assert.deepEqual(urls, ["./numfield.js", "./dialog.js", "./vars.js", "./csv.js", "./raster.js", "./doc.js", "./templates.js"]);
    assert.equal(peak, 1);
    urls.length = 0;
    peak = 0;
    await L.loadAll(BOOT, 3);
    assert.ok(peak <= 2, `peak ${peak}`);
    assert.equal(urls.at(-1), "./app.js");
    assert.equal(new Set(urls).size, urls.length, "each module once");
});

// ---- the request queue ----

function fakeClock() {
    let t = 0;
    const slept = [];
    return {
        slept,
        now: () => t,
        sleep: async (ms) => {
            slept.push(ms);
            t += ms;
            await new Promise((r) => setImmediate(r));
        },
    };
}

test("the queue keeps two requests in flight, one after the board said busy", async () => {
    const c = fakeClock();
    const q = makeQueue({ sleep: c.sleep, now: c.now, random: () => 0.5 });
    let inFlight = 0;
    let peak = 0;
    const job = (busyFirst) => {
        let n = 0;
        return q.run(async () => {
            inFlight++;
            peak = Math.max(peak, inFlight);
            await new Promise((r) => setTimeout(r, 2));
            inFlight--;
            return { status: busyFirst && n++ === 0 ? 409 : 200 };
        }, (r) => r.status === 409);
    };
    await Promise.all([job(), job(), job(), job(), job()]);
    assert.equal(peak, 2);
    assert.equal(q.stats().limit, 2);
    assert.equal(q.busySeen(), false);
    peak = 0;
    await Promise.all([job(true), job(), job(), job()]);
    assert.equal(q.busySeen(), true);
    assert.equal(q.stats().limit, 1);
    assert.ok(c.slept.includes(300));
    peak = 0;
    await Promise.all([job(), job(), job()]);
    assert.equal(peak, 1);
});

test("fetchRetry retries 409 busy, 503 and a dropped request with jittered backoff, then gives the answer", async () => {
    const c = fakeClock();
    const q = makeQueue({ sleep: c.sleep, now: c.now, random: () => 0 });
    const answers = [new TypeError("Failed to fetch"), 409, 503, 200];
    const calls = [];
    const fetchImpl = async (url) => {
        calls.push(url);
        const a = answers.shift();
        if (a instanceof Error) {
            throw a;
        }
        return new Response(a === 200 ? '{"x":1}' : '{"ok":false,"error":"busy"}', { status: a });
    };
    const r = await fetchRetry("presets/models.json", {}, { q, fetchImpl });
    assert.equal(r.status, 200);
    assert.deepEqual(await r.json(), { x: 1 });
    assert.equal(calls.length, 4);
    assert.deepEqual(c.slept.filter((ms) => ms > 10), [240, 800, 2400]);   // 300 / 1000 / 3000 × 0.8 (random 0)

    const c2 = fakeClock();
    const q2 = makeQueue({ sleep: c2.sleep, now: c2.now });
    const r2 = await fetchRetry("x", {}, { q: q2, fetchImpl: async () => new Response("", { status: 409 }) });
    assert.equal(r2.status, 409, "the last answer, after 3 retries");
    const r3 = await fetchRetry("y", {}, { q: q2, fetchImpl: async () => new Response("", { status: 404 }) });
    assert.equal(r3.status, 404, "not busy: no retry");
});
