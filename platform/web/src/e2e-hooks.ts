/**
 * Test-only hook for the Playwright boot e2e (e2e/boot.spec.ts, CLAUDE.md's
 * `npm run e2e`). Exposes exactly the two pre-wasm boot milestones the e2e
 * spec needs to assert - crossOriginIsolated and the full shared-memory
 * allocation succeeding - without needing the compiled core.wasm to exist.
 * Not a general debugging API: see docs/DESIGN.md §23 for that (Phase 6+,
 * window.__VOLAND_MEMORY__ / __VOLAND_TRACE_METADATA__).
 */

export interface VolandE2EBootMilestone {
  readonly crossOriginIsolated: boolean;
  readonly allocatedMemoryBytes: number;
}

declare global {
  interface Window {
    __VOLAND_E2E__?: VolandE2EBootMilestone;
  }
}

export function publishBootMilestone(memory: WebAssembly.Memory): void {
  window.__VOLAND_E2E__ = {
    crossOriginIsolated: typeof crossOriginIsolated === "boolean" ? crossOriginIsolated : false,
    allocatedMemoryBytes: memory.buffer.byteLength,
  };
}
