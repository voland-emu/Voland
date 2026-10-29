/**
 * Unit tests for bindings/layout.ts - the JS-side mirror of
 * core/common/layout.h (§4). Pure parsing logic, no browser globals
 * needed, so it runs directly under Node's built-in test runner.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { MEMORY_LAYOUT_STRUCT_SIZE, readMemoryLayout, toWasm32Address } from "../../bindings/layout.ts";

function memoryWithLayoutFields(values: readonly bigint[]): WebAssembly.Memory {
  const memory = new WebAssembly.Memory({ initial: 1, maximum: 1 });
  const view = new DataView(memory.buffer, 0, MEMORY_LAYOUT_STRUCT_SIZE);
  values.forEach((value, i) => view.setBigUint64(i * 8, value, true));
  return memory;
}

test("readMemoryLayout parses all 10 fields in declared order", () => {
  const values = Array.from({ length: 10 }, (_, i) => BigInt(i + 1));
  const memory = memoryWithLayoutFields(values);

  const layout = readMemoryLayout(memory, 0);

  assert.equal(layout.guestRamBase, 1);
  assert.equal(layout.guestRamSize, 2);
  assert.equal(layout.pageTableL1Base, 3);
  assert.equal(layout.framebufferSlotBase, 4);
  assert.equal(layout.audioRingBase, 5);
  assert.equal(layout.gpuRingBase, 6);
  assert.equal(layout.gpuCompletionRingBase, 7);
  assert.equal(layout.inputRegionBase, 8);
  assert.equal(layout.traceBufferBase, 9);
  assert.equal(layout.breakpointRegionBase, 10);
});

test("readMemoryLayout honors a non-zero base pointer", () => {
  const values = Array.from({ length: 10 }, (_, i) => BigInt(100 + i));
  const memory = new WebAssembly.Memory({ initial: 1, maximum: 1 });
  const offset = 256;
  const view = new DataView(memory.buffer, offset, MEMORY_LAYOUT_STRUCT_SIZE);
  values.forEach((value, i) => view.setBigUint64(i * 8, value, true));

  const layout = readMemoryLayout(memory, offset);

  assert.equal(layout.guestRamBase, 100);
  assert.equal(layout.breakpointRegionBase, 109);
});

test("readMemoryLayout yields Numbers, not bigints (wasm32, §4)", () => {
  const values = Array.from({ length: 10 }, (_, i) => BigInt(0x1000 * (i + 1)));
  const layout = readMemoryLayout(memoryWithLayoutFields(values), 0);
  Object.values(layout).forEach(value => assert.equal(typeof value, "number"));
});

test("readMemoryLayout rejects a field that is not a wasm32 address", () => {
  const values = Array.from({ length: 10 }, () => 0n);
  values[3] = 2n ** 32n;
  assert.throws(() => readMemoryLayout(memoryWithLayoutFields(values), 0), RangeError);
});

test("toWasm32Address narrows every value below 2^32", () => {
  assert.equal(toWasm32Address(0n), 0);
  assert.equal(toWasm32Address(42n), 42);
  assert.equal(toWasm32Address(2n ** 31n), 2 ** 31); // above 2 GiB stays positive
  assert.equal(toWasm32Address(2n ** 32n - 1n), 2 ** 32 - 1);
});

test("toWasm32Address rejects 2^32 and beyond", () => {
  assert.throws(() => toWasm32Address(2n ** 32n), RangeError);
  assert.throws(() => toWasm32Address(BigInt(Number.MAX_SAFE_INTEGER) + 1n), RangeError);
  assert.throws(() => toWasm32Address(-1n), RangeError);
});
