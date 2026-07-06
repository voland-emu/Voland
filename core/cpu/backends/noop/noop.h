/**
 * The default CPU backend. Maintains register state so HLE services can
 * read/write registers, but executes no ARM instructions. Every subsystem
 * except actual game execution can be built and tested with the no-op
 * backend. See docs/DESIGN.md section 9.
 */
#ifndef SWITCH_CPU_BACKENDS_NOOP_H
#define SWITCH_CPU_BACKENDS_NOOP_H

#include "cpu/cpu.h"

/* CPU_BACKEND_NOOP is declared in cpu.h; this header exists so tests and
 * integration code can include it explicitly without coupling to cpu.h's
 * registry layout. */

#endif /* SWITCH_CPU_BACKENDS_NOOP_H */
