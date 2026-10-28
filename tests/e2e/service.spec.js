// The app starts the printer service it needs (roadmap F11): printer_hub at
// boot, a driver only when a printer of it is connected or scanned for.
// The mock's PEX modules are installed; test.set {pex} says which run.
import { test, expect } from '@playwright/test';
import { op, putFile, settle, PL70E, PM220 } from './helpers.js';

const pexOps = async (page) => (await op(page, 'test.pex_ops')).ops;
const connects = async (page) => (await op(page, 'test.connects')).connects;
const STOPPED = { printer_hub: null, nelko_tspl: null, nelko_ble: null, nelko_pl70e: null, phomemo_d30: null, printer_virtual: null };

/** A clean mock with `pex` states and `printers` saved (the first current), then the app. */
async function boot(page, { pex = {}, printers = [], set = {}, before } = {}) {
  await page.goto('/printer/');
  await op(page, 'test.reset');
  await page.evaluate(() => localStorage.clear());
  await op(page, 'test.set', { pex, ...set });
  if (printers.length) {
    await putFile(page, 'printer/printers.json', JSON.stringify({
      version: 1, current: printers[0].addr,
      printers: printers.map((p) => ({ calibration: {}, auto_reconnect: true, copies: 1, density: null, speed: -1, ...p })),
    }));
  }
  if (before) await before();
  await settle(page);
  await page.goto('/printer/');
  await page.waitForFunction(() => window.btgwPrinter && window.btgwPrinter.state.boardReady);
}

test('after a board boot the app starts printer_hub only, and the page carries on as usual', async ({ page }) => {
  await boot(page, { pex: STOPPED });
  expect(await pexOps(page)).toEqual(['pex.load printer_hub', 'pex.start printer_hub']);
  await expect(page.locator('#status')).toHaveText('no printer selected');
  await expect(page.locator('#status-retry')).toBeHidden();
});

test('everything running: no pex ops at all', async ({ page }) => {
  await boot(page, { printers: [PL70E] });
  await expect(page.locator('#status')).toContainText('Shipping: connected');
  expect(await pexOps(page)).toEqual([]);
});

test('connecting a saved PM220 starts nelko_tspl first, after the Turn-on prompt', async ({ page }) => {
  await boot(page, { pex: STOPPED, printers: [PM220] });
  await expect(page.locator('#turn-on')).toBeVisible();
  expect(await pexOps(page)).toEqual(['pex.load printer_hub', 'pex.start printer_hub']);
  await page.click('#turn-on-ok');
  await expect(page.locator('#status')).toContainText('Desk: connected');
  expect(await pexOps(page)).toEqual(['pex.load printer_hub', 'pex.start printer_hub', 'pex.load nelko_tspl', 'pex.start nelko_tspl']);
  expect(await connects(page)).toEqual([{ driver: 'nelko_tspl', addr: PM220.addr }]);
});

test('loading a driver first unloads our other driver that has no printer in use', async ({ page }) => {
  // nelko_tspl runs with nothing on it; the pool would hold both (the bench board's did, with 32 bytes left)
  await boot(page, { pex: { ...STOPPED, printer_hub: 'running', nelko_tspl: 'running' }, printers: [PL70E] });
  await expect(page.locator('#status')).toContainText('Shipping: connected');
  expect(await pexOps(page)).toEqual(['pex.stop nelko_tspl', 'pex.unload nelko_tspl', 'pex.load nelko_pl70e', 'pex.start nelko_pl70e']);
  expect(await connects(page)).toEqual([{ driver: 'nelko_pl70e', addr: PL70E.addr }]);
});

test('a driver that is loaded but stopped is started, not loaded again (no second instance)', async ({ page }) => {
  // as after pex.stop on the board: still loaded, and a pex.load would add nelko_pl70e#2
  await boot(page, { pex: { ...STOPPED, printer_hub: 'running', nelko_pl70e: 'stopped' }, printers: [PL70E] });
  await expect(page.locator('#status')).toContainText('Shipping: connected');
  expect(await pexOps(page)).toEqual(['pex.start nelko_pl70e']);
  const names = (await op(page, 'pex.list')).modules.map((m) => m.name);
  expect(names.filter((n) => n.includes('#'))).toEqual([]);
});

test('printer_hub loaded but stopped at boot: started, not loaded again', async ({ page }) => {
  await boot(page, { pex: { ...STOPPED, printer_hub: 'stopped' } });
  await expect(page.locator('#status')).toHaveText('no printer selected');
  expect(await pexOps(page)).toEqual(['pex.start printer_hub']);
});

test('a driver with a connected printer is not unloaded for another one', async ({ page }) => {
  await boot(page, {
    pex: { ...STOPPED, printer_hub: 'running', nelko_tspl: 'running' }, printers: [PL70E, PM220],
    before: () => op(page, 'test.printer_state', { addr: PM220.addr, driver: 'nelko_tspl', model: 'pm220', state: 'connected' }),
  });
  await expect(page.locator('#status')).toContainText('Shipping: connected');
  expect(await pexOps(page)).toEqual(['pex.load nelko_pl70e', 'pex.start nelko_pl70e']);
});

test('PEX pool full and the other driver has a printer connected: nothing unloaded, the board\'s message and Retry', async ({ page }) => {
  await boot(page, {
    pex: { ...STOPPED, printer_hub: 'running', nelko_tspl: 'running' }, set: { pool_drivers: 1 }, printers: [PL70E, PM220],
    before: () => op(page, 'test.printer_state', { addr: PM220.addr, driver: 'nelko_tspl', model: 'pm220', state: 'connected' }),
  });
  await expect(page.locator('#status')).toContainText("Shipping: couldn't start the nelko_pl70e driver: out of PEX pool memory");
  await expect(page.locator('#status')).toContainText('the PEX pool has 12800 in one piece');
  await expect(page.locator('#status-retry')).toBeVisible();
  await page.waitForTimeout(2500);   // no retry on its own
  expect(await pexOps(page)).toEqual(['pex.load nelko_pl70e']);
  expect(await connects(page)).toEqual([]);
  // the other printer goes away; Retry now frees its driver first and connects
  await op(page, 'test.printer_state', { addr: PM220.addr, driver: 'nelko_tspl', model: 'pm220', state: 'disconnected' });
  await page.click('#status-retry');
  await expect(page.locator('#status')).toContainText('Shipping: connected');
  expect((await pexOps(page)).slice(1)).toEqual(['pex.stop nelko_tspl', 'pex.unload nelko_tspl', 'pex.load nelko_pl70e', 'pex.start nelko_pl70e']);
});

test('a scan for a printer family starts its driver first', async ({ page }) => {
  await boot(page, { pex: { ...STOPPED, printer_hub: 'running' } });
  await page.click('#printer-btn');
  const fam = page.locator('#printers-family');
  await expect(fam.locator('option')).toContainText(['Nelko PM220', 'Nelko P21', 'Nelko PL70e-BT']);
  await fam.focus();
  await fam.selectOption({ label: 'Nelko PL70e-BT' });
  await page.keyboard.press('Tab');
  await expect(page.locator('#printers-scan')).toBeFocused();
  await page.keyboard.press('Enter');
  await expect(page.locator('#printers-found li', { hasText: PL70E.addr })).toContainText('pl70e');
  expect(await pexOps(page)).toEqual(['pex.load nelko_pl70e', 'pex.start nelko_pl70e']);
  await expect(page.locator('#printers-found li', { hasText: PM220.addr })).toHaveCount(0);   // nelko_tspl was not asked
});

test('printer_hub that will not start: the board\'s message and Retry, no retry on its own', async ({ page }) => {
  await boot(page, { pex: { ...STOPPED, printer_hub: 'absent' } });
  await expect(page.locator('#status')).toHaveText("printer service didn't start: printer_hub is not installed");
  await expect(page.locator('#status-retry')).toBeVisible();
  await page.waitForTimeout(2500);
  expect(await pexOps(page)).toEqual(['pex.load printer_hub']);
  await op(page, 'test.set', { pex: { printer_hub: null } });   // now it is there
  await page.click('#status-retry');
  await expect(page.locator('#status')).toHaveText('no printer selected');
  await expect(page.locator('#status-retry')).toBeHidden();
  expect(await pexOps(page)).toEqual(['pex.load printer_hub', 'pex.load printer_hub', 'pex.start printer_hub']);
});
