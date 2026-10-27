// End-to-end tests against dev/mockserver.py (a mock board: printer_hub ops,
// LittleFS + SD over HTTP, the parent's Wi-Fi routes). One worker: the mock is
// one board; every test starts with the mock-only test.reset op.
// The flash-readonly project runs *.ro.spec.js against a second mock whose
// LittleFS refuses writes (the bench board's parent bug B3). card-busy runs
// the main suite again against a mock that, like the board, answers 409
// busy past 2 static GETs in flight and holds each one 40 ms.
import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: 'tests/e2e',
  fullyParallel: false,
  workers: 1,
  timeout: 30000,
  use: { viewport: { width: 1400, height: 860 } },
  projects: [
    { name: 'mock', testIgnore: /\.ro\.spec\.js$/, use: { baseURL: 'http://127.0.0.1:8097' } },
    { name: 'flash-readonly', testMatch: /\.ro\.spec\.js$/, use: { baseURL: 'http://127.0.0.1:8098' } },
    { name: 'card-busy', testIgnore: /\.ro\.spec\.js$/, use: { baseURL: 'http://127.0.0.1:8099' } },
  ],
  webServer: [
    {
      command: 'python3 dev/mockserver.py --port 8097 --mode companion',
      url: 'http://127.0.0.1:8097/printer/',
      reuseExistingServer: false,
    },
    {
      command: 'python3 dev/mockserver.py --port 8098 --mode companion --flash-readonly',
      url: 'http://127.0.0.1:8098/printer/',
      reuseExistingServer: false,
    },
    {
      command: 'python3 dev/mockserver.py --port 8099 --mode companion --max-concurrent 2 --static-delay-ms 40',
      url: 'http://127.0.0.1:8099/printer/',
      reuseExistingServer: false,
    },
  ],
});
