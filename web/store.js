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
