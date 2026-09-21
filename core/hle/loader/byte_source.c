#include "hle/loader/byte_source.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Reading.                                                            */
/* ------------------------------------------------------------------ */

Error byte_source_read(const Byte_Source *src, uint64_t offset, void *out,
                       uint64_t size) {
  if (!src || !src->read) {
    return ERR(RESULT_INVALID_ARGUMENT, "byte_source_read: NULL source");
  }
  if (size == 0) return OK;
  if (!out) return ERR(RESULT_INVALID_ARGUMENT, "byte_source_read: NULL out");
  /* offset + size must not wrap and must stay inside the source. */
  if (offset > src->size || size > src->size - offset) {
    return ERR(RESULT_INVALID_ARGUMENT, "byte_source_read: range outside source");
  }
  return src->read(src->user, offset, out, size);
}

/* ------------------------------------------------------------------ */
/* Memory-backed source.                                               */
/* ------------------------------------------------------------------ */

static Error memory_source_read(void *user, uint64_t offset, void *out,
                                uint64_t size) {
  /* byte_source_read already proved [offset, offset + size) is inside the
   * source, so the pointer arithmetic below cannot leave the buffer. */
  const uint8_t *bytes = (const uint8_t *)user;
  memcpy(out, bytes + offset, (size_t)size);
  return OK;
}

Byte_Source byte_source_from_memory(const void *bytes, uint64_t size) {
  Byte_Source src;
  /* The vtable takes a mutable user pointer; the memory source never
   * writes through it. */
  src.user = (void *)(uintptr_t)bytes;
  src.size = size;
  src.read = memory_source_read;
  return src;
}

/* ------------------------------------------------------------------ */
/* Slices.                                                             */
/* ------------------------------------------------------------------ */

static Error slice_read(void *user, uint64_t offset, void *out, uint64_t size) {
  const Byte_Source_Slice *slice = (const Byte_Source_Slice *)user;
  return byte_source_read(slice->parent, slice->parent_offset + offset, out, size);
}

Error byte_source_slice(const Byte_Source *parent, uint64_t offset,
                        uint64_t size, Byte_Source_Slice *out) {
  if (!parent || !out) {
    return ERR(RESULT_INVALID_ARGUMENT, "byte_source_slice: NULL argument");
  }
  if (offset > parent->size || size > parent->size - offset) {
    return ERR(RESULT_INVALID_ARGUMENT, "byte_source_slice: window outside parent");
  }
  out->parent = parent;
  out->parent_offset = offset;
  out->source.user = out;
  out->source.size = size;
  out->source.read = slice_read;
  return OK;
}
