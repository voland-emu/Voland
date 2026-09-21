/**
 * Process address-space layout: the Horizon region carve-up. See
 * docs/DESIGN.md §12 ("Process bootstrap", step 3): "carve heap/alias/
 * stack regions per the Horizon layout; vmm answers the GetInfo region
 * queries from this same layout - one source of truth."
 *
 * This struct IS that source of truth. The bootstrap places NSOs, the
 * main-thread stack and TLS inside it; the memory HLE (SetHeapSize,
 * MapMemory, GetInfo - the next §25 checkboxes) reads region bases and
 * sizes from it and never recomputes them.
 *
 * Only the 39-bit layout (NPDM_ADDRESS_SPACE_64_BIT_39, every modern
 * title) is implemented. The 36-bit and 32-bit layouts return
 * RESULT_NOT_IMPLEMENTED; they carve the same region kinds with different
 * sizes and can be added behind this interface when a title needs them.
 *
 * 39-bit layout, as Horizon lays it out (observed behavior; the numbers
 * are the kernel's per-address-space-type region sizes):
 *
 *   [ADDRESS_SPACE_39_START, ADDRESS_SPACE_39_END)   the "map"/ASLR region
 *     code      at the ASLR'd base: every NSO image, back to back
 *     alias     ADDRESS_SPACE_39_ALIAS_SIZE   (MapMemory mirrors)
 *     heap      ADDRESS_SPACE_39_HEAP_SIZE    (SetHeapSize)
 *     stack     ADDRESS_SPACE_39_STACK_SIZE   (thread stacks)
 *     tls_io    ADDRESS_SPACE_39_TLS_IO_SIZE  (TLS pages, transfer memory)
 *
 *   Regions follow the code region in that order; ASLR shifts the code
 *   base (and thereby everything after it) by a random multiple of
 *   ADDRESS_SPACE_ASLR_GRANULE. Horizon also randomizes the ORDER of the
 *   four regions; Voland keeps the order fixed - the guest observes region
 *   bases only through GetInfo and never assumes an order, and a fixed
 *   order keeps failures reproducible from a seed.
 *
 * ASLR is seeded by the caller: `aslr_seed == 0` disables it (code at
 * ADDRESS_SPACE_39_START - what tests, traces and the debugger want);
 * any other seed gives a deterministic, reproducible layout. The
 * platform supplies entropy; the core never reads a host RNG (§3).
 */
#ifndef SWITCH_HLE_KERNEL_ADDRESS_SPACE_H
#define SWITCH_HLE_KERNEL_ADDRESS_SPACE_H

#include <stdbool.h>
#include <stdint.h>

#include "common/result.h"
#include "hle/loader/npdm.h"

#define ADDRESS_SPACE_39_START ((uint64_t)0x8000000)        /* 128MB */
#define ADDRESS_SPACE_39_END ((uint64_t)1 << 39)            /* 512GB */
#define ADDRESS_SPACE_39_ALIAS_SIZE ((uint64_t)0x1000000000) /* 64GB */
#define ADDRESS_SPACE_39_HEAP_SIZE ((uint64_t)0x180000000)   /* 6GB */
#define ADDRESS_SPACE_39_STACK_SIZE ((uint64_t)0x80000000)   /* 2GB */
#define ADDRESS_SPACE_39_TLS_IO_SIZE ((uint64_t)0x1000000000) /* 64GB */

/* ASLR shifts the code base in multiples of this (2MB: Horizon's region
 * randomization granule). */
#define ADDRESS_SPACE_ASLR_GRANULE ((uint64_t)0x200000)

typedef struct Address_Region {
  uint64_t base; /* page-aligned */
  uint64_t size; /* bytes, page multiple; [base, base + size) */
} Address_Region;

typedef struct Address_Space {
  NPDM_Address_Space type;
  Address_Region aslr;   /* the whole usable space; GetInfo AslrRegion */
  Address_Region code;   /* NSO images; sized to what the bootstrap asked for */
  Address_Region alias;  /* GetInfo AliasRegion */
  Address_Region heap;   /* GetInfo HeapRegion */
  Address_Region stack;  /* GetInfo StackRegion */
  Address_Region tls_io; /* TLS pages, transfer memory */
} Address_Space;

/* Lays out the regions for `type` around a code region of `code_size`
 * bytes (rounded up to a page; must be > 0).
 *   RESULT_INVALID_ARGUMENT NULL out; code_size == 0; the code region
 *                           plus the fixed regions do not fit the space
 *   RESULT_NOT_IMPLEMENTED  type other than NPDM_ADDRESS_SPACE_64_BIT_39
 * Pure: no I/O, no allocation, no vmm calls. */
Error address_space_init(NPDM_Address_Space type, uint64_t code_size,
                         uint64_t aslr_seed, Address_Space *out);

/* True if [gva, gva + size) lies entirely inside `region`. Overflow-safe. */
bool address_region_contains(const Address_Region *region, uint64_t gva, uint64_t size);

#endif /* SWITCH_HLE_KERNEL_ADDRESS_SPACE_H */
