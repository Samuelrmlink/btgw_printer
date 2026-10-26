import { test, expect } from '@playwright/test';
import { fresh, waitJobs, PL70E } from './helpers.js';

async function addText(page, text) {
  await page.click('#add-text');
  await page.fill('#text', text);
  await page.keyboard.press('Escape');
}

/** Decode a job's bitmap and count black dots (black_bit 0 for the PL70e). */
function blackDots(job) {
  const b = Buffer.from(job.b64, 'base64');
  let n = 0;
  for (const x of b) for (let i = 0; i < 8; i++) if (!(x & (0x80 >> i))) n++;
  return n;
}

test('a template prompts for its variables, remembers them, and prints copies as one job', async ({ page }) => {
  await fresh(page, { printer: PL70E });
  await addText(page, 'Ship to {{name}}');
  await page.keyboard.press('Control+p');
  await expect(page.locator('#prompt')).toBeVisible();
  const field = page.locator('#prompt-fields input').first();
  await expect(field).toBeFocused();
  await field.fill('Alice');
  await expect(page.locator('#prompt-preview')).toContainText('Ship to Alice');
  await page.locator('#prompt-copies').fill('3');
  await page.keyboard.press('Control+Enter');
  const js = await waitJobs(page, 1);
  expect(js[0].params.copies).toBe(3);
  // remembered next time
  await page.keyboard.press('Control+p');
  await expect(page.locator('#prompt-fields input').first()).toHaveValue('Alice');
  await page.keyboard.press('Escape');
  await expect(page.locator('#prompt')).toBeHidden();
});

test('a counter makes one job per label and carries on from where it stopped', async ({ page }) => {
  await fresh(page, { printer: PL70E });
  await addText(page, 'No. {{serial}}');
  // make serial a counter from 7, 3 digits
  await page.keyboard.press('Shift+V');
  await expect(page.locator('#vars')).toBeVisible();
  await page.selectOption('#v0-type', 'counter');
  await page.fill('#v0-start', '7');
  await page.locator('#v0-start').dispatchEvent('change');
  await page.fill('#v0-pad', '3');
  await page.locator('#v0-pad').dispatchEvent('change');
  await page.keyboard.press('Escape');
  await page.keyboard.press('Control+p');
  await expect(page.locator('#prompt-fields input').first()).toHaveValue('7');
  await expect(page.locator('#prompt-preview')).toContainText('No. 007');
  await page.locator('#prompt-copies').fill('2');
  await page.click('#prompt-print');
  const js = await waitJobs(page, 2);
  expect(js.map((j) => j.params.copies)).toEqual([1, 1]);
  expect(js[0].sha256).not.toBe(js[1].sha256);
  await page.keyboard.press('Control+p');
  await expect(page.locator('#prompt-fields input').first()).toHaveValue('9');
  await expect(page.locator('#prompt-preview')).toContainText('No. 009');
});

test('batch: CSV rows fill the variables, unticked rows are skipped, copies per row', async ({ page }) => {
  await fresh(page, { printer: PL70E });
  await addText(page, '{{sku}} · {{desc}}');
  await page.keyboard.press('Control+b');
  await expect(page.locator('#batch')).toBeVisible();
  await page.fill('#batch-text', 'sku,desc\nA1,Apples\nB2,"Bananas, ripe"\nC3,Cherries\n');
  await expect(page.locator('#batch-table tr[data-row]')).toHaveCount(3);
  await expect(page.locator('#batch-map')).toContainText('{{sku}}, {{desc}}');
  // untick row 2 from the keyboard: focus its checkbox, x toggles
  await page.locator('#batch-table tr[data-row="1"] input[type=checkbox]').focus();
  await page.keyboard.press('x');
  await expect(page.locator('#batch-table tr[data-row="1"] input[type=checkbox]')).not.toBeChecked();
  await page.locator('#batch-table tr[data-row="2"] .batch-copies').fill('2');
  await page.click('#batch-print');
  const js = await waitJobs(page, 2);
  expect(js.map((j) => j.params.copies)).toEqual([1, 2]);
  expect(js[0].sha256).not.toBe(js[1].sha256);
  expect(blackDots(js[0])).toBeGreaterThan(50);
});

test('batch preview follows the selected row', async ({ page }) => {
  await fresh(page, { printer: PL70E });
  await addText(page, '{{word}}');
  await page.click('#batch-btn');
  await page.uncheck('#batch-header');
  await page.fill('#batch-text', 'short\nWWWWWWWWWWWWWW');
  await expect(page.locator('#batch-map')).toContainText('one column');
  const ink = () => page.evaluate(() => {
    const c = document.getElementById('batch-preview');
    const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
    let n = 0;
    for (let i = 0; i < d.length; i += 4) if (d[i] === 0) n++;
    return n;
  });
  const a = await ink();
  await page.locator('#batch-table tr[data-row="1"] input[type=checkbox]').focus();
  const b = await ink();
  expect(a).toBeGreaterThan(0);
  expect(b).not.toBe(a);
});
