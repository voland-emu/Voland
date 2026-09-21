/**
 * core/hle/loader/nso unit test over fixture_build_nso() images.
 */
#define CHECK_NAME "nso_test"
#include "check.h"

#include "common/arena.h"
#include "hle/loader/byte_source.h"
#include "hle/loader/nso.h"
#include "loader_fixtures.h"

#include <stdlib.h>
#include <string.h>

#define SCRATCH_BYTES (256 * 1024)
#define PAGE 0x1000u

/* Segment plaintexts: .text is compressible (repeating), .rodata is a
 * counter (incompressible-ish, exercises literal runs), .data is short. */
#define TEXT_BYTES (3 * PAGE + 123)
#define RODATA_BYTES (PAGE + 7)
#define DATA_BYTES 300u

static uint8_t g_text[TEXT_BYTES];
static uint8_t g_rodata[RODATA_BYTES];
static uint8_t g_data[DATA_BYTES];
static const uint8_t k_module_id[0x20] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03,
                                         0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
                                         0x9A, 0xBC, 0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44,
                                         0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xFF};

static void fill_plaintexts(void) {
  for (size_t i = 0; i < TEXT_BYTES; i++) g_text[i] = (uint8_t)"ARM64!"[i % 6];
  for (size_t i = 0; i < RODATA_BYTES; i++) g_rodata[i] = (uint8_t)(i * 7 + (i >> 5));
  for (size_t i = 0; i < DATA_BYTES; i++) g_data[i] = (uint8_t)(0xA0 + i % 13);
}

static Fixture_NSO_Params default_params(bool compress_text, bool compress_rodata,
                                         bool compress_data) {
  Fixture_NSO_Params p;
  memset(&p, 0, sizeof(p));
  p.text.data = g_text;
  p.text.size = TEXT_BYTES;
  p.text.memory_offset = 0;
  p.text.compress = compress_text;
  p.text.hash_flag = true;
  p.rodata.data = g_rodata;
  p.rodata.size = RODATA_BYTES;
  p.rodata.memory_offset = 4 * PAGE;
  p.rodata.compress = compress_rodata;
  p.data.data = g_data;
  p.data.size = DATA_BYTES;
  p.data.memory_offset = 6 * PAGE;
  p.data.compress = compress_data;
  p.bss_size = 0x2345;
  p.module_id = k_module_id;
  p.dynstr_offset = 0x10;
  p.dynstr_size = 0x40;
  p.dynsym_offset = 0x50;
  p.dynsym_size = 0x30;
  return p;
}

/* Reads every segment back and compares it to the plaintext. */
static void check_segments_round_trip(const NSO *nso, Arena *scratch) {
  static uint8_t out[TEXT_BYTES + 16];
  static const struct {
    NSO_Segment_Kind kind;
    const uint8_t *plaintext;
    size_t size;
  } cases[] = {
      {NSO_SEGMENT_TEXT, g_text, TEXT_BYTES},
      {NSO_SEGMENT_RODATA, g_rodata, RODATA_BYTES},
      {NSO_SEGMENT_DATA, g_data, DATA_BYTES},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    memset(out, 0xCC, sizeof(out));
    arena_reset(scratch);
    CHECK_OK(nso_read_segment(nso, cases[i].kind, scratch, out, sizeof(out)));
    CHECK(memcmp(out, cases[i].plaintext, cases[i].size) == 0);
    /* Bytes past memory_size are untouched. */
    CHECK(out[cases[i].size] == 0xCC);
  }
}

static void test_open_and_read(bool ct, bool cr, bool cd) {
  Fixture_NSO_Params p = default_params(ct, cr, cd);
  p.module_name = "smoke_module";
  p.extra_flags = 0x40; /* execute-only text (20.0.0+), carried not acted on */
  Fixture_Buffer image;
  fixture_build_nso(&p, &image);
  const Byte_Source source = byte_source_from_memory(image.bytes, image.size);

  NSO nso;
  CHECK_OK(nso_open(&source, &nso));
  CHECK(nso.source == &source);
  CHECK(nso.version == 0);
  CHECK(nso.execute_only_text);
  CHECK(nso.segments[NSO_SEGMENT_TEXT].is_compressed == ct);
  CHECK(nso.segments[NSO_SEGMENT_RODATA].is_compressed == cr);
  CHECK(nso.segments[NSO_SEGMENT_DATA].is_compressed == cd);
  CHECK(nso.segments[NSO_SEGMENT_TEXT].has_hash);
  CHECK(!nso.segments[NSO_SEGMENT_RODATA].has_hash);
  CHECK(nso.segments[NSO_SEGMENT_TEXT].memory_size == TEXT_BYTES);
  CHECK(nso.segments[NSO_SEGMENT_RODATA].memory_size == RODATA_BYTES);
  CHECK(nso.segments[NSO_SEGMENT_DATA].memory_size == DATA_BYTES);
  CHECK(nso.segments[NSO_SEGMENT_TEXT].memory_offset == 0);
  CHECK(nso.segments[NSO_SEGMENT_RODATA].memory_offset == 4 * PAGE);
  CHECK(nso.segments[NSO_SEGMENT_DATA].memory_offset == 6 * PAGE);
  /* A compressed segment is smaller in the file; a raw one is not. */
  if (ct) CHECK(nso.segments[NSO_SEGMENT_TEXT].file_size < TEXT_BYTES);
  else CHECK(nso.segments[NSO_SEGMENT_TEXT].file_size == TEXT_BYTES);
  /* File offsets must point at the payloads: independent recomputation
   * from the fixture's own header fields. */
  CHECK(nso.segments[NSO_SEGMENT_TEXT].file_offset ==
        fixture_get_le32(image.bytes + FIXTURE_NSO_OFFSET_TEXT_SEGMENT));
  CHECK(nso.bss_size == 0x2345);
  /* data end = 6 pages + 300 + 0x2345 -> rounds to 9 pages. */
  CHECK(nso.image_size == 9 * PAGE);
  CHECK(memcmp(nso.module_id, k_module_id, 0x20) == 0);
  CHECK(strcmp(nso.module_name, "smoke_module") == 0);
  CHECK(nso.api_info.size == 0);
  CHECK(nso.dynstr.offset == 0x10 && nso.dynstr.size == 0x40);
  CHECK(nso.dynsym.offset == 0x50 && nso.dynsym.size == 0x30);

  char hex[NSO_MODULE_ID_HEX_BYTES];
  CHECK_OK(nso_module_id_hex(&nso, hex));
  CHECK(strlen(hex) == 64);
  CHECK(strncmp(hex, "deadbeef00010203", 16) == 0);
  CHECK(strcmp(hex + 56, "99aabbff") == 0);

  Arena scratch;
  CHECK(arena_create(&scratch, SCRATCH_BYTES));
  check_segments_round_trip(&nso, &scratch);

  /* A raw segment needs no scratch; a compressed one refuses NULL. */
  static uint8_t out[TEXT_BYTES];
  if (!ct) CHECK_OK(nso_read_segment(&nso, NSO_SEGMENT_TEXT, NULL, out, sizeof(out)));
  else CHECK_CODE(nso_read_segment(&nso, NSO_SEGMENT_TEXT, NULL, out, sizeof(out)),
                  RESULT_INVALID_ARGUMENT);

  arena_destroy(&scratch);
  fixture_buffer_free(&image);
}

static void test_retail_style_empty_name(void) {
  Fixture_NSO_Params p = default_params(true, true, true);
  Fixture_Buffer image;
  fixture_build_nso(&p, &image);
  const Byte_Source source = byte_source_from_memory(image.bytes, image.size);
  NSO nso;
  CHECK_OK(nso_open(&source, &nso));
  CHECK(nso.module_name[0] == '\0');
  fixture_buffer_free(&image);
}

/* Opens a freshly built default image after `mutate` has edited it. */
static Error open_mutated(void (*mutate)(Fixture_Buffer *)) {
  Fixture_NSO_Params p = default_params(true, false, true);
  Fixture_Buffer image;
  fixture_build_nso(&p, &image);
  mutate(&image);
  const Byte_Source source = byte_source_from_memory(image.bytes, image.size);
  NSO nso;
  const Error err = nso_open(&source, &nso);
  fixture_buffer_free(&image);
  return err;
}

static void mutate_magic(Fixture_Buffer *b) { b->bytes[0] = 'X'; }
static void mutate_version(Fixture_Buffer *b) {
  fixture_put_le32(b->bytes + FIXTURE_NSO_OFFSET_VERSION, 1);
}
static void mutate_zstd_flag(Fixture_Buffer *b) {
  fixture_put_le32(b->bytes + FIXTURE_NSO_OFFSET_FLAGS,
                   fixture_get_le32(b->bytes + FIXTURE_NSO_OFFSET_FLAGS) | 0x80u);
}
static void mutate_text_file_range(Fixture_Buffer *b) {
  fixture_put_le32(b->bytes + FIXTURE_NSO_OFFSET_TEXT_FILE_SIZE, (uint32_t)b->size);
}
static void mutate_text_file_offset(Fixture_Buffer *b) {
  fixture_put_le32(b->bytes + FIXTURE_NSO_OFFSET_TEXT_SEGMENT, 0xFFFFFFF0u);
}
static void mutate_size_over_cap(Fixture_Buffer *b) {
  fixture_put_le32(b->bytes + FIXTURE_NSO_OFFSET_TEXT_SEGMENT + 8, 0x20000001u);
}
static void mutate_raw_size_mismatch(Fixture_Buffer *b) {
  /* .rodata is raw in this image: shrink its memory size by one. */
  fixture_put_le32(b->bytes + FIXTURE_NSO_OFFSET_RODATA_SEGMENT + 8, RODATA_BYTES - 1);
}
static void mutate_unaligned(Fixture_Buffer *b) {
  fixture_put_le32(b->bytes + FIXTURE_NSO_OFFSET_DATA_SEGMENT + 4, 6 * PAGE + 8);
}
static void mutate_overlap(Fixture_Buffer *b) {
  /* .rodata moved down onto .text (3 pages + 123 bytes long). */
  fixture_put_le32(b->bytes + FIXTURE_NSO_OFFSET_RODATA_SEGMENT + 4, 3 * PAGE);
}
static void mutate_out_of_order(Fixture_Buffer *b) {
  fixture_put_le32(b->bytes + FIXTURE_NSO_OFFSET_DATA_SEGMENT + 4, 0);
}
static void mutate_dynsym_outside(Fixture_Buffer *b) {
  fixture_put_le32(b->bytes + FIXTURE_NSO_OFFSET_DYNSYM, RODATA_BYTES - 8);
  fixture_put_le32(b->bytes + FIXTURE_NSO_OFFSET_DYNSYM + 4, 9);
}
static void mutate_name_outside(Fixture_Buffer *b) {
  fixture_put_le32(b->bytes + FIXTURE_NSO_OFFSET_MODULE_NAME_SIZE, (uint32_t)b->size);
}

static void test_rejections(void) {
  CHECK_CODE(open_mutated(mutate_magic), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(open_mutated(mutate_version), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(open_mutated(mutate_zstd_flag), RESULT_NOT_IMPLEMENTED);
  CHECK_CODE(open_mutated(mutate_text_file_range), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(open_mutated(mutate_text_file_offset), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(open_mutated(mutate_size_over_cap), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(open_mutated(mutate_raw_size_mismatch), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(open_mutated(mutate_unaligned), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(open_mutated(mutate_overlap), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(open_mutated(mutate_out_of_order), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(open_mutated(mutate_dynsym_outside), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(open_mutated(mutate_name_outside), RESULT_INVALID_ARGUMENT);

  /* Too short for a header at all. */
  uint8_t stub[0x20] = {'N', 'S', 'O', '0'};
  const Byte_Source short_source = byte_source_from_memory(stub, sizeof(stub));
  NSO nso;
  CHECK_CODE(nso_open(&short_source, &nso), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(nso_open(NULL, &nso), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(nso_open(&short_source, NULL), RESULT_INVALID_ARGUMENT);
}

static void test_read_segment_errors(void) {
  Fixture_NSO_Params p = default_params(true, true, true);
  Fixture_Buffer image;
  fixture_build_nso(&p, &image);
  const Byte_Source source = byte_source_from_memory(image.bytes, image.size);
  NSO nso;
  CHECK_OK(nso_open(&source, &nso));

  static uint8_t out[TEXT_BYTES];
  Arena scratch;
  CHECK(arena_create(&scratch, SCRATCH_BYTES));

  CHECK_CODE(nso_read_segment(NULL, NSO_SEGMENT_TEXT, &scratch, out, sizeof(out)),
             RESULT_INVALID_ARGUMENT);
  CHECK_CODE(nso_read_segment(&nso, NSO_SEGMENT_TEXT, &scratch, NULL, sizeof(out)),
             RESULT_INVALID_ARGUMENT);
  CHECK_CODE(nso_read_segment(&nso, (NSO_Segment_Kind)3, &scratch, out, sizeof(out)),
             RESULT_INVALID_ARGUMENT);
  CHECK_CODE(nso_read_segment(&nso, NSO_SEGMENT_TEXT, &scratch, out, TEXT_BYTES - 1),
             RESULT_INVALID_ARGUMENT);

  /* Scratch too small for the compressed bytes. */
  Arena tiny;
  CHECK(arena_create(&tiny, 16));
  CHECK_CODE(nso_read_segment(&nso, NSO_SEGMENT_TEXT, &tiny, out, sizeof(out)),
             RESULT_OUT_OF_MEMORY);
  arena_destroy(&tiny);

  /* Corrupt the LZ4 stream: a match offset of zero is always invalid.
   * Find the first sequence with a match by scanning for the token whose
   * literal run is followed by an offset; simpler: zero the whole
   * payload, which yields tokens with impossible literal lengths. */
  const NSO_Segment *text = &nso.segments[NSO_SEGMENT_TEXT];
  memset(image.bytes + text->file_offset, 0, text->file_size);
  arena_reset(&scratch);
  CHECK_CODE(nso_read_segment(&nso, NSO_SEGMENT_TEXT, &scratch, out, sizeof(out)),
             RESULT_INVALID_ARGUMENT);

  /* Decoded length shorter than declared: shrink the compressed size so
   * the stream ends early (LZ4_decompress_safe reports malformed) - and
   * separately, declare a larger memory size than the stream produces. */
  fixture_buffer_free(&image);
  fixture_build_nso(&p, &image);
  const Byte_Source rebuilt = byte_source_from_memory(image.bytes, image.size);
  CHECK_OK(nso_open(&rebuilt, &nso));
  nso.segments[NSO_SEGMENT_DATA].memory_size = DATA_BYTES + 1;
  arena_reset(&scratch);
  CHECK_CODE(nso_read_segment(&nso, NSO_SEGMENT_DATA, &scratch, out, sizeof(out)),
             RESULT_INVALID_ARGUMENT);

  arena_destroy(&scratch);
  fixture_buffer_free(&image);
}

int main(void) {
  fill_plaintexts();
  test_open_and_read(true, true, true);
  test_open_and_read(false, false, false);
  test_open_and_read(true, false, true);
  test_retail_style_empty_name();
  test_rejections();
  test_read_segment_errors();
  return 0;
}
