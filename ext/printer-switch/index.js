// btgw_printer's switch button for the parent UI (companion mode).
//
// Loaded by bluetooth_rp2_webapp's web/js/extensions.js from
// js/ext/printer-switch/index.js. The parent has no launcher API yet
// (docs/parent-requirements.md R3), so this puts a .tool-btn into #toolbar,
// before #settings-btn. It is a real button, so Tab reaches it.

const ID = "printer-switch-btn";
const TARGET = "/printer/";

function addButton() {
  if (document.getElementById(ID)) return true;
  const bar = document.getElementById("toolbar");
  if (!bar) return false;
  const b = document.createElement("button");
  b.type = "button";
  b.id = ID;
  b.className = "tool-btn";
  b.title = "Open the label printer app";
  b.textContent = "Printer";
  b.addEventListener("click", () => { location.href = TARGET; });
  const settings = document.getElementById("settings-btn");
  if (settings && settings.parentElement === bar) bar.insertBefore(b, settings);
  else bar.appendChild(b);
  return true;
}

export function register(_app) {
  if (!addButton()) {
    // The toolbar may be built after extensions load.
    const mo = new MutationObserver(() => { if (addButton()) mo.disconnect(); });
    mo.observe(document.documentElement, { childList: true, subtree: true });
  }
}
