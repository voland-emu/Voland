/**
 * Mirrors core/common/layout.h field-for-field. The core exports a single
 * pointer (`layout_get_ffi()`) into the shared WebAssembly.Memory; every
 * JS-side consumer reads the same ten uint64 fields back out of linear
 * memory rather than allocating its own shared buffers (§4).
 *
 * MEMORY64 makes every C `uint64_t` field and the pointer itself 64-bit;
 * addresses are read as bigint throughout and only narrowed to Number at
 * the point a typed-array view is constructed (values here top out in the
 * low gigabytes, well inside Number.MAX_SAFE_INTEGER).
 */

export interface MemoryLayout {
  readonly guestRamBase: bigint;
  readonly guestRamSize: bigint;
  readonly pageTableL1Base: bigint;
  readonly framebufferSlotBase: bigint;
  readonly audioRingBase: bigint;
  readonly gpuRingBase: bigint;
  readonly gpuCompletionRingBase: bigint;
  readonly inputRegionBase: bigint;
  readonly traceBufferBase: bigint;
  readonly breakpointRegionBase: bigint;
}

/* Field order and width must match Memory_Layout in core/common/layout.h. */
const FIELD_COUNT = 10;
const FIELD_BYTES = 8;
export const MEMORY_LAYOUT_STRUCT_SIZE = FIELD_COUNT * FIELD_BYTES;

/**
 * Reads a MemoryLayout struct out of `memory` at `layoutPtr` (the value
 * returned by the core's `layout_get_ffi()` export).
 */
export function readMemoryLayout(memory: WebAssembly.Memory, layoutPtr: bigint): MemoryLayout {
  const view = new DataView(memory.buffer, Number(layoutPtr), MEMORY_LAYOUT_STRUCT_SIZE);
  const little = true;
  return {
    guestRamBase:          view.getBigUint64(0 * FIELD_BYTES, little),
    guestRamSize:          view.getBigUint64(1 * FIELD_BYTES, little),
    pageTableL1Base:       view.getBigUint64(2 * FIELD_BYTES, little),
    framebufferSlotBase:   view.getBigUint64(3 * FIELD_BYTES, little),
    audioRingBase:         view.getBigUint64(4 * FIELD_BYTES, little),
    gpuRingBase:           view.getBigUint64(5 * FIELD_BYTES, little),
    gpuCompletionRingBase: view.getBigUint64(6 * FIELD_BYTES, little),
    inputRegionBase:       view.getBigUint64(7 * FIELD_BYTES, little),
    traceBufferBase:       view.getBigUint64(8 * FIELD_BYTES, little),
    breakpointRegionBase:  view.getBigUint64(9 * FIELD_BYTES, little),
  };
}

/** Narrow a linear-memory address to a Number offset for typed-array construction. */
export function toByteOffset(address: bigint): number {
  if (address > BigInt(Number.MAX_SAFE_INTEGER)) {
    throw new RangeError(`layout address ${address} exceeds Number.MAX_SAFE_INTEGER`);
  }
  return Number(address);
}

export function guestRamView(memory: WebAssembly.Memory, layout: MemoryLayout): Uint8Array {
  return new Uint8Array(memory.buffer, toByteOffset(layout.guestRamBase), toByteOffset(layout.guestRamSize));
}
