/**
 * Unit tests for core/hle/loader/nca_parse.c.
 *
 * The images are synthesized plaintext containers (loader_fixtures.c):
 * a header with the NCA3 magic plus sections whose payloads are tiny
 * PFS0 / RomFS images. "Encrypted" cases are modelled by what an
 * encrypted file actually looks like to a parser - noise where the
 * magic should be - not by any cipher.
 */
#define CHECK_NAME "nca_parse_test"
#include "check.h"

#include "hle/loader/nca_parse.h"
#include "loader_fixtures.h"

#include <string.h>

#define PROGRAM_ID 0x0100ABCDEF000000ull
#define HASH_LAYER_BYTES 0x400u

static const char k_main[] = "main-nso";
static const Fixture_File k_exefs_files[] = {{"main", k_main, sizeof(k_main) - 1}};
static const char k_asset[] = "asset";
static const Fixture_RomFS_Entry k_romfs_entries[] = {{"/asset.bin", false, k_asset, 5}};

typedef struct Built {
  Fixture_Buffer exefs;
  Fixture_Buffer romfs;
  Fixture_Buffer nca;
} Built;

/* A PROGRAM NCA: section 0 = PFS0 under SHA256 layers, section 1 = RomFS
 * under IVFC layers, section 2 absent, section 3 = raw PFS0 with no hash. */
static void build_program_nca(Built *b, uint32_t magic) {
  fixture_build_pfs0(k_exefs_files, 1, &b->exefs);
  fixture_build_romfs(k_romfs_entries, 1, 0, &b->romfs);
  Fixture_NCA_Section sections[4];
  memset(sections, 0, sizeof(sections));
  sections[0] = (Fixture_NCA_Section){true, FIXTURE_NCA_FS_TYPE_PFS0, FIXTURE_NCA_HASH_SHA256, 3, &b->exefs, HASH_LAYER_BYTES};
  sections[1] = (Fixture_NCA_Section){true, FIXTURE_NCA_FS_TYPE_ROMFS, FIXTURE_NCA_HASH_IVFC, 3, &b->romfs, HASH_LAYER_BYTES};
  sections[3] = (Fixture_NCA_Section){true, FIXTURE_NCA_FS_TYPE_PFS0, FIXTURE_NCA_HASH_NONE, 1, &b->exefs, 0};
  fixture_build_nca(magic, 0 /* PROGRAM */, PROGRAM_ID, sections, &b->nca);
}

static void free_built(Built *b) {
  fixture_buffer_free(&b->exefs);
  fixture_buffer_free(&b->romfs);
  fixture_buffer_free(&b->nca);
}

static void check_program_nca(const Built *b, const NCA_File *nca, uint32_t magic) {
  const NCA_Header_Info *h = &nca->header;
  CHECK(h->magic == magic);
  CHECK(h->content_type == NCA_CONTENT_PROGRAM);
  CHECK(h->distribution_type == NCA_DISTRIBUTION_DOWNLOAD);
  CHECK(h->program_id == PROGRAM_ID);
  CHECK(h->content_size == b->nca.size);
  CHECK(h->sdk_addon_version == 0x000C0000);

  /* Section 0: PFS0 behind SHA256 layers. */
  const NCA_Section_Info *s0 = &h->sections[0];
  CHECK(s0->present);
  CHECK(s0->fs_type == NCA_FS_PARTITION_FS);
  CHECK(s0->hash_type == NCA_HASH_HIERARCHICAL_SHA256);
  CHECK(s0->encryption_type_raw == 3);
  CHECK(s0->offset == fixture_nca_section_offset(&b->nca, 0));
  CHECK(s0->data_offset == s0->offset + HASH_LAYER_BYTES);
  CHECK(s0->data_size == b->exefs.size);
  CHECK(!s0->has_patch_info && !s0->has_sparse_info && !s0->has_compression_info);
  CHECK(memcmp(b->nca.bytes + s0->data_offset, "PFS0", 4) == 0);

  /* Section 1: RomFS behind IVFC. */
  const NCA_Section_Info *s1 = &h->sections[1];
  CHECK(s1->present && s1->fs_type == NCA_FS_ROMFS);
  CHECK(s1->hash_type == NCA_HASH_HIERARCHICAL_INTEGRITY);
  CHECK(s1->data_offset == s1->offset + HASH_LAYER_BYTES);
  CHECK(s1->data_size == b->romfs.size);
  CHECK(fixture_get_le64(b->nca.bytes + s1->data_offset) == 0x50);

  CHECK(!h->sections[2].present);
  CHECK(h->sections[2].offset == 0 && h->sections[2].size == 0);

  /* Section 3: no hash layers, data == whole section. */
  const NCA_Section_Info *s3 = &h->sections[3];
  CHECK(s3->present && s3->hash_type == NCA_HASH_NONE);
  CHECK(s3->encryption_type_raw == NCA_ENCRYPTION_TYPE_NONE);
  CHECK(s3->data_offset == s3->offset && s3->data_size == s3->size);
  CHECK(s3->size % NCA_MEDIA_UNIT_SIZE == 0 && s3->size >= b->exefs.size);
}

static void test_parse_and_open(void) {
  Built b;
  build_program_nca(&b, NCA_MAGIC_NCA3);
  Byte_Source src = byte_source_from_memory(b.nca.bytes, b.nca.size);

  /* Pure header parse. */
  NCA_Header_Info info;
  CHECK_OK(nca_parse_header(b.nca.bytes, &info));
  CHECK(info.magic == NCA_MAGIC_NCA3);

  NCA_File nca;
  CHECK_OK(nca_open(&src, &nca));
  CHECK(nca.source == &src);
  check_program_nca(&b, &nca, NCA_MAGIC_NCA3);

  /* Section sources read the payload, bounded to it. */
  const Byte_Source *exefs_src = nca_section_source(&nca, 0);
  CHECK(exefs_src != NULL && exefs_src->size == b.exefs.size);
  uint8_t out[16];
  CHECK_OK(byte_source_read(exefs_src, 0, out, 4));
  CHECK(memcmp(out, "PFS0", 4) == 0);
  CHECK_CODE(byte_source_read(exefs_src, exefs_src->size, out, 1), RESULT_INVALID_ARGUMENT);
  CHECK(nca_section_source(&nca, 2) == NULL);
  CHECK(nca_section_source(&nca, 4) == NULL);
  CHECK(nca_section_source(NULL, 0) == NULL);

  /* Probes see the structure magics. */
  CHECK_OK(nca_probe_section(&nca, 0));
  CHECK_OK(nca_probe_section(&nca, 1));
  CHECK_CODE(nca_probe_section(&nca, 2), RESULT_INVALID_ARGUMENT);
  CHECK_OK(nca_probe_section(&nca, 3));
  CHECK_CODE(nca_probe_section(&nca, 4), RESULT_INVALID_ARGUMENT);

  CHECK(nca_find_section(&nca, NCA_FS_PARTITION_FS) == 0);
  CHECK(nca_find_section(&nca, NCA_FS_ROMFS) == 1);
  CHECK(nca_find_section(NULL, NCA_FS_ROMFS) == -1);

  free_built(&b);
}

static void test_nca2_parses_identically(void) {
  Built b;
  build_program_nca(&b, NCA_MAGIC_NCA2);
  Byte_Source src = byte_source_from_memory(b.nca.bytes, b.nca.size);
  NCA_File nca;
  CHECK_OK(nca_open(&src, &nca));
  check_program_nca(&b, &nca, NCA_MAGIC_NCA2);
  free_built(&b);
}

static void test_encrypted_and_bad_magics(void) {
  Built b;
  build_program_nca(&b, NCA_MAGIC_NCA3);

  /* What an encrypted NCA looks like: the header region is noise. */
  uint8_t *magic = b.nca.bytes + FIXTURE_NCA_OFFSET_MAGIC;
  memcpy(magic, "\x9b\x2e\x71\xc4", 4);
  NCA_Header_Info info;
  Error err = nca_parse_header(b.nca.bytes, &info);
  CHECK(err.code == RESULT_ENCRYPTED_INPUT);
  CHECK(strstr(err.message, "docs/DUMP.md") != NULL);
  Byte_Source src = byte_source_from_memory(b.nca.bytes, b.nca.size);
  NCA_File nca;
  CHECK_CODE(nca_open(&src, &nca), RESULT_ENCRYPTED_INPUT);

  memcpy(magic, "NCA0", 4);
  CHECK_CODE(nca_parse_header(b.nca.bytes, &info), RESULT_NOT_IMPLEMENTED);

  /* A file too small to hold a header is truncation, not encryption. */
  memcpy(magic, "NCA3", 4);
  Byte_Source small = byte_source_from_memory(b.nca.bytes, NCA_HEADER_SIZE - 1);
  err = nca_open(&small, &nca);
  CHECK(err.code == RESULT_INVALID_ARGUMENT);
  CHECK(strstr(err.message, "smaller") != NULL);

  free_built(&b);
}

static void test_half_decrypted_sections(void) {
  Built b;
  build_program_nca(&b, NCA_MAGIC_NCA3);
  Byte_Source src = byte_source_from_memory(b.nca.bytes, b.nca.size);
  NCA_File nca;
  CHECK_OK(nca_open(&src, &nca));

  /* Scramble the payload starts: header parses, probes fail as encrypted. */
  memset(b.nca.bytes + nca.header.sections[0].data_offset, 0xA5, 16);
  memset(b.nca.bytes + nca.header.sections[1].data_offset, 0xA5, 16);
  Error err = nca_probe_section(&nca, 0);
  CHECK(err.code == RESULT_ENCRYPTED_INPUT);
  CHECK(strstr(err.message, "docs/DUMP.md") != NULL);
  CHECK_CODE(nca_probe_section(&nca, 1), RESULT_ENCRYPTED_INPUT);
  CHECK_OK(nca_probe_section(&nca, 3)); /* untouched */
  free_built(&b);
}

/* ---- header field rejections -------------------------------------- */

typedef void (*Mutator)(uint8_t *header);

static uint8_t *fs_header(uint8_t *header, uint32_t index) {
  return header + FIXTURE_NCA_OFFSET_FS_HEADERS + index * FIXTURE_NCA_FS_HEADER_SIZE;
}

static void expect_header_failure(Mutator mutate, Result expected) {
  Built b;
  build_program_nca(&b, NCA_MAGIC_NCA3);
  mutate(b.nca.bytes);
  NCA_Header_Info info;
  CHECK_CODE(nca_parse_header(b.nca.bytes, &info), expected);
  free_built(&b);
}

static void mutate_content_type(uint8_t *h) { h[FIXTURE_NCA_OFFSET_CONTENT_TYPE] = 6; }
static void mutate_distribution(uint8_t *h) { h[FIXTURE_NCA_OFFSET_DISTRIBUTION] = 2; }
static void mutate_fs_version(uint8_t *h) { fixture_put_le16(fs_header(h, 0), 3); }
static void mutate_fs_type(uint8_t *h) { fs_header(h, 0)[FIXTURE_NCA_FS_OFFSET_FS_TYPE] = 2; }
static void mutate_hash_auto(uint8_t *h) { fs_header(h, 0)[FIXTURE_NCA_FS_OFFSET_HASH_TYPE] = 0; }
static void mutate_hash_out_of_range(uint8_t *h) { fs_header(h, 0)[FIXTURE_NCA_FS_OFFSET_HASH_TYPE] = 7; }
static void mutate_section_end_before_start(uint8_t *h) {
  uint8_t *entry = h + FIXTURE_NCA_OFFSET_SECTION_ENTRIES;
  fixture_put_le32(entry + 4, fixture_get_le32(entry) - 1);
}
static void mutate_sha256_region_outside(uint8_t *h) {
  fixture_put_le64(fs_header(h, 0) + FIXTURE_NCA_FS_OFFSET_SHA256_REGIONS + 0x18, 1ull << 40);
}
static void mutate_sha256_layer_count_zero(uint8_t *h) {
  fixture_put_le32(fs_header(h, 0) + FIXTURE_NCA_FS_OFFSET_SHA256_LAYER_COUNT, 0);
}
static void mutate_ivfc_magic(uint8_t *h) { fs_header(h, 1)[FIXTURE_NCA_FS_OFFSET_IVFC_MAGIC] = 'X'; }
static void mutate_ivfc_layers(uint8_t *h) {
  fixture_put_le32(fs_header(h, 1) + FIXTURE_NCA_FS_OFFSET_IVFC_MAX_LAYERS, 1);
}
static void mutate_ivfc_level_outside(uint8_t *h) {
  uint8_t *level = fs_header(h, 1) + FIXTURE_NCA_FS_OFFSET_IVFC_LEVELS + 5 * FIXTURE_NCA_FS_IVFC_LEVEL_SIZE;
  fixture_put_le64(level, UINT64_MAX - 8);
}

static void test_header_rejections(void) {
  expect_header_failure(mutate_content_type, RESULT_INVALID_ARGUMENT);
  expect_header_failure(mutate_distribution, RESULT_INVALID_ARGUMENT);
  expect_header_failure(mutate_fs_version, RESULT_INVALID_ARGUMENT);
  expect_header_failure(mutate_fs_type, RESULT_INVALID_ARGUMENT);
  expect_header_failure(mutate_hash_auto, RESULT_INVALID_ARGUMENT);
  expect_header_failure(mutate_hash_out_of_range, RESULT_INVALID_ARGUMENT);
  expect_header_failure(mutate_section_end_before_start, RESULT_INVALID_ARGUMENT);
  expect_header_failure(mutate_sha256_region_outside, RESULT_INVALID_ARGUMENT);
  expect_header_failure(mutate_sha256_layer_count_zero, RESULT_INVALID_ARGUMENT);
  expect_header_failure(mutate_ivfc_magic, RESULT_INVALID_ARGUMENT);
  expect_header_failure(mutate_ivfc_layers, RESULT_INVALID_ARGUMENT);
  expect_header_failure(mutate_ivfc_level_outside, RESULT_INVALID_ARGUMENT);

  NCA_Header_Info info;
  CHECK_CODE(nca_parse_header(NULL, &info), RESULT_INVALID_ARGUMENT);
}

static void test_open_rejections(void) {
  Built b;
  build_program_nca(&b, NCA_MAGIC_NCA3);
  NCA_File nca;

  /* content_size beyond the file. */
  fixture_put_le64(b.nca.bytes + FIXTURE_NCA_OFFSET_CONTENT_SIZE, b.nca.size + 1);
  Byte_Source src = byte_source_from_memory(b.nca.bytes, b.nca.size);
  CHECK_CODE(nca_open(&src, &nca), RESULT_INVALID_ARGUMENT);

  /* content_size that cuts a section short. */
  fixture_put_le64(b.nca.bytes + FIXTURE_NCA_OFFSET_CONTENT_SIZE, FIXTURE_NCA_HEADER_SIZE);
  CHECK_CODE(nca_open(&src, &nca), RESULT_INVALID_ARGUMENT);
  fixture_put_le64(b.nca.bytes + FIXTURE_NCA_OFFSET_CONTENT_SIZE, b.nca.size);
  CHECK_OK(nca_open(&src, &nca));

  CHECK_CODE(nca_open(NULL, &nca), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(nca_open(&src, NULL), RESULT_INVALID_ARGUMENT);
  free_built(&b);
}

static void test_unsupported_layouts(void) {
  Built b;
  build_program_nca(&b, NCA_MAGIC_NCA3);
  fs_header(b.nca.bytes, 0)[FIXTURE_NCA_FS_OFFSET_PATCH_INFO + 3] = 1;
  fs_header(b.nca.bytes, 1)[FIXTURE_NCA_FS_OFFSET_SPARSE_INFO + 0x10] = 1;
  fs_header(b.nca.bytes, 3)[FIXTURE_NCA_FS_OFFSET_COMPRESSION_INFO] = 1;
  Byte_Source src = byte_source_from_memory(b.nca.bytes, b.nca.size);
  NCA_File nca;
  CHECK_OK(nca_open(&src, &nca)); /* header still parses */
  CHECK(nca.header.sections[0].has_patch_info);
  CHECK(nca.header.sections[1].has_sparse_info);
  CHECK(nca.header.sections[3].has_compression_info);
  for (uint32_t i = 0; i < 4; i++) {
    if (i == 2) continue;
    CHECK(nca_section_source(&nca, i) == NULL);
    CHECK_CODE(nca_probe_section(&nca, i), RESULT_NOT_IMPLEMENTED);
  }
  free_built(&b);
}

int main(void) {
  test_parse_and_open();
  test_nca2_parses_identically();
  test_encrypted_and_bad_magics();
  test_half_decrypted_sections();
  test_header_rejections();
  test_open_rejections();
  test_unsupported_layouts();
  printf("[nca_parse_test] all tests passed\n");
  return 0;
}
