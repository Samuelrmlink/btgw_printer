/** Dynamic templates: the variables dialog, the print-time prompt, batch printing. */

import { Dialog } from "./dialog.js";
import { effectiveDefs, findVars, fillObjects, mapColumns, normaliseDef, resolveValues, VAR_TYPES } from "./vars.js";
import { parseTable } from "./csv.js";
import { readFileText } from "./doc.js";
import { setBounds } from "./numfield.js";

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
const LAST = "btgw-printer.vars.last";
const COUNTERS = "btgw-printer.counters";

function loadLS(key, dflt) {
    try {
        const v = localStorage.getItem(key);
        return v ? JSON.parse(v) : dflt;
    } catch {
        return dflt;
    }
}
function saveLS(key, v) {
    try {
        localStorage.setItem(key, JSON.stringify(v));
    } catch {
        /* quota */
    }
}

/** Remembered counter positions, per label name. */
export function counterStore(docName) {
    const all = loadLS(COUNTERS, {});
    const k = docName || "(untitled)";
    return {
        get: () => ({ ...(all[k] || {}) }),
        set: (c) => {
            all[k] = c;
            saveLS(COUNTERS, all);
        },
    };
}

/** ctx: {objects(), vars(), setVars(defs), docName(), setStatus,
 *        printLabels(list) — list of {values, copies}, counters advance across it,
 *        renderPreview(canvas, values, index)} */
export function bindTemplates(ctx) {
    // ---- variables dialog ----
    const varsDlg = new Dialog("vars", {
        initialFocus: () => $("vars-list").querySelector("select, input") || $("vars-close"),
    });

    function defsNow() {
        return effectiveDefs(ctx.objects(), ctx.vars());
    }

    function setDef(name, patch) {
        const defs = [...ctx.vars()];
        const i = defs.findIndex((d) => d.name === name);
        const cur = i >= 0 ? defs[i] : { name, type: "text" };
        const next = normaliseDef({ ...cur, ...patch });
        if (i >= 0) {
            defs[i] = next;
        } else {
            defs.push(next);
        }
        ctx.setVars(defs);
    }

    function input(id, value, attrs = "") {
        return `<input id="${id}" type="text" value="${esc(value)}" autocomplete="off" spellcheck="false" ${attrs}>`;
    }

    function paintVars() {
        const list = $("vars-list");
        list.replaceChildren();
        const defs = defsNow();
        const counters = counterStore(ctx.docName()).get();
        $("vars-empty").hidden = defs.length > 0;
        defs.forEach((d, i) => {
            const li = document.createElement("li");
            li.className = "var-row";
            const p = `v${i}`;
            let fields = "";
            if (d.type === "counter") {
                fields = `<label>Start ${input(`${p}-start`, d.start, 'inputmode="numeric"')}</label>
                    <label>Step ${input(`${p}-step`, d.step, 'inputmode="numeric"')}</label>
                    <label>Digits ${input(`${p}-pad`, d.pad, 'inputmode="numeric"')}</label>
                    <label>Prefix ${input(`${p}-prefix`, d.prefix)}</label>
                    <label>Suffix ${input(`${p}-suffix`, d.suffix)}</label>
                    <label>Next ${input(`${p}-next`, counters[d.name] ?? d.start, 'inputmode="numeric"')}</label>`;
            } else if (d.type === "date") {
                fields = `<label>Format ${input(`${p}-format`, d.format)}</label>
                    <label>Days ahead ${input(`${p}-offset`, d.offset_days, 'inputmode="numeric"')}</label>`;
            } else {
                fields = `<label>Default ${input(`${p}-default`, d.default)}</label>`;
                if (d.type === "number") {
                    fields += `<label>Decimals ${input(`${p}-dec`, d.decimals ?? "", 'inputmode="numeric"')}</label>`;
                }
            }
            li.innerHTML = `<div class="var-head"><code>{{${esc(d.name)}}}</code>
                <select id="${p}-type" aria-label="Type of ${esc(d.name)}">${VAR_TYPES.map((t) => `<option value="${t}"${t === d.type ? " selected" : ""}>${t}</option>`).join("")}</select>
                <label>Prompt ${input(`${p}-label`, d.label, `placeholder="${esc(d.name)}"`)}</label></div>
                <div class="var-fields">${fields}</div>`;
            list.appendChild(li);
            const on = (suffix, fn) => {
                const el = document.getElementById(`${p}-${suffix}`);
                el && el.addEventListener("change", () => fn(el.value));
            };
            on("type", (v) => {
                setDef(d.name, { type: v });
                paintVars();
                document.getElementById(`${p}-type`)?.focus();
            });
            on("label", (v) => setDef(d.name, { label: v }));
            on("start", (v) => setDef(d.name, { start: v }));
            on("step", (v) => setDef(d.name, { step: v }));
            on("pad", (v) => setDef(d.name, { pad: v }));
            on("prefix", (v) => setDef(d.name, { prefix: v }));
            on("suffix", (v) => setDef(d.name, { suffix: v }));
            on("format", (v) => setDef(d.name, { format: v }));
            on("offset", (v) => setDef(d.name, { offset_days: v }));
            on("default", (v) => setDef(d.name, { default: v }));
            on("dec", (v) => setDef(d.name, { decimals: v }));
            on("next", (v) => {
                const cs = counterStore(ctx.docName());
                const c = cs.get();
                c[d.name] = Math.trunc(Number(v) || 0);
                cs.set(c);
            });
        });
    }

    $("vars-close").addEventListener("click", () => varsDlg.close());

    // ---- print-time prompt ----
    let promptResolve = null;
    const promptDlg = new Dialog("prompt", {
        onClose: () => {
            if (promptResolve) {
                promptResolve(null);
                promptResolve = null;
            }
        },
        initialFocus: () => $("prompt-fields").querySelector("input") || $("prompt-copies"),
        onKey: (e) => {
            if ((e.ctrlKey || e.metaKey) && e.key === "Enter") {
                e.preventDefault();
                $("prompt-print").click();
                return true;
            }
            if (e.key === "Enter" && e.target.tagName === "INPUT") {
                e.preventDefault();
                $("prompt-print").click();
                return true;
            }
            return false;
        },
    });
    setBounds("prompt-copies", 1, 999);

    function readPrompt(defs) {
        const values = {};
        defs.forEach((d, i) => {
            const el = document.getElementById(`pv${i}`);
            if (el) {
                values[d.name] = el.value;
            }
        });
        return values;
    }

    function paintPromptPreview(defs) {
        const values = readPrompt(defs);
        const counters = counterStore(ctx.docName()).get();
        const r = resolveValues(defs, values, 0, { counters });
        const objs = fillObjects(ctx.objects(), r).filter((o) => o.text || o.payload);
        $("prompt-preview").textContent = objs.slice(0, 3).map((o) => (o.text || o.payload).replace(/\s+/g, " ").slice(0, 40)).join(" · ");
    }

    /** → {values, count} or null (cancelled). */
    function promptValues() {
        const defs = defsNow();
        const last = loadLS(LAST, {});
        const counters = counterStore(ctx.docName()).get();
        const box = $("prompt-fields");
        box.innerHTML = defs.map((d, i) => {
            const label = esc(d.label || d.name);
            if (d.type === "counter") {
                return `<label for="pv${i}">${label} (counter, starts at)</label><input id="pv${i}" type="text" inputmode="numeric" value="${esc(counters[d.name] ?? d.start)}" autocomplete="off" spellcheck="false" data-counter="1">`;
            }
            if (d.type === "date") {
                return `<p class="cal-hint">${label}: date (${esc(d.format)}${d.offset_days ? `, +${d.offset_days} days` : ""})</p>`;
            }
            const v = last[d.name] ?? d.default ?? "";
            const mode = d.type === "number" ? ' inputmode="decimal"' : "";
            return `<label for="pv${i}">${label}</label><input id="pv${i}" type="text"${mode} value="${esc(v)}" autocomplete="off" spellcheck="false">`;
        }).join("");
        $("prompt-copies").value = "1";
        $("prompt-err").hidden = true;
        box.oninput = () => paintPromptPreview(defs);
        paintPromptPreview(defs);
        return new Promise((resolve) => {
            promptResolve = resolve;
            promptDlg.open();
        });
    }

    $("prompt-print").addEventListener("click", () => {
        const defs = defsNow();
        const values = readPrompt(defs);
        const count = Math.max(1, Math.trunc(Number($("prompt-copies").value) || 1));
        const last = loadLS(LAST, {});
        const counters = counterStore(ctx.docName()).get();
        defs.forEach((d) => {
            if (d.type === "counter") {
                counters[d.name] = Math.trunc(Number(values[d.name]) || d.start);
                delete values[d.name];
            } else if (d.type !== "date") {
                last[d.name] = values[d.name];
            }
        });
        counterStore(ctx.docName()).set(counters);
        saveLS(LAST, last);
        const r = promptResolve;
        promptResolve = null;
        promptDlg.close();
        r && r({ values, count });
    });
    $("prompt-cancel").addEventListener("click", () => promptDlg.close());

    // ---- batch ----
    let table = { header: [], rows: [] };
    let picked = [];
    let copies = [];
    let current = 0;
    const batchDlg = new Dialog("batch", {
        initialFocus: () => $("batch-text"),
        onKey: (e) => {
            if (/INPUT|TEXTAREA/.test(e.target.tagName) && e.target.type !== "checkbox") {
                return false;
            }
            if (e.key === "a") {
                e.preventDefault();
                setAll(true);
                return true;
            }
            if (e.key === "x") {
                const tr = e.target.closest && e.target.closest("tr[data-row]");
                if (tr) {
                    e.preventDefault();
                    const i = Number(tr.dataset.row);
                    picked[i] = !picked[i];
                    paintTable();
                    focusRow(i);
                    return true;
                }
            }
            return false;
        },
    });
    setBounds("batch-copies", 1, 99);

    function parse() {
        table = parseTable($("batch-text").value, { hasHeader: $("batch-header").checked });
        picked = table.rows.map(() => true);
        copies = table.rows.map(() => 1);
        current = 0;
        paintTable();
    }

    function rowValues(i) {
        const defs = defsNow();
        const map = mapColumns(table.header, defs);
        const values = {};
        const row = table.rows[i] || [];
        for (const [name, col] of Object.entries(map)) {
            values[name] = row[col];
        }
        // A single unmatched column fills the first text variable (a pasted list).
        if (!Object.keys(map).length && table.header.length === 1) {
            const first = defs.find((d) => d.type === "text" || d.type === "number");
            if (first) {
                values[first.name] = row[0];
            }
        }
        return values;
    }

    function paintMap() {
        const defs = defsNow();
        const map = mapColumns(table.header, defs);
        const used = findVars(ctx.objects());
        const matched = Object.keys(map);
        const missing = defs.filter((d) => d.type !== "counter" && d.type !== "date" && !matched.includes(d.name)).map((d) => d.name);
        let msg = `${table.rows.length} row(s)`;
        if (matched.length) {
            msg += ` · columns → ${matched.map((n) => `{{${n}}}`).join(", ")}`;
        } else if (table.header.length === 1 && defs.length) {
            msg += " · one column → the first variable";
        }
        if (missing.length && !(table.header.length === 1 && !matched.length)) {
            msg += ` · no column for ${missing.map((n) => `{{${n}}}`).join(", ")} (defaults used)`;
        }
        if (!used.length) {
            msg = "This label has no {{variables}}: every row prints the same label.";
        }
        $("batch-map").textContent = msg;
    }

    function focusRow(i) {
        document.querySelector(`#batch-table tr[data-row="${i}"] input[type=checkbox]`)?.focus();
    }

    function paintTable() {
        const t = $("batch-table");
        t.className = "nav-table";
        const head = `<tr><th>Print</th><th>Labels</th>${table.header.map((h) => `<th>${esc(h)}</th>`).join("")}</tr>`;
        const rows = table.rows.map((r, i) => `<tr data-row="${i}" class="${i === current ? "current" : ""}">
            <td><input type="checkbox" aria-label="Print row ${i + 1}" ${picked[i] ? "checked" : ""}></td>
            <td><input type="text" inputmode="numeric" class="batch-copies" aria-label="Labels for row ${i + 1}" value="${copies[i]}" autocomplete="off" spellcheck="false"></td>
            ${r.map((c) => `<td>${esc(c)}</td>`).join("")}</tr>`).join("");
        t.innerHTML = head + rows;
        t.querySelectorAll("tr[data-row]").forEach((tr) => {
            const i = Number(tr.dataset.row);
            const cb = tr.querySelector("input[type=checkbox]");
            cb.addEventListener("change", () => {
                picked[i] = cb.checked;
            });
            cb.addEventListener("focus", () => select(i));
            const cp = tr.querySelector(".batch-copies");
            cp.addEventListener("input", () => {
                copies[i] = Math.max(1, Math.trunc(Number(cp.value) || 1));
            });
            cp.addEventListener("focus", () => select(i));
            tr.addEventListener("click", () => select(i));
        });
        paintMap();
        paintPreview();
        $("batch-print").disabled = !table.rows.length;
    }

    function select(i) {
        if (current === i) {
            return;
        }
        current = i;
        document.querySelectorAll("#batch-table tr[data-row]").forEach((tr) => tr.classList.toggle("current", Number(tr.dataset.row) === i));
        paintPreview();
    }

    function paintPreview() {
        if (!table.rows.length) {
            const c = $("batch-preview");
            c.width = 1;
            c.height = 1;
            return;
        }
        ctx.renderPreview($("batch-preview"), rowValues(current), current);
    }

    function setAll(v) {
        picked = picked.map(() => v);
        paintTable();
    }

    $("batch-text").addEventListener("input", () => parse());
    $("batch-header").addEventListener("change", () => parse());
    $("batch-file").addEventListener("change", async (e) => {
        const f = e.target.files && e.target.files[0];
        e.target.value = "";
        if (!f) {
            return;
        }
        $("batch-text").value = await readFileText(f);
        parse();
    });
    $("batch-all").addEventListener("click", () => setAll(true));
    $("batch-none").addEventListener("click", () => setAll(false));
    $("batch-close").addEventListener("click", () => batchDlg.close());
    $("batch-print").addEventListener("click", async () => {
        const list = [];
        table.rows.forEach((_, i) => {
            if (picked[i]) {
                list.push({ values: rowValues(i), copies: copies[i] || 1, row: i });
            }
        });
        if (!list.length) {
            $("batch-err").hidden = false;
            $("batch-err").textContent = "no rows selected";
            return;
        }
        $("batch-err").hidden = true;
        batchDlg.close();
        await ctx.printLabels(list);
    });
    function openBatch() {
        $("batch-err").hidden = true;
        parse();
        batchDlg.open();
    }

    return {
        promptValues,
        openVars: () => {
            paintVars();
            varsDlg.open();
        },
        openBatch,
        isOpen: () => varsDlg.isOpen() || promptDlg.isOpen() || batchDlg.isOpen(),
    };
}
