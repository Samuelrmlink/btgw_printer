/** Network dialog: the pm220 Wi-Fi modal on the parent's API, plus WireGuard.
 *
 * Station / SoftAP use bluetooth_rp2_webapp's /api/wifi* routes (kept on plain
 * HTTP there so provisioning never depends on the WebSocket). WireGuard uses
 * the wg.* ops and cfg/wireguard.json; mDNS shows mdns.status. Tabs for
 * features the image lacks (sys.info modules) are hidden. */

import { call, hasModule, readJsonOr, wifi, writeJson } from "./device.js";
import { Dialog } from "./dialog.js";

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const WG_PATH = "cfg/wireguard.json";

export function bindNetwork({ setStatus }) {
    let tab = "sta";
    let status = null;
    let pollTimer = 0;
    let snapshot = { ssid: "", psk: "" };
    let selectedKnown = "";
    let known = [];
    let scanned = [];
    let wgCfg = null;

    const dlg = new Dialog("wifi", {
        onClose: () => {
            clearTimeout(pollTimer);
        },
        initialFocus: () => overlayTab(tab),
        onKey: (e) => {
            const mod = e.ctrlKey || e.metaKey;
            if (mod && !e.altKey && e.key.toLowerCase() === "s") {
                e.preventDefault();
                save();
                return true;
            }
            // Arrows on the tab strip change the pane (pm220 rule).
            if (e.target && e.target.classList && e.target.classList.contains("wifi-tab")) {
                const order = tabs();
                const i = order.indexOf(tab);
                let d = 0;
                if (["ArrowRight", "ArrowDown", "l", "j"].includes(e.key)) {
                    d = 1;
                } else if (["ArrowLeft", "ArrowUp", "h", "k"].includes(e.key)) {
                    d = -1;
                }
                if (d) {
                    e.preventDefault();
                    setTab(order[(i + d + order.length) % order.length]);
                    overlayTab(tab).focus();
                    return true;
                }
            }
            return false;
        },
    });
    const sub = new Dialog("wifi-sub", {
        initialFocus: () => (!$("wifi-sub-form").hidden ? ($("wifi-sub-ssid").readOnly ? $("wifi-sub-psk") : $("wifi-sub-ssid")) : $("wifi-sub-ok")),
        onKey: (e) => {
            if (e.key === "Enter" && e.target.id !== "wifi-sub-cancel" && e.target.tagName !== "BUTTON") {
                e.preventDefault();
                $("wifi-sub-ok").click();
                return true;
            }
            return false;
        },
    });
    let subOk = null;

    function overlayTab(name) {
        return document.querySelector(`#wifi .wifi-tab[data-tab="${name}"]`);
    }

    function tabs() {
        return [...document.querySelectorAll("#wifi .wifi-tab")].filter((b) => !b.hidden).map((b) => b.dataset.tab);
    }

    function showErr(el, msg) {
        el.hidden = !msg;
        el.textContent = msg || "";
    }

    function setTab(name) {
        tab = name;
        for (const b of document.querySelectorAll("#wifi .wifi-tab")) {
            b.setAttribute("aria-selected", b.dataset.tab === name ? "true" : "false");
        }
        for (const t of ["sta", "ap", "wg", "mdns"]) {
            $(`wifi-pane-${t}`).hidden = t !== name;
        }
        if (name === "wg") {
            refreshWg();
        } else if (name === "mdns") {
            refreshMdns();
        }
        updateSave();
    }

    function dirty() {
        return tab === "ap" && ($("wifi-ap-ssid").value.trim() !== snapshot.ssid || $("wifi-ap-psk").value !== snapshot.psk);
    }

    function updateSave() {
        $("wifi-save").hidden = !dirty();
    }

    function paintLive() {
        const s = status;
        if (!s) {
            $("wifi-live").textContent = "";
            return;
        }
        const bits = [];
        bits.push(s.mode === "sta" ? `joined ${s.ssid} · ${s.ip}` : s.mode);
        if (s.want && s.want !== s.mode) {
            bits.push(`want ${s.want}`);
        }
        if (s.ap) {
            bits.push(`hotspot ${s.ap_ssid} (${s.ap_clients} client${s.ap_clients === 1 ? "" : "s"})`);
        }
        if (s.join_deferred) {
            bits.push("join waits for Bluetooth");
        }
        if (s.error) {
            bits.push(s.error);
        }
        $("wifi-live").textContent = bits.join(" · ");
        $("wifi-scan-warn").hidden = !s.ap;
    }

    async function refreshStatus(fillForm) {
        try {
            status = await wifi.status();
            paintLive();
            if (fillForm) {
                $("wifi-mode").value = status.want || "auto";
                $("wifi-ap-ssid").value = status.ap_ssid || "";
                $("wifi-ap-psk").value = status.ap_psk || "";
                snapshot = { ssid: status.ap_ssid || "", psk: status.ap_psk || "" };
            }
        } catch (err) {
            showErr($("wifi-err"), String(err.message || err));
        }
    }

    function poll() {
        clearTimeout(pollTimer);
        pollTimer = setTimeout(async () => {
            if (!dlg.isOpen()) {
                return;
            }
            await refreshStatus(false);
            poll();
        }, 4000);
    }

    function row(label, meta, onClick, extraClass = "") {
        const li = document.createElement("li");
        li.className = extraClass;
        const b = document.createElement("button");
        b.type = "button";
        b.className = "row-main";
        b.innerHTML = `<span>${esc(label)}</span><span class="row-meta">${esc(meta)}</span>`;
        b.addEventListener("click", onClick);
        li.appendChild(b);
        return li;
    }

    function paintKnown() {
        const list = $("wifi-known-list");
        list.replaceChildren();
        for (const k of known) {
            list.appendChild(row(k.ssid, `${k.psk ? "password saved" : "(open)"}${k.current ? " · connected" : ""}`, () => {
                selectedKnown = k.ssid;
                $("wifi-known-actions").hidden = false;
                for (const li of list.children) {
                    li.classList.toggle("current", li.firstChild.textContent.startsWith(k.ssid));
                }
                $("wifi-known-connect").focus();
            }, k.ssid === selectedKnown ? "current" : ""));
        }
        $("wifi-known-empty").hidden = known.length > 0;
        $("wifi-known-actions").hidden = !known.some((k) => k.ssid === selectedKnown);
    }

    async function refreshKnown() {
        try {
            known = (await wifi.networks()).networks || [];
        } catch (err) {
            known = [];
            showErr($("wifi-err"), String(err.message || err));
        }
        paintKnown();
    }

    function paintScan() {
        const list = $("wifi-scan-list");
        list.replaceChildren();
        const aps = [...scanned].sort((a, b) => b.rssi - a.rssi);
        for (const ap of aps) {
            list.appendChild(row(ap.ssid || "(hidden)", `${ap.rssi} dBm · ch ${ap.chan} · ${ap.auth}${ap.known ? " · saved" : ""}`, () => {
                if (!ap.ssid) {
                    return;
                }
                const k = known.find((x) => x.ssid === ap.ssid);
                openSub({
                    title: `Connect to ${ap.ssid}`,
                    text: ap.auth === "open" ? "Open network." : "",
                    form: true,
                    ssid: ap.ssid,
                    ssidRo: true,
                    psk: k ? k.psk : "",
                    showPsk: ap.auth !== "open",
                    ok: async () => {
                        await wifi.connect(ap.ssid, ap.auth === "open" ? null : $("wifi-sub-psk").value);
                        setStatus(`joining ${ap.ssid}`, "ok");
                    },
                });
            }));
        }
        $("wifi-scan-box").hidden = false;
        $("wifi-scan-empty").hidden = aps.length > 0;
    }

    async function scan() {
        showErr($("wifi-err"), "");
        $("wifi-scan-btn").disabled = true;
        try {
            await wifi.scanStart();
            const t0 = Date.now();
            let saw = false;
            while (Date.now() - t0 < 12000) {
                await sleep(400);
                const r = await wifi.scan();
                if (r.scanning) {
                    saw = true;
                }
                scanned = r.aps || [];
                if ((saw && !r.scanning) || (!saw && Date.now() - t0 > 1500)) {
                    break;
                }
            }
            paintScan();
        } catch (err) {
            showErr($("wifi-err"), String(err.message || err));
        } finally {
            $("wifi-scan-btn").disabled = false;
        }
    }

    function openSub(o) {
        $("wifi-sub-title").textContent = o.title;
        $("wifi-sub-text").textContent = o.text || "";
        $("wifi-sub-form").hidden = !o.form;
        $("wifi-sub-ssid").value = o.ssid || "";
        $("wifi-sub-ssid").readOnly = !!o.ssidRo;
        $("wifi-sub-psk").value = o.psk || "";
        $("wifi-sub-psk").hidden = o.showPsk === false;
        $("wifi-sub-psk-label").hidden = o.showPsk === false;
        showErr($("wifi-sub-err"), "");
        subOk = o.ok;
        sub.open();
    }

    $("wifi-sub-ok").addEventListener("click", async () => {
        if (!subOk) {
            sub.close();
            return;
        }
        $("wifi-sub-ok").disabled = true;
        try {
            await subOk();
            sub.close();
            await refreshKnown();
            await refreshStatus(false);
        } catch (err) {
            showErr($("wifi-sub-err"), String(err.message || err));
        } finally {
            $("wifi-sub-ok").disabled = false;
        }
    });
    $("wifi-sub-cancel").addEventListener("click", () => sub.close());

    $("wifi-known-connect").addEventListener("click", async () => {
        try {
            await wifi.connect(selectedKnown, null);
            setStatus(`joining ${selectedKnown}`, "ok");
            await refreshStatus(false);
        } catch (err) {
            showErr($("wifi-err"), String(err.message || err));
        }
    });
    $("wifi-known-modify").addEventListener("click", () => {
        const k = known.find((x) => x.ssid === selectedKnown);
        openSub({
            title: `Modify ${selectedKnown}`,
            form: true,
            ssid: selectedKnown,
            ssidRo: false,
            psk: k ? k.psk : "",
            ok: async () => {
                const ssid = $("wifi-sub-ssid").value.trim();
                const psk = $("wifi-sub-psk").value;
                if (!ssid) {
                    throw new Error("SSID required");
                }
                await wifi.putNetwork(ssid, psk);
                if (ssid !== selectedKnown) {
                    await wifi.forget(selectedKnown);
                    selectedKnown = ssid;
                }
            },
        });
    });
    $("wifi-known-delete").addEventListener("click", () => {
        openSub({
            title: `Forget ${selectedKnown}?`,
            text: "The board stops joining this network.",
            form: false,
            ok: async () => {
                await wifi.forget(selectedKnown);
                selectedKnown = "";
            },
        });
    });
    $("wifi-scan-btn").addEventListener("click", () => scan());
    $("wifi-mode").addEventListener("change", async () => {
        try {
            status = await wifi.put({ mode: $("wifi-mode").value });
            paintLive();
            setStatus(`Wi-Fi mode ${$("wifi-mode").value}`, "ok");
        } catch (err) {
            showErr($("wifi-err"), String(err.message || err));
        }
    });
    for (const id of ["wifi-ap-ssid", "wifi-ap-psk"]) {
        $(id).addEventListener("input", updateSave);
    }

    async function save() {
        if (tab === "wg") {
            return saveWg();
        }
        if (!dirty()) {
            return;
        }
        showErr($("wifi-err"), "");
        try {
            const body = {};
            if ($("wifi-ap-ssid").value.trim() !== snapshot.ssid) {
                body.ap_ssid = $("wifi-ap-ssid").value.trim();
            }
            if ($("wifi-ap-psk").value !== snapshot.psk) {
                body.ap_psk = $("wifi-ap-psk").value;
            }
            status = await wifi.put(body);
            snapshot = { ssid: status.ap_ssid || "", psk: status.ap_psk || "" };
            paintLive();
            updateSave();
            setStatus("hotspot settings saved", "ok");
        } catch (err) {
            showErr($("wifi-err"), String(err.message || err));
        }
    }
    $("wifi-save").addEventListener("click", () => save());

    // ---- WireGuard ----
    function kv(el, pairs) {
        el.innerHTML = pairs.filter((p) => p[1] !== undefined && p[1] !== null && p[1] !== "")
            .map(([k, v]) => `<dt>${esc(k)}</dt><dd>${esc(v)}</dd>`).join("");
    }

    async function refreshWg() {
        try {
            const s = await call("wg.status");
            kv($("wg-status"), [
                ["State", s.connected ? (s.confirmed ? "connected" : "connected (unconfirmed)") : s.configured ? "down" : "not configured"],
                ["Address", s.address],
                ["Endpoint", s.current_endpoint ? `${s.current_endpoint}:${s.current_port}` : s.endpoint ? `${s.endpoint}:${s.endpoint_port}` : ""],
                ["Last handshake", s.last_handshake_age_ms != null && s.last_handshake_age_ms >= 0 ? `${Math.round(s.last_handshake_age_ms / 1000)} s ago` : ""],
                ["Traffic", s.rx_bytes != null ? `rx ${s.rx_bytes} · tx ${s.tx_bytes}` : ""],
                ["Uplink", s.uplink],
            ]);
        } catch (err) {
            kv($("wg-status"), [["State", String(err.message || err)]]);
        }
        try {
            wgCfg = await readJsonOr(WG_PATH, {});
        } catch {
            wgCfg = {};
        }
        const c = wgCfg || {};
        $("wg-address").value = c.address || "";
        $("wg-netmask").value = c.netmask || "";
        $("wg-private").value = c.private_key || "";
        $("wg-peer").value = c.peer_public_key || "";
        $("wg-psk").value = c.preshared_key || "";
        $("wg-allowed").value = c.allowed_ip || "";
        $("wg-allowed-mask").value = c.allowed_mask || "";
        $("wg-endpoint").value = c.endpoint || "";
        $("wg-port").value = c.endpoint_port != null ? String(c.endpoint_port) : "";
        $("wg-keepalive").value = c.keepalive != null ? String(c.keepalive) : "";
    }

    async function saveWg() {
        showErr($("wifi-err"), "");
        const c = { ...(wgCfg || {}) };
        const setStr = (k, id) => {
            const v = $(id).value.trim();
            if (v) {
                c[k] = v;
            } else {
                delete c[k];
            }
        };
        setStr("address", "wg-address");
        setStr("netmask", "wg-netmask");
        setStr("private_key", "wg-private");
        setStr("peer_public_key", "wg-peer");
        setStr("preshared_key", "wg-psk");
        setStr("allowed_ip", "wg-allowed");
        setStr("allowed_mask", "wg-allowed-mask");
        setStr("endpoint", "wg-endpoint");
        for (const [k, id] of [["endpoint_port", "wg-port"], ["keepalive", "wg-keepalive"]]) {
            const v = $(id).value.trim();
            if (v === "") {
                delete c[k];
            } else if (/^\d+$/.test(v)) {
                c[k] = Number(v);
            } else {
                showErr($("wifi-err"), `${k} must be a number`);
                return;
            }
        }
        try {
            await writeJson(WG_PATH, c);
            wgCfg = c;
            setStatus("WireGuard configuration saved — Connect to apply", "ok");
        } catch (err) {
            showErr($("wifi-err"), String(err.message || err));
        }
    }
    $("wg-save").addEventListener("click", () => saveWg());
    $("wg-up").addEventListener("click", async () => {
        try {
            await call("wg.up");
        } catch (err) {
            showErr($("wifi-err"), String(err.message || err));
        }
        setTimeout(refreshWg, 800);
    });
    $("wg-down").addEventListener("click", async () => {
        try {
            await call("wg.down");
        } catch (err) {
            showErr($("wifi-err"), String(err.message || err));
        }
        setTimeout(refreshWg, 500);
    });

    async function refreshMdns() {
        try {
            const s = await call("mdns.status");
            const host = s.hostname || s.host || "";
            kv($("mdns-status"), [["Hostname", host ? `${host}.local` : ""], ["Groups", Array.isArray(s.groups) ? s.groups.length : ""]]);
            $("wifi-mdns-hint").textContent = host ? `http://${host}.local/printer/` : "";
        } catch (err) {
            kv($("mdns-status"), [["mDNS", String(err.message || err)]]);
        }
    }

    for (const b of document.querySelectorAll("#wifi .wifi-tab")) {
        b.addEventListener("click", () => setTab(b.dataset.tab));
    }
    $("wifi-close").addEventListener("click", () => dlg.close());

    async function open() {
        showErr($("wifi-err"), "");
        $("wifi-tab-wg").hidden = !hasModule("wg");
        $("wifi-tab-mdns").hidden = !hasModule("mdns");
        $("wifi-tab-sta").hidden = false;
        if (!tabs().includes(tab)) {
            tab = "sta";
        }
        setTab(tab);
        dlg.open();
        await Promise.all([refreshStatus(true), refreshKnown()]);
        poll();
    }

    $("wifi-settings").addEventListener("click", () => open());
    return { isOpen: () => dlg.isOpen(), open, close: () => dlg.close() };
}
