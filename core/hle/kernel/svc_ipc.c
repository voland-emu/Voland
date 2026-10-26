/**
 * IPC SVCs. See svc_ipc.h for the register ABI (standard Horizon
 * convention; flagged for a libnx cross-check) and scope.
 */
#include "hle/kernel/svc_ipc.h"

#include "common/assert.h"
#include "common/log.h"

#include <string.h>

/* switchbrew's documented named-port name limit: up to 11 characters +
 * NUL. Voland recognises exactly "sm:" (svc_ipc.h) - everything else is
 * kernel/sysmodule-internal on real Horizon. */
#define SVC_IPC_PORT_NAME_MAX_BYTES 12u

void hle_svc_connect_to_named_port(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);

  /* Verified ABI (svc_ipc.h): the name pointer arrives in X1 - the
   * out-handle pointer (X0 in the caller's local frame) is never read by
   * the kernel, same reasoning as SetHeapSize's X0 (svc_memory.h). */
  const uint64_t name_gva = regs->x[1];

  char name[SVC_IPC_PORT_NAME_MAX_BYTES];
  memset(name, 0, sizeof(name));
  const Error read_err =
      vmm_read_block(context->vmm, name_gva, name, SVC_IPC_PORT_NAME_MAX_BYTES - 1u);
  if (!error_is_ok(read_err)) {
    regs->x[0] = HLE_RESULT_INVALID_POINTER;
    return;
  }

  if (strncmp(name, "sm:", sizeof(name)) != 0) {
    log_warn("[hle] ConnectToNamedPort: unknown port \"%.11s\"", name);
    regs->x[0] = HLE_RESULT_NOT_FOUND;
    return;
  }

  const uint32_t handle = ipc_session_table_open(&context->sessions, context->sm_object);
  if (handle == IPC_INVALID_HANDLE) {
    regs->x[0] = HLE_RESULT_OUT_OF_MEMORY;
    return;
  }

  regs->x[0] = HLE_RESULT_SUCCESS;
  regs->x[1] = handle;
}

void hle_svc_send_sync_request(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  const uint32_t handle = (uint32_t)regs->x[0];

  IPC_Session *session = ipc_session_table_get(&context->sessions, handle);
  if (!session) {
    regs->x[0] = HLE_RESULT_INVALID_HANDLE;
    return;
  }

  /* The whole command buffer round-trips through a local copy (vmm.h:
   * "HLE buffer traffic") rather than a vmm_guest_to_host borrow - this
   * keeps IPC_Request::data valid for the full
   * parse/dispatch/write_response chain below without any borrow-scope
   * bookkeeping, at the cost of one extra 0x100-byte copy each way. */
  uint8_t command_buffer[TLS_IPC_COMMAND_BUFFER_BYTES];
  const Error read_err = vmm_read_block(context->vmm, context->process->main_thread_tls_gva,
                                        command_buffer, sizeof(command_buffer));
  /* Phase 1 has exactly one thread (process.h) - the main thread's TLS
   * block is always the caller's. The Phase 2 scheduler's per-thread
   * CPU_State is what makes "which thread is calling" need an actual
   * answer; see that phase's Guest_Thread::tls_gva. */
  if (!error_is_ok(read_err)) {
    regs->x[0] = HLE_RESULT_INVALID_POINTER;
    return;
  }

  IPC_Request request;
  const Error parse_err =
      ipc_request_parse(command_buffer, sizeof(command_buffer), &request);
  if (!error_is_ok(parse_err)) {
    log_warn("[hle] SendSyncRequest: malformed or unsupported request on \"%s\" (%s)",
             session->object.interface ? session->object.interface->name : "<unbound>",
             parse_err.message);
    regs->x[0] = parse_err.code == RESULT_NOT_IMPLEMENTED ? HLE_RESULT_NOT_IMPLEMENTED
                                                           : HLE_RESULT_INVALID_POINTER;
    return;
  }

  IPC_Response response;
  ipc_dispatch(&session->object, &request, &response);
  if (response.result == HLE_RESULT_NOT_IMPLEMENTED) {
    log_warn("[hle] SendSyncRequest: unimplemented command 0x%x on \"%s\"", request.command_id,
             session->object.interface ? session->object.interface->name : "<unbound>");
  }

  /* Open a real session per out object the handler produced, in order -
   * ipc_response_write only serialises, it never allocates (ipc.h). */
  uint32_t out_handles[IPC_MAX_OUT_OBJECTS];
  for (uint32_t i = 0; i < response.out_object_count; i++) {
    out_handles[i] = ipc_session_table_open(&context->sessions, response.out_objects[i]);
    if (out_handles[i] == IPC_INVALID_HANDLE) {
      /* Table exhausted: the command already "succeeded" from the
       * handler's point of view, but Voland cannot hand out a handle for
       * it. Every session opened so far in this loop is leaked rather
       * than rolled back - IPC_MAX_SESSIONS=32 against Phase 1's single
       * thread making exactly one call at a time makes this
       * unreachable in practice; revisit if that stops being true. */
      regs->x[0] = HLE_RESULT_OUT_OF_MEMORY;
      return;
    }
  }

  const Error write_err = ipc_response_write(command_buffer, sizeof(command_buffer), &response,
                                             out_handles, response.out_object_count);
  SWITCH_ASSERT_ALWAYS(error_is_ok(write_err),
                       "hle_svc_send_sync_request: ipc_response_write failed on a response this "
                       "function built itself");

  const Error write_back_err = vmm_write_block(
      context->vmm, context->process->main_thread_tls_gva, command_buffer, sizeof(command_buffer));
  if (!error_is_ok(write_back_err)) {
    regs->x[0] = HLE_RESULT_INVALID_POINTER;
    return;
  }

  regs->x[0] = HLE_RESULT_SUCCESS;
}

void hle_svc_close_handle(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  const uint32_t handle = (uint32_t)regs->x[0];

  if (!ipc_session_table_close(&context->sessions, handle)) {
    regs->x[0] = HLE_RESULT_INVALID_HANDLE;
    return;
  }
  regs->x[0] = HLE_RESULT_SUCCESS;
}
