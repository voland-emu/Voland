/**
 * NSO reader: a Horizon executable (`rtld`, `main`, `subsdk0`..`subsdk9`,
 * `sdk` inside the ExeFS). See docs/DESIGN.md §12 ("Process bootstrap",
 * step 1): three segments (.text, .rodata, .data), each optionally
 * LZ4-block-compressed in the file, plus a .bss size. This header is
 * the parse-and-decompress half only; choosing a base address, mapping
 * pages and applying permissions is the bootstrap's job (§12 step 3,
 * "Load executable sections into guest memory through vmm mappings"),
 * so nothing here knows about vmm.
 *
 * Layout (offsets from the start of the file; all little-endian):
 *
 *   0x00 magic "NSO0" u32         0x04 version u32 (NSO_VERSION)
 *   0x0C flags u32 (NSO_FLAG_*)
 *   0x10 .text   file offset u32, memory offset u32, size u32
 *   0x1C module name file offset u32
 *   0x20 .rodata file offset u32, memory offset u32, size u32
 *   0x2C module name size u32
 *   0x30 .data   file offset u32, memory offset u32, size u32
 *   0x3C bss size u32
 *   0x40 module id u8[0x20]       (the ELF GNU build id; §19 mod matching)
 *   0x60 .text file size u32      (compressed size when NSO_FLAG_TEXT_COMPRESSED)
 *   0x64 .rodata file size u32    0x68 .data file size u32
 *   0x88 api_info offset u32, size u32   } relative to the start of
 *   0x90 dynstr   offset u32, size u32   } .rodata in MEMORY (not the
 *   0x98 dynsym   offset u32, size u32   } file); zero size = absent
 *   0xA0 .text SHA-256   0xC0 .rodata SHA-256   0xE0 .data SHA-256
 *   0x100 segment payloads, at each segment's file offset
 *
 * A segment's "size" is its decompressed (memory) extent; its "file
 * size" is how many bytes the file holds for it - equal to size when the
 * segment is stored raw, smaller when compressed. Compression is the LZ4
 * BLOCK format (no frame header, no checksums): one block per segment.
 *
 * Validation (the input is untrusted for memory safety, §19, never for
 * authenticity): magic and version; every segment's file range inside
 * the source; decompressed sizes under NSO_MAX_SEGMENT_BYTES so a hostile
 * header cannot drive a huge destination; a raw segment's file size equal
 * to its size; segment memory offsets page-aligned (Horizon maps each
 * segment as its own permissioned region, and permissions are page-
 * granular, so a segment starting mid-page cannot exist on hardware),
 * ascending .text < .rodata < .data and non-overlapping; the .rodata
 * sub-sections inside .rodata when present. The per-segment SHA-256
 * hashes are NOT verified, per the §12 loader note (they guard encrypted
 * media against tampering; a user's own plaintext dump has no adversary)
 * - the hash flags are carried as diagnostics only.
 *
 * NSO_FLAG_ZSTD_COMPRESSED (firmware 22.0.0+ "zbic" NSOs) is recognized
 * and refused with RESULT_NOT_IMPLEMENTED: decoding it as LZ4 would
 * produce garbage that only fails later, in the guest.
 *
 * Memory: nso_open allocates nothing - the descriptor is a fixed-size
 * struct and the header is read into a stack buffer. nso_read_segment
 * stages a compressed segment's file bytes in the caller's arena
 * (file_size bytes) before decoding into `out`; a raw segment is read
 * straight into `out` and touches the arena not at all. Peak per-segment
 * footprint is therefore file_size + memory_size. The source must
 * outlive the NSO.
 */
#ifndef SWITCH_HLE_LOADER_NSO_H
#define SWITCH_HLE_LOADER_NSO_H

#include <stdbool.h>
#include <stdint.h>

#include "common/arena.h"
#include "common/result.h"
#include "hle/loader/byte_source.h"

#define NSO_MAGIC ((uint32_t)0x304F534E) /* "NSO0" little-endian */
#define NSO_VERSION ((uint32_t)0)
#define NSO_HEADER_SIZE ((uint64_t)0x100)
#define NSO_MODULE_ID_SIZE 0x20u
#define NSO_SEGMENT_COUNT 3u

/* Header flag bits (0x0C). */
#define NSO_FLAG_TEXT_COMPRESSED ((uint32_t)1 << 0)
#define NSO_FLAG_RODATA_COMPRESSED ((uint32_t)1 << 1)
#define NSO_FLAG_DATA_COMPRESSED ((uint32_t)1 << 2)
#define NSO_FLAG_TEXT_HASH ((uint32_t)1 << 3)
#define NSO_FLAG_RODATA_HASH ((uint32_t)1 << 4)
#define NSO_FLAG_DATA_HASH ((uint32_t)1 << 5)
#define NSO_FLAG_EXECUTE_ONLY_TEXT ((uint32_t)1 << 6) /* 20.0.0+: .text is X, not RX */
#define NSO_FLAG_ZSTD_COMPRESSED ((uint32_t)1 << 7)   /* 22.0.0+: refused */

/* Segment memory offsets must be multiples of this (== VMM_PAGE_SIZE;
 * restated here so the parser stays independent of vmm.h). */
#define NSO_SEGMENT_ALIGNMENT ((uint32_t)0x1000)

/* The largest `main` in a shipping title decompresses to well under
 * 256MB; the cap bounds what a hostile size field can make the caller
 * allocate. Sizes over it are RESULT_INVALID_ARGUMENT. */
#define NSO_MAX_SEGMENT_BYTES ((uint64_t)0x20000000) /* 512MB */

/* Retail NSOs carry an empty module name (size 1, the NUL). Longer
 * names are copied up to this many characters and truncated beyond. */
#define NSO_MAX_MODULE_NAME_BYTES 0xFFu

typedef enum NSO_Segment_Kind {
  NSO_SEGMENT_TEXT = 0,
  NSO_SEGMENT_RODATA = 1,
  NSO_SEGMENT_DATA = 2,
} NSO_Segment_Kind;

typedef struct NSO_Segment {
  uint32_t file_offset;   /* absolute within the source */
  uint32_t file_size;     /* bytes stored in the file (compressed size if is_compressed) */
  uint32_t memory_offset; /* from the image base; NSO_SEGMENT_ALIGNMENT multiple */
  uint32_t memory_size;   /* decompressed extent */
  bool is_compressed;     /* LZ4 block in the file */
  bool has_hash;          /* header carries a SHA-256; never verified */
} NSO_Segment;

/* A sub-region of .rodata (api_info, dynstr, dynsym). Offsets are from
 * the start of .rodata in memory. size == 0 means absent. */
typedef struct NSO_Rodata_Section {
  uint32_t offset;
  uint32_t size;
} NSO_Rodata_Section;

typedef struct NSO {
  const Byte_Source *source; /* caller's; must outlive this struct */
  uint32_t version;
  uint32_t flags;            /* raw header flags, for diagnostics */
  NSO_Segment segments[NSO_SEGMENT_COUNT]; /* indexed by NSO_Segment_Kind */
  uint32_t bss_size;
  /* Total span the bootstrap must reserve: from memory offset 0 to the
   * end of .bss, rounded up to NSO_SEGMENT_ALIGNMENT. */
  uint64_t image_size;
  bool execute_only_text;
  uint8_t module_id[NSO_MODULE_ID_SIZE];
  char module_name[NSO_MAX_MODULE_NAME_BYTES + 1]; /* NUL-terminated copy */
  NSO_Rodata_Section api_info;
  NSO_Rodata_Section dynstr;
  NSO_Rodata_Section dynsym;
} NSO;

/* Parses the header at offset 0 of `source`. Reads NSO_HEADER_SIZE bytes
 * plus the module name; nothing else is touched.
 *   RESULT_INVALID_ARGUMENT NULL args; source shorter than the header;
 *                           magic missing; version != NSO_VERSION; a
 *                           segment's file range outside the source; a
 *                           segment size over NSO_MAX_SEGMENT_BYTES; a
 *                           raw segment whose file size != size; a
 *                           memory offset not NSO_SEGMENT_ALIGNMENT-
 *                           aligned; segments not ascending text <
 *                           rodata < data or overlapping in memory; a
 *                           .rodata sub-section outside .rodata; the
 *                           module name range outside the source
 *   RESULT_NOT_IMPLEMENTED  NSO_FLAG_ZSTD_COMPRESSED set
 *   RESULT_IO_ERROR         propagated
 * On error `out` is unspecified. */
Error nso_open(const Byte_Source *source, NSO *out);

/* Produces the decompressed bytes of one segment in `out`, which must
 * hold at least segment.memory_size bytes (out_size is checked; extra
 * bytes are left untouched - .bss zeroing is the bootstrap's). A raw
 * segment is read directly; a compressed one is staged in `scratch`
 * (file_size bytes) and LZ4-decoded. The caller resets `scratch`.
 *   RESULT_INVALID_ARGUMENT NULL args; kind out of range; out_size <
 *                           memory_size; malformed LZ4 stream (truncated
 *                           sequence, match offset before the start of
 *                           the output, literal/match running past the
 *                           output); decoded length != memory_size
 *   RESULT_OUT_OF_MEMORY    scratch arena exhausted
 *   RESULT_IO_ERROR         propagated
 * On error `out` is unspecified. `scratch` may be NULL only when the
 * segment is not compressed. */
Error nso_read_segment(const NSO *nso, NSO_Segment_Kind kind, Arena *scratch,
                       uint8_t *out, uint64_t out_size);

/* Lower-case hex rendering of module_id (the §19 buildId form). `out`
 * must hold NSO_MODULE_ID_SIZE * 2 + 1 bytes. RESULT_INVALID_ARGUMENT on
 * NULL. Pure. */
#define NSO_MODULE_ID_HEX_BYTES (NSO_MODULE_ID_SIZE * 2u + 1u)
Error nso_module_id_hex(const NSO *nso, char *out);

#endif /* SWITCH_HLE_LOADER_NSO_H */
