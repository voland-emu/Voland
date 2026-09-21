# smoke_phase0 is the cross-subsystem integration path (emulator/cpu/hle/
# vmm wired together); layout_test and vmm_test are common/layout's (§4)
# and common/vmm's (§5) own unit tests. Further per-subsystem tests land
# alongside the subsystems they cover (CLAUDE.md: "every C subsystem
# ships tests in tests/").

add_executable(smoke_phase0 smoke_phase0.c)
target_link_libraries(smoke_phase0 PRIVATE switch_core)
add_test(NAME smoke_phase0 COMMAND smoke_phase0)

add_executable(layout_test layout_test.c)
target_link_libraries(layout_test PRIVATE switch_core)
add_test(NAME layout_test COMMAND layout_test)

add_executable(vmm_test vmm_test.c)
target_link_libraries(vmm_test PRIVATE switch_core)
add_test(NAME vmm_test COMMAND vmm_test)
