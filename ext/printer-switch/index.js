// btgw_printer's switch button for the parent UI (companion mode).
//
// Loaded by bluetooth_rp2_webapp's extension loader, either from flash
// js/ext/printer-switch/index.js (today) or from the app's own folder on the
// card, sd:/apps/printer/ext/index.js, once the parent loads apps from there
// (docs/parent-requirements.md R12). With the parent's app.addLauncher it
// registers a launcher; without it, it puts a .tool-btn into #toolbar,
// before #settings-btn. It is a real button, so Tab reaches it.

const ID = "printer-switch-btn";
const LABEL = "Printer";
// From the card the app is the folder above this file. From flash the URL is
// fixed (tools/deploy.py rewrites it when the app lives on the card).
const TARGET = import.meta.url.includes("/sd/apps/") ? new URL("../", import.meta.url).pathname : "/printer/";

function addButton() {
  if (document.getElementById(ID)) return true;
  const bar = document.getElementById("toolbar");
  if (!bar) return false;
  const b = document.createElement("button");
  b.type = "button";
  b.id = ID;
  b.className = "tool-btn";
  b.title = "Open the label printer app";
  b.textContent = LABEL;
  b.addEventListener("click", () => { location.href = TARGET; });
  const settings = document.getElementById("settings-btn");
  if (settings && settings.parentElement === bar) bar.insertBefore(b, settings);
  else bar.appendChild(b);
  return true;
}

export function register(app) {
  if (app && typeof app.addLauncher === "function") {
    app.addLauncher({ id: "printer", label: LABEL, href: TARGET, title: "Open the label printer app" });
    return;
  }
  if (!addButton()) {
    // The toolbar may be built after extensions load.
    const mo = new MutationObserver(() => { if (addButton()) mo.disconnect(); });
    mo.observe(document.documentElement, { childList: true, subtree: true });
  }
}
