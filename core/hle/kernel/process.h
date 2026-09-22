/**
 * Process bootstrap: what "load a game" actually does. See docs/DESIGN.md
 * §12 ("Process bootstrap"), steps 2-4, over the parsers from step 1:
 *
 *   2. npdm supplies the address-space type, main-thread priority/core/
 *      stack size and the capability list.
 *   3. Map the ExeFS's NSOs at an ASLR'd base in the 39-bit space
 *      (address_space.h); carve alias/heap/stack/TLS regions.
 *   4. Allocate the main thread: a TLS block, a stack, and the Horizon
 *      entry ABI - X0 = 0, X1 = main thread handle, PC = rtld entry,
 *      SP = stack top, tpidrro_el0 = TLS block.
 *
 * Every byte reaches guest memory through vmm (CLAUDE.md rule 3): the
 * bootstrap allocates physical pages (page_allocator.h), maps them RW,
 * borrows a host pointer for the copy pass inside a vmm borrow scope
 * (vmm.h: "loaders wrap their section-copy pass"), decompresses NSO
 * segments straight into place, then reprotects each segment to its
 * final permissions. No pointer arithmetic into guest RAM lives here.
 *
 * Module order and placement follow Horizon's loader: `rtld`, `main`,
 * `subsdk0`..`subsdk9`, `sdk`, each image page-aligned immediately after
 * the previous one, starting at the code region base. `main` is
 * mandatory; every other module is optional. The entry point is rtld's
 * .text base when rtld is present, else main's (system modules and
 * some homebrew ship without rtld).
 *
 * Segment permissions: .text RX, .rodata R, .data + .bss RW. An NSO's
 * execute-only-text flag (firmware 20.0.0+) is honored as RX, not X:
 * the guest cannot observe the difference without a fault it never
 * takes, and X-only text would forbid the interpreter's own fetches.
 *
 * Main-thread stack: `main_thread_stack_size` from the npdm, placed at
 * the start of the stack region behind one unmapped guard page. TLS:
 * the process owns a TLS_Allocator (tls.h) over its tls_io region; the
 * main thread's block is the allocator's first allocation (block 0 of
 * the first TLS page), and threads created later (thread.h) draw from
 * the same allocator.
 *
 * The main thread handle is PROCESS_MAIN_THREAD_HANDLE. Horizon encodes
 * handles as (linear_id << 15) | index with linear ids starting at 1, so
 * the first entry of a fresh table is 0x8000; the Phase 2 handle table
 * must issue exactly that entry to the main thread. Until it exists the
 * constant is the contract.
 *
 * Failure leaves no mapping behind: every vmm_map the bootstrap made is
 * unmapped before the error returns. Physical pages are NOT returned
 * (the allocator cannot free yet, page_allocator.h); a caller that
 * retries resets the allocator. The scratch arena is likewise left
 * advanced - callers reset it.
 *
 * What is deliberately NOT here: kernel capability enforcement (the SVC
 * mask is consulted by the HLE dispatcher at call time via
 * npdm_svc_allowed), the argument block (switchbrew "Arguments": only
 * written when launch arguments exist, which they never do for a title),
 * and anything the scheduler owns (§7): the bootstrap produces a ready
 * register file; scheduler_tick runs it like any other guest thread.
 */
#ifndef SWITCH_HLE_KERNEL_PROCESS_H
#define SWITCH_HLE_KERNEL_PROCESS_H

#include <stdbool.h>
#include <stdint.h>

#include "common/arena.h"
#include "common/result.h"
#include "common/vmm.h"
#include "cpu/cpu.h"
#include "hle/kernel/address_space.h"
#include "hle/kernel/page_allocator.h"
#include "hle/kernel/tls.h"
#include "hle/loader/exefs.h"
#include "hle/loader/npdm.h"
#include "hle/loader/nso.h"

/* rtld + main + subsdk0..9 + sdk. */
#define PROCESS_MAX_MODULES 13u
#define PROCESS_MODULE_NAME_BYTES 8u /* "subsdk9" + NUL */

/* Handle encoding (linear_id << 15) | index; first entry, first id. */
#define PROCESS_MAIN_THREAD_HANDLE ((uint32_t)0x8000)

/* One guard page below the main-thread stack. */
#define PROCESS_STACK_GUARD_PAGES ((uint64_t)1)

typedef struct Process_Module {
  char name[PROCESS_MODULE_NAME_BYTES]; /* ExeFS file name */
  uint64_t base_gva;                    /* image base; page-aligned */
  uint64_t image_size;                  /* NSO image_size: through .bss */
  Address_Region text;                  /* [base + memory_offset, + size) */
  Address_Region rodata;
  Address_Region data;                  /* .data + .bss, page-rounded */
  uint8_t module_id[NSO_MODULE_ID_SIZE]; /* build id; §19 mod matching */
} Process_Module;

typedef struct Process {
  NPDM npdm; /* copy; the bootstrap's source of thread/AS parameters */
  Address_Space address_space;

  Process_Module modules[PROCESS_MAX_MODULES]; /* in load order */
  uint32_t module_count;
  uint64_t entry_point; /* rtld (or main) .text base */

  /* Main thread. */
  uint32_t main_thread_handle;
  Address_Region main_thread_stack; /* mapped RW; SP starts at base + size */
  uint64_t main_thread_tls_gva;     /* first block from `tls` */

  /* TLS blocks for every thread of this process (tls.h). One block is
   * in use after bootstrap: the main thread's. */
  TLS_Allocator tls;

  /* Bookkeeping the memory HLE reads. */
  uint64_t code_bytes_mapped; /* sum of image sizes */
} Process;

typedef struct Process_Bootstrap_Params {
  const ExeFS *exefs;    /* opened; its source must outlive the call */
  const NPDM *npdm;      /* parsed main.npdm */
  VMM_Context *vmm;
  Page_Allocator *pages;
  Arena *scratch;        /* NSO staging (compressed bytes) */
  uint64_t aslr_seed;    /* 0 = no ASLR (address_space.h) */
} Process_Bootstrap_Params;

/* Performs steps 2-4. On success `out` describes a fully mapped process
 * whose main thread is ready to run; on failure `out` is unspecified and
 * every mapping made has been undone.
 *   RESULT_INVALID_ARGUMENT NULL params or members; an NSO fails to
 *                           parse (nso_open's reasons); the modules do
 *                           not fit the code region; the npdm stack size
 *                           is zero, not page-aligned, or (with its guard
 *                           page) does not fit the stack region
 *   RESULT_NOT_FOUND        no `main` in the ExeFS
 *   RESULT_NOT_IMPLEMENTED  address-space type other than 39-bit; an NSO
 *                           refused by nso_open for that reason (zstd)
 *   RESULT_OUT_OF_MEMORY    physical pages, L2 tables, or scratch exhausted
 *   RESULT_IO_ERROR         propagated from the source */
Error process_bootstrap(const Process_Bootstrap_Params *params, Process *out);

/* Writes the Horizon entry ABI into `state` through the backend: X0 = 0,
 * X1 = main thread handle, every other X register 0, SP = stack top,
 * PC = entry_point, PSTATE = 0, tpidrro_el0 = TLS block (via
 * set_sys_reg with CPU_SYSREG_TPIDRRO_EL0). RESULT_INVALID_ARGUMENT on
 * NULL. Idempotent: calling it again re-arms the same start state. */
Error process_enter_main_thread(const Process *process, const CPU_Backend *backend,
                                CPU_State *state);

/* The module containing `gva`, or NULL. For fault reporting and §19
 * buildId lookups. */
const Process_Module *process_find_module(const Process *process, uint64_t gva);

/* Unmaps everything the bootstrap mapped (modules, stack, every TLS
 * page) and zeroes `process`. Physical pages are not reclaimed (page_allocator.h).
 * Safe on a zeroed Process. */
void process_teardown(Process *process, VMM_Context *vmm);

#endif /* SWITCH_HLE_KERNEL_PROCESS_H */
