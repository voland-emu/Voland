import { expect, test } from "@playwright/test";

import type { VolandE2EBootMilestone } from "../src/e2e-hooks";

declare global {
  interface Window {
    __VOLAND_E2E__?: VolandE2EBootMilestone;
  }
}

const EXPECTED_MEMORY_BYTES = 5_637_144_576; // ~5.25GiB, must match CMakeLists.txt INITIAL_MEMORY

test("boots, cross-origin isolation is active, and the full guest memory allocation succeeds", async ({ page }) => {
  const pageErrors: string[] = [];
  page.on("pageerror", (err) => pageErrors.push(err.message));

  await page.goto("/");

  await page.waitForFunction(() => window.__VOLAND_E2E__ !== undefined, { timeout: 15_000 });
  const milestone = await page.evaluate(() => window.__VOLAND_E2E__);

  expect(milestone).toBeDefined();
  expect(milestone?.crossOriginIsolated).toBe(true);
  expect(milestone?.allocatedMemoryBytes).toBe(EXPECTED_MEMORY_BYTES);

  // The boot path up to and including memory allocation must not throw -
  // it does not depend on the compiled core.wasm existing (§16), so this
  // is a meaningful assertion even before anyone has built the C core.
  expect(pageErrors).toEqual([]);
});
