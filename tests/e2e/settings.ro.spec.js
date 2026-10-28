// Flash that refuses writes (the bench board's LittleFS, parent bug B3): the
// settings move to the card and come back from there after a reload.
// Runs in the flash-readonly project (playwright.config.js).
import { test, expect } from '@playwright/test';
import { op, getJson } from './helpers.js';

async function start(page) {
  await page.goto('/printer/');
  await op(page, 'test.reset');
  await page.evaluate(() => localStorage.clear());
}

async function ready(page) {
  await page.goto('/printer/');
  await page.waitForFunction(() => window.btgwPrinter && window.btgwPrinter.state.boardReady);
}

test('flash read-only: a saved printer and its calibration land on the card and survive a reload', async ({ page }) => {
  await start(page);
  await ready(page);
  await page.click('#printer-btn');
  await page.click('#printers-scan');
  await page.locator('#printers-found button', { hasText: 'AA:BB:CC:00:00:00' }).click();
  await page.fill('#pe-nick', 'Desk');
  await page.click('#pe-save');
  await expect(page.locator('#printers-list')).toContainText('Desk');
  await expect(page.locator('#printers-live')).toContainText('saved in sd/printer/printers.json');
  expect(await getJson(page, 'printer/printers.json')).toBeNull();
  const saved = await getJson(page, 'sd/printer/printers.json');
  expect(saved.printers[0]).toMatchObject({ driver: 'nelko_tspl', model: 'pm220', nickname: 'Desk' });
  await page.click('#printers-close');
  await expect(page.locator('#status')).toContainText('connected', { timeout: 5000 });

  await page.keyboard.press('Shift+C');
  await expect(page.locator('#cal-for')).toContainText('(sd/printer/printers.json)');
  await expect(page.locator('#cal-offx')).toHaveValue('-1.25');
  await page.keyboard.press('ArrowUp');          // +0.25 mm
  await page.keyboard.press('Control+s');
  await expect(page.locator('#status')).toContainText('calibration saved to sd/printer/printers.json');
  expect((await getJson(page, 'sd/printer/printers.json')).printers[0].calibration['nelko-pm220-50x30'])
    .toMatchObject({ offset_x_mm: -1 });

  await ready(page);
  await expect(page.locator('#printer-name')).toContainText('Desk');
  await expect(page.locator('#status')).toContainText('Desk: connected');   // still up on the board: no prompt
  await page.keyboard.press('Shift+C');
  await expect(page.locator('#cal-offx')).toHaveValue('-1');
});

test('a printers.json on the card is read first, and an old P21 driver name loads as nelko_ble', async ({ page }) => {
  await start(page);
  await page.evaluate(async () => {
    const r = await fetch('/api/sd/files/printer/printers.json', { method: 'PUT', body: JSON.stringify({
      version: 1, current: 'AA:BB:CC:00:02:02',
      printers: [{ addr: 'AA:BB:CC:00:02:02', driver: 'nelko_p21', model: 'p21', nickname: 'Tiny', calibration: {} }],
    }) });
    if (!r.ok) throw new Error('PUT ' + r.status);
  });
  await ready(page);
  await expect(page.locator('#printer-name')).toContainText('Tiny');
  // a BLE printer: connected at load with no "Turn on" prompt
  await expect(page.locator('#status')).toContainText('connected', { timeout: 5000 });
  const st = await op(page, 'printer.status');
  expect(st.printers.find((p) => p.addr === 'AA:BB:CC:00:02:02')).toMatchObject({ driver: 'nelko_ble', model: 'p21' });
});
