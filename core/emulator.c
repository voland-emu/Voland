#include "emulator.h"

#include "common/assert.h"
#include "common/log.h"

#include <string.h>

Error emulator_create(Emulator* out) {
  if (!out) {
    return ERR(RESULT_INVALID_ARGUMENT, "emulator_create: out is NULL");
  }

  memset(out, 0, sizeof(*out));

  /* 1. Linear memory layout (§4): every region reserved in one pass. */
  Error layout_err = layout_create();
  if (!error_is_ok(layout_err)) {
    return layout_err;
  }

  /* 2. CPU backend selected at configure time. vmm is NULL until Phase 1
   * (§5) implements it; the no-op backend never dereferences it. */
  out->cpu_backend = cpu_get_active_backend();
  SWITCH_ASSERT_ALWAYS(out->cpu_backend != NULL, "no CPU backend registered");

  out->cpu_state = out->cpu_backend->create(/*vmm=*/NULL, &out->hle);
  if (!out->cpu_state) {
    layout_destroy();
    memset(out, 0, sizeof(*out));
    return ERR(RESULT_OUT_OF_MEMORY, "emulator_create: CPU backend failed to init");
  }

  /* 3. HLE context + svc/undefined hooks. */
  hle_context_init(&out->hle, out->cpu_backend);
  out->cpu_backend->set_svc_handler(out->cpu_state, hle_on_svc);
  out->cpu_backend->set_undefined_handler(out->cpu_state, hle_on_undefined);

  log_info("[emulator] created (backend=%s %s)",
           out->cpu_backend->name,
           out->cpu_backend->version);

  return OK;
}

void emulator_destroy(Emulator* emulator) {
  if (!emulator) return;
  if (emulator->cpu_backend && emulator->cpu_state) {
    emulator->cpu_backend->destroy(emulator->cpu_state);
  }
  layout_destroy();
  memset(emulator, 0, sizeof(*emulator));
}

CPU_ExitReason emulator_run(Emulator* emulator, uint64_t cycle_budget) {
  SWITCH_ASSERT_ALWAYS(emulator != NULL, "emulator_run: emulator is NULL");
  return emulator->cpu_backend->run(emulator->cpu_state, cycle_budget);
}

CPU_ExitReason emulator_step(Emulator* emulator) {
  SWITCH_ASSERT_ALWAYS(emulator != NULL, "emulator_step: emulator is NULL");
  return emulator->cpu_backend->step(emulator->cpu_state);
}

const char* emulator_backend_name(const Emulator* emulator) {
  if (!emulator || !emulator->cpu_backend) return "<none>";
  return emulator->cpu_backend->name;
}
