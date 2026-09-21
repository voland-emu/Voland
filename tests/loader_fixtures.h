/**
 * Synthesized loader images for the core/hle/loader tests.
 *
 * Every builder here restates the on-media layout independently of the
 * parser under test (offsets are spelled out again rather than imported
 * from the parser's private constants), so a parser that misreads the
 * format cannot be validated by a fixture that shares its misreading.
 * No Nintendo data is involved: contents are whatever the test supplies.
 *
 * Tests are allowed to malloc (the no-malloc rule is for core/), so the
 * growable buffer below is the only allocator these builders need.
 */
#ifndef VOLAND_TESTS_LOADER_FIXTURES_H
#define VOLAND_TESTS_LOADER_FIXTURES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Growable byte buffer.                                               */
/* ------------------------------------------------------------------ */

typedef struct Fixture_Buffer {
  uint8_t *bytes;
  size_t size;
  size_t capacity;
} Fixture_Buffer;

void fixture_buffer_init(Fixture_Buffer *buffer);
void fixture_buffer_free(Fixture_Buffer *buffer);
/* Append; returns the offset the data landed at. */
size_t fixture_append(Fixture_Buffer *buffer, const void *data, size_t size);
size_t fixture_append_zeros(Fixture_Buffer *buffer, size_t size);
/* Pad with zeros up to a multiple of `alignment`. */
void fixture_align(Fixture_Buffer *buffer, size_t alignment);

void fixture_put_le16(uint8_t *at, uint16_t value);
void fixture_put_le32(uint8_t *at, uint32_t value);
void fixture_put_le64(uint8_t *at, uint64_t value);
uint32_t fixture_get_le32(const uint8_t *at);
uint64_t fixture_get_le64(const uint8_t *at);

/* ------------------------------------------------------------------ */
/* PFS0 (ExeFS).                                                       */
/* ------------------------------------------------------------------ */

typedef struct Fixture_File {
  const char *name;
  const void *data;
  size_t size;
} Fixture_File;

#define FIXTURE_PFS0_HEADER_SIZE 0x10u
#define FIXTURE_PFS0_ENTRY_SIZE 0x18u
#define FIXTURE_PFS0_OFFSET_FILE_COUNT 0x04u
#define FIXTURE_PFS0_OFFSET_STRING_TABLE_SIZE 0x08u

void fixture_build_pfs0(const Fixture_File *files, uint32_t count, Fixture_Buffer *out);

/* ------------------------------------------------------------------ */
/* RomFS.                                                              */
/* ------------------------------------------------------------------ */

/* Paths are absolute ("/dir/file"). A directory's parent must appear
 * before it in the list; the root exists implicitly. */
typedef struct Fixture_RomFS_Entry {
  const char *path;
  bool is_dir;
  const void *data; /* files only */
  size_t size;
} Fixture_RomFS_Entry;

#define FIXTURE_ROMFS_MAX_ENTRIES 64u
#define FIXTURE_ROMFS_HEADER_SIZE 0x50u
#define FIXTURE_ROMFS_OFFSET_DIR_HASH_OFFSET 0x08u
#define FIXTURE_ROMFS_OFFSET_DIR_HASH_SIZE 0x10u
#define FIXTURE_ROMFS_OFFSET_DIR_TABLE_OFFSET 0x18u
#define FIXTURE_ROMFS_OFFSET_DIR_TABLE_SIZE 0x20u
#define FIXTURE_ROMFS_OFFSET_FILE_HASH_OFFSET 0x28u
#define FIXTURE_ROMFS_OFFSET_FILE_HASH_SIZE 0x30u
#define FIXTURE_ROMFS_OFFSET_FILE_TABLE_OFFSET 0x38u
#define FIXTURE_ROMFS_OFFSET_FILE_TABLE_SIZE 0x40u
#define FIXTURE_ROMFS_OFFSET_FILE_DATA_OFFSET 0x48u
#define FIXTURE_ROMFS_DIR_ENTRY_FIXED_SIZE 0x18u
#define FIXTURE_ROMFS_FILE_ENTRY_FIXED_SIZE 0x20u
#define FIXTURE_ROMFS_NO_ENTRY 0xFFFFFFFFu

/* Builds header | dir hash | dir table | file hash | file table | data,
 * with `bucket_count` buckets in each hash table (0 = one per entry). */
void fixture_build_romfs(const Fixture_RomFS_Entry *entries, uint32_t count,
                         uint32_t bucket_count, Fixture_Buffer *out);

/* The format's hash, restated for fixture chain construction. */
uint32_t fixture_romfs_hash(uint32_t parent, const char *name, uint32_t length);

/* ------------------------------------------------------------------ */
/* NPDM.                                                               */
/* ------------------------------------------------------------------ */

typedef struct Fixture_NPDM_Params {
  uint8_t flags;
  uint8_t main_thread_priority;
  uint8_t main_thread_core;
  uint32_t main_thread_stack_size;
  uint32_t system_resource_size;
  uint32_t version;
  const char *name;
  const char *product_code;
  uint32_t acid_flags;
  uint64_t program_id_min;
  uint64_t program_id_max;
  const uint32_t *acid_capabilities;
  uint32_t acid_capability_count;
  uint64_t program_id;
  uint32_t fs_access_size;      /* zero-filled blob */
  uint32_t service_access_size; /* zero-filled blob */
  const uint32_t *aci0_capabilities;
  uint32_t aci0_capability_count;
} Fixture_NPDM_Params;

#define FIXTURE_NPDM_META_SIZE 0x80u
#define FIXTURE_NPDM_ACID_HEADER_SIZE 0x240u
#define FIXTURE_NPDM_ACI0_HEADER_SIZE 0x40u
#define FIXTURE_NPDM_OFFSET_ACI0_OFFSET 0x70u
#define FIXTURE_NPDM_OFFSET_ACI0_SIZE 0x74u
#define FIXTURE_NPDM_OFFSET_ACID_OFFSET 0x78u
#define FIXTURE_NPDM_OFFSET_ACID_SIZE 0x7Cu
/* ACID lands right after META; ACI0 after the ACID. */
#define FIXTURE_NPDM_ACID_OFFSET FIXTURE_NPDM_META_SIZE

void fixture_build_npdm(const Fixture_NPDM_Params *params, Fixture_Buffer *out);

/* ------------------------------------------------------------------ */
/* NCA (plaintext).                                                    */
/* ------------------------------------------------------------------ */

typedef struct Fixture_NCA_Section {
  bool present;
  uint8_t fs_type;
  uint8_t hash_type;
  uint8_t encryption_type_raw;
  const Fixture_Buffer *payload;  /* the PFS0 / RomFS image */
  uint64_t hash_layer_bytes;      /* zero-filled prefix standing in for hash layers */
} Fixture_NCA_Section;

#define FIXTURE_NCA_HEADER_SIZE 0xC00u
#define FIXTURE_NCA_MEDIA_UNIT 0x200u
#define FIXTURE_NCA_OFFSET_MAGIC 0x200u
#define FIXTURE_NCA_OFFSET_DISTRIBUTION 0x204u
#define FIXTURE_NCA_OFFSET_CONTENT_TYPE 0x205u
#define FIXTURE_NCA_OFFSET_CONTENT_SIZE 0x208u
#define FIXTURE_NCA_OFFSET_PROGRAM_ID 0x210u
#define FIXTURE_NCA_OFFSET_CONTENT_INDEX 0x218u
#define FIXTURE_NCA_OFFSET_SDK_VERSION 0x21Cu
#define FIXTURE_NCA_OFFSET_SECTION_ENTRIES 0x240u
#define FIXTURE_NCA_OFFSET_FS_HEADERS 0x400u
#define FIXTURE_NCA_FS_HEADER_SIZE 0x200u
#define FIXTURE_NCA_FS_OFFSET_VERSION 0x00u
#define FIXTURE_NCA_FS_OFFSET_FS_TYPE 0x02u
#define FIXTURE_NCA_FS_OFFSET_HASH_TYPE 0x03u
#define FIXTURE_NCA_FS_OFFSET_ENCRYPTION_TYPE 0x04u
#define FIXTURE_NCA_FS_OFFSET_SHA256_LAYER_COUNT 0x2Cu
#define FIXTURE_NCA_FS_OFFSET_SHA256_REGIONS 0x30u
#define FIXTURE_NCA_FS_OFFSET_IVFC_MAGIC 0x08u
#define FIXTURE_NCA_FS_OFFSET_IVFC_MAX_LAYERS 0x14u
#define FIXTURE_NCA_FS_OFFSET_IVFC_LEVELS 0x18u
#define FIXTURE_NCA_FS_IVFC_LEVEL_SIZE 0x18u
#define FIXTURE_NCA_FS_OFFSET_PATCH_INFO 0x100u
#define FIXTURE_NCA_FS_OFFSET_SPARSE_INFO 0x148u
#define FIXTURE_NCA_FS_OFFSET_COMPRESSION_INFO 0x178u
#define FIXTURE_NCA_HASH_NONE 1u
#define FIXTURE_NCA_HASH_SHA256 2u
#define FIXTURE_NCA_HASH_IVFC 3u
#define FIXTURE_NCA_FS_TYPE_ROMFS 0u
#define FIXTURE_NCA_FS_TYPE_PFS0 1u

/* Offset of section i's start within the built image (media aligned). */
uint64_t fixture_nca_section_offset(const Fixture_Buffer *nca, uint32_t index);

void fixture_build_nca(uint32_t magic, uint8_t content_type, uint64_t program_id,
                       const Fixture_NCA_Section sections[4], Fixture_Buffer *out);

#endif /* VOLAND_TESTS_LOADER_FIXTURES_H */
