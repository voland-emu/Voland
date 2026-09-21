/**
 * core/hle/kernel/address_space unit test.
 */
#define CHECK_NAME "address_space_test"
#include "check.h"

#include "common/vmm.h"
#include "hle/kernel/address_space.h"

#define CODE_BYTES ((uint64_t)0x3000)

static bool page_aligned(const Address_Region *r) {
  return (r->base & VMM_PAGE_OFFSET_MASK) == 0 && (r->size & VMM_PAGE_OFFSET_MASK) == 0;
}

static bool inside(const Address_Region *outer, const Address_Region *inner) {
  return address_region_contains(outer, inner->base, inner->size);
}

static bool disjoint(const Address_Region *a, const Address_Region *b) {
  return a->base + a->size <= b->base || b->base + b->size <= a->base;
}

/* Structural invariants every layout must satisfy. */
static void check_well_formed(const Address_Space *as) {
  const Address_Region *regions[] = {&as->code, &as->alias, &as->heap, &as->stack, &as->tls_io};
  const size_t n = sizeof(regions) / sizeof(regions[0]);
  CHECK(as->aslr.base == ADDRESS_SPACE_39_START);
  CHECK(as->aslr.base + as->aslr.size == ADDRESS_SPACE_39_END);
  for (size_t i = 0; i < n; i++) {
    CHECK(page_aligned(regions[i]));
    CHECK(regions[i]->size > 0);
    CHECK(inside(&as->aslr, regions[i]));
    for (size_t j = i + 1; j < n; j++) CHECK(disjoint(regions[i], regions[j]));
  }
  CHECK(as->alias.size == ADDRESS_SPACE_39_ALIAS_SIZE);
  CHECK(as->heap.size == ADDRESS_SPACE_39_HEAP_SIZE);
  CHECK(as->stack.size == ADDRESS_SPACE_39_STACK_SIZE);
  CHECK(as->tls_io.size == ADDRESS_SPACE_39_TLS_IO_SIZE);
}

int main(void) {
  Address_Space as;

  /* No ASLR: the canonical layout, with exact bases. */
  CHECK_OK(address_space_init(NPDM_ADDRESS_SPACE_64_BIT_39, CODE_BYTES, 0, &as));
  check_well_formed(&as);
  CHECK(as.type == NPDM_ADDRESS_SPACE_64_BIT_39);
  CHECK(as.code.base == ADDRESS_SPACE_39_START);
  CHECK(as.code.size == CODE_BYTES);
  CHECK(as.alias.base == as.code.base + as.code.size);
  CHECK(as.heap.base == as.alias.base + ADDRESS_SPACE_39_ALIAS_SIZE);
  CHECK(as.stack.base == as.heap.base + ADDRESS_SPACE_39_HEAP_SIZE);
  CHECK(as.tls_io.base == as.stack.base + ADDRESS_SPACE_39_STACK_SIZE);

  /* code_size is rounded up to a page. */
  CHECK_OK(address_space_init(NPDM_ADDRESS_SPACE_64_BIT_39, CODE_BYTES + 1, 0, &as));
  CHECK(as.code.size == CODE_BYTES + VMM_PAGE_SIZE);

  /* ASLR: deterministic per seed, a 2MB-granule shift, still well formed. */
  Address_Space seeded_a, seeded_b, seeded_c;
  CHECK_OK(address_space_init(NPDM_ADDRESS_SPACE_64_BIT_39, CODE_BYTES, 0x1234, &seeded_a));
  CHECK_OK(address_space_init(NPDM_ADDRESS_SPACE_64_BIT_39, CODE_BYTES, 0x1234, &seeded_b));
  CHECK_OK(address_space_init(NPDM_ADDRESS_SPACE_64_BIT_39, CODE_BYTES, 0x5678, &seeded_c));
  check_well_formed(&seeded_a);
  check_well_formed(&seeded_c);
  CHECK(seeded_a.code.base == seeded_b.code.base);
  CHECK(seeded_a.code.base != seeded_c.code.base);
  CHECK(seeded_a.code.base != ADDRESS_SPACE_39_START);
  CHECK((seeded_a.code.base - ADDRESS_SPACE_39_START) % ADDRESS_SPACE_ASLR_GRANULE == 0);
  CHECK(seeded_a.alias.base == seeded_a.code.base + seeded_a.code.size);

  /* Largest code region that fits; one page more does not. */
  const uint64_t fixed = ADDRESS_SPACE_39_ALIAS_SIZE + ADDRESS_SPACE_39_HEAP_SIZE +
                         ADDRESS_SPACE_39_STACK_SIZE + ADDRESS_SPACE_39_TLS_IO_SIZE;
  const uint64_t max_code = (ADDRESS_SPACE_39_END - ADDRESS_SPACE_39_START) - fixed;
  CHECK_OK(address_space_init(NPDM_ADDRESS_SPACE_64_BIT_39, max_code, 0, &as));
  check_well_formed(&as);
  CHECK(as.tls_io.base + as.tls_io.size == ADDRESS_SPACE_39_END);
  /* With no slack, ASLR has nowhere to go and degrades to the fixed layout. */
  CHECK_OK(address_space_init(NPDM_ADDRESS_SPACE_64_BIT_39, max_code, 0x99, &as));
  CHECK(as.code.base == ADDRESS_SPACE_39_START);
  CHECK_CODE(address_space_init(NPDM_ADDRESS_SPACE_64_BIT_39, max_code + VMM_PAGE_SIZE, 0, &as),
             RESULT_INVALID_ARGUMENT);
  CHECK_CODE(address_space_init(NPDM_ADDRESS_SPACE_64_BIT_39, UINT64_MAX, 0, &as),
             RESULT_INVALID_ARGUMENT);

  /* Rejections. */
  CHECK_CODE(address_space_init(NPDM_ADDRESS_SPACE_64_BIT_39, 0, 0, &as), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(address_space_init(NPDM_ADDRESS_SPACE_64_BIT_39, CODE_BYTES, 0, NULL),
             RESULT_INVALID_ARGUMENT);
  CHECK_CODE(address_space_init(NPDM_ADDRESS_SPACE_64_BIT_36, CODE_BYTES, 0, &as),
             RESULT_NOT_IMPLEMENTED);
  CHECK_CODE(address_space_init(NPDM_ADDRESS_SPACE_32_BIT, CODE_BYTES, 0, &as),
             RESULT_NOT_IMPLEMENTED);
  CHECK_CODE(address_space_init(NPDM_ADDRESS_SPACE_32_BIT_NO_RESERVED, CODE_BYTES, 0, &as),
             RESULT_NOT_IMPLEMENTED);

  /* address_region_contains, including the overflow edges. */
  const Address_Region r = {0x1000, 0x2000};
  CHECK(address_region_contains(&r, 0x1000, 0x2000));
  CHECK(address_region_contains(&r, 0x2FFF, 1));
  CHECK(address_region_contains(&r, 0x3000, 0));
  CHECK(!address_region_contains(&r, 0x0FFF, 1));
  CHECK(!address_region_contains(&r, 0x2FFF, 2));
  CHECK(!address_region_contains(&r, 0x3001, 0));
  CHECK(!address_region_contains(&r, 0x1000, UINT64_MAX));
  CHECK(!address_region_contains(&r, UINT64_MAX, 1));
  CHECK(!address_region_contains(NULL, 0x1000, 1));
  return 0;
}
