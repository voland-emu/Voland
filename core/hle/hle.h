/**
 * Stub HLE dispatcher for Phase 0. Real syscall implementations arrive in
 * later phases (see docs/DESIGN.md section 12 for the priority list).
 *
 * For now the dispatcher:
 *   - owns an HLE_Context that holds the active CPU backend vtable
 *   - intercepts every SVC via the CPU backend's svc handler
 *   - logs the syscall id and writes HLE_RESULT_NOT_IMPLEMENTED back into X0
 */
#ifndef SWITCH_HLE_HLE_H
#define SWITCH_HLE_HLE_H

#include "cpu/cpu.h"

/* Switch OS result codes (subset relevant to Phase 0). Horizon Result
 * encoding: (description << 9) | module. Module 1 is the kernel - these
 * constants are genuine kernel results, e.g. 0xE401 = (114 << 9) | 1.
 * Service-specific results use their own module (§12). */
#define HLE_RESULT_SUCCESS 0x00000000u
#define HLE_RESULT_NOT_IMPLEMENTED 0xF601u
#define HLE_RESULT_INVALID_HANDLE 0xE401u
#define HLE_RESULT_INVALID_POINTER 0xCC01u
#define HLE_RESULT_OUT_OF_MEMORY 0x1A01u
#define HLE_RESULT_NOT_FOUND 0xE002u
#define HLE_RESULT_ALREADY_EXISTS 0xFA02u

#define HLE_MAKE_RESULT(module, description) \
  ((uint32_t)(((description) << 9) | ((module) & 0x1FF)))

typedef struct HLE_Context
{
  const CPU_Backend *cpu_backend;
  uint64_t svc_call_count;
} HLE_Context;

void hle_context_init(HLE_Context *context, const CPU_Backend *backend);

/* CPU_SVC_Handler-compatible entry point. `swi` is the SVC instruction's
 * immediate - the actual Horizon syscall id. It is NOT in X8; that is the
 * Linux ARM64 convention and does not apply here (§12). */
void hle_on_svc(CPU_State *cpu_state, uint32_t swi, void *userdata);

/* CPU_Undefined_Handler-compatible entry point. */
void hle_on_undefined(CPU_State *cpu_state, uint32_t instruction, void *userdata);

#endif /* SWITCH_HLE_HLE_H */
