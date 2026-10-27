import { test, expect } from '@playwright/test';
import { fresh, getJson, listDir, PL70E, waitJobs, reload } from './helpers.js';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';

async function addText(page, text) {
  await page.click('#add-text');
  await page.fill('#text', text);
  await page.keyboard.press('Escape');
}

test('save to flash and to the SD card, reopen, rename, delete — keyboard only', async ({ page }) => {
  await fresh(page);
  await addText(page, 'Flash label');
  await page.keyboard.press('Control+s');            // no name yet → save dialog
  await expect(page.locator('#picker')).toBeVisible();
  await expect(page.locator('#picker-stores')).toBeVisible();
  await expect(page.locator('#picker-name')).toBeFocused();
  await page.keyboard.press('Control+a');
  await page.keyboard.type('shelf');
  await page.keyboard.press('Enter');
  await expect(page.locator('#picker')).toBeHidden();
  await expect(page.locator('#status')).toContainText('saved shelf.json');
  expect(await listDir(page, 'printer/labels')).toEqual(['shelf.json.gz']);
  const doc = await getJson(page, 'printer/labels/shelf.json.gz');
  expect(doc.format).toBe('btgw-printer-label');
  expect(doc.media.id).toBe('nelko-4x6');
  expect(doc.objects[0].text).toBe('Flash label');

  // Save as, on the card: ] switches the store
  await page.keyboard.press('Control+Shift+S');
  await page.locator('#picker-name').press('Escape').catch(() => {});
  await expect(page.locator('#picker')).toBeHidden();
  await page.keyboard.press('Control+Shift+S');
  await page.locator('.picker-panel').first().focus();
  await page.keyboard.press(']');
  await expect(page.locator('.store-tab[data-store="sd"]')).toHaveAttribute('aria-selected', 'true');
  await page.locator('#picker-name').fill('bin');
  await page.locator('#picker-name').press('Enter');
  await expect(page.locator('#picker')).toBeHidden();
  expect(await listDir(page, 'sd/printer/labels')).toEqual(['bin.json.gz']);

  // Quick save goes back to the card, where bin came from
  await page.click('#add-qr');
  await page.keyboard.press('Escape');
  await page.keyboard.press('Control+s');
  await expect.poll(async () => (await getJson(page, 'sd/printer/labels/bin.json.gz')).objects.length).toBe(2);

  // Open from flash
  await page.keyboard.press('Control+o');
  await expect(page.locator('#picker-title')).toHaveText('Open label');
  await page.locator('.picker-panel').first().focus();
  await page.keyboard.press('[');
  await expect(page.locator('.store-tab[data-store="flash"]')).toHaveAttribute('aria-selected', 'true');
  await expect(page.locator('#picker-list li')).toHaveCount(1);
  await page.keyboard.press('Enter');
  await expect(page.locator('#picker')).toBeHidden();
  await expect(page.locator('#status')).toContainText('opened shelf.json');
  expect(await page.evaluate(() => window.btgwPrinter.editor.boxes.length)).toBe(1);

  // Rename: row actions are h/l
  await page.keyboard.press('Control+o');
  await expect(page.locator('#picker-list li')).toHaveCount(1);
  await page.locator('.picker-panel').first().focus();
  await page.keyboard.press('l');
  await page.keyboard.press('l');
  await page.keyboard.press('Enter');
  const input = page.locator('.picker-rename');
  await expect(input).toBeFocused();
  await input.fill('shelf-a');
  await input.press('Enter');
  await expect(page.locator('#picker-list li')).toHaveText(/shelf-a\.json/);
  expect(await listDir(page, 'printer/labels')).toEqual(['shelf-a.json.gz']);

  // Delete asks first, Cancel is focused
  await expect(page.locator('#picker-list li')).toHaveCount(1);
  await page.locator('.picker-panel').first().focus();
  await page.keyboard.press('l');
  await page.keyboard.press('l');
  await page.keyboard.press('l');
  await page.keyboard.press('Enter');
  await expect(page.locator('#confirm')).toBeVisible();
  await expect(page.locator('#confirm-cancel')).toBeFocused();
  await page.keyboard.press('Escape');
  await expect(page.locator('#confirm')).toBeHidden();
  await expect(page.locator('#picker')).toBeVisible();
  expect(await listDir(page, 'printer/labels')).toEqual(['shelf-a.json.gz']);
  await page.locator('#picker-list li button', { hasText: 'Delete' }).click();
  await expect(page.locator('#confirm')).toBeVisible();
  await page.click('#confirm-ok');
  await expect(page.locator('#picker-empty')).toBeVisible();
  expect(await listDir(page, 'printer/labels')).toEqual([]);
});

test('the SD tab is hidden when there is no card', async ({ page }) => {
  await fresh(page);
  await page.route('**/api/sd', (r) => r.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true,"present":false,"mounted":false,"state":"none"}' }));
  await reload(page);
  await expect(page.locator('#status')).not.toHaveText('connecting…');
  await page.keyboard.press('Control+o');
  await expect(page.locator('#picker')).toBeVisible();
  await expect(page.locator('#picker-stores')).toBeHidden();
});

test('a pm220_rp2_webapp label (gzipped) imports as a 50 x 30 PM220 label', async ({ page }) => {
  await fresh(page);
  const pm = { format: 'pm220-label', version: 2, media: { width_dots: 384, height_dots: 240, dpi: 203, width_mm: 50, height_mm: 30 },
    objects: [{ id: 1, type: 'text', x: 192, y: 60, width: 200, height: 60, text: 'Old label', size: 24, autoSize: true, wrap: true, align: 'left', valign: 'top', rotate: 0 }] };
  const f = path.join(os.tmpdir(), `old-${Date.now()}.pm220.json.gz`);
  fs.writeFileSync(f, zlib.gzipSync(JSON.stringify(pm)));
  await page.keyboard.press('Control+o');
  await page.setInputFiles('#file', f);
  await expect(page.locator('#status')).toContainText('pm220 label');
  await expect(page.locator('#media-name')).toHaveText('50 × 30 mm');
  const st = await page.evaluate(() => ({ boxes: window.btgwPrinter.editor.boxes.map((b) => b.text), model: window.btgwPrinter.state.model.id,
    page: [window.btgwPrinter.page().width_dots, window.btgwPrinter.page().height_dots] }));
  expect(st).toEqual({ boxes: ['Old label'], model: 'pm220', page: [384, 240] });
  fs.unlinkSync(f);
});

test('a label left by pm220_rp2_webapp in this browser is restored', async ({ page }) => {
  await page.goto('/printer/');
  await page.evaluate(() => {
    localStorage.clear();
    localStorage.setItem('pm220-editor-v1', JSON.stringify({ format: 'pm220-label', version: 2, objects: [{ id: 4, type: 'qr', x: 100, y: 100, width: 80, height: 80, payload: 'legacy' }] }));
  });
  await reload(page);
  await expect(page.locator('#media-name')).toHaveText('50 × 30 mm');
  expect(await page.evaluate(() => window.btgwPrinter.editor.boxes.map((b) => b.payload))).toEqual(['legacy']);
});

test('download writes plain JSON even for a gzipped stored label', async ({ page }) => {
  await fresh(page);
  await addText(page, 'dl');
  await page.keyboard.press('Control+s');
  await page.locator('#picker-name').fill('dl');
  await page.locator('#picker-name').press('Enter');
  await expect(page.locator('#picker')).toBeHidden();
  await page.keyboard.press('Control+o');
  const [download] = await Promise.all([
    page.waitForEvent('download'),
    page.locator('#picker-list li button', { hasText: 'Download' }).click(),
  ]);
  expect(download.suggestedFilename()).toBe('dl.json');
  const text = fs.readFileSync(await download.path(), 'utf8');
  expect(JSON.parse(text).objects[0].text).toBe('dl');
});

test('a printed label from a saved template still prints after a reload', async ({ page }) => {
  await fresh(page, { printer: PL70E });
  await addText(page, 'persist');
  await reload(page);
  await expect(page.locator('#status')).toContainText('connected');
  expect(await page.evaluate(() => window.btgwPrinter.editor.boxes.map((b) => b.text))).toEqual(['persist']);
  await page.click('#print');
  await waitJobs(page, 1);
});
