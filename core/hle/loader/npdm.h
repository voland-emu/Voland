/**
 * NPDM parser: the title's process descriptor (`main.npdm` in the ExeFS).
 * See docs/DESIGN.md §12 ("Process bootstrap", step 2): it supplies the
 * address-space type, main-thread priority/core/stack size, and the
 * kernel capability list the bootstrap and kernel HLE act on.
 *
 * The file is small (a few KB), so it is parsed from a memory buffer the
 * bootstrap reads out of the ExeFS into an arena; no Byte_Source here.
 *
 * Layout (offsets from the start of the file):
 *
 *   META header, NPDM_META_HEADER_SIZE bytes:
 *     0x00 magic "META" u32
 *     0x0C flags u8: bit 0 64-bit instructions; bits 1-3 address space
 *          (NPDM_Address_Space); bit 4 optimize memory allocation;
 *          bit 5 disable device address-space merge; bit 6 enable alias
 *          region extra size
 *     0x0E main thread priority u8      0x0F main thread core u8
 *     0x14 system resource size u32     0x18 version u32
 *     0x1C main thread stack size u32
 *     0x20 name char[0x10]              0x30 product code char[0x10]
 *     0x70 ACI0 offset u32  0x74 ACI0 size u32
 *     0x78 ACID offset u32  0x7C ACID size u32
 *
 *   ACID (the developer-signed "may do" set), NPDM_ACID_HEADER_SIZE:
 *     0x000 signature 0x100, 0x100 public key 0x100 - both ignored
 *     0x200 magic "ACID" u32   0x204 size u32
 *     0x20C flags u32 (bit 0 production; bit 1 unqualified approval)
 *     0x210 program id min u64 0x218 program id max u64
 *     0x220/0x224 fs access control offset/size
 *     0x228/0x22C service access control offset/size
 *     0x230/0x234 kernel capabilities offset/size
 *     (offsets relative to the ACID start)
 *
 *   ACI0 (the "does do" set that binds the process), NPDM_ACI0_HEADER_SIZE:
 *     0x00 magic "ACI0" u32   0x10 program id u64
 *     0x20/0x24 fs access header offset/size
 *     0x28/0x2C service access control offset/size
 *     0x30/0x34 kernel capabilities offset/size
 *     (offsets relative to the ACI0 start)
 *
 *   Kernel capabilities: an array of u32 descriptors. The descriptor
 *   type is the count of trailing one-bits (3 = thread info, 4 = enable
 *   system calls, 6 = memory map, 7 = io memory map, 10 = memory region
 *   map, 11 = enable interrupts, 13 = misc params, 14 = kernel version,
 *   15 = handle table size, 16 = misc flags); 0xFFFFFFFF is padding.
 *   Field bit positions are documented at each NPDM_CAP_* constant.
 *
 * What is parsed: everything the bootstrap and kernel HLE consume
 * (thread info, SVC mask, kernel version, handle table size, program
 * type, debug flags). Memory-map / io-map / interrupt / region
 * descriptors are counted, not decoded - they describe hardware access
 * no HLE'd process is granted. The fs and service access-control blobs
 * are located, not decoded; sm: (Phase 1 stub) and fsp-srv (Phase 4)
 * decode them when they need to.
 *
 * Not verified: the ACID signature (no key material, §1.6), and that
 * the ACI0 capabilities are a subset of the ACID's (Horizon's loader
 * does; a user's own dump is not adversarial, and a mismatch would only
 * make the process less privileged than its developer intended).
 */
#ifndef SWITCH_HLE_LOADER_NPDM_H
#define SWITCH_HLE_LOADER_NPDM_H

#include <stdbool.h>
#include <stdint.h>

#include "common/result.h"

#define NPDM_META_MAGIC ((uint32_t)0x4154454D) /* "META" little-endian */
#define NPDM_ACID_MAGIC ((uint32_t)0x44494341) /* "ACID" */
#define NPDM_ACI0_MAGIC ((uint32_t)0x30494341) /* "ACI0" */

#define NPDM_META_HEADER_SIZE ((uint32_t)0x80)
#define NPDM_ACID_HEADER_SIZE ((uint32_t)0x240)
#define NPDM_ACI0_HEADER_SIZE ((uint32_t)0x40)
#define NPDM_NAME_SIZE 0x10u
#define NPDM_PRODUCT_CODE_SIZE 0x10u

/* A hostile size field must not drive an arena allocation elsewhere;
 * real files are under 8KB. */
#define NPDM_MAX_FILE_BYTES ((uint64_t)0x100000)

/* Horizon has NPDM_SVC_COUNT syscall numbers; the mask is a bitmap. */
#define NPDM_SVC_COUNT 0xC0u
#define NPDM_SVC_MASK_WORDS (NPDM_SVC_COUNT / 64u) /* 3 */
#define NPDM_MAX_CAPABILITY_COUNT 0x100u

/* Priority and core bounds the thread-info descriptor must satisfy. */
#define NPDM_PRIORITY_MAX 63u
#define NPDM_CORE_MAX 3u

typedef enum NPDM_Address_Space {
  NPDM_ADDRESS_SPACE_32_BIT = 0,
  NPDM_ADDRESS_SPACE_64_BIT_36 = 1,           /* pre-2.0 "old" 64-bit */
  NPDM_ADDRESS_SPACE_32_BIT_NO_RESERVED = 2,
  NPDM_ADDRESS_SPACE_64_BIT_39 = 3,           /* every modern title */
} NPDM_Address_Space;

typedef enum NPDM_Program_Type {
  NPDM_PROGRAM_TYPE_SYSTEM_MODULE = 0,
  NPDM_PROGRAM_TYPE_APPLICATION = 1,
  NPDM_PROGRAM_TYPE_APPLET = 2,
} NPDM_Program_Type;

/* Byte range inside the NPDM buffer, for blobs decoded by later phases. */
typedef struct NPDM_Blob {
  uint32_t offset; /* from the start of the file; validated in-bounds */
  uint32_t size;
} NPDM_Blob;

typedef struct NPDM_Kernel_Capabilities {
  /* Thread info descriptor (present at most once). */
  bool has_thread_info;
  uint8_t lowest_priority;
  uint8_t highest_priority; /* numerically <= lowest (Horizon: 0 is highest) */
  uint8_t min_core;
  uint8_t max_core;

  /* Union of every enable-system-calls descriptor. Bit n set = SVC n
   * allowed. A process with no such descriptor may call nothing. */
  uint64_t svc_mask[NPDM_SVC_MASK_WORDS];

  bool has_kernel_version;
  uint16_t kernel_version_major;
  uint8_t kernel_version_minor;

  bool has_handle_table_size;
  uint16_t handle_table_size;

  bool has_program_type;
  NPDM_Program_Type program_type;

  bool enable_debug;
  bool force_debug;

  /* Descriptors recognized but not decoded (memory maps, io maps,
   * interrupts, region maps). Logged at parse time. */
  uint32_t undecoded_descriptor_count;
} NPDM_Kernel_Capabilities;

typedef struct NPDM {
  /* META */
  bool is_64bit_instruction;
  NPDM_Address_Space address_space;
  bool optimize_memory_allocation;
  bool disable_device_address_space_merge;
  bool enable_alias_region_extra_size;
  uint8_t main_thread_priority;
  uint8_t main_thread_core_number;
  uint32_t main_thread_stack_size;
  uint32_t system_resource_size;
  uint32_t version;
  char name[NPDM_NAME_SIZE + 1];                 /* NUL-terminated copy */
  char product_code[NPDM_PRODUCT_CODE_SIZE + 1];

  /* ACID */
  bool acid_is_production;
  bool acid_unqualified_approval;
  uint64_t program_id_min;
  uint64_t program_id_max;
  NPDM_Kernel_Capabilities acid_capabilities;

  /* ACI0 - the binding set. */
  uint64_t program_id;
  NPDM_Blob fs_access_header;
  NPDM_Blob service_access_control;
  NPDM_Kernel_Capabilities capabilities;
} NPDM;

/* Parses an NPDM held in memory. Pure: no I/O, no allocation.
 *   RESULT_INVALID_ARGUMENT NULL args; size < NPDM_META_HEADER_SIZE or
 *                           > NPDM_MAX_FILE_BYTES; META/ACID/ACI0 magic
 *                           missing; ACID/ACI0 range outside the buffer
 *                           or smaller than its header; any blob or
 *                           capability range outside its container;
 *                           capability size not a multiple of 4 or over
 *                           NPDM_MAX_CAPABILITY_COUNT entries; address
 *                           space out of range; thread-info priorities
 *                           or cores out of bounds or inverted; program
 *                           id outside [program_id_min, program_id_max];
 *                           main thread stack size zero or not
 *                           page-aligned; an unknown descriptor type
 * On error `out` is unspecified. */
Error npdm_parse(const uint8_t *bytes, uint64_t size, NPDM *out);

/* True if the ACI0 capability set permits `svc_id` (< NPDM_SVC_COUNT).
 * The HLE dispatcher consults this before handling an SVC so that a
 * title calling outside its declared set faults as it would on hardware.
 * false for svc_id >= NPDM_SVC_COUNT. */
bool npdm_svc_allowed(const NPDM *npdm, uint32_t svc_id);

#endif /* SWITCH_HLE_LOADER_NPDM_H */
