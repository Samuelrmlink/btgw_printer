/** Label sizes: pick one for the label, edit the library, set orientation. */

import { Dialog, confirmDialog } from "./dialog.js";
import { allMedia, deleteUserMedia, fitsModel, mediaFor, newMediaId, saveUserMedia } from "./catalog.js";
import { setBounds } from "./numfield.js";

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);

export function mediaSummary(m) {
    if (!m) {
        return "–";
    }
    const len = m.type === "continuous" ? `${m.h_mm} mm roll` : `${m.h_mm}`;
    return `${m.w_mm} × ${len}${m.type === "continuous" ? "" : " mm"}`;
}

/** ctx: {current() → {model, media, orient}, choose(media), setOrient(deg), setStatus} */
export function bindMedia(ctx) {
    let editing = null;     // {id, isNew}

    const dlg = new Dialog("media", {
        initialFocus: () => $("media-list").querySelector("li.selected button") || $("media-list").querySelector("button") || $("media-new"),
        onKey: (e) => {
            if (e.key === "+" && !/INPUT|TEXTAREA/.test(e.target.tagName) && $("media-edit").hidden) {
                e.preventDefault();
                edit(null);
                return true;
            }
            return false;
        },
    });
    for (const id of ["me-w", "me-h"]) {
        setBounds(id, 5, 1000, 0.5);
    }
    setBounds("me-gap", 0, 20, 0.25);
    for (const id of ["me-sl", "me-st", "me-sr", "me-sb"]) {
        setBounds(id, 0, 50, 0.25);
    }

    function showErr(msg) {
        $("media-err").hidden = !msg;
        $("media-err").textContent = msg || "";
    }

    function btn(html, fn, cls = "") {
        const b = document.createElement("button");
        b.type = "button";
        b.className = cls;
        b.innerHTML = html;
        b.addEventListener("click", (e) => {
            e.stopPropagation();
            fn();
        });
        return b;
    }

    function paint() {
        const { model, media } = ctx.current();
        $("media-for").textContent = model ? `For ${model.vendor || ""} ${model.name} · ${model.dpi} dpi · head ${model.head_dots} dots` : "";
        $("media-orient").value = String(ctx.current().orient || 0);
        const list = $("media-list");
        list.replaceChildren();
        for (const m of mediaFor(model)) {
            const li = document.createElement("li");
            const cur = media && media.id === m.id;
            li.classList.toggle("selected", cur);
            const warn = fitsModel(m, model) ? "" : " · does not fit this model";
            li.append(
                btn(`<span>${esc(m.name)}${cur ? " ✓" : ""}</span><span class="row-meta">${esc(mediaSummary(m))} · ${esc(m.type || "gap")}${m.gap_mm ? ` · gap ${m.gap_mm} mm` : ""}${m.preset ? "" : " · yours"}${m.unconfirmed ? " · unconfirmed" : ""}${warn}</span>`,
                    () => choose(m), "row-main"),
                btn(m.preset ? "Copy" : "Edit", () => edit(m)),
            );
            if (!m.preset) {
                li.append(btn("Delete", () => remove(m)));
            }
            list.appendChild(li);
        }
    }

    function choose(m) {
        ctx.choose(m);
        paint();
        ctx.setStatus(`label ${m.name}`, "ok");
    }

    async function remove(m) {
        if (!(await confirmDialog(`Delete the label size ${m.name}?`, { okLabel: "Delete", danger: true }))) {
            return;
        }
        try {
            await deleteUserMedia(m.id);
            paint();
        } catch (err) {
            showErr(String(err.message || err));
        }
    }

    function edit(m) {
        const { model } = ctx.current();
        const src = m || { name: "", w_mm: model ? Math.round(model.head_dots * 25.4 / model.dpi) : 50, h_mm: 30, gap_mm: 2, type: "gap", safe_mm: [2, 2, 2, 2], orient: 0 };
        editing = { id: m && !m.preset ? m.id : null, isNew: !m || m.preset, models: m ? m.models : null };
        $("media-edit-title").textContent = !m ? "New label size" : m.preset ? `Copy of ${m.name}` : `Edit ${m.name}`;
        $("me-name").value = m && m.preset ? `${m.name} (mine)` : src.name;
        $("me-w").value = String(src.w_mm);
        $("me-h").value = String(src.h_mm);
        $("me-gap").value = String(src.gap_mm ?? 0);
        $("me-type").value = src.type || "gap";
        $("me-orient").value = String(src.orient || 0);
        const s = src.safe_mm || [2, 2, 2, 2];
        ["me-sl", "me-st", "me-sr", "me-sb"].forEach((id, i) => {
            $(id).value = String(s[i] ?? 0);
        });
        $("me-all").checked = !!(m && (!m.models || !m.models.length));
        $("media-edit").hidden = false;
        $("media-actions").hidden = true;
        $("me-name").focus();
    }

    function endEdit() {
        editing = null;
        $("media-edit").hidden = true;
        $("media-actions").hidden = false;
        paint();
        ($("media-list").querySelector("button") || $("media-new")).focus();
    }

    $("me-save").addEventListener("click", async () => {
        const { model } = ctx.current();
        const n = (id) => Number(String($(id).value).trim());
        const w = n("me-w");
        const h = n("me-h");
        if (!(w > 0) || !(h > 0)) {
            showErr("width and length must be positive millimetres");
            return;
        }
        const name = $("me-name").value.trim() || `${w} × ${h} mm`;
        const rec = {
            id: editing.id || newMediaId(name),
            name,
            w_mm: w,
            h_mm: h,
            gap_mm: Number.isFinite(n("me-gap")) ? n("me-gap") : 0,
            type: $("me-type").value,
            orient: Number($("me-orient").value) || 0,
            safe_mm: ["me-sl", "me-st", "me-sr", "me-sb"].map((id) => (Number.isFinite(n(id)) ? n(id) : 0)),
            models: $("me-all").checked ? [] : (editing.models && editing.models.length ? editing.models : (model ? [model.id] : [])),
        };
        try {
            await saveUserMedia(rec);
            endEdit();
            choose(allMedia().find((x) => x.id === rec.id) || rec);
        } catch (err) {
            showErr(String(err.message || err));
        }
    });
    $("me-cancel").addEventListener("click", () => endEdit());
    $("media-new").addEventListener("click", () => edit(null));
    $("media-close").addEventListener("click", () => dlg.close());
    $("media-orient").addEventListener("change", () => ctx.setOrient(Number($("media-orient").value) || 0));
    $("media-btn").addEventListener("click", () => open());

    function open() {
        showErr("");
        $("media-edit").hidden = true;
        $("media-actions").hidden = false;
        paint();
        dlg.open();
    }

    return { open, isOpen: () => dlg.isOpen(), paint };
}
