/**
 * IPC SVCs. See svc_ipc.h for the register ABI (verified against libnx's
 * svc.s), scope, and the session-rollback rules.
 */
#include "hle/kernel/svc_ipc.h"

#include "common/assert.h"
#include "common/log.h"

#include <string.h>

/* Named-port name limit: up to 11 characters + NUL. Voland recognises
 * exactly "sm:" (svc_ipc.h) - everything else is kernel/sysmodule-internal
 * on real Horizon. */
#define SVC_IPC_PORT_NAME_MAX_BYTES 12u
#define SVC_IPC_SM_PORT_NAME "sm:"

static const char *session_name(const IPC_Session *session) {
  return session->object.interface ? session->object.interface->name : "<unbound>";
}

/* Closes the first `count` handles of `handles` - the rollback for a
 * response whose handles never reach the guest. */
static void close_handles(IPC_Session_Table *sessions, const uint32_t *handles, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    const bool closed = ipc_session_table_close(sessions, handles[i]);
    SWITCH_ASSERT_ALWAYS(closed, "svc_ipc: rolling back a session that was never opened");
  }
}

void hle_svc_connect_to_named_port(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);

  /* Verified ABI (svc_ipc.h): the name pointer arrives in X1 - the
   * out-handle pointer (X0 in the caller's local frame) is never read by
   * the kernel, same reasoning as SetHeapSize's X0 (svc_memory.h). */
  const uint64_t name_gva = regs->x[1];

  /* Byte by byte up to the NUL, like the kernel's user-string copy: a
   * valid name that ends just before an unmapped page must not fault. */
  char name[SVC_IPC_PORT_NAME_MAX_BYTES];
  uint32_t length = 0;
  for (;;) {
    if (length == SVC_IPC_PORT_NAME_MAX_BYTES) {
      regs->x[0] = HLE_RESULT_OUT_OF_RANGE; /* no NUL within the limit */
      return;
    }
    const Error read_err = vmm_read_block(context->vmm, name_gva + length, &name[length], 1);
    if (!error_is_ok(read_err)) {
      regs->x[0] = HLE_RESULT_INVALID_POINTER;
      return;
    }
    if (name[length] == '\0') break;
    length++;
  }

  if (strcmp(name, SVC_IPC_SM_PORT_NAME) != 0) {
    log_warn("[hle] ConnectToNamedPort: unknown port \"%s\"", name);
    regs->x[0] = HLE_RESULT_NOT_FOUND;
    return;
  }

  const uint32_t handle = ipc_session_table_open(&context->sessions, context->sm_object);
  if (handle == IPC_INVALID_HANDLE) {
    regs->x[0] = HLE_RESULT_OUT_OF_SESSIONS;
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
  if (session->server_closed) {
    regs->x[0] = HLE_RESULT_CONNECTION_CLOSED;
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
             session_name(session), parse_err.message);
    regs->x[0] = parse_err.code == RESULT_NOT_IMPLEMENTED ? HLE_RESULT_NOT_IMPLEMENTED
                                                           : HLE_RESULT_INVALID_POINTER;
    return;
  }

  if (request.type == IPC_MESSAGE_TYPE_CLOSE) {
    /* libnx's serviceClose: Close, then svcCloseHandle. The server end
     * goes away and nothing is written back; libnx discards this SVC's
     * Result, so success is reported rather than modelling the exact
     * kernel Result for a request the server answers by closing. */
    session->server_closed = true;
    regs->x[0] = HLE_RESULT_SUCCESS;
    return;
  }

  IPC_Response response;
  ipc_dispatch(&session->object, &request, &response);
  if (response.result == HLE_RESULT_NOT_IMPLEMENTED) {
    log_warn("[hle] SendSyncRequest: unimplemented command 0x%x on \"%s\"", request.command_id,
             session_name(session));
  }

  /* Open a real session per out object the handler produced, in order -
   * ipc_response_write only serialises, it never allocates (ipc.h). Any
   * failure from here on rolls back every session this response opened:
   * a handle the guest never receives must not hold a slot. */
  uint32_t out_handles[IPC_MAX_OUT_OBJECTS];
  for (uint32_t i = 0; i < response.out_object_count; i++) {
    out_handles[i] = ipc_session_table_open(&context->sessions, response.out_objects[i]);
    if (out_handles[i] == IPC_INVALID_HANDLE) {
      close_handles(&context->sessions, out_handles, i);
      regs->x[0] = HLE_RESULT_OUT_OF_SESSIONS;
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
    close_handles(&context->sessions, out_handles, response.out_object_count);
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
