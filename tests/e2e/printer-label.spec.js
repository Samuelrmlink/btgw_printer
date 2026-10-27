// Each saved printer remembers its label size and orientation (printers.json,
// next to its calibration), so a reload or a switch back brings them back.
import { test, expect } from '@playwright/test';
import { fresh, getJson, putFile, reload, PL70E } from './helpers.js';

const BENCH = { addr: 'AA:BB:CC:00:01:02', driver: 'nelko_pl70e', model: 'pl70e', nickname: 'Bench' };

test('the label size chosen for a printer is saved and comes back after a reload in a clean browser', async ({ page }) => {
  await fresh(page, { printer: PL70E });
  await page.keyboard.press('Shift+L');
  await page.locator('#media-list li', { hasText: '3 × 1 in' }).locator('button.row-main').click();
  await expect(page.locator('#media-name')).toHaveText('3 × 1 in');
  await expect.poll(async () => (await getJson(page, 'printer/printers.json')).printers[0])
    .toMatchObject({ media: 'nelko-3x1', orient: 0 });
  await page.evaluate(() => localStorage.clear());
  await reload(page);
  await page.waitForFunction(() => window.btgwPrinter && window.btgwPrinter.state.boardReady);
  await expect(page.locator('#media-name')).toHaveText('3 × 1 in');
  expect(await page.evaluate(() => window.btgwPrinter.page().width_dots)).toBe(609);
});

test('switching printers brings each one its own label size', async ({ page }) => {
  await fresh(page, { printer: PL70E });
  const j = await getJson(page, 'printer/printers.json');
  j.printers[0].media = 'nelko-3x1';
  j.printers.push({ calibration: {}, auto_reconnect: true, copies: 1, density: null, speed: -1, ...BENCH, media: 'nelko-4x6', orient: 0 });
  await putFile(page, 'printer/printers.json', JSON.stringify(j));
  await page.evaluate(() => localStorage.clear());
  await reload(page);
  await page.waitForFunction(() => window.btgwPrinter && window.btgwPrinter.state.boardReady);
  await expect(page.locator('#media-name')).toHaveText('3 × 1 in');

  await page.click('#printer-btn');
  await page.locator('#printers-list li', { hasText: 'Bench' }).locator('button.row-main').click();
  await expect(page.locator('#media-name')).toHaveText('4 × 6 in shipping');
  await page.locator('#printers-list li', { hasText: 'Shipping' }).locator('button.row-main').click();
  await expect(page.locator('#media-name')).toHaveText('3 × 1 in');
});
