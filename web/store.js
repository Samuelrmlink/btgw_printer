/** Where the settings files (printers.json, media.json) live.
 *
 * Flash (printer/) as a rule; the card (sd/printer/) when the app is served
 * from the card, when a settings file is found there, or when a flash write
 * fails with the card mounted (parent bug B3: LittleFS writes failing). Once
 * on the card, the rest of the session stays there. Reads try the card
 * first whenever it is mounted. No board access here: read/write/sdUp are
 * passed in (catalog.js), so the choice is unit-testable. */

export const FLASH_DIR = "printer/";
export const SD_DIR = "sd/printer/";

export function servedFromSd(pathname) {
    return String(pathname || "").startsWith("/sd/");
}

/** read(path) → JSON or null when missing; write(path, obj) throws on failure. */
export function settingsStore({ read, write, sdUp, served = false }) {
    let onSd = !!served;
    return {
        onSd: () => onSd,
        path: (name) => (onSd ? SD_DIR : FLASH_DIR) + name,
        async load(name) {
            for (const dir of sdUp() ? [SD_DIR, FLASH_DIR] : [FLASH_DIR]) {
                const j = await read(dir + name);
                if (j != null) {
                    onSd = onSd || dir === SD_DIR;
                    return j;
                }
            }
            return null;
        },
        /** → the path written. */
        async save(name, obj) {
            if (!onSd) {
                try {
                    await write(FLASH_DIR + name, obj);
                    return FLASH_DIR + name;
                } catch (e) {
                    if (!sdUp()) {
                        throw e;
                    }
                    onSd = true;
                }
            }
            await write(SD_DIR + name, obj);
            return SD_DIR + name;
        },
    };
}

/** nelko_pm220 and nelko_p21 became one driver, nelko_tspl (models pm220, p21). */
const MERGED = { nelko_pm220: "pm220", nelko_p21: "p21" };

export function migrateDriver(driver) {
    return MERGED[driver] ? "nelko_tspl" : driver;
}

/** A printers.json as loaded → the current driver names. */
export function migratePrinters(j) {
    if (!j || !Array.isArray(j.printers)) {
        return { version: 1, printers: [] };
    }
    return { ...j, printers: j.printers.map((p) =>
        (MERGED[p.driver] ? { ...p, driver: "nelko_tspl", model: p.model || MERGED[p.driver] } : p)) };
}

// ---- connecting a saved Bluetooth Classic printer ------------------------
// Paging a Classic printer that is switched off stalls the gateway's whole
// Wi-Fi for about half a minute (parent B4), so the app asks before it pages
// a saved Classic printer: the user switches it on, then says Connect. Not
// for BLE, not for one being added from a scan, and not for one a scan in
// this page has found (it is on), until the driver says it is lost again.

export function isClassic(transport) {
    return String(transport || "").toLowerCase() === "classic";
}

/** saved: in printers.json; seen: a scan in this page found it and no
 *  lostAgain(why) came after. */
export function askBeforeConnect({ transport, saved, seen = false }) {
    return isClassic(transport) && !!saved && !seen;
}

/** A printer.state why that means the printer is gone again (off or away). */
export function lostAgain(why) {
    return /link lost|not reachable|stopped retrying/i.test(String(why || ""));
}

// ---- the printer service: the PEX modules the app needs ------------------
// Nothing loads them after a board boot (parent B3: cfg/launch.json can't be
// written; the parent's launchers don't read app.json's pex list). So the app
// starts printer_hub itself at boot, and a driver only when a printer of that
// driver is about to be connected or scanned for (hub.js).

export const HUB_MODULE = "printer_hub";
export const FALLBACK_DRIVERS = ["nelko_tspl"];

/** app.json (or null) → our driver modules, in the manifest's order. */
export function serviceDrivers(manifest) {
    const names = (manifest && Array.isArray(manifest.pex) ? manifest.pex : [])
        .map((m) => (typeof m === "string" ? m : m && m.name))
        .filter((n) => typeof n === "string" && /^[A-Za-z0-9_]+$/.test(n) && n !== HUB_MODULE);
    return names.length ? [...new Set(names)] : [...FALLBACK_DRIVERS];
}

/** A pex.load / pex.start error that means another client got there first. */
export function alreadyDone(err) {
    return /already/i.test(`${(err && err.code) || ""} ${(err && err.message) || ""}`);
}

/** "Nelko PM220 / P21" for a driver, from its models ({driver, vendor, name}). */
export function familyLabel(driver, models) {
    const mine = (models || []).filter((m) => m.driver === driver);
    if (!mine.length) {
        return driver;
    }
    const vendor = mine[0].vendor || "";
    return `${vendor} ${mine.map((m) => m.name).join(" / ")}`.trim();
}
