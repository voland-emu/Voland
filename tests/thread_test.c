/**
 * core/hle/kernel/thread unit test: thread_create/thread_destroy over
 * the noop backend, asserting the armed register file and tpidrro_el0
 * through the CPU backend interface (§8) and the TLS block through the
 * allocator (tls.h). The SVC path is exercised the way smoke_phase0
 * does: the thread's state is handed to hle_on_svc and the result
 * register is read back.
 */
#define CHECK_NAME "thread_test"
#include "check.h"

#include "common/layout.h"
#include "common/vmm.h"
#include "hle/hle.h"
#include "hle/kernel/thread.h"
#include "hle/kernel/tls.h"

#define PAGE VMM_PAGE_SIZE
#define TLS_REGION_BASE ((uint64_t)0x20000000)
#define ENTRY ((uint64_t)0x8001000)
#define STACK_TOP ((uint64_t)0x9000000)
#define ARGUMENT ((uint64_t)0xC0FFEE0000000042ull)

static void expect_armed(const CPU_Backend *cpu, const Guest_Thread *thread,
                         const Thread_Create_Params *params) {
  CPU_State *state = thread->cpu_state;
  CHECK(state != NULL);
  CHECK(cpu->get_reg(state, CPU_REG_X0) == params->argument);
  for (uint8_t reg = CPU_REG_X0 + 1; reg <= CPU_REG_X30; reg++) {
    CHECK(cpu->get_reg(state, reg) == 0);
  }
  CHECK(cpu->get_sp(state) == params->stack_top);
  CHECK(cpu->get_pc(state) == params->entry_point);
  CHECK(cpu->get_pstate(state) == 0);
  CHECK(cpu->get_sys_reg(state, CPU_SYSREG_TPIDRRO_EL0) == thread->tls_gva);
  CHECK(thread->entry_point == params->entry_point);
  CHECK(thread->stack_top == params->stack_top);
  CHECK(thread->priority == params->priority);
  CHECK(thread->preferred_core == params->preferred_core);
}

int main(void) {
  CHECK_OK(layout_create());
  VMM_Context *vmm = vmm_create();
  CHECK(vmm != NULL);
  Page_Allocator pages;
  CHECK_OK(page_allocator_init(&pages, 0, 16 * PAGE));
  const CPU_Backend *cpu = cpu_get_active_backend();
  CHECK(cpu != NULL);
  HLE_Context hle;
  hle_context_init(&hle, cpu);
  TLS_Allocator tls;
  const Address_Region region = {TLS_REGION_BASE, 2 * PAGE};
  CHECK_OK(tls_allocator_init(&tls, vmm, &pages, region));
  const Thread_Env env = {cpu, vmm, &tls, &hle};

  const Thread_Create_Params params = {
      .entry_point = ENTRY,
      .argument = ARGUMENT,
      .stack_top = STACK_TOP,
      .priority = 44,
      .preferred_core = 1,
  };

  /* Create: TLS block owned, register file armed, tpidrro_el0 plumbed. */
  Guest_Thread a;
  CHECK_OK(thread_create(&env, &params, &a));
  expect_armed(cpu, &a, &params);
  CHECK(a.tls_gva == TLS_REGION_BASE);
  CHECK(tls_is_allocated(&tls, a.tls_gva));
  CHECK(tls.blocks_in_use == 1);

  /* A second thread: distinct state, distinct block, distinct tpidrro. */
  Thread_Create_Params params_b = params;
  params_b.argument = 7;
  params_b.stack_top = STACK_TOP - 0x10000;
  params_b.priority = THREAD_PRIORITY_LOWEST;
  params_b.preferred_core = THREAD_CORE_COUNT - 1;
  Guest_Thread b;
  CHECK_OK(thread_create(&env, &params_b, &b));
  expect_armed(cpu, &b, &params_b);
  CHECK(b.cpu_state != a.cpu_state);
  CHECK(b.tls_gva == a.tls_gva + TLS_BLOCK_BYTES);
  CHECK(cpu->get_sys_reg(a.cpu_state, CPU_SYSREG_TPIDRRO_EL0) !=
        cpu->get_sys_reg(b.cpu_state, CPU_SYSREG_TPIDRRO_EL0));
  CHECK(tls.blocks_in_use == 2);

  /* The state works with the HLE dispatcher: an SVC on the thread's
   * state is counted and answers in X0, as the emulator's state does. */
  const uint64_t svc_before = hle.svc_call_count;
  hle_on_svc(b.cpu_state, 0x01, &hle);
  CHECK(hle.svc_call_count == svc_before + 1);
  CHECK(cpu->get_reg(b.cpu_state, CPU_REG_X0) == HLE_RESULT_NOT_IMPLEMENTED);
  CHECK(cpu->get_reg(a.cpu_state, CPU_REG_X0) == ARGUMENT); /* a untouched */
  /* And obeys the bounded-run contract (§9). */
  CHECK(cpu->run(b.cpu_state, 100) == CPU_EXIT_CYCLES_ELAPSED);

  /* Rejections leave the pool untouched. */
  Guest_Thread rejected;
  Thread_Create_Params bad = params;
  bad.priority = THREAD_PRIORITY_LOWEST + 1;
  CHECK_CODE(thread_create(&env, &bad, &rejected), RESULT_INVALID_ARGUMENT);
  bad = params;
  bad.preferred_core = THREAD_CORE_COUNT;
  CHECK_CODE(thread_create(&env, &bad, &rejected), RESULT_INVALID_ARGUMENT);
  bad = params;
  bad.stack_top = STACK_TOP + 8;
  CHECK_CODE(thread_create(&env, &bad, &rejected), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(thread_create(NULL, &params, &rejected), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(thread_create(&env, NULL, &rejected), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(thread_create(&env, &params, NULL), RESULT_INVALID_ARGUMENT);
  Thread_Env no_hle = env;
  no_hle.hle = NULL;
  CHECK_CODE(thread_create(&no_hle, &params, &rejected), RESULT_INVALID_ARGUMENT);
  CHECK(tls.blocks_in_use == 2);

  /* TLS exhaustion propagates and leaks no state: fill the two-page
   * region, then one more. */
  Guest_Thread filler[2 * TLS_BLOCKS_PER_PAGE];
  uint32_t filled = 0;
  for (; filled < 2 * TLS_BLOCKS_PER_PAGE - 2; filled++) {
    CHECK_OK(thread_create(&env, &params, &filler[filled]));
  }
  CHECK(tls.blocks_in_use == 2 * TLS_BLOCKS_PER_PAGE);
  CHECK_CODE(thread_create(&env, &params, &rejected), RESULT_OUT_OF_MEMORY);
  CHECK(rejected.cpu_state == NULL);
  CHECK(tls.blocks_in_use == 2 * TLS_BLOCKS_PER_PAGE);

  /* Destroy frees the block; the next create reuses it. */
  const uint64_t a_tls = a.tls_gva;
  thread_destroy(&env, &a);
  CHECK(a.cpu_state == NULL && a.tls_gva == 0);
  CHECK(!tls_is_allocated(&tls, a_tls));
  CHECK(tls.blocks_in_use == 2 * TLS_BLOCKS_PER_PAGE - 1);
  Guest_Thread c;
  CHECK_OK(thread_create(&env, &params, &c));
  CHECK(c.tls_gva == a_tls);
  thread_destroy(&env, &c);
  thread_destroy(&env, &a); /* safe on a zeroed thread */
  thread_destroy(&env, &b);
  for (uint32_t i = 0; i < filled; i++) thread_destroy(&env, &filler[i]);
  CHECK(tls.blocks_in_use == 0);

  tls_allocator_teardown(&tls);
  vmm_destroy(vmm);
  layout_destroy();
  printf("[" CHECK_NAME "] ok\n");
  return 0;
}
