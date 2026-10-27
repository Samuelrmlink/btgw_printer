/** Keyboard-complete dialogs (the pm220 dialog rules, shared by the new ones).
 *
 * Esc / q (when not typing) closes. Tab / Shift+Tab walk every visible control
 * in document order. In a row list (ul.row-list > li), j/k or Up/Down move to
 * the same button in the next row and h/l or Left/Right move along the row.
 * On a button row (.picker-actions) every arrow cycles its buttons. Enter or
 * Space activates (the browser does that for buttons). Numeric fields step
 * with Up/Down (k/j), Shift for ×8 (numfield.js). */

import { isNumField, stepNumField } from "./numfield.js";

const stack = [];

export function anyDialogOpen() {
    return stack.length > 0 || [...document.querySelectorAll("[data-overlay]")].some((el) => !el.hidden);
}

export function topDialog() {
    return stack[stack.length - 1] || null;
}

function visible(el) {
    if (!el || el.disabled) {
        return false;
    }
    if (el.closest("[hidden]")) {
        return false;
    }
    const st = getComputedStyle(el);
    return st.display !== "none" && st.visibility !== "hidden";
}

function typing(el) {
    if (!el) {
        return false;
    }
    if (el.tagName === "TEXTAREA") {
        return true;
    }
    if (el.tagName === "SELECT") {
        return false;
    }
    return el.tagName === "INPUT" && !["button", "checkbox", "radio", "file"].includes(el.type);
}

export class Dialog {
    /** opts: {onClose(), onKey(e) → true if handled, initialFocus: () => el} */
    constructor(id, opts = {}) {
        this.root = document.getElementById(id);
        this.panel = this.root.querySelector(".picker-panel");
        this.opts = opts;
        this.panel.setAttribute("tabindex", "-1");
        this.root.addEventListener("click", (e) => {
            if (e.target.classList.contains("picker-backdrop")) {
                this.close();
            }
        });
        this._key = (e) => this._onKey(e);
    }

    isOpen() {
        return !this.root.hidden;
    }

    open() {
        if (this.isOpen()) {
            return;
        }
        this.root.hidden = false;
        stack.push(this);
        document.addEventListener("keydown", this._key, true);
        const f = this.opts.initialFocus && this.opts.initialFocus();
        (f || this.panel).focus();
    }

    close() {
        if (!this.isOpen()) {
            return;
        }
        if (this.root.contains(document.activeElement)) {
            document.activeElement.blur();   // or the page's keys would go to a hidden control
        }
        this.root.hidden = true;
        const i = stack.indexOf(this);
        if (i >= 0) {
            stack.splice(i, 1);
        }
        document.removeEventListener("keydown", this._key, true);
        this.opts.onClose && this.opts.onClose();
    }

    focusables() {
        return [...this.panel.querySelectorAll("input, select, textarea, button, [data-nav]")].filter(visible);
    }

    _onKey(e) {
        if (topDialog() !== this) {
            return;
        }
        const t = e.target;
        if (this.opts.onKey && this.opts.onKey(e)) {
            e.stopImmediatePropagation();
            return;
        }
        const tp = typing(t);
        if (e.key === "Escape" || (e.key === "q" && !tp && !e.ctrlKey && !e.metaKey)) {
            if (t && t.dataset && t.dataset.escapeLocal) {
                return;
            }
            e.preventDefault();
            e.stopImmediatePropagation();
            this.close();
            return;
        }
        if (e.key === "Tab") {
            e.preventDefault();
            e.stopImmediatePropagation();
            const els = this.focusables();
            if (!els.length) {
                return;
            }
            let i = els.indexOf(document.activeElement);
            if (i < 0) {
                i = e.shiftKey ? 0 : -1;
            }
            const next = els[(i + (e.shiftKey ? -1 : 1) + els.length) % els.length];
            next.focus();
            if (next.select && typing(next)) {
                next.select();
            }
            return;
        }
        if (isNumField(t) && !t.disabled) {
            let d = 0;
            if (e.key === "ArrowUp" || e.key === "k") {
                d = e.shiftKey ? 8 : 1;
            } else if (e.key === "ArrowDown" || e.key === "j") {
                d = e.shiftKey ? -8 : -1;
            } else if (e.key === "K") {
                d = 8;
            } else if (e.key === "J") {
                d = -8;
            }
            if (d) {
                e.preventDefault();
                e.stopImmediatePropagation();
                stepNumField(t, d);
                return;
            }
        }
        if (tp || (t && t.tagName === "SELECT" && (e.key === "ArrowUp" || e.key === "ArrowDown"))) {
            return;
        }
        const k = e.key;
        const down = k === "ArrowDown" || k === "j";
        const up = k === "ArrowUp" || k === "k";
        const right = k === "ArrowRight" || k === "l";
        const left = k === "ArrowLeft" || k === "h";
        if (!(down || up || right || left)) {
            return;
        }
        const li = t && t.closest && t.closest("ul.row-list > li, table.nav-table tr");
        if (li) {
            const rowBtns = (row) => [...row.querySelectorAll("button, input[type=checkbox], input[data-nav], [data-nav]")].filter(visible);
            const btns = rowBtns(li);
            const col = Math.max(0, btns.indexOf(t));
            e.preventDefault();
            e.stopImmediatePropagation();
            if (right || left) {
                const n = btns.length;
                if (n) {
                    btns[(col + (right ? 1 : n - 1)) % n].focus();
                }
                return;
            }
            const rows = [...li.parentElement.children].filter((r) => visible(r) && rowBtns(r).length);
            const ri = rows.indexOf(li);
            const nr = rows[(ri + (down ? 1 : rows.length - 1)) % rows.length];
            if (nr) {
                const nb = rowBtns(nr);
                (nb[Math.min(col, nb.length - 1)] || nb[0]).focus();
            }
            return;
        }
        const bar = t && t.closest && t.closest(".picker-actions, .wifi-known-actions, .store-tabs");
        if (bar) {
            const btns = [...bar.querySelectorAll("button")].filter(visible);
            const i = btns.indexOf(t);
            if (i >= 0 && btns.length) {
                e.preventDefault();
                e.stopImmediatePropagation();
                btns[(i + (right || down ? 1 : btns.length - 1)) % btns.length].focus();
            }
        }
    }
}

/** An in-app confirm (keyboard: Enter = the focused button, Esc = Cancel).
 *  Cancel has focus first, as the parent's destructive-action rule asks. */
let confirmDlg = null;
export function confirmDialog(text, { okLabel = "OK", danger = false } = {}) {
    if (!confirmDlg) {
        confirmDlg = new Dialog("confirm", {
            onClose: () => confirmDlg._resolve && confirmDlg._resolve(false),
            initialFocus: () => document.getElementById(confirmDlg._danger ? "confirm-cancel" : "confirm-ok"),
        });
        document.getElementById("confirm-ok").addEventListener("click", () => {
            const r = confirmDlg._resolve;
            confirmDlg._resolve = null;
            confirmDlg.close();
            r && r(true);
        });
        document.getElementById("confirm-cancel").addEventListener("click", () => confirmDlg.close());
    }
    document.getElementById("confirm-text").textContent = text;
    const ok = document.getElementById("confirm-ok");
    ok.textContent = okLabel;
    ok.classList.toggle("danger", !!danger);
    confirmDlg._danger = danger;
    return new Promise((resolve) => {
        confirmDlg._resolve = resolve;
        confirmDlg.open();
    });
}

/** Before a saved Bluetooth Classic printer is paged (hub.js requestConnect):
 *  → true for Connect (Enter), false for Cancel (Esc). */
let turnOnDlg = null;
export function turnOnDialog(name) {
    if (!turnOnDlg) {
        turnOnDlg = new Dialog("turn-on", {
            onClose: () => turnOnDlg._resolve && turnOnDlg._resolve(false),
            initialFocus: () => document.getElementById("turn-on-ok"),
        });
        document.getElementById("turn-on-ok").addEventListener("click", () => {
            const r = turnOnDlg._resolve;
            turnOnDlg._resolve = null;
            turnOnDlg.close();
            r && r(true);
        });
        document.getElementById("turn-on-cancel").addEventListener("click", () => turnOnDlg.close());
    }
    document.getElementById("turn-on-text").textContent =
        `Switch on ${name} and keep it nearby, then press Connect. ` +
        "(While the gateway looks for a printer that is off, its Wi-Fi pauses for about half a minute.)";
    return new Promise((resolve) => {
        turnOnDlg._resolve = (v) => {
            turnOnDlg._resolve = null;
            resolve(v);
        };
        turnOnDlg.open();
    });
}
