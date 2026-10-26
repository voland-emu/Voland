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
 * ReplyAndReceive (0x27) likewise: it is the SERVER side of a session
 * (used by a process that itself registered a service and now waits for
 * requests on it) - Voland has no second guest process playing that
 * role; every "service" here is a host-side C call wired directly into
 * sm:'s registry (sm.h), never a guest-visible server loop. Proposals to
 * implement a guest-facing RegisterService/UnregisterService/
 * AcceptSession path belong against that same reasoning: redirect to
 * sm.h's header comment.
 *
 * Register ABI. Standard Horizon SVC convention (verify against libnx's
 * svc.s/svc.h before this lands, same as svc_memory.h's own note): W0 is
 * always the Result; an out-pointer C parameter is never itself sent to
 * the kernel, so the first real scalar argument occupies whichever
 * register the AAPCS would have given it, and an out VALUE the C
 * signature returns through a pointer comes back in W1 (W2, ... in C
 * param order) for the stub to write through that pointer.
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
 * ConnectToNamedPort's name scope: Voland accepts exactly "sm:" (up to 11
 * bytes + NUL, switchbrew's documented named-port name limit; read via
 * vmm_read_block, never a raw pointer per CLAUDE.md rule 3). Every other
 * named port is kernel/sysmodule-internal on real Horizon and no
 * application-facing title connects to one directly; a name other than
 * "sm:" is RESULT_NOT_FOUND, matching real kernel behaviour for an
 * unregistered port.
 *
 * Session storage: ipc.h's IPC_Session_Table (the Phase 1 handle-table
 * stand-in - see that header's "KNOWN SIMPLIFICATION"). This file assumes
 * HLE_Context gains an `IPC_Session_Table sessions` field (hle.h change,
 * proposed alongside this header) and that `hle_context_init` is
 * followed by a new `ipc_session_table_init(&context->sessions)` call and
 * a one-time `hle_svc_register_sm(context)` (see sm.h) at the same point
 * process.h's bootstrap currently arms the main thread - both are
 * emulator_create()/emulator_load_program() wiring, not part of this
 * header, called out here so the integration isn't silently assumed.
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
