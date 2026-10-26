import { test, expect } from '@playwright/test';
import { fresh, op, waitJobs, jobs, PL70E, PM220, P21, putFile, getJson } from './helpers.js';

test('keyboard only: find a printer, save it, add a text box, print', async ({ page }) => {
  await fresh(page);
  await page.locator('body').click({ position: { x: 600, y: 400 } });
  await page.keyboard.press('Escape');
  await page.keyboard.press('Shift+P');
  await expect(page.locator('#printers')).toBeVisible();
  await page.keyboard.press('s');
  await expect(page.locator('#printers-found li')).toHaveCount(5);
  // Tab to the found list, then j to the PL70e (second row) and Enter
  const pl70 = page.locator('#printers-found li', { hasText: 'PL70e' }).locator('button');
  for (let i = 0; i < 12 && !(await pl70.evaluate((b) => b === document.activeElement)); i++) {
    const inFound = await page.evaluate(() => !!document.activeElement.closest('#printers-found'));
    await page.keyboard.press(inFound ? 'j' : 'Tab');
  }
  await expect(pl70).toBeFocused();
  await page.keyboard.press('Enter');
  await expect(page.locator('#pe-nick')).toBeFocused();
  await page.keyboard.press('Control+a');
  await page.keyboard.type('Shipping');
  for (let i = 0; i < 10 && !(await page.locator('#pe-save').evaluate((b) => b === document.activeElement)); i++) {
    await page.keyboard.press('Tab');
  }
  await page.keyboard.press('Enter');
  await expect(page.locator('#printers-list li')).toHaveCount(1);
  await expect(page.locator('#printers-list li').first()).toContainText('connected');
  await page.keyboard.press('Escape');
  await expect(page.locator('#printers')).toBeHidden();
  await expect(page.locator('#status')).toContainText('Shipping: connected');
  await page.keyboard.press('t');
  await expect(page.locator('#editor-pane')).toBeVisible();
  await page.keyboard.press('Escape');
  await page.keyboard.press('Control+p');
  const js = await waitJobs(page, 1);
  expect(js[0].params).toMatchObject({ driver: 'nelko_pl70e', model: 'pl70e', width_dots: 812, rows: 1218, copies: 1 });
  expect(js[0].params.media).toMatchObject({ w_mm: 101.6, h_mm: 152.4, type: 'gap' });
  expect(js[0].bytes).toBe(102 * 1218);
  await expect(page.locator('#status')).toContainText('printed 1 label');
  const saved = await getJson(page, 'printer/printers.json');
  expect(saved.current).toBe(PL70E.addr);
});

test('PM220 label: bitmap is the pm220 384x216 at 0,24 with TSPL polarity', async ({ page }) => {
  await fresh(page, { printer: PM220 });
  await expect(page.locator('#media-name')).toHaveText('50 × 30 mm');
  await page.click('#print');
  const [j] = await waitJobs(page, 1);
  expect(j.params).toMatchObject({ width_dots: 384, rows: 216, origin: [0, 24], density: 8 });
  expect(j.params.media).toMatchObject({ w_mm: 50, h_mm: 30, gap_mm: 2 });
  // an empty label is all white: black_bit 0 means every byte is 0xff
  const bytes = Buffer.from(j.b64, 'base64');
  expect(bytes.length).toBe(48 * 216);
  expect(bytes.every((b) => b === 0xff)).toBe(true);
});

test('P21 landscape label prints head-wide with the origin baked in', async ({ page }) => {
  await fresh(page, { printer: P21 });
  await expect(page.locator('#media-name')).toHaveText('14 × 40 mm');
  const pg = await page.evaluate(() => { const p = window.btgwPrinter.page(); return [p.width_dots, p.height_dots, p.orient]; });
  expect(pg).toEqual([320, 96, 90]);
  await page.click('#print');
  const [j] = await waitJobs(page, 1);
  expect(j.params.width_dots).toBe(96);
  expect(j.params.origin).toEqual([0, 0]);
  const bytes = Buffer.from(j.b64, 'base64');
  expect(bytes.every((b) => b === 0xff)).toBe(true); // the P21 burns 0 bits, so white is 1
});

test('a failed job is reported and cancel stops a running one', async ({ page }) => {
  await fresh(page, { printer: PL70E });
  await op(page, 'test.set', { fail_next: 'paper out' });
  await page.click('#print');
  await expect(page.locator('#status')).toContainText('paper out');
  await op(page, 'test.set', { job_ms: 400 });
  await page.click('#print');
  await expect(page.locator('#job-bar')).toBeVisible();
  await expect.poll(async () => (await jobs(page)).length).toBe(2);
  await page.click('#job-cancel');
  await expect(page.locator('#status')).toContainText('cancelled');
  await expect(page.locator('#job-bar')).toBeHidden();
  const js = await jobs(page);
  expect(js.map((x) => x.state)).toEqual(['failed', 'cancelled']);
});

test('print is refused without a connected printer and the printer dialog opens', async ({ page }) => {
  await fresh(page);
  await expect(page.locator('#print')).toBeDisabled();
  await page.keyboard.press('Control+p');
  await expect(page.locator('#printers')).toBeVisible();
  await expect(page.locator('#status')).toContainText('pick a printer first');
});
