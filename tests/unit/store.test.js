import { test } from "node:test";
import assert from "node:assert/strict";
import { migrateDriver, migratePrinters, servedFromSd, settingsStore } from "../../web/store.js";

/** An in-memory board: flash and card as path → object maps. */
function board({ flashRo = false, sd = true } = {}) {
    const files = new Map();
    const writes = [];
    return {
        files,
        writes,
        sd: () => sd,
        read: async (p) => (files.has(p) ? files.get(p) : null),
        write: async (p, obj) => {
            writes.push(p);
            if (p.startsWith("sd/") ? !sd : flashRo) {
                throw new Error("write failed");
            }
            files.set(p, obj);
        },
    };
}
const storeOn = (b, served) => settingsStore({ read: b.read, write: b.write, sdUp: b.sd, served });

test("servedFromSd: only paths under /sd/", () => {
    assert.equal(servedFromSd("/sd/apps/printer/"), true);
    assert.equal(servedFromSd("/printer/"), false);
    assert.equal(servedFromSd("/sdcard/"), false);
    assert.equal(servedFromSd(undefined), false);
});

test("flash by default; a writable flash keeps the settings there", async () => {
    const b = board();
    const s = storeOn(b);
    assert.equal(s.path("printers.json"), "printer/printers.json");
    assert.equal(await s.save("printers.json", { a: 1 }), "printer/printers.json");
    assert.deepEqual(b.writes, ["printer/printers.json"]);
    assert.equal(s.onSd(), false);
});

test("served from the card: settings go to sd/printer/ without trying flash", async () => {
    const b = board();
    const s = storeOn(b, true);
    assert.equal(s.path("media.json"), "sd/printer/media.json");
    assert.equal(await s.save("media.json", {}), "sd/printer/media.json");
    assert.deepEqual(b.writes, ["sd/printer/media.json"]);
});

test("a failed flash write moves to the card for the rest of the session", async () => {
    const b = board({ flashRo: true });
    const s = storeOn(b);
    assert.equal(await s.save("printers.json", { a: 1 }), "sd/printer/printers.json");
    assert.equal(await s.save("media.json", { b: 2 }), "sd/printer/media.json");
    assert.deepEqual(b.writes, ["printer/printers.json", "sd/printer/printers.json", "sd/printer/media.json"]);
    assert.equal(s.path("printers.json"), "sd/printer/printers.json");
});

test("a failed flash write with no card is an error, and the store stays on flash", async () => {
    const b = board({ flashRo: true, sd: false });
    const s = storeOn(b);
    await assert.rejects(s.save("printers.json", {}), /write failed/);
    assert.equal(s.onSd(), false);
});

test("reads try the card first when it is mounted, then flash", async () => {
    const b = board();
    b.files.set("printer/printers.json", { from: "flash" });
    const s = storeOn(b);
    assert.deepEqual(await s.load("printers.json"), { from: "flash" });
    assert.equal(s.onSd(), false);
    b.files.set("sd/printer/printers.json", { from: "sd" });
    assert.deepEqual(await s.load("printers.json"), { from: "sd" });
    // found on the card: saves follow it there, or the next load would read a stale card copy
    assert.equal(s.path("printers.json"), "sd/printer/printers.json");
    assert.equal(await s.load("nothing.json"), null);
});

test("no card: the card is never read", async () => {
    const b = board({ sd: false });
    b.files.set("sd/printer/printers.json", { from: "sd" });
    assert.equal(await storeOn(b).load("printers.json"), null);
});

test("saved printers: nelko_pm220 / nelko_p21 load as nelko_tspl with their model", () => {
    const j = migratePrinters({ version: 1, current: "A", printers: [
        { addr: "A", driver: "nelko_pm220", model: "pm220", calibration: { x: 1 } },
        { addr: "B", driver: "nelko_p21", model: "p21" },
        { addr: "C", driver: "nelko_p21" },
        { addr: "D", driver: "nelko_pl70e", model: "pl70e" },
    ] });
    assert.equal(j.current, "A");
    assert.deepEqual(j.printers.map((p) => [p.driver, p.model]),
        [["nelko_tspl", "pm220"], ["nelko_tspl", "p21"], ["nelko_tspl", "p21"], ["nelko_pl70e", "pl70e"]]);
    assert.deepEqual(j.printers[0].calibration, { x: 1 });
    assert.deepEqual(migratePrinters(null), { version: 1, printers: [] });
    assert.deepEqual(migratePrinters({ printers: "bad" }), { version: 1, printers: [] });
    assert.equal(migrateDriver("nelko_pm220"), "nelko_tspl");
    assert.equal(migrateDriver("phomemo_d30"), "phomemo_d30");
});
