// Lazy loading (web/lazy.js): a small boot graph fetched a couple of files at
// a time, everything else on first use, every request through one queue of
// two. In the card-busy project the mock answers 409 busy past 2 static GETs
// in flight (and holds each 40 ms), like the board does past its limit.
import { test, expect } from '@playwright/test';
import { op, PL70E, putFile, settle } from './helpers.js';

const BOOT_JS = ['lazy.js', 'js/btgw/wsapi.js', 'js/btgw/fs.js', 'raster.js', 'layout.js', 'store.js', 'keymap.js', 'numfield.js',
  'doc.js', 'device.js', 'editor.js', 'catalog.js', 'hub.js', 'app.js'];

async function clean(page, printer) {
  await page.goto('/printer/');
  await op(page, 'test.reset');
  await page.evaluate(() => localStorage.clear());
  if (printer) {
    await putFile(page, 'printer/printers.json', JSON.stringify({ version: 1, current: printer.addr, printers: [{ calibration: {}, ...printer }] }));
  }
  await settle(page);
}
const ready = (page) => page.waitForFunction(() => window.btgwPrinter && window.btgwPrinter.state.boardReady);

/** Every HTTP request the page makes to the mock (not the WebSocket), in flight. */
function track(page) {
  const origin = new URL(test.info().project.use.baseURL).origin;
  const t = { inFlight: 0, peak: 0, js: [], busy: [], on: true };
  const mine = (r) => t.on && r.url().startsWith(origin) && new URL(r.url()).pathname !== '/ws';
  page.on('request', (r) => {
    if (!mine(r)) return;
    r._tracked = true;
    t.inFlight++;
    t.peak = Math.max(t.peak, t.inFlight);
    const rel = new URL(r.url()).pathname.replace(/^\/printer\//, '');
    if (rel.endsWith('.js')) t.js.push(rel);
  });
  const end = (r) => { if (r._tracked) { r._tracked = false; t.inFlight--; } };
  page.on('requestfinished', end);
  page.on('requestfailed', end);
  page.on('response', (r) => { if (r.url().startsWith(origin) && (r.status() === 409 || r.status() === 503)) t.busy.push(r.url()); });
  return t;
}

test('boot fetches only the boot graph, at most 2 requests at once, and never hears busy', async ({ page }) => {
  await clean(page, PL70E);
  const t = track(page);
  const t0 = Date.now();
  await page.goto('/printer/');
  await ready(page);
  const ms = Date.now() - t0;
  t.on = false;
  console.log(`boot: ${ms} ms, peak ${t.peak} requests in flight, ${t.js.length} modules`);
  expect(t.peak).toBeLessThanOrEqual(2);
  expect([...t.js].sort()).toEqual([...BOOT_JS].sort());
  expect(t.busy).toEqual([]);
  await expect(page.locator('#status')).toContainText('Shipping: connected');
});

test('every lazy dialog opens from the keyboard, two requests at most, saying loading… while it loads', async ({ page }) => {
  await clean(page);
  // slow the lazy modules down so the first open has to wait for one
  await page.route(/\/printer\/(dialog|printers|media|cal|pack|files|network|vars|csv|templates)\.js/, async (r) => {
    await new Promise((x) => setTimeout(x, 300));
    await r.continue();
  });
  const t = track(page);
  await page.goto('/printer/');
  await ready(page);
  await page.locator('body').click({ position: { x: 600, y: 400 } });
  await page.keyboard.press('Escape');
  await page.keyboard.press('Shift+P');
  await expect(page.locator('#status')).toHaveText('loading…');
  for (const [key, id] of [['Shift+P', '#printers'], ['Shift+L', '#media'], ['Shift+V', '#vars'], ['Shift+C', '#cal'],
    ['Shift+W', '#wifi'], ['Control+o', '#picker'], ['Control+b', '#batch']]) {
    if (key !== 'Shift+P') await page.keyboard.press(key);
    await expect(page.locator(id), key).toBeVisible();
    await page.keyboard.press('Escape');
    await expect(page.locator(id), key).toBeHidden();
  }
  await expect(page.locator('#status')).not.toHaveText('loading…');
  t.on = false;
  console.log(`dialogs: peak ${t.peak} requests in flight, ${t.js.length} modules`);
  expect(t.peak).toBeLessThanOrEqual(2);
  expect(t.busy).toEqual([]);
});

test('a printer whose name came late shows its name and model; one that is not the driver\'s goes', async ({ page }) => {
  await clean(page);
  await page.goto('/printer/');
  await ready(page);
  await page.click('#printer-btn');
  await page.click('#printers-scan');
  const late = page.locator('#printers-found li', { hasText: 'AA:BB:CC:00:10:10' });
  await expect(late).toContainText('PM220');
  await expect(late).toContainText('pm220 · AA:BB:CC:00:10:10');
  await expect(late).toHaveCount(1);
  const pl70e = page.locator('#printers-found li', { hasText: 'AA:BB:CC:00:01:01' });
  await expect(pl70e).toHaveCount(1);        // nelko_tspl's claim is gone, nelko_pl70e's stays
  await expect(pl70e).toContainText('pl70e');
  await expect(page.locator('#printers-found li', { hasText: 'nelko_tspl' })).toHaveCount(0);
});
