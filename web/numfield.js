/** Numeric text fields: bounds, steps, and the ▲▼ spin buttons (pm220 app.js). */

const INT_RE = /^-?\d+$/;
const bounds = new Map();     // id -> {lo, hi, step}
let dynamic = () => null;

export function isNumField(el) {
    if (!el || el.tagName !== "INPUT") {
        return false;
    }
    const mode = el.getAttribute("inputmode");
    return mode === "numeric" || mode === "decimal";
}

/** step: the amount one key press moves (mm fields: 0.25, as in pm220). */
export function setBounds(id, lo, hi, step = 1) {
    bounds.set(id, { lo, hi, step });
}

/** fn(el) → {lo, hi, step} | null, for bounds that depend on the page. */
export function setDynamicBounds(fn) {
    dynamic = fn;
}

export function boundsOf(el) {
    return dynamic(el) || bounds.get(el.id) || { lo: null, hi: null, step: el.getAttribute("inputmode") === "decimal" ? 0.25 : 1 };
}

export function parseIntValue(raw) {
    const s = String(raw || "").trim();
    if (!INT_RE.test(s)) {
        return null;
    }
    const n = Number(s);
    return Number.isFinite(n) ? n : null;
}

export function stepNumField(el, delta) {
    if (!el || el.disabled || !isNumField(el)) {
        return;
    }
    const { lo, hi, step } = boundsOf(el);
    let n;
    if (el.getAttribute("inputmode") === "decimal") {
        n = Number(el.value);
        if (!Number.isFinite(n)) {
            n = 0;
        }
        n = Math.round((n + delta * (step || 0.25)) * 100) / 100;
    } else {
        n = parseIntValue(el.value);
        if (n == null) {
            n = 0;
        }
        n += delta * (step || 1);
    }
    if (lo != null && n < lo) {
        n = lo;
    }
    if (hi != null && n > hi) {
        n = hi;
    }
    el.value = String(n);
    el.dispatchEvent(new Event("input", { bubbles: true }));
}

/** Wrap every numeric input under the roots in a spin box (shown on touch/narrow screens). */
export function wrapNumSpins(selector) {
    for (const el of document.querySelectorAll(selector)) {
        if (el.parentElement && el.parentElement.classList.contains("num-spin")) {
            continue;
        }
        const wrap = document.createElement("div");
        wrap.className = "num-spin";
        el.replaceWith(wrap);
        wrap.appendChild(el);
        const col = document.createElement("div");
        col.className = "num-spin-btns";
        const up = document.createElement("button");
        up.type = "button";
        up.tabIndex = -1;
        up.className = "num-spin-up";
        up.setAttribute("aria-label", "Increase");
        up.textContent = "▲";
        const down = document.createElement("button");
        down.type = "button";
        down.tabIndex = -1;
        down.className = "num-spin-down";
        down.setAttribute("aria-label", "Decrease");
        down.textContent = "▼";
        col.append(up, down);
        wrap.appendChild(col);
    }
}

let spinsBound = false;
export function bindSpinHolds() {
    if (spinsBound) {
        return;
    }
    spinsBound = true;
    let holdWait = 0;
    let holdRep = 0;
    const stopHold = () => {
        clearTimeout(holdWait);
        clearInterval(holdRep);
        holdWait = 0;
        holdRep = 0;
    };
    document.addEventListener("pointerdown", (e) => {
        const btn = e.target.closest && e.target.closest(".num-spin-btns button");
        if (!btn) {
            return;
        }
        const input = btn.closest(".num-spin").querySelector("input");
        if (!input || input.disabled) {
            return;
        }
        e.preventDefault();
        const delta = btn.classList.contains("num-spin-up") ? 1 : -1;
        stepNumField(input, delta);
        stopHold();
        holdWait = setTimeout(() => {
            holdRep = setInterval(() => stepNumField(input, delta), 50);
        }, 300);
        btn.setPointerCapture?.(e.pointerId);
    });
    document.addEventListener("pointerup", stopHold);
    document.addEventListener("pointercancel", stopHold);
    document.addEventListener("lostpointercapture", stopHold, true);
}
