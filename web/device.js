/** The board: the parent's WebSocket op API and its filesystem API.
 *
 * Replaces pm220_rp2_webapp's api.js (plain REST to its own firmware). The two
 * modules under js/btgw/ are bluetooth_rp2_webapp's own web/js/wsapi.js and
 * web/js/fs.js, copied in at dist time (tools/make_dist.py), so a parent fix
 * reaches this app with the submodule bump. */

import { WsApi } from "./js/btgw/wsapi.js";
import * as fs from "./js/btgw/fs.js";
import { isBusy, queue } from "./lazy.js";

// Every HTTP request to the board goes through lazy.js's queue (two at most
// in flight; a busy answer or a lost request backs off and is queued again).
// A read is retried on any 409 or 503; a write only on the connection limit's
// 409 "busy" or a 503, not on LittleFS's own 409 "fs write busy or failed",
// which the settings store answers by moving to the card (store.js).
const readLost = (r, err) => !!err || !!r.offline || isBusy(r.status);
const writeLost = (r, err) => !!err || !!r.offline || r.status === 503 || (r.status === 409 && r.error === "busy");
const read = (fn) => queue.run(fn, readLost);
const write = (fn) => queue.run(fn, writeLost);

export { fs };

function wsUrl() {
    const q = new URLSearchParams(location.search);
    if (q.get("ws")) {
        return q.get("ws");
    }
    const host = q.get("host") || location.hostname;
    const port = q.get("port") || location.port;
    const proto = location.protocol === "https:" ? "wss" : "ws";
    return `${proto}://${host}${port ? ":" + port : ""}/ws`;
}

export const ws = new WsApi(wsUrl());

let info = null;
let sdUp = false;
const infoWaiters = [];

export function call(op, params = {}, timeoutMs) {
    if (timeoutMs) {
        const prev = ws.timeoutMs;
        ws.timeoutMs = timeoutMs;
        const p = ws.call(op, params);
        ws.timeoutMs = prev;
        return p;
    }
    return ws.call(op, params);
}

export const on = (topic, fn) => ws.on(topic, fn);

/** sys.info, refreshed on every (re)connect. */
export function sysInfo() {
    return info;
}

export function hasModule(ns) {
    return !!(info && Array.isArray(info.modules) && info.modules.includes(ns));
}

export function whenInfo() {
    return info ? Promise.resolve(info) : new Promise((r) => infoWaiters.push(r));
}

export function sdAvailable() {
    return sdUp;
}

export async function refreshSd() {
    sdUp = false;
    if (!hasModule("sd")) {
        fs.setSd(false);
        return false;
    }
    fs.setSd(true);
    const s = await read(() => fs.sdStatus());
    sdUp = !!(s.ok && s.mounted);
    if (!sdUp) {
        fs.setSd(false);
    }
    return sdUp;
}

async function onOpen() {
    try {
        info = await ws.call("sys.info");
    } catch {
        info = { modules: [] };
    }
    try {
        await refreshSd();
    } catch {
        sdUp = false;
    }
    while (infoWaiters.length) {
        infoWaiters.shift()(info);
    }
}

export function connect() {
    ws.on("_open", onOpen);
    ws.connect();
}

// ---- files -----------------------------------------------------------------

export function isGzip(bytes) {
    return bytes && bytes.length >= 2 && bytes[0] === 0x1f && bytes[1] === 0x8b;
}

export async function gunzip(bytes) {
    const ds = new DecompressionStream("gzip");
    const out = await new Response(new Blob([bytes]).stream().pipeThrough(ds)).arrayBuffer();
    return new Uint8Array(out);
}

export async function gzipUtf8(text) {
    if (typeof CompressionStream === "undefined") {
        return null;
    }
    const cs = new CompressionStream("gzip");
    const out = await new Response(new Blob([new TextEncoder().encode(text)]).stream().pipeThrough(cs)).arrayBuffer();
    return new Uint8Array(out);
}

/** A file's bytes, gunzipped if it was stored gzipped. */
export async function readBytes(path) {
    const r = await read(() => fs.readBlob(path));
    if (r.ok) {
        const bytes = new Uint8Array(await r.blob.arrayBuffer());
        return isGzip(bytes) ? gunzip(bytes) : bytes;
    }
    const err = new Error(r.error || "read failed");
    err.status = r.status;
    throw err;
}

export async function readJson(path) {
    const bytes = await readBytes(path);
    return JSON.parse(new TextDecoder().decode(bytes));
}

/** null when the file does not exist. */
export async function readJsonOr(path, fallback = null) {
    try {
        return await readJson(path);
    } catch (e) {
        if (e.status === 404 || /not found/i.test(e.message)) {
            return fallback;
        }
        throw e;
    }
}

export async function writeFile(path, data) {
    const body = data instanceof Uint8Array ? new Blob([data]) : data;
    const r = await write(() => fs.write(path, body));
    if (!r.ok) {
        throw new Error(r.error || "write failed");
    }
    return r;
}

export async function writeJson(path, obj) {
    return writeFile(path, JSON.stringify(obj, null, 1));
}

export async function listDir(dir) {
    const r = await read(() => fs.list(dir));
    if (!r.ok) {
        if (r.status === 404) {
            return [];
        }
        throw new Error(r.error || "list failed");
    }
    return r.files || [];
}

export async function removeFile(path) {
    const r = await write(() => fs.remove(path));
    if (!r.ok) {
        throw new Error(r.error || "delete failed");
    }
}

export async function renameFile(from, to) {
    const r = await write(() => fs.rename(from, to));
    if (!r.ok) {
        throw new Error(r.error || "rename failed");
    }
}

/** Save a stored file to the computer. A .gz on the board is unpacked first
 *  when the name it is saved under has no .gz (pm220 bug: it saved gzip bytes
 *  as .json). */
export async function downloadStored(path, name) {
    let bytes = await (async () => {
        const r = await read(() => fs.readBlob(path));
        if (!r.ok) {
            throw new Error(r.error || "download failed");
        }
        return new Uint8Array(await r.blob.arrayBuffer());
    })();
    if (isGzip(bytes) && !/\.gz$/i.test(name)) {
        bytes = await gunzip(bytes);
    }
    saveBlob(new Blob([bytes], { type: "application/octet-stream" }), name);
}

export function saveBlob(blob, name) {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = name;
    a.rel = "noopener";
    a.style.display = "none";
    document.body.appendChild(a);
    a.click();
    setTimeout(() => {
        URL.revokeObjectURL(a.href);
        a.remove();
    }, 1000);
}

// ---- Wi-Fi: plain HTTP on the parent (src/net/http.c wifi_routes) ----------

async function wifiReq(method, path, body) {
    let got;
    try {
        got = await queue.run(async () => {
            const res = await fetch(fs.apiUrl(path), {
                method,
                cache: "no-store",
                body: body == null ? undefined : JSON.stringify(body),
                headers: body == null ? undefined : { "Content-Type": "text/plain" },
            });
            let data = {};
            try {
                data = await res.json();
            } catch {
                /* empty */
            }
            return { res, data };
        }, (g, err) => !!err || isBusy(g.res.status));
    } catch {
        throw new Error("device unreachable");
    }
    const { res, data } = got;
    if (!res.ok || data.ok === false) {
        throw new Error(data.error || `HTTP ${res.status}`);
    }
    return data;
}

export const wifi = {
    status: () => wifiReq("GET", "/api/wifi"),
    put: (body) => wifiReq("PUT", "/api/wifi", body),
    scanStart: () => wifiReq("POST", "/api/wifi/scan"),
    scan: () => wifiReq("GET", "/api/wifi/scan"),
    networks: () => wifiReq("GET", "/api/wifi/networks"),
    putNetwork: (ssid, psk) => wifiReq("PUT", "/api/wifi/networks", psk == null ? { ssid } : { ssid, psk }),
    forget: (ssid) => wifiReq("POST", "/api/wifi/forget", { ssid }),
    connect: (ssid, psk) => wifiReq("POST", "/api/wifi/connect", psk == null ? { ssid } : { ssid, psk }),
};

export function deviceLabel() {
    const q = new URLSearchParams(location.search);
    return q.get("host") || location.host || "device";
}
