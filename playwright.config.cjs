const { defineConfig } = require('@playwright/test');
module.exports = defineConfig({
  testDir: './tests/browser', workers: 1,
  use: { baseURL: 'http://127.0.0.1:18740', locale: 'en-US', headless: true },
  webServer: { command: 'node tests/web-server.cjs', url: 'http://127.0.0.1:18740', reuseExistingServer: false },
});
