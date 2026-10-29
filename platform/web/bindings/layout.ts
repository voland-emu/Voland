/**
 * Mirrors core/common/layout.h field-for-field. The core exports a single
 * pointer (`layout_get_ffi()`) into the shared WebAssembly.Memory; every
 * JS-side consumer reads the same ten uint64 fields back out of linear
 * memory rather than allocating its own shared buffers (§4).
 *
 * wasm32 (v3.29, §4): pointers are 32-bit and cross the JS boundary as
 * Numbers. The C struct keeps uint64_t fields (one layout for native and
 * web), so each field is read as a 64-bit value and narrowed here, once;
 * a value that doesn't fit a wasm32 address is a corrupted struct, not a
 * large address, and throws.
 */

export interface MemoryLayout {
  readonly guestRamBase: number;
  readonly guestRamSize: number;
  readonly pageTableL1Base: number;
  readonly framebufferSlotBase: number;
  readonly audioRingBase: number;
  readonly gpuRingBase: number;
  readonly gpuCompletionRingBase: number;
  readonly inputRegionBase: number;
  readonly traceBufferBase: number;
  readonly breakpointRegionBase: number;
}

/* Field order and width must match Memory_Layout in core/common/layout.h. */
const FIELD_COUNT = 10;
const FIELD_BYTES = 8;
export const MEMORY_LAYOUT_STRUCT_SIZE = FIELD_COUNT * FIELD_BYTES;

/* Every wasm32 linear-memory address and size is below 2^32. */
const WASM32_ADDRESS_LIMIT = 2n ** 32n;

/** Narrow a 64-bit layout field to a wasm32 address (or size) Number. */
export function toWasm32Address(value: bigint): number {
  if (value < 0n || value >= WASM32_ADDRESS_LIMIT) {
    throw new RangeError(`layout value ${value} is not a wasm32 address`);
  }
  return Number(value);
}

/**
 * Reads a MemoryLayout struct out of `memory` at `layoutPtr` (the value
 * returned by the core's `layout_get_ffi()` export).
 */
export function readMemoryLayout(memory: WebAssembly.Memory, layoutPtr: number): MemoryLayout {
  const view = new DataView(memory.buffer, layoutPtr, MEMORY_LAYOUT_STRUCT_SIZE);
  const little = true;
  const field = (index: number): number => toWasm32Address(view.getBigUint64(index * FIELD_BYTES, little));
  return {
    guestRamBase:          field(0),
    guestRamSize:          field(1),
    pageTableL1Base:       field(2),
    framebufferSlotBase:   field(3),
    audioRingBase:         field(4),
    gpuRingBase:           field(5),
    gpuCompletionRingBase: field(6),
    inputRegionBase:       field(7),
    traceBufferBase:       field(8),
    breakpointRegionBase:  field(9),
  };
}

export function guestRamView(memory: WebAssembly.Memory, layout: MemoryLayout): Uint8Array {
  return new Uint8Array(memory.buffer, layout.guestRamBase, layout.guestRamSize);
}
