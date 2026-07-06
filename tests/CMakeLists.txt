# Phase 0 tests. smoke_phase0 is the cross-subsystem integration path
# (emulator/cpu/hle wired together); layout_test is common/layout's own
# unit test (§4). Further per-subsystem tests land alongside the
# subsystems they cover in later phases (CLAUDE.md: "every C subsystem
# ships tests in tests/").

add_executable(smoke_phase0 smoke_phase0.c)
target_link_libraries(smoke_phase0 PRIVATE switch_core)
add_test(NAME smoke_phase0 COMMAND smoke_phase0)

add_executable(layout_test layout_test.c)
target_link_libraries(layout_test PRIVATE switch_core)
add_test(NAME layout_test COMMAND layout_test)
