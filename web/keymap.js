/** Every key binding, as data: the help overlay is built from it and
 * tests/unit/keymap.test.js checks that no two bindings in one context (or a
 * context and the global one) share a key.
 *
 * Contexts:
 *   global  — anywhere, no dialog open (Ctrl/Meta combos, ?/F1)
 *   browse  — nothing selected, not typing
 *   object  — an object selected, focus not on the move pad, not typing
 *   nudge   — the move pad focused (object keys stay live)
 *   field   — a numeric field focused
 *   dialog  — any dialog (shared navigation)
 *   picker / cal / wifi / printers / media / batch / prompt — extra keys in one dialog
 *
 * Key names: "Ctrl+S", "Ctrl+Shift+S", "?", "F1", "n", "N", "Space", "Enter",
 * "Esc", "Delete", "Backspace", "Tab", "Shift+Tab", "Up", "Down", "Left",
 * "Right", "Shift+Up", ... Letters are case-sensitive ("N" means Shift+n). */

export const KEYMAP = [
    // ---- document (pm220 map) ----
    { ctx: "global", keys: ["Ctrl+O"], action: "open", label: "Open label", section: "Document" },
    { ctx: "global", keys: ["Ctrl+S"], action: "save", label: "Save", section: "Document" },
    { ctx: "global", keys: ["Ctrl+Shift+S"], action: "saveAs", label: "Save as", section: "Document" },
    { ctx: "global", keys: ["Ctrl+P"], action: "print", label: "Print", section: "Document" },
    { ctx: "global", keys: ["Ctrl+B"], action: "batch", label: "Batch print", section: "Document" },
    { ctx: "global", keys: ["Ctrl+Z"], action: "undo", label: "Undo", section: "Document" },
    { ctx: "global", keys: ["Ctrl+Shift+Z", "Ctrl+Y"], action: "redo", label: "Redo", section: "Document" },
    { ctx: "global", keys: ["?", "F1"], action: "help", label: "This list", section: "Help" },
    // ---- browse ----
    { ctx: "browse", keys: ["n"], action: "next", label: "Next object", section: "Browse (nothing selected)" },
    { ctx: "browse", keys: ["N"], action: "prev", label: "Previous object", section: "Browse (nothing selected)" },
    { ctx: "browse", keys: ["Space", "Enter"], action: "select", label: "Select", section: "Browse (nothing selected)" },
    { ctx: "browse", keys: ["P"], action: "printers", label: "Printers", section: "Panels (nothing selected)" },
    { ctx: "browse", keys: ["L"], action: "media", label: "Label size", section: "Panels (nothing selected)" },
    { ctx: "browse", keys: ["V"], action: "vars", label: "Template variables", section: "Panels (nothing selected)" },
    { ctx: "browse", keys: ["C"], action: "calibrate", label: "Calibrate", section: "Panels (nothing selected)" },
    { ctx: "browse", keys: ["W"], action: "network", label: "Network", section: "Panels (nothing selected)" },
    { ctx: "browse", keys: ["t"], action: "addText", label: "Add text box", section: "Panels (nothing selected)" },
    // ---- object ----
    { ctx: "object", keys: ["m"], action: "move", label: "Move (focus D-pad)", section: "Object" },
    { ctx: "object", keys: ["r"], action: "rotate", label: "Rotate 90°", section: "Object" },
    { ctx: "object", keys: ["d"], action: "duplicate", label: "Duplicate", section: "Object" },
    { ctx: "object", keys: ["Delete", "Backspace"], action: "delete", label: "Delete", section: "Object" },
    { ctx: "object", keys: ["Esc", "q"], action: "deselect", label: "Deselect", section: "Object" },
    { ctx: "object", keys: ["Tab", "n"], action: "nextField", label: "Next field", section: "Fields" },
    { ctx: "object", keys: ["Shift+Tab", "N"], action: "prevField", label: "Previous field", section: "Fields" },
    // ---- move pad ----
    { ctx: "nudge", keys: ["Left", "h", "H"], action: "nudgeLeft", label: "Nudge left", section: "Move pad" },
    { ctx: "nudge", keys: ["Right", "l", "L"], action: "nudgeRight", label: "Nudge right", section: "Move pad" },
    { ctx: "nudge", keys: ["Up", "k", "K"], action: "nudgeUp", label: "Nudge up", section: "Move pad" },
    { ctx: "nudge", keys: ["Down", "j", "J"], action: "nudgeDown", label: "Nudge down", section: "Move pad" },
    { ctx: "nudge", keys: ["Shift+Left", "Shift+Right", "Shift+Up", "Shift+Down"], action: "resize", label: "Resize", section: "Move pad" },
    // ---- numeric fields ----
    { ctx: "field", keys: ["Up", "k"], action: "inc", label: "+1 (mm fields +0.25)", section: "Fields" },
    { ctx: "field", keys: ["Down", "j"], action: "dec", label: "−1", section: "Fields" },
    { ctx: "field", keys: ["Shift+Up", "K"], action: "inc8", label: "+8", section: "Fields" },
    { ctx: "field", keys: ["Shift+Down", "J"], action: "dec8", label: "−8", section: "Fields" },
    { ctx: "field", keys: ["Esc", "q"], action: "leave", label: "Leave field", section: "Fields" },
    // ---- every dialog ----
    { ctx: "dialog", keys: ["Esc", "q"], action: "close", label: "Close", section: "Dialogs" },
    { ctx: "dialog", keys: ["Tab", "Shift+Tab"], action: "region", label: "Next / previous field or region", section: "Dialogs" },
    { ctx: "dialog", keys: ["Up", "Down", "j", "k"], action: "row", label: "Move through rows", section: "Dialogs" },
    { ctx: "dialog", keys: ["Left", "Right", "h", "l"], action: "col", label: "Row actions / buttons", section: "Dialogs" },
    { ctx: "dialog", keys: ["Enter", "Space"], action: "activate", label: "Activate", section: "Dialogs" },
    { ctx: "picker", keys: ["[", "]"], action: "store", label: "Open/Save: Flash ⇄ SD card", section: "Dialogs" },
    { ctx: "cal", keys: ["Ctrl+S"], action: "save", label: "Calibrate: save", section: "Dialogs" },
    { ctx: "wifi", keys: ["Ctrl+S"], action: "save", label: "Network: save", section: "Dialogs" },
    { ctx: "printers", keys: ["s"], action: "scan", label: "Printers: scan", section: "Dialogs" },
    { ctx: "media", keys: ["+"], action: "new", label: "Label size: new", section: "Dialogs" },
    { ctx: "batch", keys: ["a"], action: "all", label: "Batch: select all rows", section: "Dialogs" },
    { ctx: "batch", keys: ["x"], action: "toggle", label: "Batch: toggle row", section: "Dialogs" },
    { ctx: "prompt", keys: ["Ctrl+Enter"], action: "print", label: "Variables: print", section: "Dialogs" },
];

/** "Ctrl+Shift+S"-style name for a KeyboardEvent. */
export function keyName(e) {
    let k = e.key;
    const named = {
        " ": "Space", Escape: "Esc", ArrowUp: "Up", ArrowDown: "Down", ArrowLeft: "Left", ArrowRight: "Right",
    };
    k = named[k] || k;
    const parts = [];
    if (e.ctrlKey || e.metaKey) {
        parts.push("Ctrl");
    }
    if (e.altKey) {
        parts.push("Alt");
    }
    // Shift is part of a printable letter ("N"); it is named for the rest.
    const printable = k.length === 1;
    if (e.shiftKey && (!printable || parts.length)) {
        parts.push("Shift");
    }
    if (printable && parts.length) {
        k = k.toUpperCase();
    }
    parts.push(k);
    return parts.join("+");
}

/** The action bound to an event in one context, or null. */
export function actionFor(ctx, e) {
    const name = keyName(e);
    for (const b of KEYMAP) {
        if (b.ctx === ctx && b.keys.includes(name)) {
            return b.action;
        }
    }
    return null;
}

/** Contexts whose keys may be live at the same moment as `ctx`. */
export const OVERLAPS = {
    browse: ["global"],
    object: ["global"],
    nudge: ["global", "object"],   // m/r/d/Esc/Delete/Tab still work on the pad
    field: ["global"],
    dialog: [],
    picker: ["dialog"],
    cal: ["dialog"],
    wifi: ["dialog"],
    printers: ["dialog"],
    media: ["dialog"],
    batch: ["dialog"],
    prompt: ["dialog"],
};

/** [{key, a, b}] for every clash. */
export function clashes(map = KEYMAP) {
    const out = [];
    const ctxs = [...new Set(map.map((b) => b.ctx))];
    for (const ctx of ctxs) {
        const live = [ctx, ...(OVERLAPS[ctx] || [])];
        const seen = new Map();
        for (const b of map.filter((x) => live.includes(x.ctx))) {
            for (const k of b.keys) {
                const prev = seen.get(k);
                if (prev && prev !== b) {
                    out.push({ ctx, key: k, a: `${prev.ctx}:${prev.action}`, b: `${b.ctx}:${b.action}` });
                }
                seen.set(k, b);
            }
        }
    }
    return out;
}

const esc = (s) => String(s).replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" })[c]);

function kbd(name) {
    return name.split("+").map((p) => `<kbd>${esc(p === "Up" ? "↑" : p === "Down" ? "↓" : p === "Left" ? "←" : p === "Right" ? "→" : p)}</kbd>`).join("+");
}

/** The help overlay's sections, in first-use order. */
export function helpHtml(map = KEYMAP) {
    const sections = new Map();
    for (const b of map) {
        if (!sections.has(b.section)) {
            sections.set(b.section, []);
        }
        const list = sections.get(b.section);
        if (list.some((x) => x.label === b.label)) {
            continue;
        }
        list.push(b);
    }
    let html = "";
    for (const [title, list] of sections) {
        html += `<section><h3>${esc(title)}</h3>`;
        for (const b of list) {
            html += `<p>${b.keys.map(kbd).join(" / ")} ${esc(b.label)}</p>`;
        }
        html += "</section>";
    }
    return html;
}
