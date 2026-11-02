/**
 * Unit tests for bindings/input.ts - the JS-side half of
 * core/common/input_region.h's wire format (§18). Pure byte-packing
 * logic, no DOM/Gamepad API needed, so it runs under Node's test runner
 * the same way tests/unit/layout.test.ts does.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import {
  INPUT_REGION_SLOT_BYTES,
  INPUT_TOUCH_BLOCK_BYTES,
  InputFlags,
  NEUTRAL_CONTROLLER_STATE,
  readControllerStateFfi,
  writeControllerSlot,
  writeTouchBlock,
  type ControllerState,
} from "../../bindings/input.ts";

function sharedMemory(pages = 1): WebAssembly.Memory {
  return new WebAssembly.Memory({ initial: pages, maximum: pages, shared: true });
}

test("writeControllerSlot packs every field at its documented offset (§18)", () => {
  const memory = sharedMemory();
  const state: ControllerState = {
    buttons: 0x12345678,
    axes: [1, -1, 100, -100, 32767, -32767, 0, 7],
    flags: InputFlags.Connected,
    motionSample: 0,
    accel: [1.5, -2.5, 0],
    gyro: [0, 3.25, -3.25],
  };

  writeControllerSlot(memory, 0, 2, state);

  const view = new DataView(memory.buffer, 2 * INPUT_REGION_SLOT_BYTES, INPUT_REGION_SLOT_BYTES);
  assert.equal(view.getUint32(0, true) % 2, 0); // seqlock left even after a complete write
  assert.equal(view.getUint32(4, true), state.buttons);
  for (let i = 0; i < 8; i++) assert.equal(view.getInt16(8 + i * 2, true), state.axes[i]);
  assert.equal(view.getUint32(24, true), state.flags);
  assert.equal(view.getUint32(28, true), state.motionSample);
  for (let i = 0; i < 3; i++) assert.equal(view.getFloat32(32 + i * 4, true), state.accel[i]);
  for (let i = 0; i < 3; i++) assert.equal(view.getFloat32(44 + i * 4, true), state.gyro[i]);
});

test("writeControllerSlot rejects an out-of-range slot", () => {
  const memory = sharedMemory();
  assert.throws(() => writeControllerSlot(memory, 0, 8, NEUTRAL_CONTROLLER_STATE), RangeError);
  assert.throws(() => writeControllerSlot(memory, 0, -1, NEUTRAL_CONTROLLER_STATE), RangeError);
});

test("writeControllerSlot does not touch neighboring slots", () => {
  const memory = sharedMemory();
  writeControllerSlot(memory, 0, 0, { ...NEUTRAL_CONTROLLER_STATE, buttons: 0xaaaaaaaa });
  writeControllerSlot(memory, 0, 1, { ...NEUTRAL_CONTROLLER_STATE, buttons: 0xbbbbbbbb });

  const view0 = new DataView(memory.buffer, 0, INPUT_REGION_SLOT_BYTES);
  const view1 = new DataView(memory.buffer, INPUT_REGION_SLOT_BYTES, INPUT_REGION_SLOT_BYTES);
  assert.equal(view0.getUint32(4, true), 0xaaaaaaaa);
  assert.equal(view1.getUint32(4, true), 0xbbbbbbbb);
});

test("writeTouchBlock packs point count and points right after the 8 controller slots", () => {
  const memory = sharedMemory();
  const touchBlockOffset = 8 * INPUT_REGION_SLOT_BYTES;

  writeTouchBlock(memory, 0, [
    { fingerId: 3, x: 10.5, y: 20.25 },
    { fingerId: 9, x: 1279, y: 719 },
  ]);

  const view = new DataView(memory.buffer, touchBlockOffset, INPUT_TOUCH_BLOCK_BYTES);
  assert.equal(view.getUint32(0, true) % 2, 0);
  assert.equal(view.getUint32(4, true), 2);
  assert.equal(view.getUint32(8, true), 3);
  assert.equal(view.getFloat32(12, true), 10.5);
  assert.equal(view.getFloat32(16, true), 20.25);
  assert.equal(view.getUint32(20, true), 9);
  assert.equal(view.getFloat32(24, true), 1279);
  assert.equal(view.getFloat32(28, true), 719);
  // Unused point slots are zeroed, not left stale.
  assert.equal(view.getUint32(20 + 12, true), 0);
});

test("writeTouchBlock rejects more than 10 points", () => {
  const memory = sharedMemory();
  const points = Array.from({ length: 11 }, (_, i) => ({ fingerId: i, x: 0, y: 0 }));
  assert.throws(() => writeTouchBlock(memory, 0, points), RangeError);
});

test("readControllerStateFfi decodes wasm_entry.c's struct layout (no sequence word, offsets 4 bytes earlier)", () => {
  const memory = sharedMemory();
  const ptr = 100;
  const view = new DataView(memory.buffer, ptr, 52);
  view.setUint32(0, 0x1000, true);
  for (let i = 0; i < 8; i++) view.setInt16(4 + i * 2, i - 4, true);
  view.setUint32(20, InputFlags.Connected, true);
  view.setUint32(24, 7, true);
  for (let i = 0; i < 3; i++) view.setFloat32(28 + i * 4, i + 0.5, true);
  for (let i = 0; i < 3; i++) view.setFloat32(40 + i * 4, -(i + 0.5), true);

  const state = readControllerStateFfi(memory, ptr);

  assert.equal(state.buttons, 0x1000);
  assert.deepEqual(state.axes, [-4, -3, -2, -1, 0, 1, 2, 3]);
  assert.equal(state.flags, InputFlags.Connected);
  assert.equal(state.motionSample, 7);
  assert.deepEqual(state.accel, [0.5, 1.5, 2.5]);
  assert.deepEqual(state.gyro, [-0.5, -1.5, -2.5]);
});
