import { defineConfig, devices } from "@playwright/test";

/**
 * Boot e2e (CLAUDE.md's `npm run e2e`): headless Chromium loads the built
 * app, asserts crossOriginIsolated is active, and asserts the full
 * ~5.25GiB shared memory allocation succeeds (§4/§16). Runs against a
 * production build under `vite preview`, which serves the same COOP/COEP/
 * CSP headers configured in vite.config.ts - the dev server would work
 * too, but a real build is what actually ships.
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
  ],
});
