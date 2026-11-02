const { test, expect } = require('@playwright/test');

test('switching preserves authored messages, names, drafts, attachments and message actions', async ({ page }) => {
  const errors = [], writes = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.goto('/');
  await expect(page.locator('#station')).toHaveText('Station A');
  await expect(page.locator('.bubble')).toHaveText('Připraveno');
  await expect(page.locator('.author')).toContainText('Zrušit');
  await page.locator('#author').fill('My unchanged name');
  await page.locator('#text').fill('Do not translate this: Zrušit');
  await page.locator('#file').setInputFiles({ name: 'Připraveno.txt', mimeType: 'text/plain', buffer: Buffer.from('example message') });
  await expect(page.locator('#attachmentName')).toHaveText('Připraveno.txt');
  page.on('request', request => { if (request.method() === 'POST') writes.push(request.url()); });
  await page.locator('#ui-language').selectOption('cs');
  await expect(page.locator('#station')).toHaveText('Stanice A');
  await expect(page.locator('.meta .cancel')).toHaveText('Zrušit');
  await page.locator('#ui-language').selectOption('en');
  await expect(page.locator('.meta .cancel')).toHaveText('Cancel');
  await expect(page.locator('#author')).toHaveValue('My unchanged name');
  await expect(page.locator('#text')).toHaveValue('Do not translate this: Zrušit');
  await expect(page.locator('#attachmentName')).toHaveText('Připraveno.txt');
  await expect(page.locator('.bubble')).toHaveText('Připraveno');
  await expect(page.locator('.author')).toHaveText('Zrušit · you');
  expect(writes).toEqual([]);
  expect(errors).toEqual([]);
});

test('language and author preferences survive a reload independently', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('#station')).toHaveText('Station A');
  await page.locator('#author').fill('Example author');
  await page.locator('#ui-language').selectOption('cs');
  await page.reload();
  await expect(page.locator('html')).toHaveAttribute('lang', 'cs');
  await expect(page.locator('#author')).toHaveValue('Example author');
  await expect(page.locator('.meta .cancel')).toHaveText('Zrušit');
});

test('Czech browser preference works without storage access', async ({ browser }) => {
  const context = await browser.newContext({ locale: 'cs-CZ' });
  const page = await context.newPage();
  await page.addInitScript(() => {
    Object.defineProperty(Storage.prototype, 'getItem', { value() { throw new Error('disabled'); } });
    Object.defineProperty(Storage.prototype, 'setItem', { value() { throw new Error('disabled'); } });
  });
  await page.goto('http://127.0.0.1:18740');
  await expect(page.locator('#station')).toHaveText('Stanice A');
  await page.locator('#ui-language').selectOption('en');
  await expect(page.locator('#station')).toHaveText('Station A');
  await context.close();
});

test('a language change preserves offline status and disabled radio controls', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('#station')).toHaveText('Station A');
  await page.route('**/api/state', route => route.abort());
  await expect(page.locator('#connection')).toHaveText('Wi-Fi disconnected');
  await page.locator('#ui-language').selectOption('cs');
  await expect(page.locator('#connection')).toHaveText('Wi-Fi odpojena');
  await expect(page.locator('#send')).toBeDisabled();
  await expect(page.locator('#applyProfile')).toBeDisabled();
  await page.locator('#ui-language').selectOption('en');
  await expect(page.locator('#connection')).toHaveText('Wi-Fi disconnected');
});

test('mobile language selection stays within the viewport', async ({ page }) => {
  await page.setViewportSize({ width: 360, height: 800 });
  await page.goto('/');
  const selector = page.locator('#ui-language');
  await expect(selector).toBeVisible();
  const bounds = await selector.boundingBox();
  expect(bounds.x).toBeGreaterThanOrEqual(0);
  expect(bounds.x + bounds.width).toBeLessThanOrEqual(360);
  await selector.selectOption('cs');
  await expect(page.locator('html')).toHaveAttribute('lang', 'cs');
});

test('an unsubmitted radio profile survives switching and confirmation stays explicit', async ({ page }) => {
  const writes = [];
  await page.goto('/');
  await expect(page.locator('#station')).toHaveText('Station A');
  await page.locator('#openLab').click();
  await page.locator('#profileSetup summary').click();
  await page.locator('#radioProfile').selectOption('6');
  await expect(page.locator('#applyProfile')).toBeEnabled();
  await page.locator('#closeLab').click();
  page.on('request', request => { if (request.method() === 'POST') writes.push(request.url()); });
  await page.locator('#ui-language').selectOption('cs');
  await page.locator('#openLab').click();
  await expect(page.locator('#radioProfile')).toHaveValue('6');
  await page.locator('#closeLab').click();
  await page.locator('#ui-language').selectOption('en');
  await page.locator('#openLab').click();
  await expect(page.locator('#radioProfile')).toHaveValue('6');
  page.once('dialog', async dialog => {
    expect(dialog.message()).toContain('Switch BOTH boards');
    expect(dialog.message()).toContain('GFSK 150');
    await dialog.dismiss();
  });
  await page.locator('#applyProfile').click();
  expect(writes).toEqual([]);
});

test('server errors follow the selected language', async ({ page }) => {
  await page.goto('/');
  await expect(page.locator('.meta .cancel')).toHaveText('Cancel');
  await page.locator('.meta .cancel').click();
  await expect(page.locator('#error')).toHaveText('Unknown request.');
  await page.locator('#ui-language').selectOption('cs');
  await expect(page.locator('#error')).toHaveText('Neznámý požadavek.');
});
