// The calibration dialog's files: an old pm220 settings/print.json, our own
// .cal.json export, and a print.json left on the board by pm220_rp2_webapp.
import { test, expect } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { fresh, getJson, putFile, PM220, P21 } from './helpers.js';

const PRINT_JSON = 'tests/fixtures/pm220_print.json';
const EFFECTIVE = 'BITMAP 5,20  384×216  (canvas 384×240, label 400×240, offset -0.75/2.5 mm, gap 2.2 mm, safe 20,18–380,222)';
const FIELDS = { '#cal-offx': '-0.75', '#cal-offy': '2.5', '#cal-gap': '2.2', '#cal-ox': '3', '#cal-oy': '0', '#cal-ph': '-4',
  '#cal-sx0': '20', '#cal-sy0': '18', '#cal-sx1': '380', '#cal-sy1': '222' };

async function expectFields(page) {
  for (const [sel, v] of Object.entries(FIELDS)) await expect(page.locator(sel)).toHaveValue(v);
  await expect(page.locator('#cal-effective')).toHaveText(EFFECTIVE);
}

test('an old pm220 print.json loads into the dialog, saves per printer × label size and survives a reload', async ({ page }) => {
  await fresh(page, { printer: PM220 });
  await page.keyboard.press('Shift+C');
  await expect(page.locator('#cal')).toBeVisible();
  await page.setInputFiles('#cal-file', PRINT_JSON);
  await expect(page.locator('#cal-loaded')).toHaveText('loaded from pm220_print.json — Save to keep it');
  await expectFields(page);
  expect((await getJson(page, 'printer/printers.json')).printers[0].calibration).toEqual({});   // not saved yet
  await page.keyboard.press('Control+s');
  await expect(page.locator('#status')).toContainText('calibration saved to printer/printers.json');
  await page.goto('/printer/');
  await page.waitForFunction(() => window.btgwPrinter && window.btgwPrinter.state.boardReady);
  await page.keyboard.press('Shift+C');
  await expect(page.locator('#cal')).toBeVisible();
  await expectFields(page);
  await expect(page.locator('#cal-loaded')).toBeHidden();
});

test('Save file… downloads <printer>-<media>.cal.json that loads back', async ({ page }) => {
  await fresh(page, { printer: PM220 });
  await page.keyboard.press('Shift+C');
  await expect(page.locator('#cal')).toBeVisible();
  await page.setInputFiles('#cal-file', PRINT_JSON);
  const [dl] = await Promise.all([page.waitForEvent('download'), page.click('#cal-export')]);
  expect(dl.suggestedFilename()).toBe('Desk-nelko-pm220-50x30.cal.json');
  const path = await dl.path();
  const j = JSON.parse(readFileSync(path, 'utf8'));
  expect(j).toMatchObject({ format: 'btgw-printer-cal', version: 1, model: 'pm220', media_id: 'nelko-pm220-50x30',
    offset_x_mm: -0.75, offset_y_mm: 2.5, origin_x_adj: 3, print_height_adj: -4, gap_mm: 2.2, safe_x0: 20, safe_y1: 222 });
  await page.click('#cal-reset');
  await expect(page.locator('#cal-offx')).toHaveValue('-1.25');
  await page.setInputFiles('#cal-file', { name: 'Desk-nelko-pm220-50x30.cal.json', mimeType: 'application/json', buffer: readFileSync(path) });
  await expectFields(page);
  await expect(page.locator('#cal-loaded')).toHaveText('loaded from Desk-nelko-pm220-50x30.cal.json — Save to keep it');
});

test('a settings/print.json on the board is offered only when it exists; a pm220 file on a P21 says so', async ({ page }) => {
  await fresh(page, { printer: PM220 });
  await page.keyboard.press('Shift+C');
  await expect(page.locator('#cal')).toBeVisible();
  await expect(page.locator('#cal-export')).toBeVisible();
  await expect(page.locator('#cal-board')).toBeHidden();
  // keyboard: the file buttons sit between Reset and Save
  await page.focus('#cal-reset');
  await page.keyboard.press('ArrowRight');
  await expect(page.locator('#cal-load')).toBeFocused();
  await page.keyboard.press('Tab');
  await expect(page.locator('#cal-export')).toBeFocused();
  await page.keyboard.press('Tab');
  await expect(page.locator('#cal-save')).toBeFocused();
  await page.keyboard.press('Escape');

  await putFile(page, 'settings/print.json', readFileSync(PRINT_JSON, 'utf8'));
  await page.keyboard.press('Shift+C');
  await expect(page.locator('#cal')).toBeVisible();
  await expect(page.locator('#cal-board')).toHaveText('Load from board (settings/print.json)');
  await page.click('#cal-board');
  await expectFields(page);
  await expect(page.locator('#cal-loaded')).toHaveText('loaded from settings/print.json — Save to keep it');
  await page.keyboard.press('Escape');

  await fresh(page, { printer: P21 });
  await page.keyboard.press('Shift+C');
  await expect(page.locator('#cal')).toBeVisible();
  await page.setInputFiles('#cal-file', PRINT_JSON);
  await expect(page.locator('#cal-loaded')).toContainText('a pm220 calibration for its 50 × 30 mm label; this is a P21');
  await expect(page.locator('#cal-offx')).toHaveValue('-0.75');
});
