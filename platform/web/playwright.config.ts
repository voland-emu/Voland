import { defineConfig, devices } from "@playwright/test";

/**
 * Boot e2e (CLAUDE.md's `npm run e2e`): each engine loads the built app,
 * asserts crossOriginIsolated is active, and asserts the full 65535-page
 * (~4GiB) shared wasm32 memory allocation succeeds (§4/§16). Chromium,
 * Firefox and WebKit, because Safari is why the core is wasm32 (§16
 * "Browser requirements"). Runs against a production build under
 * `vite preview`, which serves the same COOP/COEP/CSP headers configured
 * in vite.config.ts - the dev server would work too, but a real build is
 * what actually ships.
 */
export default defineConfig({
  testDir: "./e2e",
  timeout: 30_000,
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 2 : 0,
  reporter: "list",
  use: {
    baseURL: "http://localhost:5174",
    trace: "retain-on-failure",
  },
  webServer: {
    command: "npm run build && npm run preview",
    url: "http://localhost:5174",
    reuseExistingServer: !process.env.CI,
    timeout: 60_000,
  },
  projects: [
    { name: "chromium", use: { ...devices["Desktop Chrome"] } },
    { name: "firefox",  use: { ...devices["Desktop Firefox"] } },
    { name: "webkit",   use: { ...devices["Desktop Safari"] } },
  ],
});
