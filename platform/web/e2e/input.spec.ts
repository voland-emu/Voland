import { expect, test, type Page } from "@playwright/test";

import type { ControllerState } from "../bindings/input";
import type { VolandE2EBootMilestone } from "../src/e2e-hooks";

declare global {
  interface Window {
    __VOLAND_E2E__?: VolandE2EBootMilestone;
  }
}

const WASM_MAGIC = Buffer.from([0x00, 0x61, 0x73, 0x6d]); // "\0asm"

async function gotoIsolated(page: Page): Promise<void> {
  await page.goto("/");
  await page.waitForFunction(() => crossOriginIsolated, undefined, { timeout: 15_000 });
}

/**
 * Cross-language round-trip for §18 Phase 1 (input region + seqlock
 * writer/reader): writes a synthetic controller state through the real
 * main-thread writer (bindings/input.ts's writeControllerSlot, running
 * in this page), then asks the CPU worker to read the same slot back
 * through the real C reader (core/common/input_region.c, via the
 * `_input_region_read_controller_ffi` debug export). No hid: service
 * exists yet to exercise this path any other way (Phase 4).
 *
 * Needs the compiled core served at /core/switch_core.{js,wasm} - a
 * manual build step, not yet wired into CI (see cpu.worker.ts's header
 * comment) - so it skips, like boot.spec.ts's full-boot test, when the
 * core isn't present.
 */
test("the main-thread writer and the CPU worker's C reader agree on the input region wire format", async ({ page }) => {
  const pageErrors: string[] = [];
  page.on("pageerror", (err) => pageErrors.push(err.message));

  await gotoIsolated(page);

  const hasWebGPU = await page.evaluate(() => "gpu" in navigator);
  test.skip(!hasWebGPU, "this engine build exposes no navigator.gpu; boot stops at the WebGPU gate before the core loads (§16)");

  const wasmResponse = await page.request.get("/core/switch_core.wasm");
  const coreServed = wasmResponse.ok() && (await wasmResponse.body()).subarray(0, 4).equals(WASM_MAGIC);
  test.skip(!coreServed, "compiled core not served at /core/switch_core.wasm - build it and copy it into public/core/ first (§24)");

  await page.waitForFunction(() => window.__VOLAND_E2E__?.inputRegionHandle !== undefined, undefined, { timeout: 15_000 });

  const fixture: ControllerState = {
    buttons: 0x1357,
    axes: [1, 2, 3, 4, 5, 6, 7, 8],
    flags: 1, // InputFlags.Connected
    motionSample: 0,
    accel: [1.5, -2.5, 0],
    gyro: [0, 3.25, -3.25],
  };
  const slot = 3;

  const roundTripped = await page.evaluate(async ({ slot, fixture }) => {
    const handle = window.__VOLAND_E2E__?.inputRegionHandle;
    if (!handle) throw new Error("inputRegionHandle missing after waitForFunction resolved");

    handle.writeController(slot, fixture);

    return await new Promise((resolve) => {
      function onMessage(event: MessageEvent<{ type: string; slot: number; state: unknown }>): void {
        if (event.data?.type === "debugInputController" && event.data.slot === slot) {
          handle.cpuWorker.removeEventListener("message", onMessage);
          resolve(event.data.state);
        }
      }
      handle.cpuWorker.addEventListener("message", onMessage);
      handle.cpuWorker.postMessage({ type: "debugReadInputController", slot });
    });
  }, { slot, fixture });

  expect(roundTripped).toEqual(fixture);
  expect(pageErrors).toEqual([]);
});
