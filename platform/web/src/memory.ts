/**
 * The one shared WebAssembly.Memory (docs/DESIGN.md §4, §16). Kept free of
 * DOM imports so the unit tests and the Playwright spec can share its
 * numbers with the boot path.
 *
 * wasm32 since v3.29: 65535 pages, 4 GiB minus one 64 KiB page. Not 65536:
 * a region ending exactly at 2^32 has an end pointer that wraps to 0 in a
 * 32-bit size_t (§4). Fixed at boot - `initial === maximum`, growth
 * disabled, so views never go stale. This MUST equal CMakeLists.txt's
 * -sINITIAL_MEMORY and core/common/layout.h's LAYOUT_LINEAR_MEMORY_PAGES
 * (tests/unit/memory.test.ts checks both), or the core module fails to
 * instantiate.
 */

export const WASM_PAGE_BYTES = 65_536;
export const TOTAL_MEMORY_PAGES = 65_535;
export const TOTAL_MEMORY_BYTES = TOTAL_MEMORY_PAGES * WASM_PAGE_BYTES; // 4_294_901_760

/**
 * Creates the shared memory, or throws what the engine threw. A plain
 * wasm32 descriptor: Number page counts and no `address` key. memory64
 * needed `address: "i64"` with BigInt counts; §4 keeps that for Switch 2.
 */
export function createSharedMemory(): WebAssembly.Memory {
  return new WebAssembly.Memory({
    initial: TOTAL_MEMORY_PAGES,
    maximum: TOTAL_MEMORY_PAGES,
    shared:  true,
  });
}
