// A saved Bluetooth Classic printer is never paged without the user (paging
// one that is off stalls the gateway's Wi-Fi, parent B4): "Turn on the
// printer" first. BLE printers, printers just added from a scan and printers
// a scan in this page found connect directly.
import { test, expect } from '@playwright/test';
import { fresh, op, waitJobs, jobs, PL70E, PM220 } from './helpers.js';

const connects = async (page) => (await op(page, 'test.connects')).connects;

async function addText(page, text) {
  await page.click('#add-text');
  await page.fill('#text', text);
  await page.keyboard.press('Escape');
}

test('a saved Classic printer is not paged at boot: the prompt asks, Cancel leaves Connect, Connect connects once', async ({ page }) => {
  await fresh(page, { printer: PM220, connect: false });
  await expect(page.locator('#turn-on-title')).toHaveText('Turn on the printer');
  await expect(page.locator('#turn-on-text')).toContainText('Switch on Desk and keep it nearby, then press Connect.');
  await expect(page.locator('#turn-on-text')).toContainText('Wi-Fi pauses');
  await expect(page.locator('#turn-on-ok')).toBeFocused();
  expect(await connects(page)).toEqual([]);
  await page.keyboard.press('Escape');
  await expect(page.locator('#turn-on')).toBeHidden();
  await expect(page.locator('#status-connect')).toBeVisible();
  await page.waitForTimeout(300);
  expect(await connects(page)).toEqual([]);
  // the status line's Connect asks again; Tab reaches Cancel and back, Enter on Connect connects
  await page.click('#status-connect');
  await expect(page.locator('#turn-on-ok')).toBeFocused();
  await page.keyboard.press('Tab');
  await expect(page.locator('#turn-on-cancel')).toBeFocused();
  await page.keyboard.press('Tab');
  await expect(page.locator('#turn-on-ok')).toBeFocused();
  await page.keyboard.press('Enter');
  await expect(page.locator('#status')).toContainText('Desk: connected');
  await expect(page.locator('#status-connect')).toBeHidden();
  expect(await connects(page)).toEqual([{ driver: 'nelko_tspl', addr: PM220.addr }]);
});

test('a BLE printer (PL70e) still connects at boot with no prompt', async ({ page }) => {
  await fresh(page, { printer: PL70E });
  await expect(page.locator('#turn-on')).toBeHidden();
  expect(await connects(page)).toEqual([{ driver: 'nelko_pl70e', addr: PL70E.addr }]);
});

test('Print with a disconnected Classic printer asks, then prints after Connect; Cancel prints nothing', async ({ page }) => {
  await fresh(page, { printer: PM220, connect: false });
  await page.keyboard.press('Escape');
  await addText(page, 'hello');
  await page.click('#print');
  await expect(page.locator('#turn-on')).toBeVisible();
  await page.click('#turn-on-cancel');
  await expect(page.locator('#status')).toContainText('print cancelled: Desk is not connected');
  expect(await jobs(page)).toEqual([]);
  expect(await connects(page)).toEqual([]);
  await page.click('#print');
  await page.click('#turn-on-ok');
  const [j] = await waitJobs(page, 1);
  expect(j.params).toMatchObject({ driver: 'nelko_tspl', addr: PM220.addr, model: 'pm220' });
  expect(await connects(page)).toHaveLength(1);
});

test('a PM220 picked from a scan and saved connects without the prompt', async ({ page }) => {
  await fresh(page);
  await page.click('#printer-btn');
  await page.click('#printers-scan');
  await page.locator('#printers-found button', { hasText: PM220.addr }).click();
  await page.fill('#pe-nick', 'Desk');
  await page.click('#pe-save');
  await expect(page.locator('#printers-list')).toContainText('connected');
  await expect(page.locator('#turn-on')).toBeHidden();
  expect(await connects(page)).toEqual([{ driver: 'nelko_tspl', addr: PM220.addr }]);
});

test('a saved PM220 a scan found connects from the dialog without the prompt, until it is lost again', async ({ page }) => {
  await fresh(page, { printer: PM220, connect: false });
  await page.keyboard.press('Escape');
  await page.click('#printer-btn');
  const row = page.locator('#printers-list li', { hasText: 'Desk' });
  // not seen yet: the dialog's Connect asks
  await row.getByRole('button', { name: 'Connect', exact: true }).click();
  await expect(page.locator('#turn-on')).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(page.locator('#turn-on')).toBeHidden();
  expect(await connects(page)).toEqual([]);
  // a scan finds it (and the PM220 that is named late, the scan's last report)
  await page.click('#printers-scan');
  await expect(page.locator('#printers-found li', { hasText: 'AA:BB:CC:00:10:10' })).toContainText('PM220');
  await row.getByRole('button', { name: 'Connect', exact: true }).click();
  await expect(row).toContainText('connected');
  await expect(page.locator('#turn-on')).toBeHidden();
  expect(await connects(page)).toHaveLength(1);
  // the driver says the link is lost: from now on it asks again
  await op(page, 'test.printer_state', { addr: PM220.addr, driver: 'nelko_tspl', model: 'pm220', state: 'disconnected', why: 'link lost' });
  await row.getByRole('button', { name: 'Connect', exact: true }).click();
  await expect(page.locator('#turn-on')).toBeVisible();
  await page.click('#turn-on-cancel');
  expect(await connects(page)).toHaveLength(1);
});
