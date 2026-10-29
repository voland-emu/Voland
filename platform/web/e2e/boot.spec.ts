import { expect, test, type Page } from "@playwright/test";

import type { VolandE2EBootMilestone } from "../src/e2e-hooks";
import { TOTAL_MEMORY_BYTES, TOTAL_MEMORY_PAGES } from "../src/memory";

declare global {
  interface Window {
    __VOLAND_E2E__?: VolandE2EBootMilestone;
  }
}

/* core/common/layout.h LAYOUT_GUEST_RAM_SIZE: application pool (3285 MiB)
 * plus the HLE carve-out (48 MiB), §4. */
const EXPECTED_GUEST_RAM_BYTES = 3333 * 1024 * 1024;

const WASM_MAGIC = Buffer.from([0x00, 0x61, 0x73, 0x6d]); // "\0asm"

async function gotoIsolated(page: Page): Promise<void> {
  await page.goto("/");
  // The first visit may reload once so the Service Worker controls the
  // page (§16); wait for the page that stays.
  await page.waitForFunction(() => crossOriginIsolated, undefined, { timeout: 15_000 });
}

/*
 * Runs on every engine. Asserts the engine itself can allocate the exact
 * memory the boot sequence asks for, on a page served with Voland's real
 * COOP/COEP headers. Independent of WebGPU, which not every Playwright
 * engine build exposes, so the allocation is asserted everywhere even
 * where the boot below stops at the WebGPU gate (§16 boot order).
 */
test("the engine allocates the shared 65535-page wasm32 memory under cross-origin isolation", async ({ page }) => {
  await gotoIsolated(page);

  const allocated = await page.evaluate((pages) => {
    const memory = new WebAssembly.Memory({ initial: pages, maximum: pages, shared: true });
    return {
      byteLength: memory.buffer.byteLength,
      shared: memory.buffer instanceof SharedArrayBuffer,
    };
  }, TOTAL_MEMORY_PAGES);

  expect(allocated.byteLength).toBe(TOTAL_MEMORY_BYTES);
  expect(allocated.shared).toBe(true);
});

/*
 * The real boot path. Allocation comes after the WebGPU and OPFS gates
 * (§16), so an engine build without navigator.gpu stops before it: that
 * case is skipped with its reason, not passed. Where it boots, the
 * allocation is the app's own, and once the compiled core is served the
 * layout handshake must report §4's guest RAM size.
 */
test("boots, cross-origin isolation is active, and the full guest memory allocation succeeds", async ({ page }) => {
  const pageErrors: string[] = [];
  page.on("pageerror", (err) => pageErrors.push(err.message));

  await gotoIsolated(page);

  const hasWebGPU = await page.evaluate(() => "gpu" in navigator);
  test.skip(!hasWebGPU, "this engine build exposes no navigator.gpu; boot stops at the WebGPU gate before allocating (§16)");

  await page.waitForFunction(() => window.__VOLAND_E2E__ !== undefined, undefined, { timeout: 15_000 });
  const milestone = await page.evaluate(() => window.__VOLAND_E2E__);

  expect(milestone).toBeDefined();
  expect(milestone?.crossOriginIsolated).toBe(true);
  expect(milestone?.allocatedMemoryBytes).toBe(TOTAL_MEMORY_BYTES);

  // With the core served (CI places it in public/core), the CPU worker
  // instantiates it into that memory and posts the layout back. Without it
  // the worker reports a load error and there is no handshake to assert.
  // Checked by magic number: vite preview's SPA fallback answers a
  // missing file with index.html and a 200.
  const wasmResponse = await page.request.get("/core/switch_core.wasm");
  const coreServed = wasmResponse.ok() && (await wasmResponse.body()).subarray(0, 4).equals(WASM_MAGIC);
  if (coreServed) {
    await page.waitForFunction(() => window.__VOLAND_E2E__?.guestRamBytes !== undefined, undefined, { timeout: 15_000 });
    const booted = await page.evaluate(() => window.__VOLAND_E2E__);
    expect(booted?.guestRamBytes).toBe(EXPECTED_GUEST_RAM_BYTES);
  }

  // The boot path up to and including memory allocation must not throw.
  expect(pageErrors).toEqual([]);
});
