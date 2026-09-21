/**
 * End-to-end smoke test. Exercises the full path:
 *   emulator_create -> layout reserved -> vmm live -> set register -> bounded run
 *   (no-op backend honors the exit-reason contract) -> fire an SVC
 *   manually via the swi immediate -> confirm the HLE dispatcher ran and
 *   wrote HLE_RESULT_NOT_IMPLEMENTED into X0.
 */
#include "common/layout.h"
#include "common/log.h"
#include "common/vmm.h"
#include "cpu/cpu.h"
#include "emulator.h"
#include "hle/hle.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                   \
  do                                                                  \
  {                                                                   \
    if (!(cond))                                                      \
    {                                                                 \
      fprintf(stderr, "[smoke] FAIL %s:%d: %s\n", __FILE__, __LINE__, \
              #cond);                                                 \
      exit(1);                                                        \
    }                                                                 \
  } while (0)

int main(void)
{
  Emulator emu;
  Error err = emulator_create(&emu);
  CHECK(err.code == RESULT_OK);
  CHECK(emu.cpu_backend != NULL);
  CHECK(emu.cpu_state != NULL);

  const CPU_Backend *cpu = emu.cpu_backend;

  /* Layout (§4): every region reserved, guest RAM sized for Switch 1. */
  const Memory_Layout *layout = layout_get();
  CHECK(layout != NULL);
  CHECK(layout->guest_ram_size == ((uint64_t)4 * 1024 * 1024 * 1024));
  CHECK(layout->guest_ram_base != 0);
  CHECK(layout->page_table_l1_base != 0);
  CHECK(layout->trace_buffer_base != 0);

  /* Softmmu (§5): created by emulator_create over the layout's L1 region
   * and handed to the backend. A round trip through a mapping proves the
   * checked path is wired end to end. */
  CHECK(emu.vmm != NULL);
  CHECK((uint64_t)(uintptr_t)vmm_page_table_l1(emu.vmm) == layout->page_table_l1_base);
  err = vmm_map(emu.vmm, 0x8000000ULL, 0x1000ULL, VMM_PAGE_SIZE, VMM_PERM_RW);
  CHECK(err.code == RESULT_OK);
  CHECK(vmm_write32(emu.vmm, 0x8000010ULL, 0xC0FFEE42u).code == RESULT_OK);
  uint32_t word = 0;
  CHECK(vmm_read32(emu.vmm, 0x8000010ULL, &word).code == RESULT_OK);
  CHECK(word == 0xC0FFEE42u);
  CHECK(vmm_unmap(emu.vmm, 0x8000000ULL, VMM_PAGE_SIZE).code == RESULT_OK);

  /* Register read/write round-trip. */
  cpu->set_reg(emu.cpu_state, CPU_REG_X0, 0xdeadbeefcafebabeULL);
  CHECK(cpu->get_reg(emu.cpu_state, CPU_REG_X0) == 0xdeadbeefcafebabeULL);

  cpu->set_reg(emu.cpu_state, CPU_REG_X30, 0x1122334455667788ULL);
  CHECK(cpu->get_reg(emu.cpu_state, CPU_REG_X30) == 0x1122334455667788ULL);

  /* PC round-trip. */
  cpu->set_pc(emu.cpu_state, 0x1000);
  CHECK(cpu->get_pc(emu.cpu_state) == 0x1000);

  /* get_register_file gives HLE its fast path onto the same state. */
  CPU_Register_File *regs = cpu->get_register_file(emu.cpu_state);
  CHECK(regs != NULL);
  CHECK(regs->pc == 0x1000);
  CHECK(regs->x[0] == 0xdeadbeefcafebabeULL);

  /* Bounded run: the no-op backend must honor the exit-reason contract
   * (§9) rather than running to completion. */
  const CPU_ExitReason reason = emulator_run(&emu, 5000);
  CHECK(reason == CPU_EXIT_CYCLES_ELAPSED);
  CHECK(cpu->get_cycles_consumed(emu.cpu_state) == 5000);

  /* Simulate the backend calling the SVC handler on a CPU_EXIT_SVC: `swi`
   * is the SVC instruction's immediate, the actual Horizon syscall id -
   * never X8 (§12). SetHeapSize is swi 0x01. */
  hle_on_svc(emu.cpu_state, 0x01, &emu.hle);
  CHECK(regs->x[0] == HLE_RESULT_NOT_IMPLEMENTED);
  CHECK(emu.hle.svc_call_count == 1);

  emulator_destroy(&emu);
  printf("[smoke] OK\n");
  return 0;
}
