import { expect } from '@playwright/test';

export async function op(page, name, params = {}) {
  return page.evaluate(async ([n, p]) => {
    const r = await fetch('/api/op', { method: 'POST', body: JSON.stringify({ id: 1, op: n, params: p }) });
    const j = await r.json();
    if (!j.ok) throw new Error(j.error.message);
    return j.result;
  }, [name, params]);
}

export async function putFile(page, path, text) {
  await page.evaluate(async ([p, t]) => {
    const r = await fetch('/api/fs/' + p, { method: 'PUT', body: t });
    if (!r.ok) throw new Error('PUT ' + p + ' ' + r.status);
  }, [path, text]);
}

export async function getJson(page, path) {
  return page.evaluate(async (p) => {
    const r = await fetch(p.startsWith('sd/') ? '/api/sd/files/' + p.slice(3) : '/api/fs/' + p);
    if (!r.ok) return null;
    const b = new Uint8Array(await r.arrayBuffer());
    let bytes = b;
    if (b[0] === 0x1f && b[1] === 0x8b) {
      bytes = new Uint8Array(await new Response(new Blob([b]).stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer());
    }
    return JSON.parse(new TextDecoder().decode(bytes));
  }, path);
}

export async function listDir(page, dir) {
  return page.evaluate(async (d) => {
    const url = d.startsWith('sd/') ? '/api/sd/files/' + d.slice(3) : '/api/fs/' + d + '/';
    const r = await fetch(url);
    if (!r.ok) return [];
    return (await r.json()).files.map((f) => f.name);
  }, dir);
}

/** Leave the page and let the mock finish what it was serving it, so the
 *  next page load does not share the card-busy mock's slots with the last. */
export async function settle(page) {
  await page.goto('about:blank');
  await page.waitForTimeout(150);
}

/** page.reload(), after settle(): a reload right after the page was busy
 *  would share the card-busy mock's two slots with the old page's requests. */
export async function reload(page) {
  const url = page.url();
  await settle(page);
  await page.goto(url);
}

/** A fresh mock and a fresh page (localStorage cleared). With a printer
 *  saved as current: a Classic one asks "Turn on the printer" after boot
 *  (hub.js requestConnect), which this answers with Connect unless
 *  connect: false; a BLE one connects by itself. */
export async function fresh(page, { printer = null, connect = true } = {}) {
  await page.goto('/printer/');
  await op(page, 'test.reset');
  await page.evaluate(() => localStorage.clear());
  if (printer) {
    await putFile(page, 'printer/printers.json', JSON.stringify({
      version: 1, current: printer.addr,
      printers: [{ calibration: {}, auto_reconnect: true, copies: 1, density: null, speed: -1, ...printer }],
    }));
  }
  await settle(page);
  await page.goto('/printer/');
  await page.waitForFunction(() => window.btgwPrinter && window.btgwPrinter.state.boardReady);
  if (printer && CLASSIC.has(printer.model)) {
    await expect(page.locator('#turn-on')).toBeVisible();
    if (!connect) return;
    await page.click('#turn-on-ok');
  }
  if (printer) await expect(page.locator('#status')).toContainText('connected', { timeout: 5000 });
}

/** The models web/presets/models.json gives transport "classic". */
export const CLASSIC = new Set(['pm220', 'p21', 'd30']);

export const PL70E = { addr: 'AA:BB:CC:00:01:01', driver: 'nelko_pl70e', model: 'pl70e', nickname: 'Shipping' };
export const PM220 = { addr: 'AA:BB:CC:00:00:00', driver: 'nelko_tspl', model: 'pm220', nickname: 'Desk' };
export const P21 = { addr: 'AA:BB:CC:00:02:02', driver: 'nelko_tspl', model: 'p21', nickname: 'Tiny' };

export async function jobs(page) {
  return (await op(page, 'test.jobs')).jobs;
}

export async function waitJobs(page, n) {
  await expect.poll(async () => (await jobs(page)).filter((j) => j.state === 'done').length, { timeout: 10000 }).toBe(n);
  return jobs(page);
}
