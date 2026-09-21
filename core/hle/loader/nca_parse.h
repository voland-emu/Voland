/**
 * Decrypted-NCA container parser. See docs/DESIGN.md §1.6, §12.
 *
 * PRE-DECRYPTED INPUT ONLY. This file describes the plaintext NCA layout
 * as produced by the user's separate dumping/decryption tools (hactool
 * `--plaintext`, nxdumptool, or equivalent). It contains no cipher, no
 * key material, no key derivation, and never will (CLAUDE.md rule 1).
 * Nintendo's cipher-mode enumerants are deliberately NOT named here: the
 * FsHeader's encryption-type byte is carried through as a raw value for
 * diagnostics only, and the parser never branches on it.
 *
 * Detecting non-decrypted input (§12 "Loader subsystem note"): an
 * encrypted NCA's header is ciphertext, so its magic at NCA_MAGIC_OFFSET
 * is noise. A missing magic yields RESULT_ENCRYPTED_INPUT with a message
 * naming docs/DUMP.md - the same answer serves "still encrypted" and
 * "not an NCA at all", because the user's next step is identical. A
 * header that parses but whose sections lack their own structure magic
 * (PFS0 / RomFS header) is a half-decrypted dump and gets the same
 * result code from nca_probe_section() with a section-specific message.
 *
 * Plaintext NCA layout (offsets in bytes from the start of the file):
 *
 *   0x000  signatures                 2 x 0x100  ignored (no verification)
 *   0x200  magic                      u32        "NCA3" (also "NCA2": same
 *                                                plaintext layout; "NCA0"
 *                                                is a pre-release layout,
 *                                                RESULT_NOT_IMPLEMENTED)
 *   0x204  distribution type          u8
 *   0x205  content type               u8         NCA_Content_Type
 *   0x208  content size               u64        bytes; must be <= source size
 *   0x210  program id                 u64
 *   0x218  content index              u32
 *   0x21C  sdk addon version          u32
 *   0x240  section entries            4 x 0x10   { start u32, end u32 } in
 *                                                NCA_MEDIA_UNIT_SIZE units;
 *                                                both zero = section absent
 *   0x280  section header hashes      4 x 0x20   ignored (no verification)
 *   0x300  key area                   0x40       IGNORED. Never read.
 *   0x400  fs headers                 4 x 0x200  one per section, see below
 *   0xC00  end of header
 *
 * FsHeader (0x200 bytes, offsets relative to the FsHeader):
 *
 *   0x000  version                    u16        NCA_FS_HEADER_VERSION
 *   0x002  fs type                    u8         NCA_Fs_Type
 *   0x003  hash type                  u8         NCA_Hash_Type
 *   0x004  encryption type            u8         raw; reported, not used
 *   0x008  hash data                  0xF8       layout depends on hash type:
 *            HIERARCHICAL_SHA256 (and its SHA3 twin):
 *              0x008 master hash 0x20 (ignored), 0x028 block size u32,
 *              0x02C layer count u32, 0x030 layer regions
 *              NCA_SHA256_MAX_LAYERS x { offset u64, size u64 }; the LAST
 *              layer is the filesystem data
 *            HIERARCHICAL_INTEGRITY (and its SHA3 twin):
 *              0x008 "IVFC" u32, 0x00C version u32, 0x010 master hash size
 *              u32, 0x014 max layers u32, 0x018 levels
 *              NCA_IVFC_MAX_LEVELS x { logical offset u64, size u64,
 *              block size log2 u32, reserved u32 }; the level at index
 *              max_layers - 2 is the filesystem data
 *            NONE: the whole section is filesystem data
 *          All region/level offsets are relative to the section start.
 *   0x100  patch info                 0x40       nonzero = update NCA that
 *                                                needs its base; recorded,
 *                                                unsupported for now
 *   0x148  sparse info                0x30       nonzero = sparse layout;
 *                                                recorded, unsupported
 *   0x178  compression info           0x28       nonzero = compressed
 *                                                layout; recorded, unsupported
 *
 * Nothing here is verified cryptographically: hashes exist to detect
 * tampering of encrypted media, and a user's own plaintext dump has no
 * adversary to guard against. Bounds are what the parser enforces - the
 * input is untrusted for memory-safety purposes (§19), never for
 * authenticity.
 *
 * Memory: NCA_File is a plain value the caller owns; nca_open allocates
 * nothing. Section sources are embedded slices (byte_source.h) over the
 * caller's Byte_Source, which must outlive the NCA_File.
 */
#ifndef SWITCH_HLE_LOADER_NCA_PARSE_H
#define SWITCH_HLE_LOADER_NCA_PARSE_H

#include <stdbool.h>
#include <stdint.h>

#include "common/result.h"
#include "hle/loader/byte_source.h"

/* ------------------------------------------------------------------ */
/* Layout constants.                                                   */
/* ------------------------------------------------------------------ */

#define NCA_HEADER_SIZE ((uint64_t)0xC00)
#define NCA_SECTION_COUNT 4u
#define NCA_MEDIA_UNIT_SIZE ((uint64_t)0x200)
#define NCA_FS_HEADER_SIZE ((uint64_t)0x200)
#define NCA_FS_HEADER_VERSION ((uint16_t)2)

#define NCA_MAGIC_OFFSET ((uint64_t)0x200)
#define NCA_MAGIC_NCA3 ((uint32_t)0x3341434E) /* "NCA3" little-endian */
#define NCA_MAGIC_NCA2 ((uint32_t)0x3241434E) /* "NCA2" */
#define NCA_MAGIC_NCA0 ((uint32_t)0x3041434E) /* "NCA0" - rejected */

#define NCA_SHA256_MAX_LAYERS 5u
#define NCA_IVFC_MAGIC ((uint32_t)0x43465649) /* "IVFC" */
#define NCA_IVFC_MAX_LEVELS 6u

/* ------------------------------------------------------------------ */
/* Enumerations (values are the on-media encodings).                   */
/* ------------------------------------------------------------------ */

typedef enum NCA_Distribution_Type {
  NCA_DISTRIBUTION_DOWNLOAD = 0,
  NCA_DISTRIBUTION_GAMECARD = 1,
} NCA_Distribution_Type;

typedef enum NCA_Content_Type {
  NCA_CONTENT_PROGRAM = 0,     /* ExeFS (section 0) + RomFS (section 1) */
  NCA_CONTENT_META = 1,        /* CNMT */
  NCA_CONTENT_CONTROL = 2,     /* NACP + icons, RomFS */
  NCA_CONTENT_MANUAL = 3,      /* HTML manual / legal info, RomFS */
  NCA_CONTENT_DATA = 4,
  NCA_CONTENT_PUBLIC_DATA = 5,
} NCA_Content_Type;

typedef enum NCA_Fs_Type {
  NCA_FS_ROMFS = 0,
  NCA_FS_PARTITION_FS = 1, /* PFS0; the ExeFS */
} NCA_Fs_Type;

typedef enum NCA_Hash_Type {
  NCA_HASH_AUTO = 0,                        /* unresolved; RESULT_INVALID_ARGUMENT */
  NCA_HASH_NONE = 1,
  NCA_HASH_HIERARCHICAL_SHA256 = 2,
  NCA_HASH_HIERARCHICAL_INTEGRITY = 3,      /* IVFC */
  NCA_HASH_AUTO_SHA3 = 4,                   /* unresolved; RESULT_INVALID_ARGUMENT */
  NCA_HASH_HIERARCHICAL_SHA3_256 = 5,       /* same layout as SHA256 */
  NCA_HASH_HIERARCHICAL_INTEGRITY_SHA3 = 6, /* same layout as INTEGRITY */
} NCA_Hash_Type;

/* Only the one value the parser can act on is named. Any other raw value
 * means the original media was ciphered; a correctly decrypted dump has
 * plaintext in the section regardless, so the parser never consults this
 * beyond echoing it in diagnostics. */
#define NCA_ENCRYPTION_TYPE_NONE ((uint8_t)1)

/* ------------------------------------------------------------------ */
/* Parsed header.                                                      */
/* ------------------------------------------------------------------ */

typedef struct NCA_Section_Info {
  bool present;               /* false: every other field is zero */
  uint64_t offset;            /* section start, bytes from NCA start */
  uint64_t size;              /* section bytes (end - start, media units) */
  NCA_Fs_Type fs_type;
  NCA_Hash_Type hash_type;
  uint8_t encryption_type_raw;

  /* The filesystem payload (PFS0 or RomFS image) inside the section,
   * located from the hash-layer info. Bytes from NCA start. The pair is
   * validated to lie within [offset, offset + size). */
  uint64_t data_offset;
  uint64_t data_size;

  /* Layouts recorded but not yet supported; nca_section_source() returns
   * NULL and nca_probe_section() RESULT_NOT_IMPLEMENTED for any of them. */
  bool has_patch_info;
  bool has_sparse_info;
  bool has_compression_info;
} NCA_Section_Info;

typedef struct NCA_Header_Info {
  uint32_t magic;             /* NCA_MAGIC_NCA3 or NCA_MAGIC_NCA2 */
  NCA_Distribution_Type distribution_type;
  NCA_Content_Type content_type;
  uint64_t content_size;
  uint64_t program_id;
  uint32_t content_index;
  uint32_t sdk_addon_version;
  NCA_Section_Info sections[NCA_SECTION_COUNT];
} NCA_Header_Info;

typedef struct NCA_File {
  const Byte_Source *source;  /* caller's; must outlive this struct */
  NCA_Header_Info header;
  Byte_Source_Slice section_sources[NCA_SECTION_COUNT]; /* over data_offset/size */
} NCA_File;

/* ------------------------------------------------------------------ */
/* Parsing.                                                            */
/* ------------------------------------------------------------------ */

/* Decodes a header already in memory. Pure: no I/O, no allocation. Used
 * by nca_open and directly by tests.
 *   RESULT_ENCRYPTED_INPUT  magic is not NCA3/NCA2 (message: docs/DUMP.md)
 *   RESULT_NOT_IMPLEMENTED  magic is NCA0
 *   RESULT_INVALID_ARGUMENT NULL args; unknown content/fs type; FsHeader
 *                           version != NCA_FS_HEADER_VERSION on a present
 *                           section; hash type AUTO/AUTO_SHA3 or out of
 *                           range; a section whose start > end, or whose
 *                           hash-layer data region falls outside the
 *                           section; IVFC magic/level count invalid
 * Absent sections (start == end == 0) are skipped, not errors. The
 * content_size <= file size check needs the source and lives in nca_open. */
Error nca_parse_header(const uint8_t *header_bytes, NCA_Header_Info *out);

/* Reads NCA_HEADER_SIZE bytes from `source`, parses them, and prepares a
 * slice per present, supported section. Additionally to
 * nca_parse_header's results:
 *   RESULT_INVALID_ARGUMENT source smaller than NCA_HEADER_SIZE (message
 *                           distinguishes this from a bad magic so a
 *                           truncated file is not reported as encrypted);
 *                           content_size > source->size; a present
 *                           section extends past content_size
 *   RESULT_IO_ERROR         propagated from the source
 * On any error `out` is unspecified. */
Error nca_open(const Byte_Source *source, NCA_File *out);

/* The filesystem payload of section `index` as a bounded source, or NULL
 * if index >= NCA_SECTION_COUNT, the section is absent, or it uses an
 * unsupported (patch/sparse/compressed) layout. Valid while `nca` and its
 * underlying source live. */
const Byte_Source *nca_section_source(const NCA_File *nca, uint32_t index);

/* Confirms the section's payload has the structure magic its fs_type
 * implies - "PFS0" at offset 0 for NCA_FS_PARTITION_FS, a RomFS header
 * (header size field == ROMFS_HEADER_SIZE) at offset 0 for NCA_FS_ROMFS.
 * This is the second half of the encrypted-input check: a decrypted
 * header over still-ciphered sections fails here.
 *   RESULT_INVALID_ARGUMENT index out of range or section absent
 *   RESULT_NOT_IMPLEMENTED  patch/sparse/compressed section
 *   RESULT_ENCRYPTED_INPUT  magic missing (message: which section, and
 *                           docs/DUMP.md)
 *   RESULT_IO_ERROR         propagated
 * Reads at most 16 bytes; no allocation. The bootstrap (§12) calls this
 * before exefs_open/romfs_open so the user sees the §1.6 message instead
 * of a generic parse failure. */
Error nca_probe_section(const NCA_File *nca, uint32_t index);

/* Convenience for the common shapes. Returns the index of the first
 * present section with the given fs_type, or -1. For a PROGRAM NCA the
 * ExeFS is the PARTITION_FS section and the RomFS the ROMFS section. */
int nca_find_section(const NCA_File *nca, NCA_Fs_Type fs_type);

#endif /* SWITCH_HLE_LOADER_NCA_PARSE_H */
