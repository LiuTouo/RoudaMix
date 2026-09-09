import { defineConfig, devices } from '@playwright/test';
export default defineConfig({
  testDir: './tests/browser',
  fullyParallel: true,
  workers: process.env.CI ? 2 : 4,
  use: { baseURL: 'http://127.0.0.1:4387/RoudaMix/', trace: 'retain-on-failure' },
  webServer: { command: 'npm run dev -- --port 4387 --strictPort', url: 'http://127.0.0.1:4387/RoudaMix/', reuseExistingServer: !process.env.CI },
  projects: [
    { name: 'desktop', use: { ...devices['Desktop Chrome'], viewport: { width: 1440, height: 1000 } } },
    { name: 'mobile', use: { ...devices['iPhone 13'], defaultBrowserType: 'chromium' } },
  ],
});
