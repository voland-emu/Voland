/**
 * NPDM parser. See npdm.h for the layout.
 */
#include "hle/loader/npdm.h"

#include "common/log.h"
#include "hle/loader/byte_source.h"

#include <string.h>

/* META field offsets. */
#define NPDM_META_OFFSET_MAGIC ((uint32_t)0x00)
#define NPDM_META_OFFSET_FLAGS ((uint32_t)0x0C)
#define NPDM_META_OFFSET_MAIN_THREAD_PRIORITY ((uint32_t)0x0E)
#define NPDM_META_OFFSET_MAIN_THREAD_CORE ((uint32_t)0x0F)
#define NPDM_META_OFFSET_SYSTEM_RESOURCE_SIZE ((uint32_t)0x14)
#define NPDM_META_OFFSET_VERSION ((uint32_t)0x18)
#define NPDM_META_OFFSET_MAIN_THREAD_STACK_SIZE ((uint32_t)0x1C)
#define NPDM_META_OFFSET_NAME ((uint32_t)0x20)
#define NPDM_META_OFFSET_PRODUCT_CODE ((uint32_t)0x30)
#define NPDM_META_OFFSET_ACI0_OFFSET ((uint32_t)0x70)
#define NPDM_META_OFFSET_ACI0_SIZE ((uint32_t)0x74)
#define NPDM_META_OFFSET_ACID_OFFSET ((uint32_t)0x78)
#define NPDM_META_OFFSET_ACID_SIZE ((uint32_t)0x7C)

/* META flags byte. */
#define NPDM_FLAG_64BIT_INSTRUCTION ((uint8_t)0x01)
#define NPDM_FLAG_ADDRESS_SPACE_SHIFT 1u
#define NPDM_FLAG_ADDRESS_SPACE_MASK ((uint8_t)0x07)
#define NPDM_FLAG_OPTIMIZE_MEMORY_ALLOCATION ((uint8_t)0x10)
#define NPDM_FLAG_DISABLE_DEVICE_ADDRESS_SPACE_MERGE ((uint8_t)0x20)
#define NPDM_FLAG_ENABLE_ALIAS_REGION_EXTRA_SIZE ((uint8_t)0x40)

/* The main thread stack is carved in whole pages (§12). */
#define NPDM_MAIN_THREAD_STACK_ALIGNMENT ((uint32_t)0x1000)

/* ACID field offsets (relative to the ACID start). */
#define NPDM_ACID_OFFSET_MAGIC ((uint32_t)0x200)
#define NPDM_ACID_OFFSET_FLAGS ((uint32_t)0x20C)
#define NPDM_ACID_OFFSET_PROGRAM_ID_MIN ((uint32_t)0x210)
#define NPDM_ACID_OFFSET_PROGRAM_ID_MAX ((uint32_t)0x218)
#define NPDM_ACID_OFFSET_KERNEL_CAPS_OFFSET ((uint32_t)0x230)
#define NPDM_ACID_OFFSET_KERNEL_CAPS_SIZE ((uint32_t)0x234)
#define NPDM_ACID_FLAG_PRODUCTION ((uint32_t)0x1)
#define NPDM_ACID_FLAG_UNQUALIFIED_APPROVAL ((uint32_t)0x2)

/* ACI0 field offsets (relative to the ACI0 start). */
#define NPDM_ACI0_OFFSET_MAGIC ((uint32_t)0x00)
#define NPDM_ACI0_OFFSET_PROGRAM_ID ((uint32_t)0x10)
#define NPDM_ACI0_OFFSET_FS_ACCESS_OFFSET ((uint32_t)0x20)
#define NPDM_ACI0_OFFSET_FS_ACCESS_SIZE ((uint32_t)0x24)
#define NPDM_ACI0_OFFSET_SERVICE_ACCESS_OFFSET ((uint32_t)0x28)
#define NPDM_ACI0_OFFSET_SERVICE_ACCESS_SIZE ((uint32_t)0x2C)
#define NPDM_ACI0_OFFSET_KERNEL_CAPS_OFFSET ((uint32_t)0x30)
#define NPDM_ACI0_OFFSET_KERNEL_CAPS_SIZE ((uint32_t)0x34)

/* Kernel capability descriptor types, identified by trailing one-bit
 * count, and their field positions. */
#define NPDM_CAP_PADDING ((uint32_t)0xFFFFFFFF)
#define NPDM_CAP_TYPE_THREAD_INFO 3u
#define NPDM_CAP_TYPE_ENABLE_SYSTEM_CALLS 4u
#define NPDM_CAP_TYPE_MEMORY_MAP 6u
#define NPDM_CAP_TYPE_IO_MEMORY_MAP 7u
#define NPDM_CAP_TYPE_MEMORY_REGION_MAP 10u
#define NPDM_CAP_TYPE_ENABLE_INTERRUPTS 11u
#define NPDM_CAP_TYPE_MISC_PARAMS 13u
#define NPDM_CAP_TYPE_KERNEL_VERSION 14u
#define NPDM_CAP_TYPE_HANDLE_TABLE_SIZE 15u
#define NPDM_CAP_TYPE_MISC_FLAGS 16u

#define NPDM_CAP_THREAD_INFO_LOWEST_PRIORITY_SHIFT 4u
#define NPDM_CAP_THREAD_INFO_HIGHEST_PRIORITY_SHIFT 10u
#define NPDM_CAP_THREAD_INFO_PRIORITY_MASK ((uint32_t)0x3F)
#define NPDM_CAP_THREAD_INFO_MIN_CORE_SHIFT 16u
#define NPDM_CAP_THREAD_INFO_MAX_CORE_SHIFT 24u
#define NPDM_CAP_THREAD_INFO_CORE_MASK ((uint32_t)0xFF)

#define NPDM_CAP_SVC_MASK_SHIFT 5u
#define NPDM_CAP_SVC_MASK_BITS 24u
#define NPDM_CAP_SVC_MASK_MASK ((uint32_t)0xFFFFFF)
#define NPDM_CAP_SVC_INDEX_SHIFT 29u
#define NPDM_CAP_SVC_INDEX_MASK ((uint32_t)0x7)

#define NPDM_CAP_MISC_PARAMS_PROGRAM_TYPE_SHIFT 14u
#define NPDM_CAP_MISC_PARAMS_PROGRAM_TYPE_MASK ((uint32_t)0x7)

#define NPDM_CAP_KERNEL_VERSION_MINOR_SHIFT 15u
#define NPDM_CAP_KERNEL_VERSION_MINOR_MASK ((uint32_t)0xF)
#define NPDM_CAP_KERNEL_VERSION_MAJOR_SHIFT 19u
#define NPDM_CAP_KERNEL_VERSION_MAJOR_MASK ((uint32_t)0x1FFF)

#define NPDM_CAP_HANDLE_TABLE_SIZE_SHIFT 16u
#define NPDM_CAP_HANDLE_TABLE_SIZE_MASK ((uint32_t)0x3FF)

#define NPDM_CAP_MISC_FLAGS_ENABLE_DEBUG ((uint32_t)1 << 17)
#define NPDM_CAP_MISC_FLAGS_FORCE_DEBUG ((uint32_t)1 << 18)

/* ------------------------------------------------------------------ */
/* Helpers.                                                            */
/* ------------------------------------------------------------------ */

/* [offset, offset + size) must lie inside a container of `container_size`. */
static bool range_fits(uint32_t offset, uint32_t size, uint64_t container_size) {
  return offset <= container_size && size <= container_size - offset;
}

static void copy_fixed_string(char *out, const uint8_t *in, size_t size) {
  memcpy(out, in, size);
  out[size] = '\0';
}

static uint32_t trailing_ones(uint32_t value) {
  uint32_t count = 0;
  while (count < 32 && (value & ((uint32_t)1 << count))) count++;
  return count;
}

static Error apply_thread_info(uint32_t descriptor, NPDM_Kernel_Capabilities *caps) {
  if (caps->has_thread_info) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: duplicate thread info descriptor");
  }
  const uint32_t lowest =
      (descriptor >> NPDM_CAP_THREAD_INFO_LOWEST_PRIORITY_SHIFT) & NPDM_CAP_THREAD_INFO_PRIORITY_MASK;
  const uint32_t highest =
      (descriptor >> NPDM_CAP_THREAD_INFO_HIGHEST_PRIORITY_SHIFT) & NPDM_CAP_THREAD_INFO_PRIORITY_MASK;
  const uint32_t min_core =
      (descriptor >> NPDM_CAP_THREAD_INFO_MIN_CORE_SHIFT) & NPDM_CAP_THREAD_INFO_CORE_MASK;
  const uint32_t max_core =
      (descriptor >> NPDM_CAP_THREAD_INFO_MAX_CORE_SHIFT) & NPDM_CAP_THREAD_INFO_CORE_MASK;
  if (lowest > NPDM_PRIORITY_MAX || highest > NPDM_PRIORITY_MAX || highest > lowest) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: thread info priorities out of bounds");
  }
  if (min_core > NPDM_CORE_MAX || max_core > NPDM_CORE_MAX || min_core > max_core) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: thread info cores out of bounds");
  }
  caps->has_thread_info = true;
  caps->lowest_priority = (uint8_t)lowest;
  caps->highest_priority = (uint8_t)highest;
  caps->min_core = (uint8_t)min_core;
  caps->max_core = (uint8_t)max_core;
  return OK;
}

static Error apply_system_calls(uint32_t descriptor, NPDM_Kernel_Capabilities *caps) {
  const uint32_t mask = (descriptor >> NPDM_CAP_SVC_MASK_SHIFT) & NPDM_CAP_SVC_MASK_MASK;
  const uint32_t index = (descriptor >> NPDM_CAP_SVC_INDEX_SHIFT) & NPDM_CAP_SVC_INDEX_MASK;
  for (uint32_t bit = 0; bit < NPDM_CAP_SVC_MASK_BITS; bit++) {
    if (!(mask & ((uint32_t)1 << bit))) continue;
    const uint32_t svc = index * NPDM_CAP_SVC_MASK_BITS + bit;
    if (svc >= NPDM_SVC_COUNT) {
      return ERR(RESULT_INVALID_ARGUMENT, "npdm: syscall number out of range");
    }
    caps->svc_mask[svc / 64u] |= (uint64_t)1 << (svc % 64u);
  }
  return OK;
}

static Error parse_capabilities(const uint8_t *bytes, uint32_t size,
                                NPDM_Kernel_Capabilities *caps) {
  memset(caps, 0, sizeof(*caps));
  if (size % sizeof(uint32_t) != 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: capability size not a multiple of 4");
  }
  const uint32_t count = size / sizeof(uint32_t);
  if (count > NPDM_MAX_CAPABILITY_COUNT) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: too many capability descriptors");
  }

  for (uint32_t i = 0; i < count; i++) {
    const uint32_t descriptor = byte_source_le32(bytes + (uint64_t)i * sizeof(uint32_t));
    if (descriptor == NPDM_CAP_PADDING) continue;
    Error err = OK;
    switch (trailing_ones(descriptor)) {
      case NPDM_CAP_TYPE_THREAD_INFO:
        err = apply_thread_info(descriptor, caps);
        break;
      case NPDM_CAP_TYPE_ENABLE_SYSTEM_CALLS:
        err = apply_system_calls(descriptor, caps);
        break;
      case NPDM_CAP_TYPE_MEMORY_MAP:
      case NPDM_CAP_TYPE_IO_MEMORY_MAP:
      case NPDM_CAP_TYPE_MEMORY_REGION_MAP:
      case NPDM_CAP_TYPE_ENABLE_INTERRUPTS:
        caps->undecoded_descriptor_count++;
        break;
      case NPDM_CAP_TYPE_MISC_PARAMS:
        caps->has_program_type = true;
        caps->program_type = (NPDM_Program_Type)(
            (descriptor >> NPDM_CAP_MISC_PARAMS_PROGRAM_TYPE_SHIFT) &
            NPDM_CAP_MISC_PARAMS_PROGRAM_TYPE_MASK);
        break;
      case NPDM_CAP_TYPE_KERNEL_VERSION:
        caps->has_kernel_version = true;
        caps->kernel_version_minor = (uint8_t)(
            (descriptor >> NPDM_CAP_KERNEL_VERSION_MINOR_SHIFT) & NPDM_CAP_KERNEL_VERSION_MINOR_MASK);
        caps->kernel_version_major = (uint16_t)(
            (descriptor >> NPDM_CAP_KERNEL_VERSION_MAJOR_SHIFT) & NPDM_CAP_KERNEL_VERSION_MAJOR_MASK);
        break;
      case NPDM_CAP_TYPE_HANDLE_TABLE_SIZE:
        caps->has_handle_table_size = true;
        caps->handle_table_size = (uint16_t)(
            (descriptor >> NPDM_CAP_HANDLE_TABLE_SIZE_SHIFT) & NPDM_CAP_HANDLE_TABLE_SIZE_MASK);
        break;
      case NPDM_CAP_TYPE_MISC_FLAGS:
        caps->enable_debug = (descriptor & NPDM_CAP_MISC_FLAGS_ENABLE_DEBUG) != 0;
        caps->force_debug = (descriptor & NPDM_CAP_MISC_FLAGS_FORCE_DEBUG) != 0;
        break;
      default:
        log_warn("npdm: unknown capability descriptor 0x%08x", (unsigned)descriptor);
        return ERR(RESULT_INVALID_ARGUMENT, "npdm: unknown capability descriptor type");
    }
    if (!error_is_ok(err)) return err;
  }
  if (caps->undecoded_descriptor_count > 0) {
    log_debug("npdm: %u capability descriptors recorded but not decoded",
              (unsigned)caps->undecoded_descriptor_count);
  }
  return OK;
}

/* ------------------------------------------------------------------ */
/* Sections.                                                           */
/* ------------------------------------------------------------------ */

static Error parse_meta(const uint8_t *bytes, NPDM *out) {
  if (byte_source_le32(bytes + NPDM_META_OFFSET_MAGIC) != NPDM_META_MAGIC) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: META magic missing");
  }
  const uint8_t flags = bytes[NPDM_META_OFFSET_FLAGS];
  const uint8_t address_space = (flags >> NPDM_FLAG_ADDRESS_SPACE_SHIFT) & NPDM_FLAG_ADDRESS_SPACE_MASK;
  if (address_space > (uint8_t)NPDM_ADDRESS_SPACE_64_BIT_39) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: address space type out of range");
  }
  out->is_64bit_instruction = (flags & NPDM_FLAG_64BIT_INSTRUCTION) != 0;
  out->address_space = (NPDM_Address_Space)address_space;
  out->optimize_memory_allocation = (flags & NPDM_FLAG_OPTIMIZE_MEMORY_ALLOCATION) != 0;
  out->disable_device_address_space_merge =
      (flags & NPDM_FLAG_DISABLE_DEVICE_ADDRESS_SPACE_MERGE) != 0;
  out->enable_alias_region_extra_size = (flags & NPDM_FLAG_ENABLE_ALIAS_REGION_EXTRA_SIZE) != 0;

  out->main_thread_priority = bytes[NPDM_META_OFFSET_MAIN_THREAD_PRIORITY];
  out->main_thread_core_number = bytes[NPDM_META_OFFSET_MAIN_THREAD_CORE];
  if (out->main_thread_priority > NPDM_PRIORITY_MAX) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: main thread priority out of bounds");
  }
  if (out->main_thread_core_number > NPDM_CORE_MAX) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: main thread core out of bounds");
  }
  out->system_resource_size = byte_source_le32(bytes + NPDM_META_OFFSET_SYSTEM_RESOURCE_SIZE);
  out->version = byte_source_le32(bytes + NPDM_META_OFFSET_VERSION);
  out->main_thread_stack_size = byte_source_le32(bytes + NPDM_META_OFFSET_MAIN_THREAD_STACK_SIZE);
  if (out->main_thread_stack_size == 0 ||
      out->main_thread_stack_size % NPDM_MAIN_THREAD_STACK_ALIGNMENT != 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: main thread stack size zero or unaligned");
  }
  copy_fixed_string(out->name, bytes + NPDM_META_OFFSET_NAME, NPDM_NAME_SIZE);
  copy_fixed_string(out->product_code, bytes + NPDM_META_OFFSET_PRODUCT_CODE,
                    NPDM_PRODUCT_CODE_SIZE);
  return OK;
}

static Error parse_acid(const uint8_t *acid, uint32_t acid_size, NPDM *out) {
  if (byte_source_le32(acid + NPDM_ACID_OFFSET_MAGIC) != NPDM_ACID_MAGIC) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: ACID magic missing");
  }
  const uint32_t flags = byte_source_le32(acid + NPDM_ACID_OFFSET_FLAGS);
  out->acid_is_production = (flags & NPDM_ACID_FLAG_PRODUCTION) != 0;
  out->acid_unqualified_approval = (flags & NPDM_ACID_FLAG_UNQUALIFIED_APPROVAL) != 0;
  out->program_id_min = byte_source_le64(acid + NPDM_ACID_OFFSET_PROGRAM_ID_MIN);
  out->program_id_max = byte_source_le64(acid + NPDM_ACID_OFFSET_PROGRAM_ID_MAX);

  const uint32_t caps_offset = byte_source_le32(acid + NPDM_ACID_OFFSET_KERNEL_CAPS_OFFSET);
  const uint32_t caps_size = byte_source_le32(acid + NPDM_ACID_OFFSET_KERNEL_CAPS_SIZE);
  if (!range_fits(caps_offset, caps_size, acid_size)) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: ACID kernel capabilities outside ACID");
  }
  return parse_capabilities(acid + caps_offset, caps_size, &out->acid_capabilities);
}

static Error parse_aci0(const uint8_t *aci0, uint32_t aci0_size, uint32_t aci0_file_offset,
                        NPDM *out) {
  if (byte_source_le32(aci0 + NPDM_ACI0_OFFSET_MAGIC) != NPDM_ACI0_MAGIC) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: ACI0 magic missing");
  }
  out->program_id = byte_source_le64(aci0 + NPDM_ACI0_OFFSET_PROGRAM_ID);

  const uint32_t fs_offset = byte_source_le32(aci0 + NPDM_ACI0_OFFSET_FS_ACCESS_OFFSET);
  const uint32_t fs_size = byte_source_le32(aci0 + NPDM_ACI0_OFFSET_FS_ACCESS_SIZE);
  const uint32_t sac_offset = byte_source_le32(aci0 + NPDM_ACI0_OFFSET_SERVICE_ACCESS_OFFSET);
  const uint32_t sac_size = byte_source_le32(aci0 + NPDM_ACI0_OFFSET_SERVICE_ACCESS_SIZE);
  const uint32_t caps_offset = byte_source_le32(aci0 + NPDM_ACI0_OFFSET_KERNEL_CAPS_OFFSET);
  const uint32_t caps_size = byte_source_le32(aci0 + NPDM_ACI0_OFFSET_KERNEL_CAPS_SIZE);
  if (!range_fits(fs_offset, fs_size, aci0_size) || !range_fits(sac_offset, sac_size, aci0_size) ||
      !range_fits(caps_offset, caps_size, aci0_size)) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: ACI0 blob outside ACI0");
  }
  out->fs_access_header.offset = aci0_file_offset + fs_offset;
  out->fs_access_header.size = fs_size;
  out->service_access_control.offset = aci0_file_offset + sac_offset;
  out->service_access_control.size = sac_size;
  return parse_capabilities(aci0 + caps_offset, caps_size, &out->capabilities);
}

/* ------------------------------------------------------------------ */
/* Public API.                                                         */
/* ------------------------------------------------------------------ */

Error npdm_parse(const uint8_t *bytes, uint64_t size, NPDM *out) {
  if (!bytes || !out) return ERR(RESULT_INVALID_ARGUMENT, "npdm_parse: NULL argument");
  if (size < NPDM_META_HEADER_SIZE) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: smaller than META header");
  }
  if (size > NPDM_MAX_FILE_BYTES) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: file over size cap");
  }
  memset(out, 0, sizeof(*out));

  Error err = parse_meta(bytes, out);
  if (!error_is_ok(err)) return err;

  const uint32_t aci0_offset = byte_source_le32(bytes + NPDM_META_OFFSET_ACI0_OFFSET);
  const uint32_t aci0_size = byte_source_le32(bytes + NPDM_META_OFFSET_ACI0_SIZE);
  const uint32_t acid_offset = byte_source_le32(bytes + NPDM_META_OFFSET_ACID_OFFSET);
  const uint32_t acid_size = byte_source_le32(bytes + NPDM_META_OFFSET_ACID_SIZE);
  if (!range_fits(acid_offset, acid_size, size) || acid_size < NPDM_ACID_HEADER_SIZE) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: ACID range invalid");
  }
  if (!range_fits(aci0_offset, aci0_size, size) || aci0_size < NPDM_ACI0_HEADER_SIZE) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: ACI0 range invalid");
  }

  err = parse_acid(bytes + acid_offset, acid_size, out);
  if (!error_is_ok(err)) return err;
  err = parse_aci0(bytes + aci0_offset, aci0_size, aci0_offset, out);
  if (!error_is_ok(err)) return err;

  if (out->program_id < out->program_id_min || out->program_id > out->program_id_max) {
    return ERR(RESULT_INVALID_ARGUMENT, "npdm: program id outside ACID range");
  }
  log_debug("npdm: parsed '%s' program_id=%016llx address_space=%d stack=0x%x",
            out->name, (unsigned long long)out->program_id, (int)out->address_space,
            (unsigned)out->main_thread_stack_size);
  return OK;
}

bool npdm_svc_allowed(const NPDM *npdm, uint32_t svc_id) {
  if (!npdm || svc_id >= NPDM_SVC_COUNT) return false;
  return (npdm->capabilities.svc_mask[svc_id / 64u] >> (svc_id % 64u)) & 1u;
}
