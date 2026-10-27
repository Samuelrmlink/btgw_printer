/** The Printers dialog: saved configs, scan, connect / disconnect.
 *
 * Loaded on first use (lazy.js); the live state it shows is hub.js's. Ops
 * (docs/architecture.md): printer.scan, printer.devices, printer.disconnect,
 * and the printer.found event; the rest go through hub.js. */

import { call, on } from "./device.js";
import { Dialog, confirmDialog } from "./dialog.js";
import {
    currentPrinterAddr, findModel, findPrinter, forgetPrinter, models, printersPath, savePrinter, savedPrinters,
    setCurrentPrinter,
} from "./catalog.js";
import { setBounds } from "./numfield.js";
import * as hub from "./hub.js";
import { familyLabel } from "./store.js";

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);

/** onChange({printerChanged}) after the current printer or its config changed. */
export function bindPrinters({ onChange }) {
    const { stateOf, stateText } = hub;
    let found = [];
    let editing = null;           // {addr, driver, isNew}
    let scanning = false;

    const dlg = new Dialog("printers", {
        initialFocus: () => $("printers-list").querySelector("button") || $("printers-scan"),
        onKey: (e) => {
            if (e.key === "s" && !/INPUT|TEXTAREA|SELECT/.test(e.target.tagName) && $("printer-edit").hidden) {
                e.preventDefault();
                scan();
                return true;
            }
            return false;
        },
    });

    setBounds("pe-density", 0, 15);
    setBounds("pe-speed", -1, 15);
    setBounds("pe-copies", 1, 99);

    function showErr(msg) {
        $("printers-err").hidden = !msg;
        $("printers-err").textContent = msg || "";
    }

    function modelName(p) {
        const m = findModel({ driver: p.driver, id: p.model });
        return m ? `${m.vendor || ""} ${m.name}`.trim() : p.model || "?";
    }

    function rowButton(label, cls, fn, title) {
        const b = document.createElement("button");
        b.type = "button";
        b.className = cls || "";
        b.innerHTML = label;
        if (title) {
            b.title = title;
        }
        b.addEventListener("click", (e) => {
            e.stopPropagation();
            fn();
        });
        return b;
    }

    function paint() {
        const list = $("printers-list");
        list.replaceChildren();
        const cur = currentPrinterAddr();
        for (const p of savedPrinters()) {
            const li = document.createElement("li");
            li.classList.toggle("selected", p.addr === cur);
            li.dataset.addr = p.addr;
            const st = stateOf(p.addr).state;
            const connected = st === "connected" || st === "printing";
            li.append(
                rowButton(`<span>${esc(p.nickname || p.addr)}${p.addr === cur ? " ✓" : ""}</span><span class="row-meta">${esc(modelName(p))} · ${esc(stateText(p.addr))}</span>`,
                    "row-main", () => use(p.addr), "Use for printing"),
                rowButton(connected ? "Disconnect" : "Connect", "", () => (connected ? disconnect(p) : connect(p))),
                rowButton("Edit", "", () => edit(p, false)),
                rowButton("Forget", "", () => forget(p)),
            );
            list.appendChild(li);
        }
        $("printers-empty").hidden = savedPrinters().length > 0;
        const fl = $("printers-found");
        fl.replaceChildren();
        const saved = new Set(savedPrinters().map((p) => p.addr));
        const show = found.filter((d) => !saved.has(d.addr));
        for (const d of show) {
            const li = document.createElement("li");
            li.append(rowButton(`<span>${esc(d.name || d.addr)}</span><span class="row-meta">${esc(d.model || d.driver)} · ${esc(d.addr)}${d.rssi != null ? ` · ${d.rssi} dBm` : ""}</span>`,
                "row-main", () => edit({ addr: d.addr, driver: d.driver, model: d.model, nickname: d.name }, true), "Save this printer"));
            fl.appendChild(li);
        }
        $("printers-found-empty").hidden = show.length > 0;
        $("printers-found-empty").textContent = scanning ? "Scanning…" : "Nothing found yet. Scan (s) with the printer switched on.";
        $("printers-live").textContent = hub.hubUp() ? `printer_hub running · ${models().filter((m) => m.live).length} live model(s) · saved in ${printersPath()}` : "printer_hub is not running on the board: printing is off, designing still works.";
        $("printers-scan").disabled = !hub.hubUp() || scanning;
    }

    async function refreshDevices() {
        try {
            const r = await call("printer.devices", {});
            for (const d of r.devices || []) {
                upsert(d);
            }
        } catch {
            /* keep */
        }
        if (dlg.isOpen()) {
            paint();
        }
    }

    async function scan() {
        if (!hub.hubUp() || scanning) {
            return;
        }
        showErr("");
        scanning = true;
        paint();
        // a scan only reaches drivers that run: start the chosen family's first
        const driver = $("printers-family").value;
        try {
            await hub.ensureDriver(driver);
            await call("printer.scan", driver ? { ms: 8000, driver } : { ms: 8000 });
        } catch (err) {
            showErr(err.module ? `couldn't start the ${err.module} driver: ${err.message}` : String(err.message || err));
            scanning = false;
            paint();
            return;
        }
        setTimeout(async () => {
            scanning = false;
            await refreshDevices();
            paint();
        }, 8500);
    }

    async function connect(p) {
        showErr("");
        const done = hub.requestConnect(p).catch((err) =>
            showErr(err.module ? `couldn't start the ${err.module} driver: ${err.message}` : String(err.message || err)));
        paint();
        await done;
        paint();
    }

    async function disconnect(p) {
        try {
            await call("printer.disconnect", { driver: p.driver, addr: p.addr });
        } catch (err) {
            showErr(String(err.message || err));
        }
    }

    /** opts.fresh: the printer was just added from a scan (hub.requestConnect). */
    async function use(addr, opts) {
        await setCurrentPrinter(addr).catch((err) => showErr(String(err.message || err)));
        paint();
        onChange && onChange({ printerChanged: true });
        const p = findPrinter(addr);
        if (p) {
            hub.connectIfDown(p, opts).then(() => paint());
        }
    }

    async function forget(p) {
        if (!(await confirmDialog(`Forget ${p.nickname || p.addr}? Its calibration is deleted too.`, { okLabel: "Forget", danger: true }))) {
            return;
        }
        await forgetPrinter(p.addr).catch((err) => showErr(String(err.message || err)));
        paint();
        onChange && onChange({ printerChanged: true });
    }

    function fillModels(driver, sel) {
        const s = $("pe-model");
        s.replaceChildren();
        for (const m of models().filter((x) => !driver || x.driver === driver)) {
            const o = document.createElement("option");
            o.value = m.key;
            o.textContent = `${m.vendor || ""} ${m.name}`.trim() + (m.live ? "" : " (offline)");
            s.appendChild(o);
        }
        const want = models().find((m) => (m.driver === driver || !driver) && m.id === sel);
        if (want) {
            s.value = want.key;
        }
    }

    function edit(p, isNew) {
        editing = { addr: p.addr, driver: p.driver, isNew };
        $("printer-edit-title").textContent = isNew ? `Save ${p.nickname || p.addr}` : `Edit ${p.nickname || p.addr}`;
        $("pe-nick").value = p.nickname || "";
        fillModels(p.driver, p.model);
        $("pe-density").value = p.density != null ? String(p.density) : "";
        $("pe-speed").value = p.speed != null && p.speed >= 0 ? String(p.speed) : "";
        $("pe-copies").value = String(p.copies || 1);
        $("pe-reconnect").checked = p.auto_reconnect !== false;
        $("printer-edit").hidden = false;
        $("printers-actions").hidden = true;
        $("pe-nick").focus();
    }

    function endEdit() {
        editing = null;
        $("printer-edit").hidden = true;
        $("printers-actions").hidden = false;
        paint();
        ($("printers-list").querySelector("button") || $("printers-scan")).focus();
    }

    $("pe-save").addEventListener("click", async () => {
        if (!editing) {
            return;
        }
        const key = $("pe-model").value;
        const m = models().find((x) => x.key === key);
        const num = (id) => {
            const v = $(id).value.trim();
            return v === "" ? null : Number(v);
        };
        const rec = {
            addr: editing.addr,
            driver: m ? m.driver : editing.driver,
            model: m ? m.id : "",
            nickname: $("pe-nick").value.trim() || editing.addr,
            density: num("pe-density"),
            speed: num("pe-speed") ?? -1,
            copies: Math.max(1, num("pe-copies") || 1),
            auto_reconnect: $("pe-reconnect").checked,
        };
        try {
            await savePrinter(rec);
            const wasNew = editing.isNew;
            endEdit();
            if (wasNew || !currentPrinterAddr()) {
                await use(rec.addr, { fresh: wasNew });   // just picked from a scan: it is on
            }
            onChange && onChange({ printerChanged: true });
        } catch (err) {
            showErr(String(err.message || err));
        }
    });
    $("pe-cancel").addEventListener("click", () => endEdit());
    $("printers-scan").addEventListener("click", () => scan());
    $("printers-close").addEventListener("click", () => dlg.close());

    hub.onHub(() => {
        if (dlg.isOpen()) {
            paint();
        }
    });
    function upsert(d) {
        const i = found.findIndex((x) => x.addr === d.addr && x.driver === d.driver);
        if (d.gone) {
            if (i >= 0) {
                found.splice(i, 1);
            }
        } else if (i >= 0) {
            found[i] = { ...found[i], ...d };
        } else {
            found.push(d);
        }
    }

    // A driver reports a device again when more is known (a Bluetooth name
    // that came late fills in name and model), or with gone: true when the
    // late name shows it is not that driver's printer. One entry per driver
    // and address: two drivers may both claim one address.
    on("printer.found", (d) => {
        if (!d || !d.addr) {
            return;
        }
        upsert(d);
        if (dlg.isOpen()) {
            paint();
        }
    });

    /** "Scan for": our drivers (app.json), the current printer's first time round. */
    function fillFamilies() {
        const sel = $("printers-family");
        const keep = sel.value;
        sel.replaceChildren();
        for (const d of hub.ourDrivers()) {
            const o = document.createElement("option");
            o.value = d;
            o.textContent = familyLabel(d, models());
            sel.appendChild(o);
        }
        const cur = findPrinter(currentPrinterAddr());
        const want = keep || (cur && cur.driver);
        if (want && hub.ourDrivers().includes(want)) {
            sel.value = want;
        }
    }

    async function open() {
        fillFamilies();
        showErr("");
        $("printer-edit").hidden = true;
        $("printers-actions").hidden = false;
        dlg.open();
        paint();
        await hub.refreshDrivers();
        await hub.refreshStatus();
        await refreshDevices();
        paint();
    }

    return {
        open,
        isOpen: () => dlg.isOpen(),
    };
}
