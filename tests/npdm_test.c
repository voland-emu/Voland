/**
 * Unit tests for core/hle/loader/npdm.c.
 */
#define CHECK_NAME "npdm_test"
#include "check.h"

#include "hle/loader/npdm.h"
#include "loader_fixtures.h"

#include <string.h>

/* Descriptor encoders, restating the spec: the type is the trailing
 * one-bit count, fields sit above the terminating zero bit. */
#define CAP_THREAD_INFO(lowest, highest, min_core, max_core) \
  (0x7u | ((uint32_t)(lowest) << 4) | ((uint32_t)(highest) << 10) | \
   ((uint32_t)(min_core) << 16) | ((uint32_t)(max_core) << 24))
#define CAP_SYSCALLS(mask, index) (0xFu | ((uint32_t)(mask) << 5) | ((uint32_t)(index) << 29))
#define CAP_MEMORY_MAP(bits) (0x3Fu | ((uint32_t)(bits) << 7))
#define CAP_MISC_PARAMS(program_type) (0x1FFFu | ((uint32_t)(program_type) << 14))
#define CAP_KERNEL_VERSION(major, minor) \
  (0x3FFFu | ((uint32_t)(minor) << 15) | ((uint32_t)(major) << 19))
#define CAP_HANDLE_TABLE(size) (0x7FFFu | ((uint32_t)(size) << 16))
#define CAP_MISC_FLAGS(enable_debug, force_debug) \
  (0xFFFFu | ((uint32_t)(enable_debug) << 17) | ((uint32_t)(force_debug) << 18))
#define CAP_PADDING 0xFFFFFFFFu
#define CAP_UNKNOWN_TYPE_5 0x1Fu

#define FLAGS_64BIT_39 ((uint8_t)(0x01 | (3 << 1)))
#define PROGRAM_ID 0x0100000000010000ull

static const uint32_t k_aci0_caps[] = {
    CAP_THREAD_INFO(59, 28, 0, 2),
    CAP_SYSCALLS(0x00000FFF, 0),  /* SVCs 0..11 */
    CAP_SYSCALLS(0x00800001, 1),  /* SVCs 24 and 47 */
    CAP_SYSCALLS(0x00000004, 7),  /* SVC 170 */
    CAP_MEMORY_MAP(0x1234),
    CAP_MEMORY_MAP(0x5678),
    CAP_MISC_PARAMS(1),
    CAP_KERNEL_VERSION(13, 2),
    CAP_HANDLE_TABLE(512),
    CAP_MISC_FLAGS(1, 0),
    CAP_PADDING,
};
static const uint32_t k_acid_caps[] = {
    CAP_THREAD_INFO(63, 0, 0, 3),
    CAP_SYSCALLS(0xFFFFFF, 0),
    CAP_MISC_FLAGS(1, 1),
};

static Fixture_NPDM_Params base_params(void) {
  Fixture_NPDM_Params p;
  memset(&p, 0, sizeof(p));
  p.flags = FLAGS_64BIT_39 | 0x10 | 0x40; /* + optimize alloc, + alias extra */
  p.main_thread_priority = 44;
  p.main_thread_core = 0;
  p.main_thread_stack_size = 0x100000;
  p.system_resource_size = 0x2000000;
  p.version = 0x00010203;
  p.name = "TestApplication";
  p.product_code = "VOLAND-TEST";
  p.acid_flags = 0x1; /* production */
  p.program_id_min = 0x0100000000010000ull;
  p.program_id_max = 0x01FFFFFFFFFFFFFFull;
  p.acid_capabilities = k_acid_caps;
  p.acid_capability_count = sizeof(k_acid_caps) / sizeof(k_acid_caps[0]);
  p.program_id = PROGRAM_ID;
  p.fs_access_size = 0x2C;
  p.service_access_size = 0x40;
  p.aci0_capabilities = k_aci0_caps;
  p.aci0_capability_count = sizeof(k_aci0_caps) / sizeof(k_aci0_caps[0]);
  return p;
}

static void test_full_parse(void) {
  Fixture_NPDM_Params p = base_params();
  Fixture_Buffer image;
  fixture_build_npdm(&p, &image);

  NPDM npdm;
  CHECK_OK(npdm_parse(image.bytes, image.size, &npdm));

  /* META */
  CHECK(npdm.is_64bit_instruction);
  CHECK(npdm.address_space == NPDM_ADDRESS_SPACE_64_BIT_39);
  CHECK(npdm.optimize_memory_allocation);
  CHECK(!npdm.disable_device_address_space_merge);
  CHECK(npdm.enable_alias_region_extra_size);
  CHECK(npdm.main_thread_priority == 44);
  CHECK(npdm.main_thread_core_number == 0);
  CHECK(npdm.main_thread_stack_size == 0x100000);
  CHECK(npdm.system_resource_size == 0x2000000);
  CHECK(npdm.version == 0x00010203);
  CHECK(strcmp(npdm.name, "TestApplication") == 0);
  CHECK(strcmp(npdm.product_code, "VOLAND-TEST") == 0);

  /* ACID */
  CHECK(npdm.acid_is_production);
  CHECK(!npdm.acid_unqualified_approval);
  CHECK(npdm.program_id_min == p.program_id_min);
  CHECK(npdm.program_id_max == p.program_id_max);
  CHECK(npdm.acid_capabilities.has_thread_info);
  CHECK(npdm.acid_capabilities.lowest_priority == 63);
  CHECK(npdm.acid_capabilities.highest_priority == 0);
  CHECK(npdm.acid_capabilities.max_core == 3);
  CHECK(npdm.acid_capabilities.force_debug);

  /* ACI0 */
  CHECK(npdm.program_id == PROGRAM_ID);
  CHECK(npdm.fs_access_header.size == 0x2C);
  CHECK(npdm.service_access_control.size == 0x40);
  CHECK(npdm.fs_access_header.offset + 0x2C == npdm.service_access_control.offset);
  CHECK(npdm.service_access_control.offset + 0x40 <= image.size);

  const NPDM_Kernel_Capabilities *caps = &npdm.capabilities;
  CHECK(caps->has_thread_info);
  CHECK(caps->lowest_priority == 59 && caps->highest_priority == 28);
  CHECK(caps->min_core == 0 && caps->max_core == 2);
  CHECK(caps->has_program_type && caps->program_type == NPDM_PROGRAM_TYPE_APPLICATION);
  CHECK(caps->has_kernel_version);
  CHECK(caps->kernel_version_major == 13 && caps->kernel_version_minor == 2);
  CHECK(caps->has_handle_table_size && caps->handle_table_size == 512);
  CHECK(caps->enable_debug && !caps->force_debug);
  CHECK(caps->undecoded_descriptor_count == 2);

  /* SVC bitmap: three descriptors merged. */
  for (uint32_t svc = 0; svc < 12; svc++) CHECK(npdm_svc_allowed(&npdm, svc));
  CHECK(!npdm_svc_allowed(&npdm, 12));
  CHECK(!npdm_svc_allowed(&npdm, 23));
  CHECK(npdm_svc_allowed(&npdm, 24));
  CHECK(!npdm_svc_allowed(&npdm, 25));
  CHECK(npdm_svc_allowed(&npdm, 47));
  CHECK(!npdm_svc_allowed(&npdm, 48));
  CHECK(npdm_svc_allowed(&npdm, 170));
  CHECK(!npdm_svc_allowed(&npdm, 169));
  CHECK(!npdm_svc_allowed(&npdm, NPDM_SVC_COUNT));
  CHECK(!npdm_svc_allowed(&npdm, 0xFFFFFFFFu));
  CHECK(!npdm_svc_allowed(NULL, 0));

  fixture_buffer_free(&image);
}

static void test_name_without_terminator(void) {
  Fixture_NPDM_Params p = base_params();
  p.name = "0123456789ABCDEF"; /* exactly NPDM_NAME_SIZE, no room for NUL on media */
  Fixture_Buffer image;
  fixture_build_npdm(&p, &image);
  NPDM npdm;
  CHECK_OK(npdm_parse(image.bytes, image.size, &npdm));
  CHECK(strlen(npdm.name) == NPDM_NAME_SIZE);
  CHECK(strcmp(npdm.name, "0123456789ABCDEF") == 0);
  fixture_buffer_free(&image);
}

/* ---- rejections --------------------------------------------------- */

static void expect_failure_with_caps(const uint32_t *caps, uint32_t count) {
  Fixture_NPDM_Params p = base_params();
  p.aci0_capabilities = caps;
  p.aci0_capability_count = count;
  Fixture_Buffer image;
  fixture_build_npdm(&p, &image);
  NPDM npdm;
  CHECK_CODE(npdm_parse(image.bytes, image.size, &npdm), RESULT_INVALID_ARGUMENT);
  fixture_buffer_free(&image);
}

static void expect_failure_after(void (*mutate)(Fixture_Buffer *), Result expected) {
  Fixture_NPDM_Params p = base_params();
  Fixture_Buffer image;
  fixture_build_npdm(&p, &image);
  mutate(&image);
  NPDM npdm;
  CHECK_CODE(npdm_parse(image.bytes, image.size, &npdm), expected);
  fixture_buffer_free(&image);
}

static void mutate_meta_magic(Fixture_Buffer *b) { b->bytes[0] = 'X'; }
static void mutate_acid_magic(Fixture_Buffer *b) { b->bytes[FIXTURE_NPDM_ACID_OFFSET + 0x200] = 'X'; }
static void mutate_aci0_magic(Fixture_Buffer *b) {
  b->bytes[fixture_get_le32(b->bytes + FIXTURE_NPDM_OFFSET_ACI0_OFFSET)] = 'X';
}
static void mutate_acid_past_buffer(Fixture_Buffer *b) {
  fixture_put_le32(b->bytes + FIXTURE_NPDM_OFFSET_ACID_SIZE, (uint32_t)b->size);
}
static void mutate_acid_too_small(Fixture_Buffer *b) {
  fixture_put_le32(b->bytes + FIXTURE_NPDM_OFFSET_ACID_SIZE, FIXTURE_NPDM_ACID_HEADER_SIZE - 1);
}
static void mutate_aci0_offset_overflow(Fixture_Buffer *b) {
  fixture_put_le32(b->bytes + FIXTURE_NPDM_OFFSET_ACI0_OFFSET, 0xFFFFFFF0u);
}
static void mutate_aci0_caps_outside(Fixture_Buffer *b) {
  const uint32_t aci0 = fixture_get_le32(b->bytes + FIXTURE_NPDM_OFFSET_ACI0_OFFSET);
  fixture_put_le32(b->bytes + aci0 + 0x34, 0x10000); /* caps size beyond ACI0 */
}
static void mutate_aci0_caps_misaligned(Fixture_Buffer *b) {
  const uint32_t aci0 = fixture_get_le32(b->bytes + FIXTURE_NPDM_OFFSET_ACI0_OFFSET);
  const uint32_t size = fixture_get_le32(b->bytes + aci0 + 0x34);
  fixture_put_le32(b->bytes + aci0 + 0x34, size - 1);
}
static void mutate_address_space_out_of_range(Fixture_Buffer *b) { b->bytes[0x0C] = (uint8_t)(4 << 1); }
static void mutate_priority_out_of_range(Fixture_Buffer *b) { b->bytes[0x0E] = 64; }
static void mutate_core_out_of_range(Fixture_Buffer *b) { b->bytes[0x0F] = 4; }
static void mutate_stack_unaligned(Fixture_Buffer *b) { fixture_put_le32(b->bytes + 0x1C, 0x1001); }
static void mutate_stack_zero(Fixture_Buffer *b) { fixture_put_le32(b->bytes + 0x1C, 0); }
static void mutate_program_id_outside_acid(Fixture_Buffer *b) {
  const uint32_t aci0 = fixture_get_le32(b->bytes + FIXTURE_NPDM_OFFSET_ACI0_OFFSET);
  fixture_put_le64(b->bytes + aci0 + 0x10, 0x0200000000000000ull);
}

static void test_rejections(void) {
  expect_failure_after(mutate_meta_magic, RESULT_INVALID_ARGUMENT);
  expect_failure_after(mutate_acid_magic, RESULT_INVALID_ARGUMENT);
  expect_failure_after(mutate_aci0_magic, RESULT_INVALID_ARGUMENT);
  expect_failure_after(mutate_acid_past_buffer, RESULT_INVALID_ARGUMENT);
  expect_failure_after(mutate_acid_too_small, RESULT_INVALID_ARGUMENT);
  expect_failure_after(mutate_aci0_offset_overflow, RESULT_INVALID_ARGUMENT);
  expect_failure_after(mutate_aci0_caps_outside, RESULT_INVALID_ARGUMENT);
  expect_failure_after(mutate_aci0_caps_misaligned, RESULT_INVALID_ARGUMENT);
  expect_failure_after(mutate_address_space_out_of_range, RESULT_INVALID_ARGUMENT);
  expect_failure_after(mutate_priority_out_of_range, RESULT_INVALID_ARGUMENT);
  expect_failure_after(mutate_core_out_of_range, RESULT_INVALID_ARGUMENT);
  expect_failure_after(mutate_stack_unaligned, RESULT_INVALID_ARGUMENT);
  expect_failure_after(mutate_stack_zero, RESULT_INVALID_ARGUMENT);
  expect_failure_after(mutate_program_id_outside_acid, RESULT_INVALID_ARGUMENT);

  static const uint32_t unknown_type[] = {CAP_UNKNOWN_TYPE_5};
  expect_failure_with_caps(unknown_type, 1);
  static const uint32_t inverted_priorities[] = {CAP_THREAD_INFO(10, 20, 0, 3)};
  expect_failure_with_caps(inverted_priorities, 1);
  static const uint32_t core_too_high[] = {CAP_THREAD_INFO(63, 0, 0, 4)};
  expect_failure_with_caps(core_too_high, 1);
  static const uint32_t inverted_cores[] = {CAP_THREAD_INFO(63, 0, 2, 1)};
  expect_failure_with_caps(inverted_cores, 1);
  static const uint32_t duplicate_thread_info[] = {CAP_THREAD_INFO(63, 0, 0, 3),
                                                   CAP_THREAD_INFO(63, 0, 0, 3)};
  expect_failure_with_caps(duplicate_thread_info, 2);
  /* The highest encodable SVC is index 7, bit 23 = 191 = NPDM_SVC_COUNT - 1,
   * so no descriptor can name an out-of-range SVC; prove the top slot works. */
  {
    static const uint32_t top_svc[] = {CAP_SYSCALLS(0x800000, 7)};
    Fixture_NPDM_Params p = base_params();
    p.aci0_capabilities = top_svc;
    p.aci0_capability_count = 1;
    Fixture_Buffer image;
    fixture_build_npdm(&p, &image);
    NPDM parsed;
    CHECK_OK(npdm_parse(image.bytes, image.size, &parsed));
    CHECK(npdm_svc_allowed(&parsed, NPDM_SVC_COUNT - 1));
    CHECK(!npdm_svc_allowed(&parsed, NPDM_SVC_COUNT - 2));
    CHECK(!parsed.capabilities.has_thread_info);
    fixture_buffer_free(&image);
  }

  /* Size bounds. */
  NPDM npdm;
  uint8_t tiny[FIXTURE_NPDM_META_SIZE - 1] = {0};
  CHECK_CODE(npdm_parse(tiny, sizeof(tiny), &npdm), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(npdm_parse(tiny, NPDM_MAX_FILE_BYTES + 1, &npdm), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(npdm_parse(NULL, 0x100, &npdm), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(npdm_parse(tiny, sizeof(tiny), NULL), RESULT_INVALID_ARGUMENT);
}

int main(void) {
  test_full_parse();
  test_name_without_terminator();
  test_rejections();
  printf("[npdm_test] all tests passed\n");
  return 0;
}
