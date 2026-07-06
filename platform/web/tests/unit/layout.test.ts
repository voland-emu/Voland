/**
 * Unit tests for bindings/layout.ts - the JS-side mirror of
 * core/common/layout.h (§4). Pure parsing logic, no browser globals
 * needed, so it runs directly under Node's built-in test runner.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { MEMORY_LAYOUT_STRUCT_SIZE, readMemoryLayout, toByteOffset } from "../../bindings/layout.ts";

function memoryWithLayoutFields(values: readonly bigint[]): WebAssembly.Memory {
  const memory = new WebAssembly.Memory({ initial: 1, maximum: 1 });
  const view = new DataView(memory.buffer, 0, MEMORY_LAYOUT_STRUCT_SIZE);
  values.forEach((value, i) => view.setBigUint64(i * 8, value, true));
  return memory;
}

test("readMemoryLayout parses all 10 fields in declared order", () => {
  const values = Array.from({ length: 10 }, (_, i) => BigInt(i + 1));
  const memory = memoryWithLayoutFields(values);

  const layout = readMemoryLayout(memory, 0n);

  assert.equal(layout.guestRamBase, 1n);
  assert.equal(layout.guestRamSize, 2n);
  assert.equal(layout.pageTableL1Base, 3n);
  assert.equal(layout.framebufferSlotBase, 4n);
  assert.equal(layout.audioRingBase, 5n);
  assert.equal(layout.gpuRingBase, 6n);
  assert.equal(layout.gpuCompletionRingBase, 7n);
  assert.equal(layout.inputRegionBase, 8n);
  assert.equal(layout.traceBufferBase, 9n);
  assert.equal(layout.breakpointRegionBase, 10n);
});

test("readMemoryLayout honors a non-zero base pointer", () => {
  const values = Array.from({ length: 10 }, (_, i) => BigInt(100 + i));
  const memory = new WebAssembly.Memory({ initial: 1, maximum: 1 });
  const offset = 256;
  const view = new DataView(memory.buffer, offset, MEMORY_LAYOUT_STRUCT_SIZE);
  values.forEach((value, i) => view.setBigUint64(i * 8, value, true));

  const layout = readMemoryLayout(memory, BigInt(offset));

  assert.equal(layout.guestRamBase, 100n);
  assert.equal(layout.breakpointRegionBase, 109n);
});

test("toByteOffset narrows an in-range bigint to a Number", () => {
  assert.equal(toByteOffset(0n), 0);
  assert.equal(toByteOffset(42n), 42);
  assert.equal(toByteOffset(BigInt(Number.MAX_SAFE_INTEGER)), Number.MAX_SAFE_INTEGER);
});

test("toByteOffset rejects addresses beyond Number.MAX_SAFE_INTEGER", () => {
  assert.throws(
    () => toByteOffset(BigInt(Number.MAX_SAFE_INTEGER) + 1n),
    RangeError,
  );
});
