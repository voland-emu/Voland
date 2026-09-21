/**
 * Process address-space layout. See address_space.h.
 */
#include "hle/kernel/address_space.h"

#include "common/vmm.h"

#include <string.h>

static uint64_t align_up_page(uint64_t value) {
  return (value + VMM_PAGE_OFFSET_MASK) & ~VMM_PAGE_OFFSET_MASK;
}

/* xorshift64: enough to spread a caller-supplied seed over the ASLR
 * window reproducibly. Not a security boundary - ASLR on a single-
 * process emulator is about matching hardware, not defending it. */
static uint64_t mix_seed(uint64_t seed) {
  uint64_t x = seed;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return x;
}

Error address_space_init(NPDM_Address_Space type, uint64_t code_size,
                         uint64_t aslr_seed, Address_Space *out) {
  if (!out) {
    return ERR(RESULT_INVALID_ARGUMENT, "address_space_init: out is NULL");
  }
  if (code_size == 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "address_space_init: code size is zero");
  }
  if (type != NPDM_ADDRESS_SPACE_64_BIT_39) {
    return ERR(RESULT_NOT_IMPLEMENTED,
               "address_space_init: only the 39-bit address space is implemented");
  }
  memset(out, 0, sizeof(*out));

  const uint64_t span = ADDRESS_SPACE_39_END - ADDRESS_SPACE_39_START;
  const uint64_t fixed_regions = ADDRESS_SPACE_39_ALIAS_SIZE + ADDRESS_SPACE_39_HEAP_SIZE +
                                 ADDRESS_SPACE_39_STACK_SIZE + ADDRESS_SPACE_39_TLS_IO_SIZE;
  const uint64_t code_bytes = align_up_page(code_size);
  if (code_bytes < code_size || code_bytes > span - fixed_regions) {
    return ERR(RESULT_INVALID_ARGUMENT, "address_space_init: code region does not fit");
  }

  /* Everything after the code region is placed back to back, so the
   * slack is the only room ASLR has to shift the whole train. */
  const uint64_t slack = span - fixed_regions - code_bytes;
  uint64_t aslr_offset = 0;
  if (aslr_seed != 0) {
    const uint64_t granules = slack / ADDRESS_SPACE_ASLR_GRANULE;
    if (granules > 0) {
      aslr_offset = (mix_seed(aslr_seed) % (granules + 1)) * ADDRESS_SPACE_ASLR_GRANULE;
    }
  }

  out->type = type;
  out->aslr.base = ADDRESS_SPACE_39_START;
  out->aslr.size = span;

  uint64_t cursor = ADDRESS_SPACE_39_START + aslr_offset;
  out->code.base = cursor;
  out->code.size = code_bytes;
  cursor += code_bytes;
  out->alias.base = cursor;
  out->alias.size = ADDRESS_SPACE_39_ALIAS_SIZE;
  cursor += ADDRESS_SPACE_39_ALIAS_SIZE;
  out->heap.base = cursor;
  out->heap.size = ADDRESS_SPACE_39_HEAP_SIZE;
  cursor += ADDRESS_SPACE_39_HEAP_SIZE;
  out->stack.base = cursor;
  out->stack.size = ADDRESS_SPACE_39_STACK_SIZE;
  cursor += ADDRESS_SPACE_39_STACK_SIZE;
  out->tls_io.base = cursor;
  out->tls_io.size = ADDRESS_SPACE_39_TLS_IO_SIZE;
  return OK;
}

bool address_region_contains(const Address_Region *region, uint64_t gva, uint64_t size) {
  if (!region) return false;
  if (gva < region->base) return false;
  const uint64_t offset = gva - region->base;
  if (offset > region->size) return false;
  return size <= region->size - offset;
}
