/**
 * Random-access byte source for the loader subsystem (§12, §15).
 *
 * Game content never lives in a C buffer the core owns. On web, decrypted
 * NCAs are `File` objects held by the CPU worker and read piecewise with
 * `blob.slice()` (§15: "never copy multi-GB game files into browser
 * storage"); on native they are files on disk. The core must not know
 * which. Every loader parser (nca_parse, exefs, romfs, and later nso/nro)
 * therefore reads through this vtable instead of taking a pointer to the
 * whole image. The platform supplies `read`; the parsers supply
 * bounds-checking and structure.
 *
 * Contract for `read` implementations:
 *   - Synchronous. It returns only when `size` bytes at `offset` are in
 *     `out`, or with an error and `out` unspecified. The web platform
 *     realizes this with FileReaderSync over `File.slice()` inside the
 *     CPU worker (synchronous Blob reads are available in dedicated
 *     workers); native uses pread/ReadFile.
 *   - Called only with `offset + size <= source->size` (byte_source_read
 *     checks this before delegating), so an implementation need not
 *     re-validate range - a short read is RESULT_IO_ERROR, never OK.
 *   - Re-entrant with respect to other Byte_Sources but never called
 *     concurrently on the same source: the loader is single-threaded
 *     (it runs on the CPU worker before the scheduler starts).
 *
 * Slices give a parser a bounded view of a parent source (an NCA section,
 * a file inside an ExeFS) with no copying: the slice forwards reads to its
 * parent at `parent_offset + offset`. Slice storage is caller-owned and
 * must outlive every read through it; a slice holds a POINTER to its
 * parent, so the parent must outlive the slice too.
 *
 * Endianness: every on-media integer in NCA/PFS0/RomFS/NPDM is little-
 * endian. Parsers decode fields with the byte_source_le* helpers rather
 * than casting struct pointers over buffers, so they are correct for any
 * host alignment and endianness and never rely on packed-struct layout.
 */
#ifndef SWITCH_HLE_LOADER_BYTE_SOURCE_H
#define SWITCH_HLE_LOADER_BYTE_SOURCE_H

#include <stdint.h>

#include "common/result.h"

/* Reads exactly `size` bytes starting at `offset` into `out`. Only ever
 * invoked through byte_source_read(), which has already validated the
 * range against the source size and rejected size == 0. */
typedef Error (*Byte_Source_Read_Fn)(void *user, uint64_t offset, void *out,
                                     uint64_t size);

typedef struct Byte_Source {
  void *user;             /* opaque; passed back to `read` */
  uint64_t size;          /* total bytes addressable through this source */
  Byte_Source_Read_Fn read;
} Byte_Source;

/* A bounded window [parent_offset, parent_offset + source.size) onto a
 * parent source. `source` is the usable Byte_Source; its `user` points at
 * the enclosing slice. Fill with byte_source_slice(); never by hand. */
typedef struct Byte_Source_Slice {
  Byte_Source source;
  const Byte_Source *parent;
  uint64_t parent_offset;
} Byte_Source_Slice;

/* ------------------------------------------------------------------ */
/* Reading.                                                            */
/* ------------------------------------------------------------------ */

/* Bounds-checked read. RESULT_INVALID_ARGUMENT if `src`, `src->read` or
 * `out` is NULL, or if [offset, offset + size) exceeds src->size
 * (including overflow of offset + size). size == 0 is a no-op returning
 * OK without touching `read`. Otherwise forwards to src->read. */
Error byte_source_read(const Byte_Source *src, uint64_t offset, void *out,
                       uint64_t size);

/* ------------------------------------------------------------------ */
/* Constructors.                                                       */
/* ------------------------------------------------------------------ */

/* A source over caller-owned memory (test fixtures, small blobs already
 * pulled into an arena such as main.npdm). The memory must outlive the
 * source; nothing is copied. `bytes` may be NULL only when size == 0. */
Byte_Source byte_source_from_memory(const void *bytes, uint64_t size);

/* Initializes `out` as a window onto `parent`. RESULT_INVALID_ARGUMENT if
 * `parent` or `out` is NULL or the window exceeds parent->size. A
 * zero-size window is legal (reads of size 0 succeed, any other read
 * fails the range check). */
Error byte_source_slice(const Byte_Source *parent, uint64_t offset,
                        uint64_t size, Byte_Source_Slice *out);

/* ------------------------------------------------------------------ */
/* Little-endian field decoding (all Switch on-media formats are LE).   */
/* `p` must point at least `n` readable bytes; alignment is irrelevant. */
/* ------------------------------------------------------------------ */

static inline uint16_t byte_source_le16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t byte_source_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline uint64_t byte_source_le64(const uint8_t *p) {
  return (uint64_t)byte_source_le32(p) | ((uint64_t)byte_source_le32(p + 4) << 32);
}

#endif /* SWITCH_HLE_LOADER_BYTE_SOURCE_H */
