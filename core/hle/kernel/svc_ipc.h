/**
 * IPC SVCs: ConnectToNamedPort, SendSyncRequest, CloseHandle - the thin
 * layer over ipc.h's protocol/dispatch machinery, the same relationship
 * svc_memory.h has to vmm.h. Phase 1, §25 checkbox "Minimal IPC + sm:
 * stub"; §12 priority list item 3.
 *
 * Scope. Exactly these three SVCs. SendSyncRequestWithUserBuffer (0x22)
 * and SendAsyncRequestWithUserBuffer (0x23) stay HLE_RESULT_NOT_IMPLEMENTED
 * in hle.c's dispatch table - they exist so a reply can land in a buffer
 * other than the caller's TLS block, which nothing in Phase 1 needs.
 * ReplyAndReceive (0x43) likewise: it is the SERVER side of a session
 * (used by a process that itself registered a service and now waits for
 * requests on it) - Voland has no second guest process playing that
 * role; every "service" here is a host-side C call wired directly into
 * sm:'s registry (sm.h), never a guest-visible server loop. Proposals to
 * implement a guest-facing RegisterService/UnregisterService/
 * AcceptSession path belong against that same reasoning: redirect to
 * sm.h's header comment.
 *
 * Register ABI, verified against libnx's svc.s (b156b47): W0 is always
 * the Result; an out-pointer C parameter is never itself sent to the
 * kernel (libnx's stub spills X0 to its own stack and stores W1 through
 * it after the SVC returns), so an out VALUE comes back in W1. SVC ids:
 * ConnectToNamedPort 0x1F, SendSyncRequest 0x21, CloseHandle 0x16.
 *
 *   ConnectToNamedPort  C: Result(Handle* handle_out, const char* name)
 *                       in:  X1 = name (a GUEST address; handle_out
 *                            occupies X0 in the stub's local frame only,
 *                            never read by the kernel - same reasoning
 *                            as SetHeapSize's X0, svc_memory.h)
 *                       out: W0 = Result, W1 = handle
 *
 *   SendSyncRequest     C: Result(Handle handle)
 *                       in:  X0 = handle
 *                       out: W0 = Result
 *                       (the actual request/response payload is never in
 *                       registers - it is entirely the TLS command
 *                       buffer, both directions; that is HIPC's whole
 *                       point)
 *
 *   CloseHandle         C: Result(Handle handle)
 *                       in:  X0 = handle
 *                       out: W0 = Result
 *
 * ConnectToNamedPort's name scope: Voland accepts exactly "sm:". The
 * name is read from guest memory one byte at a time up to its NUL (never
 * a raw pointer, CLAUDE.md rule 3), so a name that ends just before an
 * unmapped page reads cleanly; a byte that faults is
 * HLE_RESULT_INVALID_POINTER, and no NUL within 12 bytes (11 characters +
 * NUL) is HLE_RESULT_OUT_OF_RANGE. Every other named port is
 * kernel/sysmodule-internal on real Horizon and no application-facing
 * title connects to one directly; a name other than "sm:" is
 * HLE_RESULT_NOT_FOUND, the kernel's Result for an unregistered port.
 * libnx's smInitialize retries on exactly that Result, so it must stay
 * the kernel's NotFound.
 *
 * Session results: a full session table is HLE_RESULT_OUT_OF_SESSIONS,
 * both for ConnectToNamedPort and for a response's out objects. If a
 * response cannot be delivered (table exhausted part-way, or the write
 * back to TLS faults), every session that response opened is closed
 * again before the SVC returns. A Close message (libnx's serviceClose)
 * marks the session's server end closed and writes no response; later
 * requests on it return HLE_RESULT_CONNECTION_CLOSED, and CloseHandle
 * still releases the handle.
 *
 * Session storage: HLE_Context::sessions (ipc.h's IPC_Session_Table, the
 * Phase 1 handle-table stand-in - see that header's "KNOWN
 * SIMPLIFICATION"), emptied by hle_context_init and on program unload by
 * hle_reset_process_state (hle.h). The "sm:" port resolves to
 * HLE_Context::sm_object, which emulator.c binds at emulator_create().
 */
#ifndef SWITCH_HLE_KERNEL_SVC_IPC_H
#define SWITCH_HLE_KERNEL_SVC_IPC_H

#include "cpu/cpu.h"
#include "hle/hle.h"

/* CPU_SVC_Handler-shaped, called from hle_on_svc's dispatch (§12) with the
 * same (context, cpu_state) pair hle_on_svc itself received - same
 * contract as svc_memory.h's handlers. */
void hle_svc_connect_to_named_port(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_send_sync_request(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_close_handle(HLE_Context *context, CPU_State *cpu_state);

#endif /* SWITCH_HLE_KERNEL_SVC_IPC_H */
