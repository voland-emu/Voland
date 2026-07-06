/**
 * Guest virtual memory manager (softmmu). See docs/DESIGN.md section 5.
 *
 * Phase 0 ships only the opaque type: the CPU backend interface (§8)
 * already takes a `VMM_Context*` at CPU_State creation, so this header
 * must exist before any backend does. Page tables, map/unmap/reprotect,
 * and the checked read/write/guest_to_host calling convention are a
 * Phase 1 deliverable (§5) - until then every caller passes NULL and no
 * backend dereferences it.
 */
#ifndef SWITCH_COMMON_VMM_H
#define SWITCH_COMMON_VMM_H

typedef struct VMM_Context VMM_Context;

#endif /* SWITCH_COMMON_VMM_H */
