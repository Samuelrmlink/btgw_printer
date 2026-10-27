import { test, expect } from '@playwright/test';
import { fresh, getJson, putFile, waitJobs, PL70E, PM220, reload } from './helpers.js';

test('switching label size on an open label rescales it; a custom size is saved', async ({ page }) => {
  await fresh(page, { printer: PL70E });
  await page.click('#add-qr');
  await page.keyboard.press('Escape');
  const before = await page.evaluate(() => ({ ...window.btgwPrinter.editor.boxes[0] }));
  await page.keyboard.press('Shift+L');
  await expect(page.locator('#media')).toBeVisible();
  await expect(page.locator('#media-list li')).toHaveCount(5);
  await page.locator('#media-list li', { hasText: '3 × 1 in' }).locator('button.row-main').click();
  await expect(page.locator('#media-name')).toHaveText('3 × 1 in');
  const after = await page.evaluate(() => ({ box: { ...window.btgwPrinter.editor.boxes[0] }, w: window.btgwPrinter.page().width_dots, h: window.btgwPrinter.page().height_dots }));
  expect([after.w, after.h]).toEqual([609, 203]);
  expect(after.box.width).toBeLessThan(before.width);
  expect(after.box.x + after.box.width / 2).toBeLessThanOrEqual(609);

  // a new size, from the keyboard (+)
  await page.locator('#media-list button').first().focus();
  await page.keyboard.press('+');
  await expect(page.locator('#me-name')).toBeFocused();
  await page.keyboard.type('Jar 2x2');
  await page.fill('#me-w', '50.8');
  await page.fill('#me-h', '50.8');
  await page.click('#me-save');
  await expect(page.locator('#media-name')).toHaveText('Jar 2x2');
  const saved = await getJson(page, 'printer/media.json');
  expect(saved.media).toHaveLength(1);
  expect(saved.media[0]).toMatchObject({ name: 'Jar 2x2', w_mm: 50.8, h_mm: 50.8, models: ['pl70e'] });
  await page.keyboard.press('Escape');
  await page.click('#print');
  const [j] = await waitJobs(page, 1);
  expect([j.params.width_dots, j.params.rows]).toEqual([406, 406]);
});

test('orientation turns the editor page and the job comes back in head orientation', async ({ page }) => {
  await fresh(page, { printer: PL70E });
  await page.click('#media-btn');
  await page.selectOption('#media-orient', '90');
  await page.keyboard.press('Escape');
  const pg = await page.evaluate(() => [window.btgwPrinter.page().width_dots, window.btgwPrinter.page().height_dots]);
  expect(pg).toEqual([1218, 812]);
  await page.click('#print');
  const [j] = await waitJobs(page, 1);
  expect([j.params.width_dots, j.params.rows]).toEqual([812, 1218]);
});

test('calibration is saved per printer and label size, and the test frame prints', async ({ page }) => {
  await fresh(page, { printer: PM220 });
  await page.keyboard.press('Shift+C');
  await expect(page.locator('#cal')).toBeVisible();
  await expect(page.locator('#cal-offx')).toBeFocused();
  await expect(page.locator('#cal-offx')).toHaveValue('-1.25');
  await expect(page.locator('#cal-effective')).toContainText('BITMAP 0,24  384×216');
  await page.keyboard.press('ArrowUp');          // +0.25 mm
  await page.keyboard.press('Tab');              // offset Y
  await page.keyboard.press('Shift+ArrowDown');  // -8 × 0.25 = -2 mm
  await expect(page.locator('#cal-offy')).toHaveValue('1');
  await page.keyboard.press('Control+s');
  await expect(page.locator('#status')).toContainText('calibration saved');
  const saved = await getJson(page, 'printer/printers.json');
  expect(saved.printers[0].calibration['nelko-pm220-50x30']).toMatchObject({ offset_x_mm: -1, offset_y_mm: 1 });
  await page.click('#cal-test');
  const [j] = await waitJobs(page, 1);
  // offset y 1 mm = 8 dots; offset x -1 mm = -8 + 8 centring = 0
  expect(j.params.origin).toEqual([0, 8]);
  expect(j.params.rows).toBe(232);
  await page.keyboard.press('Escape');
  await expect(page.locator('#cal')).toBeHidden();
  // the editor's safe rect follows the saved calibration after a reload
  await reload(page);
  await page.waitForFunction(() => window.btgwPrinter && window.btgwPrinter.state.boardReady);
  const L = await page.evaluate(() => window.btgwPrinter.page().layout);
  expect([L.originX, L.originY, L.printH]).toEqual([0, 8, 232]);
});

test('companion mode: BTGW button here, Printer button in the parent UI', async ({ page }) => {
  await fresh(page);
  await expect(page.locator('#mode-switch')).toBeVisible();
  await page.click('#mode-switch');
  await expect(page).toHaveURL(/\/$/);
  const btn = page.locator('#toolbar #printer-switch-btn');
  await expect(btn).toBeVisible();
  // it sits before the parent's settings button
  expect(await page.evaluate(() => document.getElementById('printer-switch-btn').nextElementSibling.id)).toBe('settings-btn');
  await btn.click();
  await expect(page).toHaveURL(/\/printer\/$/);
});

test('standalone mode hides the BTGW button', async ({ page }) => {
  await fresh(page);
  await putFile(page, 'printer/mode.json', '{"mode":"standalone"}');
  await reload(page);
  await page.waitForFunction(() => window.btgwPrinter && window.btgwPrinter.state.boardReady);
  await expect(page.locator('#mode-switch')).toBeHidden();
});

test('network dialog: tabs from the keyboard, Wi-Fi state, WireGuard status and config', async ({ page }) => {
  await fresh(page);
  await page.keyboard.press('Shift+W');
  await expect(page.locator('#wifi')).toBeVisible();
  await expect(page.locator('#wifi-live')).toContainText('BenchNet');
  await expect(page.locator('#wifi-known-list li')).toHaveCount(1);
  await expect(page.locator('#wifi-tab-sta')).toBeFocused();
  await page.keyboard.press('l');
  await expect(page.locator('#wifi-pane-ap')).toBeVisible();
  await page.fill('#wifi-ap-ssid', 'Labels');
  await expect(page.locator('#wifi-save')).toBeVisible();
  await page.keyboard.press('Control+s');
  await expect(page.locator('#status')).toContainText('hotspot settings saved');
  await page.locator('#wifi-tab-ap').focus();
  await page.keyboard.press('l');
  await expect(page.locator('#wifi-pane-wg')).toBeVisible();
  await expect(page.locator('#wg-status')).toContainText('connected');
  await expect(page.locator('#wg-address')).toHaveValue('10.255.255.23');
  await page.fill('#wg-keepalive', '15');
  await page.click('#wg-save');
  const cfg = await getJson(page, 'cfg/wireguard.json');
  expect(cfg.keepalive).toBe(15);
  expect(cfg.private_key).toBe('x'.repeat(44));   // untouched keys are kept
  await page.keyboard.press('Escape');
  await expect(page.locator('#wifi')).toBeHidden();
});

test('help lists the keys; Delete inside the text box edits text, not the object', async ({ page }) => {
  await fresh(page);
  await page.keyboard.press('?');
  await expect(page.locator('#help')).toBeVisible();
  await expect(page.locator('#help-cols')).toContainText('Batch print');
  await expect(page.locator('#help-cols')).toContainText('Open/Save: Flash ⇄ SD card');
  await page.keyboard.press('q');
  await expect(page.locator('#help')).toBeHidden();
  await page.click('#add-text');
  await page.locator('#text').click();
  await page.keyboard.press('End');
  await page.keyboard.press('Delete');
  await page.keyboard.press('Backspace');
  expect(await page.evaluate(() => window.btgwPrinter.editor.boxes.length)).toBe(1);
  await expect(page.locator('#text')).toHaveValue('Tex');
  // on the label, Delete removes it
  await page.keyboard.press('Escape');
  await page.keyboard.press('n');
  await page.keyboard.press(' ');
  await page.keyboard.press('Delete');
  expect(await page.evaluate(() => window.btgwPrinter.editor.boxes.length)).toBe(0);
});

test('without printer_hub the app still designs and says why printing is off', async ({ page }) => {
  await fresh(page);
  await page.evaluate(async () => fetch('/api/op', { method: 'POST', body: JSON.stringify({ op: 'test.set', params: { hub: false } }) }));
  await reload(page);
  await page.waitForFunction(() => window.btgwPrinter && window.btgwPrinter.state.boardReady);
  await expect(page.locator('#status')).toContainText('no printer_hub');
  await expect(page.locator('#print')).toBeDisabled();
  await page.click('#add-text');
  expect(await page.evaluate(() => window.btgwPrinter.editor.boxes.length)).toBe(1);
  await page.click('#printer-btn');
  await expect(page.locator('#printers-live')).toContainText('printer_hub is not running');
  await expect(page.locator('#printers-scan')).toBeDisabled();
});
