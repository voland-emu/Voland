/**
 * Process bootstrap. See process.h for the contract and docs/DESIGN.md
 * §12 for the steps.
 */
#include "hle/kernel/process.h"

#include "common/log.h"

#include <string.h>

/* X1 carries the main thread handle at entry (step 4). */
#define PROCESS_REG_MAIN_THREAD_HANDLE ((uint8_t)(CPU_REG_X0 + 1))

/* One stack on top of the modules; TLS pages roll back through their
 * own allocator. */
#define BOOTSTRAP_MAX_MAPPINGS (PROCESS_MAX_MODULES + 1u)

typedef struct Mapping_List {
  Address_Region ranges[BOOTSTRAP_MAX_MAPPINGS];
  uint32_t count;
} Mapping_List;

static uint64_t align_up_page(uint64_t value) {
  return (value + VMM_PAGE_OFFSET_MASK) & ~VMM_PAGE_OFFSET_MASK;
}

/* Bounded copy of a well-known module name (every name in the load
 * order fits PROCESS_MODULE_NAME_BYTES; the bound is defensive). */
static void copy_name(char out[PROCESS_MODULE_NAME_BYTES], const char *name) {
  const size_t length = strlen(name);
  const size_t copy =
      length < PROCESS_MODULE_NAME_BYTES - 1 ? length : PROCESS_MODULE_NAME_BYTES - 1;
  memcpy(out, name, copy);
  out[copy] = '\0';
}

/* Horizon load order: rtld, main, subsdk0..subsdk9, sdk. */
static void module_name_at(uint32_t index, char out[PROCESS_MODULE_NAME_BYTES]) {
  if (index == 0) {
    copy_name(out, EXEFS_FILE_RTLD);
  } else if (index == 1) {
    copy_name(out, EXEFS_FILE_MAIN);
  } else if (index < 2 + EXEFS_SUBSDK_COUNT) {
    const size_t prefix_length = strlen(EXEFS_FILE_SUBSDK_PREFIX);
    memcpy(out, EXEFS_FILE_SUBSDK_PREFIX, prefix_length);
    out[prefix_length] = (char)('0' + (index - 2));
    out[prefix_length + 1] = '\0';
  } else {
    copy_name(out, EXEFS_FILE_SDK);
  }
}

/* The scratch arena is shared with the caller (the ExeFS directory lives
 * in it), so a full arena_reset() is off limits; each staged segment is
 * released by rewinding to the mark taken at entry instead. */
static void arena_rewind_to(Arena *arena, size_t mark) {
  arena->used_bytes = mark;
}

static void unmap_all(VMM_Context *vmm, const Mapping_List *list) {
  for (uint32_t i = 0; i < list->count; i++) {
    const Error err = vmm_unmap(vmm, list->ranges[i].base, list->ranges[i].size);
    if (!error_is_ok(err)) {
      log_error("process: rollback unmap of 0x%llx failed: %s",
                (unsigned long long)list->ranges[i].base, err.message);
    }
  }
}

/* Allocates physical pages for `size` bytes and maps them RW at `gva`,
 * recording the range for rollback. */
static Error map_fresh_rw(VMM_Context *vmm, Page_Allocator *pages, Mapping_List *list,
                          uint64_t gva, uint64_t size) {
  uint64_t pa = 0;
  Error err = page_allocator_allocate(pages, size >> VMM_PAGE_BITS, &pa);
  if (!error_is_ok(err)) return err;
  err = vmm_map(vmm, gva, pa, size, VMM_PERM_RW);
  if (!error_is_ok(err)) return err;
  list->ranges[list->count].base = gva;
  list->ranges[list->count].size = size;
  list->count++;
  return OK;
}

/* Zeroes a freshly mapped RW range through a borrowed host pointer. */
static Error zero_range(VMM_Context *vmm, uint64_t gva, uint64_t size) {
  void *host = NULL;
  vmm_borrow_scope_begin(vmm);
  Error err = vmm_guest_to_host(vmm, gva, size, VMM_PERM_RW, &host);
  if (error_is_ok(err)) memset(host, 0, (size_t)size);
  vmm_borrow_scope_end(vmm);
  return err;
}

/* Fills the mapped RW image at `module->base_gva`: zero everything (so
 * .bss and inter-segment gaps are clean), decompress each segment into
 * place, then apply final permissions. */
static Error load_module_image(VMM_Context *vmm, Arena *scratch, size_t scratch_mark,
                               const NSO *nso, Process_Module *module) {
  void *host = NULL;
  vmm_borrow_scope_begin(vmm);
  Error err = vmm_guest_to_host(vmm, module->base_gva, module->image_size, VMM_PERM_RW, &host);
  if (error_is_ok(err)) {
    memset(host, 0, (size_t)module->image_size);
    for (uint32_t kind = 0; kind < NSO_SEGMENT_COUNT && error_is_ok(err); kind++) {
      const NSO_Segment *segment = &nso->segments[kind];
      err = nso_read_segment(nso, (NSO_Segment_Kind)kind, scratch,
                             (uint8_t *)host + segment->memory_offset, segment->memory_size);
      arena_rewind_to(scratch, scratch_mark);
    }
  }
  vmm_borrow_scope_end(vmm);
  if (!error_is_ok(err)) return err;

  /* Final permissions. Regions were page-rounded when the module was
   * described; the image is RW until here. */
  if (module->text.size > 0) {
    err = vmm_reprotect(vmm, module->text.base, module->text.size, VMM_PERM_RX);
    if (!error_is_ok(err)) return err;
  }
  if (module->rodata.size > 0) {
    err = vmm_reprotect(vmm, module->rodata.base, module->rodata.size, VMM_PERM_R);
    if (!error_is_ok(err)) return err;
  }
  /* .data + .bss stay RW. */
  return OK;
}

/* Derives the page-rounded segment regions of a module from its NSO. A
 * segment of size 0 yields an empty region. */
static void describe_module(const NSO *nso, const char *name, uint64_t base_gva,
                            Process_Module *module) {
  memset(module, 0, sizeof(*module));
  copy_name(module->name, name);
  module->base_gva = base_gva;
  module->image_size = nso->image_size;
  memcpy(module->module_id, nso->module_id, NSO_MODULE_ID_SIZE);

  const NSO_Segment *text = &nso->segments[NSO_SEGMENT_TEXT];
  const NSO_Segment *rodata = &nso->segments[NSO_SEGMENT_RODATA];
  const NSO_Segment *data = &nso->segments[NSO_SEGMENT_DATA];
  module->text.base = base_gva + text->memory_offset;
  module->text.size = align_up_page(text->memory_size);
  module->rodata.base = base_gva + rodata->memory_offset;
  module->rodata.size = align_up_page(rodata->memory_size);
  module->data.base = base_gva + data->memory_offset;
  module->data.size = nso->image_size - data->memory_offset; /* through .bss */
}

Error process_bootstrap(const Process_Bootstrap_Params *params, Process *out) {
  if (!params || !out || !params->exefs || !params->npdm || !params->vmm || !params->pages ||
      !params->scratch) {
    return ERR(RESULT_INVALID_ARGUMENT, "process_bootstrap: NULL argument");
  }
  const NPDM *npdm = params->npdm;
  if (npdm->main_thread_stack_size == 0 ||
      (npdm->main_thread_stack_size & VMM_PAGE_OFFSET_MASK) != 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "process_bootstrap: npdm stack size not page-aligned");
  }
  memset(out, 0, sizeof(*out));
  const size_t scratch_mark = params->scratch->used_bytes;

  /* Pass 1 (step 1's tail): open every present NSO in load order and
   * total the code region. Nothing is mapped yet, so failures here need
   * no rollback. */
  NSO nsos[PROCESS_MAX_MODULES];
  Byte_Source_Slice sources[PROCESS_MAX_MODULES];
  char names[PROCESS_MAX_MODULES][PROCESS_MODULE_NAME_BYTES];
  uint32_t module_count = 0;
  uint64_t code_size = 0;
  bool has_main = false;
  for (uint32_t i = 0; i < PROCESS_MAX_MODULES; i++) {
    char name[PROCESS_MODULE_NAME_BYTES];
    module_name_at(i, name);
    const ExeFS_Entry *entry = exefs_find(params->exefs, name);
    if (!entry) continue;

    Error err = exefs_entry_source(params->exefs, entry, &sources[module_count]);
    if (!error_is_ok(err)) return err;
    err = nso_open(&sources[module_count].source, &nsos[module_count]);
    if (!error_is_ok(err)) {
      log_error("process: %s: %s", name, err.message);
      return err;
    }
    if (code_size > UINT64_MAX - nsos[module_count].image_size) {
      return ERR(RESULT_INVALID_ARGUMENT, "process_bootstrap: code size overflow");
    }
    code_size += nsos[module_count].image_size;
    copy_name(names[module_count], name);
    if (i == 1) has_main = true;
    module_count++;
  }
  if (!has_main) {
    return ERR(RESULT_NOT_FOUND, "process_bootstrap: ExeFS has no `main` NSO");
  }

  /* Step 2 + 3a: the address space from the npdm's type. */
  Error err = address_space_init(npdm->address_space, code_size, params->aslr_seed,
                                 &out->address_space);
  if (!error_is_ok(err)) return err;

  /* Step 3b: map the modules back to back from the code base. From here
   * on every failure unwinds `mapped`. */
  Mapping_List mapped;
  mapped.count = 0;
  uint64_t cursor = out->address_space.code.base;
  for (uint32_t m = 0; m < module_count; m++) {
    Process_Module *module = &out->modules[m];
    describe_module(&nsos[m], names[m], cursor, module);
    err = map_fresh_rw(params->vmm, params->pages, &mapped, cursor, module->image_size);
    if (!error_is_ok(err)) goto fail;
    err = load_module_image(params->vmm, params->scratch, scratch_mark, &nsos[m], module);
    if (!error_is_ok(err)) goto fail;
    cursor += module->image_size;
    out->module_count++;
    log_info("process: %-8s at 0x%010llx (%llu KB)", module->name,
             (unsigned long long)module->base_gva,
             (unsigned long long)(module->image_size / 1024));
  }
  out->code_bytes_mapped = code_size;
  /* rtld is index 0 when present (load order is fixed). Enter at its
   * .text, not the image base: nso_open allows a page-aligned non-zero
   * .text offset, and the image base is RW there. */
  out->entry_point = out->modules[0].text.base;

  /* Step 4a: main-thread stack behind a guard page. */
  out->main_thread_stack.base =
      out->address_space.stack.base + PROCESS_STACK_GUARD_PAGES * VMM_PAGE_SIZE;
  out->main_thread_stack.size = npdm->main_thread_stack_size;
  /* Validate against the layout, not against vmm: a stack that spills
   * past the stack region would otherwise fail only if something else
   * happens to be mapped there. */
  if (!address_region_contains(&out->address_space.stack, out->main_thread_stack.base,
                               out->main_thread_stack.size)) {
    err = ERR(RESULT_INVALID_ARGUMENT,
              "process_bootstrap: main thread stack does not fit the stack region");
    goto fail;
  }
  err = map_fresh_rw(params->vmm, params->pages, &mapped, out->main_thread_stack.base,
                     out->main_thread_stack.size);
  if (!error_is_ok(err)) goto fail;
  err = zero_range(params->vmm, out->main_thread_stack.base, out->main_thread_stack.size);
  if (!error_is_ok(err)) goto fail;

  /* Step 4b: the process's TLS allocator over tls_io; the main thread's
   * block is its first allocation (block 0 of the first page, zeroed). */
  err = tls_allocator_init(&out->tls, params->vmm, params->pages, out->address_space.tls_io);
  if (!error_is_ok(err)) goto fail;
  err = tls_allocate(&out->tls, &out->main_thread_tls_gva);
  if (!error_is_ok(err)) goto fail;

  out->main_thread_handle = PROCESS_MAIN_THREAD_HANDLE;
  out->npdm = *npdm;
  log_info("process: '%s' bootstrapped, %u modules, entry 0x%010llx, stack 0x%010llx+0x%x, tls 0x%010llx",
           npdm->name, (unsigned)out->module_count, (unsigned long long)out->entry_point,
           (unsigned long long)out->main_thread_stack.base, (unsigned)npdm->main_thread_stack_size,
           (unsigned long long)out->main_thread_tls_gva);
  return OK;

fail:
  tls_allocator_teardown(&out->tls);
  unmap_all(params->vmm, &mapped);
  arena_rewind_to(params->scratch, scratch_mark);
  memset(out, 0, sizeof(*out));
  return err;
}

Error process_enter_main_thread(const Process *process, const CPU_Backend *backend,
                                CPU_State *state) {
  if (!process || !backend || !state) {
    return ERR(RESULT_INVALID_ARGUMENT, "process_enter_main_thread: NULL argument");
  }
  for (uint8_t reg = CPU_REG_X0; reg <= CPU_REG_X30; reg++) {
    backend->set_reg(state, reg, 0);
  }
  /* Horizon entry ABI (§12 step 4). X0 is the argument pointer, NULL
   * for a title. */
  backend->set_reg(state, CPU_REG_X0, 0);
  backend->set_reg(state, PROCESS_REG_MAIN_THREAD_HANDLE, process->main_thread_handle);
  backend->set_sp(state, process->main_thread_stack.base + process->main_thread_stack.size);
  backend->set_pc(state, process->entry_point);
  backend->set_pstate(state, 0);
  backend->set_sys_reg(state, CPU_SYSREG_TPIDRRO_EL0, process->main_thread_tls_gva);
  return OK;
}

const Process_Module *process_find_module(const Process *process, uint64_t gva) {
  if (!process) return NULL;
  for (uint32_t m = 0; m < process->module_count; m++) {
    const Process_Module *module = &process->modules[m];
    if (gva >= module->base_gva && gva - module->base_gva < module->image_size) {
      return module;
    }
  }
  return NULL;
}

void process_teardown(Process *process, VMM_Context *vmm) {
  if (!process || !vmm) return;
  Mapping_List mapped;
  mapped.count = 0;
  for (uint32_t m = 0; m < process->module_count; m++) {
    mapped.ranges[mapped.count].base = process->modules[m].base_gva;
    mapped.ranges[mapped.count].size = process->modules[m].image_size;
    mapped.count++;
  }
  if (process->main_thread_stack.size > 0) {
    mapped.ranges[mapped.count++] = process->main_thread_stack;
  }
  unmap_all(vmm, &mapped);
  tls_allocator_teardown(&process->tls);
  memset(process, 0, sizeof(*process));
}
