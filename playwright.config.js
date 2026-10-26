// End-to-end tests against dev/mockserver.py (a mock board: printer_hub ops,
// LittleFS + SD over HTTP, the parent's Wi-Fi routes). One worker: the mock is
// one board; every test starts with the mock-only test.reset op.
import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: 'tests/e2e',
  fullyParallel: false,
  workers: 1,
  timeout: 30000,
  use: { baseURL: 'http://127.0.0.1:8097', viewport: { width: 1400, height: 860 } },
  webServer: {
    command: 'python3 dev/mockserver.py --port 8097 --mode companion',
    url: 'http://127.0.0.1:8097/printer/',
    reuseExistingServer: false,
  },
});
