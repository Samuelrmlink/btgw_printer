/** Printers: saved configs, scan, connect / disconnect, live state.
 *
 * All through printer_hub's ops (docs/architecture.md): printer.drivers,
 * printer.scan, printer.devices, printer.connect, printer.disconnect,
 * printer.status, and the printer.found / printer.state / printer.status events. */

import { call, on } from "./device.js";
import { Dialog, confirmDialog } from "./dialog.js";
import {
    currentPrinterAddr, findModel, findPrinter, forgetPrinter, models, savePrinter, savedPrinters,
    setCurrentPrinter, setLiveDrivers,
} from "./catalog.js";
import { setBounds } from "./numfield.js";

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);

export function bindPrinters({ setStatus, onChange }) {
    const live = new Map();       // addr -> {state, why, battery, paper, lid, media, model, driver}
    let found = [];
    let hubUp = false;
    let editing = null;           // {addr, driver, isNew}
    let scanning = false;

    const dlg = new Dialog("printers", {
        initialFocus: () => $("printers-list").querySelector("button") || $("printers-scan"),
        onKey: (e) => {
            if (e.key === "s" && !/INPUT|TEXTAREA/.test(e.target.tagName) && $("printer-edit").hidden) {
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

    function stateOf(addr) {
        return live.get(addr) || { state: "unknown" };
    }

    function stateText(addr) {
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
        $("printers-live").textContent = hubUp ? `printer_hub running · ${models().filter((m) => m.live).length} live model(s)` : "printer_hub is not running on the board: printing is off, designing still works.";
        $("printers-scan").disabled = !hubUp || scanning;
    }

    async function refreshDrivers() {
        try {
            const r = await call("printer.drivers");
            setLiveDrivers(r.drivers || []);
            hubUp = true;
        } catch {
            setLiveDrivers(null);
            hubUp = false;
        }
        return hubUp;
    }

    async function refreshStatus() {
        if (!hubUp) {
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
        onChange && onChange();
        if (dlg.isOpen()) {
            paint();
        }
    }

    async function refreshDevices() {
        try {
            const r = await call("printer.devices", {});
            found = r.devices || [];
        } catch {
            /* keep */
        }
        if (dlg.isOpen()) {
            paint();
        }
    }

    async function scan() {
        if (!hubUp || scanning) {
            return;
        }
        showErr("");
        scanning = true;
        paint();
        try {
            await call("printer.scan", { ms: 8000 });
        } catch (err) {
            showErr(String(err.message || err));
        }
        setTimeout(async () => {
            scanning = false;
            await refreshDevices();
            paint();
        }, 8500);
    }

    async function connect(p) {
        showErr("");
        try {
            live.set(p.addr, { ...stateOf(p.addr), state: "connecting", why: "" });
            paint();
            await call("printer.connect", { driver: p.driver, addr: p.addr, config: configOf(p) });
        } catch (err) {
            live.set(p.addr, { ...stateOf(p.addr), state: "error", why: String(err.message || err) });
            showErr(String(err.message || err));
        }
        paint();
        onChange && onChange();
    }

    async function disconnect(p) {
        try {
            await call("printer.disconnect", { driver: p.driver, addr: p.addr });
        } catch (err) {
            showErr(String(err.message || err));
        }
    }

    function configOf(p) {
        return { model: p.model, density: p.density, speed: p.speed, nickname: p.nickname };
    }

    async function use(addr) {
        await setCurrentPrinter(addr).catch((err) => showErr(String(err.message || err)));
        paint();
        onChange && onChange({ printerChanged: true });
        const p = findPrinter(addr);
        if (p && hubUp) {
            const st = stateOf(addr).state;
            if (st !== "connected" && st !== "printing" && st !== "connecting") {
                connect(p);
            }
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
                await use(rec.addr);
            }
            onChange && onChange({ printerChanged: true });
        } catch (err) {
            showErr(String(err.message || err));
        }
    });
    $("pe-cancel").addEventListener("click", () => endEdit());
    $("printers-scan").addEventListener("click", () => scan());
    $("printers-close").addEventListener("click", () => dlg.close());
    $("printer-btn").addEventListener("click", () => open());

    on("printer.state", (d) => {
        if (!d || !d.addr) {
            return;
        }
        live.set(d.addr, { ...stateOf(d.addr), ...d });
        onChange && onChange();
        if (dlg.isOpen()) {
            paint();
        }
    });
    on("printer.status", (d) => {
        if (!d || !d.addr) {
            return;
        }
        live.set(d.addr, { ...stateOf(d.addr), ...d });
        onChange && onChange();
    });
    on("printer.found", (d) => {
        if (!d || !d.addr || found.some((x) => x.addr === d.addr)) {
            return;
        }
        found.push(d);
        if (dlg.isOpen()) {
            paint();
        }
    });

    async function open() {
        showErr("");
        $("printer-edit").hidden = true;
        $("printers-actions").hidden = false;
        dlg.open();
        paint();
        await refreshDrivers();
        await refreshStatus();
        await refreshDevices();
        paint();
    }

    /** Connect the current printer if it wants auto-reconnect and is not up. */
    async function autoConnect() {
        const addr = currentPrinterAddr();
        const p = addr && findPrinter(addr);
        if (!p || !hubUp || p.auto_reconnect === false) {
            return;
        }
        const st = stateOf(addr).state;
        if (st !== "connected" && st !== "printing" && st !== "connecting") {
            connect(p);
        }
    }

    return {
        open,
        isOpen: () => dlg.isOpen(),
        refreshDrivers,
        refreshStatus,
        autoConnect,
        stateOf,
        stateText,
        hubUp: () => hubUp,
    };
}
